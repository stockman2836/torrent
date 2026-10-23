const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');

const hash = data => crypto.createHash('sha1').update(data).digest();
function encode(value) {
  if (Buffer.isBuffer(value)) return Buffer.concat([Buffer.from(`${value.length}:`), value]);
  if (typeof value === 'string') return encode(Buffer.from(value));
  if (typeof value === 'number') return Buffer.from(`i${value}e`);
  if (Array.isArray(value)) return Buffer.concat([Buffer.from('l'), ...value.map(encode), Buffer.from('e')]);
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(k => [encode(k), encode(value[k])]), Buffer.from('e')]);
}
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5);
  header.writeUInt32BE(payload.length + 1);
  header[4] = id;
  return Buffer.concat([header, payload]);
}
function listen(server) {
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
}

async function main() {
  const [scenario, executable, partialExecutable] = process.argv.slice(2);
  assert.ok(['multi', 'resume', 'corrupt', 'disconnect', 'multifile'].includes(scenario));
  const work = fs.mkdtempSync(path.join(process.cwd(), `transfer-${scenario}-`));
  const pieceLength = 32768;
  const data = Buffer.alloc(pieceLength * 7 + 123);
  for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + Math.floor(i / pieceLength) * 19) % 256;
  const pieceCount = Math.ceil(data.length / pieceLength);
  const pieces = Array.from({ length: pieceCount }, (_, i) => hash(data.subarray(i * pieceLength, (i + 1) * pieceLength)));
  const info = { name: 'payload', 'piece length': pieceLength, pieces: Buffer.concat(pieces) };
  if (scenario === 'multifile') {
    info.files = [{ length: 10007, path: ['first.bin'] }, { length: 40009, path: ['nested', 'second.bin'] },
      { length: data.length - 50016, path: ['last.bin'] }];
  } else { info.name = 'payload.bin'; info.length = data.length; }
  const infoHash = hash(encode(info));
  const sockets = new Set();
  const peers = [];
  const requests = [];
  const announces = [];
  let stage = scenario === 'resume' || scenario === 'corrupt' ? 'partial' : 'full';
  let protocolError;
  let disconnected = false;
  let child;

  function seed(id) {
    const server = net.createServer(socket => {
      sockets.add(socket);
      socket.on('close', () => sockets.delete(socket));
      socket.on('error', () => {});
      let pending = Buffer.alloc(0), handshaken = false;
      const advertised = Array.from({ length: pieceCount }, (_, i) => stage === 'partial' ? i === 0 : true);
      socket.on('data', chunk => {
        try {
          pending = Buffer.concat([pending, chunk]);
          if (!handshaken) {
            if (pending.length < 68) return;
            assert.deepEqual(pending.subarray(28, 48), infoHash);
            pending = pending.subarray(68);
            handshaken = true;
            socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'),
              Buffer.alloc(8), infoHash, Buffer.from(`-TEST0${id}-123456789012`)]));
            const bits = Buffer.alloc(Math.ceil(pieceCount / 8));
            advertised.forEach((has, i) => { if (has) bits[Math.floor(i / 8)] |= 128 >> (i % 8); });
            socket.write(packet(5, bits));
            socket.write(packet(1));
          }
          while (pending.length >= 4) {
            const length = pending.readUInt32BE();
            if (pending.length < length + 4) break;
            const message = pending.subarray(4, 4 + length);
            pending = pending.subarray(4 + length);
            if (message[0] !== 6) continue;
            assert.equal(message.length, 13);
            const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
            assert.ok(index < pieceCount && advertised[index], `Requested unavailable piece ${index}`);
            const start = index * pieceLength + offset;
            assert.ok(size > 0 && size <= 16384 && start + size <= Math.min((index + 1) * pieceLength, data.length));
            requests.push({ id, index, offset, stage });
            if (scenario === 'disconnect' && id === 1 && !disconnected) {
              disconnected = true;
              socket.end(packet(7, Buffer.concat([message.subarray(1, 9), data.subarray(start, start + size)])));
              break;
            }
            const reply = packet(7, Buffer.concat([message.subarray(1, 9), data.subarray(start, start + size)]));
            setTimeout(() => { if (!socket.destroyed) socket.write(reply); }, 5);
          }
        } catch (error) { protocolError = error; socket.destroy(); }
      });
      // An idle peer is allowed to stay connected after the client finishes.
      // Do not close it in response to tracker announces: test client shutdown.
    });
    peers.push(server);
    return server;
  }

  const tracker = http.createServer((req, res) => {
    const query = new URL(req.url, 'http://localhost').searchParams;
    announces.push({ stage, event: query.get('event'), left: Number(query.get('left')),
      downloaded: Number(query.get('downloaded')) });
    const compact = peers.map(peer => {
      const entry = Buffer.from([127, 0, 0, 1, 0, 0]);
      entry.writeUInt16BE(peer.address().port, 4);
      return entry;
    });
    res.end(encode({ interval: 60, peers: Buffer.concat(compact) }));
  });

  async function run(program, label) {
    const start = Date.now();
    let output = '';
    const args = program === executable ? ['test.torrent', '--config', 'config.json'] : ['test.torrent', 'config.json'];
    child = spawn(path.resolve(program), args, { cwd: work });
    child.stdout.on('data', chunk => { output += chunk; });
    child.stderr.on('data', chunk => { output += chunk; });
    const timer = setTimeout(() => child.kill(), 15000);
    const code = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('exit', resolve);
    }).finally(() => clearTimeout(timer));
    fs.writeFileSync(path.join(work, `${label}.log`), output);
    if (protocolError) throw protocolError;
    assert.equal(code, 0, `Client failed/timed out (${Date.now() - start}ms):\n${output}`);
    return output;
  }

  try {
    await listen(seed(0));
    if (scenario === 'multi' || scenario === 'disconnect') await listen(seed(1));
    await listen(tracker);
    fs.writeFileSync(path.join(work, 'test.torrent'), encode({ announce: `http://127.0.0.1:${tracker.address().port}/announce`, info }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_pex: false,
      enable_lsd: false, enable_webseeds: false, enable_utp: false, enable_encryption: false,
      sequential_download: scenario !== 'multi', download_dir: './files', log_file: './client.log', listen_port: 16881 }));
    if (stage === 'partial') {
      await run(partialExecutable, 'partial');
      assert.deepEqual(fs.readFileSync(path.join(work, 'files', 'payload.bin')).subarray(0, pieceLength), data.subarray(0, pieceLength));
      if (scenario === 'corrupt') {
        const file = fs.openSync(path.join(work, 'files', 'payload.bin'), 'r+');
        fs.writeSync(file, Buffer.from([data[0] ^ 0xff]), 0, 1, 0);
        fs.closeSync(file);
      }
      stage = 'full';
    }
    const output = await run(executable, 'full');
    assert.ok(output.includes('Download complete!'), output);
    if (scenario === 'multifile') {
      const saved = Buffer.concat(info.files.map(file => fs.readFileSync(path.join(work, 'files', info.name, ...file.path))));
      assert.deepEqual(saved, data);
    } else assert.deepEqual(fs.readFileSync(path.join(work, 'files', 'payload.bin')), data);

    const full = requests.filter(r => r.stage === 'full');
    for (const announce of announces) assert.ok(announce.left >= 0 && announce.left <= data.length,
      `Invalid remaining byte count: ${announce.left}`);
    assert.equal(announces.find(a => a.stage === 'full' && a.event === 'stopped').left, 0);
    if (scenario === 'multi') {
      assert.ok(full.some(r => r.id === 0) && full.some(r => r.id === 1), 'Both peers must contribute');
      const keys = full.map(r => `${r.index}:${r.offset}`);
      assert.equal(new Set(keys).size, keys.length, 'Duplicate block requests across peers');
      assert.equal(announces.find(a => a.event === 'stopped').downloaded, data.length);
    }
    if (scenario === 'resume') {
      assert.ok(output.includes('Resume data loaded successfully'));
      assert.ok(!full.some(r => r.index === 0), 'Verified saved piece was requested again');
      assert.equal(announces.find(a => a.stage === 'full' && a.event === 'started').left, data.length - pieceLength);
    }
    if (scenario === 'corrupt') {
      assert.ok(full.some(r => r.index === 0), 'Corrupted saved piece must be downloaded again');
      assert.ok(output.includes('failed verification'));
    }
    if (scenario === 'disconnect') assert.ok(disconnected && full.some(r => r.id === 0));
    console.log(`PASS ${scenario}: ${data.length} bytes verified; ${full.length} block requests.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const socket of sockets) socket.destroy();
    for (const peer of peers) peer.close();
    tracker.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
