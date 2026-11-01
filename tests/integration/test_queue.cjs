// Two torrents share a tracker and seed. Assertions cover actual transfer overlap.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { availablePort } = require('./fixture_helpers.cjs');

function encode(value) {
  if (Buffer.isBuffer(value)) return Buffer.concat([Buffer.from(`${value.length}:`), value]);
  if (typeof value === 'string') return encode(Buffer.from(value));
  if (typeof value === 'number') return Buffer.from(`i${value}e`);
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(k => [encode(k), encode(value[k])]), Buffer.from('e')]);
}
const sha1 = data => crypto.createHash('sha1').update(data).digest();
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}

async function main() {
  const executable = path.resolve(process.argv[2]), mode = process.argv[3] || 'serial';
  const scratch = fs.existsSync(path.join(process.cwd(), 'build')) ? path.join(process.cwd(), 'build') : process.cwd();
  const work = fs.mkdtempSync(path.join(scratch, 'queue-test-'));
  const seedMode = mode.startsWith('seed-') || mode === 'ui';
  const collision = mode === 'collision' || mode === 'seed-collision';
  const pieceLength = 32768;
  const torrents = [0, 1].map(id => {
    const data = Buffer.alloc(100003);
    for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + id * 31) % 256;
    const pieces = [];
    for (let offset = 0; offset < data.length; offset += pieceLength) pieces.push(sha1(data.subarray(offset, offset + pieceLength)));
    const name = collision ? 'shared.bin' : `${id}.bin`;
    const info = { length: data.length, name, 'piece length': pieceLength, pieces: Buffer.concat(pieces) };
    return { id, data, info, hash: sha1(encode(info)), stops: 0, requests: [], sockets: new Set(),
      uploads: [], held: [], started: 0, stopped: 0, uploadStarted: 0, completed: 0, resumedCompleted: 0, seededRequest: false };
  });
  let protocolError, maxActive = 0, child;
  const active = new Set(), sockets = new Set(), timers = new Set();
  function requestSeededBlocks(torrent, socket) {
    if (torrent.seededRequest || mode === 'seed-collision') return;
    torrent.seededRequest = true;
    const timer = setTimeout(() => {
      timers.delete(timer);
      if (socket.destroyed) return;
      for (let n = 0; n < 2; ++n) {
        const request = Buffer.alloc(12); request.writeUInt32BE(16384, 8);
        socket.write(packet(6, request));
      }
    }, mode === 'ui' ? 1200 : 800);
    timers.add(timer);
  }
  const peer = net.createServer(socket => {
    sockets.add(socket); socket.on('close', () => sockets.delete(socket)); socket.on('error', () => {});
    let pending = Buffer.alloc(0), torrent;
    socket.on('data', chunk => {
      try {
        pending = Buffer.concat([pending, chunk]);
        if (!torrent) {
          if (pending.length < 68) return;
          torrent = torrents.find(t => t.hash.equals(pending.subarray(28, 48)));
          assert.ok(torrent, 'Unexpected info hash');
          torrent.sockets.add(socket); socket.on('close', () => torrent.sockets.delete(socket));
          pending = pending.subarray(68);
          socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8),
            torrent.hash, Buffer.from('-QUEUE0-123456789012')]));
          socket.write(packet(5, Buffer.from([0xf0]))); socket.write(packet(1));
        }
        while (pending.length >= 4) {
          const length = pending.readUInt32BE();
          if (pending.length < length + 4) break;
          const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
          const uploadMode = torrent.id === 0 && ['upload', 'upload-live', 'upload-stop'].includes(mode);
          if (mode === 'seed-resume' && torrent.id === 0 && torrent.stops > 0 && message[0] === 5) {
            assert.equal(message[1] & 0xf0, 0xf0, 'Resumed seed did not advertise verified pieces');
            requestSeededBlocks(torrent, socket);
          }
          if (uploadMode && message[0] === 4 && message.readUInt32BE(1) === 0 && !torrent.uploadStarted) {
            torrent.uploadStarted = Date.now();
            for (let n = 0; n < 4; ++n) {
              const request = Buffer.alloc(12); request.writeUInt32BE(16384, 8);
              socket.write(packet(6, request));
            }
          }
          if ((uploadMode || seedMode) && message[0] === 7) {
            assert.equal(message.readUInt32BE(1), 0); assert.equal(message.readUInt32BE(5), 0);
            assert.deepEqual(message.subarray(9), torrent.data.subarray(0, 16384));
            torrent.uploads.push(Date.now());
            if (torrent.uploads.length === 4) {
              for (const held of torrent.held) if (!held.socket.destroyed) held.socket.write(held.response);
              torrent.held = [];
            }
          }
          if (message[0] !== 6) continue;
          const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
          assert.ok(index < 4 && size > 0 && size <= 16384);
          torrent.requests.push({ index, phase: torrent.stops });
          if (torrent.id === 0 && ['stop', 'remove'].includes(mode) && !torrent.stops && index !== 0) continue;
          const start = index * pieceLength + offset;
          assert.ok(start + size <= torrent.data.length);
          const response = packet(7, Buffer.concat([message.subarray(1, 9), torrent.data.subarray(start, start + size)]));
          if (uploadMode && index > 0 && torrent.uploads.length < 4) {
            torrent.held.push({ socket, response }); continue;
          }
          const timer = setTimeout(() => { timers.delete(timer); if (!socket.destroyed) socket.write(response); }, mode === 'ui' ? 450 : 80);
          timers.add(timer);
        }
      } catch (error) { protocolError = error; socket.destroy(); }
    });
  });
  const tracker = http.createServer((req, res) => {
    try {
      const query = new URL(req.url, 'http://localhost').searchParams;
      // info_hash uses arbitrary bytes, so match the exact percent-encoded bytes.
      const raw = req.url.match(/[?&]info_hash=([^&]+)/)[1];
      const hash = Buffer.from(raw.replace(/%([0-9a-f]{2})/gi, (_, hex) => String.fromCharCode(parseInt(hex, 16))), 'latin1');
      const torrent = torrents.find(t => t.hash.equals(hash)); assert.ok(torrent);
      if (query.get('event') === 'started') { active.add(torrent.id); torrent.started = Date.now(); }
      if (query.get('event') === 'completed') {
        ++torrent.completed;
        if (torrent.stops > 0) ++torrent.resumedCompleted;
        if (seedMode && torrent.id === 0 && mode !== 'seed-resume') {
          assert.equal(Number(query.get('left')), 0);
          for (const socket of torrent.sockets) requestSeededBlocks(torrent, socket);
        }
      }
      if (query.get('event') === 'stopped') {
        active.delete(torrent.id); ++torrent.stops;
        torrent.stopped = Date.now();
        for (const socket of torrent.sockets) socket.end();
      }
      maxActive = Math.max(maxActive, active.size);
      const compact = Buffer.from([127, 0, 0, 1, 0, 0]); compact.writeUInt16BE(peer.address().port, 4);
      res.end(encode({ interval: 60, peers: compact }));
    } catch (error) { protocolError = error; res.statusCode = 500; res.end(); }
  });
  try {
    await listen(peer); await listen(tracker);
    for (const torrent of torrents) fs.writeFileSync(path.join(work, `${torrent.id}.torrent`), encode({
      announce: `http://127.0.0.1:${tracker.address().port}/announce`, info: torrent.info }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_lsd: false,
      enable_pex: false, enable_utp: false, enable_webseeds: false, enable_encryption: false,
      sequential_download: true, download_dir: './files', listen_port: await availablePort(2) }));
    const args = mode === 'ui'
      ? ['--ui-queue-test', path.join(work, '0.torrent'), path.join(work, '1.torrent'), path.join(work, 'files'), path.join(work, 'config.json')]
      : ['0.torrent', '1.torrent', 'config.json', mode];
    child = spawn(executable, args, { cwd: work, windowsHide: true });
    let output = '';
    child.stdout.on('data', chunk => { output += chunk; }); child.stderr.on('data', chunk => { output += chunk; });
    const timer = setTimeout(() => child.kill(), 25000);
    const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); })
      .finally(() => clearTimeout(timer));
    fs.writeFileSync(path.join(work, 'output.log'), output);
    if (protocolError) throw protocolError;
    assert.equal(code, 0, output);
    assert.equal(maxActive, mode === 'parallel' || seedMode && !collision ? 2 : 1, 'Unexpected number of overlapping sessions');
    for (const torrent of torrents) {
      if (collision && torrent.id === 0) continue;
      const saved = fs.readFileSync(path.join(work, 'files', torrent.info.name));
      const partial = ['remove', 'upload-stop'].includes(mode) && torrent.id === 0;
      assert.deepEqual(partial ? saved.subarray(0, pieceLength) : saved, partial ? torrent.data.subarray(0, pieceLength) : torrent.data);
    }
    if (mode === 'stop') assert.ok(!torrents[0].requests.some(r => r.phase > 0 && r.index === 0), 'Resume requested a saved verified piece');
    if (mode === 'limited') {
      assert.ok(torrents[0].stopped - torrents[0].started >= 4000, 'Download limit did not throttle the transfer');
      assert.ok(torrents[1].stopped - torrents[1].started < 3000, 'A per-torrent limit leaked to another job');
    }
    if (mode === 'live') assert.ok(torrents[0].stopped - torrents[0].started < 4000, 'Removing the limit did not release the active transfer');
    if (['upload', 'upload-live'].includes(mode)) {
      assert.equal(torrents[0].uploads.length, 4);
      const elapsed = torrents[0].uploads[3] - torrents[0].uploadStarted;
      if (mode === 'upload') assert.ok(elapsed >= 5500, `Upload limit did not throttle: ${elapsed}ms`);
      else assert.ok(elapsed < 5000, `Upload limit change did not release the active transfer: ${elapsed}ms`);
    }
    if (mode === 'upload-stop') assert.equal(torrents[0].uploads.length, 0, 'Cancelled upload was sent');
    if (seedMode && !collision) {
      assert.equal(torrents[0].uploads.length, 2, 'Verified payload was not seeded');
      assert.equal(torrents[0].completed, 1, 'Incorrect completion announcements');
      if (mode === 'seed-resume') {
        assert.equal(torrents[0].resumedCompleted, 0, 'Resumed seed announced another completion');
        assert.ok(!torrents[0].requests.some(r => r.phase > 0), 'A complete resumed seed downloaded data again');
      }
    }
    assert.equal(active.size, 0);
    console.log(`PASS queue ${mode}: both torrents verified; max active ${maxActive}.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const timer of timers) clearTimeout(timer);
    for (const socket of sockets) socket.destroy(); peer.close(); tracker.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
