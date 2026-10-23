const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');

function encode(value) {
  if (Buffer.isBuffer(value)) return Buffer.concat([Buffer.from(`${value.length}:`), value]);
  if (typeof value === 'string') return encode(Buffer.from(value));
  if (typeof value === 'number') return Buffer.from(`i${value}e`);
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(k => [encode(k), encode(value[k])]), Buffer.from('e')]);
}
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}
const hash = data => crypto.createHash('sha1').update(data).digest();
const listen = server => new Promise((resolve, reject) => {
  server.once('error', reject); server.listen(0, '127.0.0.1', resolve);
});

async function main() {
  const [scenario, program] = process.argv.slice(2);
  assert.ok(['cancel', 'late', 'cap', 'disconnect'].includes(scenario));
  const work = fs.mkdtempSync(path.join(process.cwd(), `endgame-${scenario}-`));
  const pieceLength = 32768, data = Buffer.alloc(7 * pieceLength + 123);
  for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + Math.floor(i / pieceLength) * 19) % 256;
  const pieces = Array.from({ length: 8 }, (_, i) => hash(data.subarray(i * pieceLength, (i + 1) * pieceLength)));
  const info = { name: 'payload.bin', length: data.length, 'piece length': pieceLength, pieces: Buffer.concat(pieces) };
  const infoHash = hash(encode(info)), sockets = new Set(), peers = [], requests = [], cancels = [], timers = new Set();
  const held = new Map();
  let slowSocket, child, protocolError, output = '', lateSent = false, stopped;
  const later = (fn, ms) => { const timer = setTimeout(() => { timers.delete(timer); fn(); }, ms); timers.add(timer); };

  function seed(id) {
    const server = net.createServer(socket => {
      sockets.add(socket); socket.on('close', () => sockets.delete(socket)); socket.on('error', () => {});
      if (id === 0) slowSocket = socket;
      let pending = Buffer.alloc(0), handshaken = false;
      socket.on('data', chunk => {
        try {
          pending = Buffer.concat([pending, chunk]);
          if (!handshaken) {
            if (pending.length < 68) return;
            assert.deepEqual(pending.subarray(28, 48), infoHash);
            pending = pending.subarray(68); handshaken = true;
            socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8),
              infoHash, Buffer.from(`-END00${id}-123456789012`)]));
            socket.write(packet(5, Buffer.from([id === 0 ? 0x80 : 0xff])));
            // Give the slow peer deterministic ownership of piece zero.
            later(() => { if (!socket.destroyed) socket.write(packet(1)); }, id === 0 ? 0 : 100);
          }
          while (pending.length >= 4) {
            const length = pending.readUInt32BE();
            if (pending.length < length + 4) break;
            const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
            if (message[0] !== 6 && message[0] !== 8) continue;
            assert.equal(message.length, 13);
            const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
            const key = `${index}:${offset}`, start = index * pieceLength + offset;
            assert.ok(index < 8 && size > 0 && size <= 16384 && start + size <= Math.min((index + 1) * pieceLength, data.length));
            if (message[0] === 8) {
              assert.ok(requests.some(r => r.id === id && r.key === key && r.size === size), 'CANCEL must match a request');
              cancels.push({ id, key, size });
              if (scenario === 'late' && id === 0 && offset === 0 && !lateSent) {
                // A response can already be in flight when CANCEL arrives.
                // Corrupt it so accepting it into the unfinished piece is observable.
                const bytes = Buffer.from(held.get(key)); bytes[bytes.length - 1] ^= 1;
                later(() => { if (!socket.destroyed) { socket.write(bytes); lateSent = true; } }, 10);
              }
              continue;
            }
            requests.push({ id, index, offset, size, key });
            const reply = packet(7, Buffer.concat([message.subarray(1, 9), data.subarray(start, start + size)]));
            if (id === 0 && (offset !== 0 || scenario === 'late' || scenario === 'cap')) {
              held.set(key, reply); continue; // This seed will never deliver these requests normally.
            }
            if (id !== 0 && index === 0 && scenario === 'disconnect') slowSocket.destroy();
            const delay = id !== 0 && index === 0 ? (scenario === 'late' && offset !== 0 ? 250 : 80) : 5;
            later(() => { if (!socket.destroyed) socket.write(reply); }, delay);
          }
        } catch (error) { protocolError = error; socket.destroy(); }
      });
    });
    peers.push(server); return server;
  }
  const tracker = http.createServer((req, res) => {
    const query = new URL(req.url, 'http://localhost').searchParams;
    if (query.get('event') === 'stopped') stopped = { left: Number(query.get('left')), downloaded: Number(query.get('downloaded')) };
    const entries = peers.map(peer => { const entry = Buffer.from([127, 0, 0, 1, 0, 0]); entry.writeUInt16BE(peer.address().port, 4); return entry; });
    res.end(encode({ interval: 60, peers: Buffer.concat(entries) }));
  });
  try {
    // The client consumes compact peers from the end, so list the slow seed last.
    await listen(seed(1)); if (scenario === 'cap') await listen(seed(2)); await listen(seed(0)); await listen(tracker);
    fs.writeFileSync(path.join(work, 'test.torrent'), encode({ announce: `http://127.0.0.1:${tracker.address().port}/announce`, info }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_pex: false,
      enable_lsd: false, enable_webseeds: false, enable_utp: false, enable_encryption: false,
      sequential_download: true, download_dir: './files', log_file: './client.log', listen_port: 16881 }));
    child = spawn(path.resolve(program), ['test.torrent', '--config', 'config.json'], { cwd: work });
    child.stdout.on('data', chunk => { output += chunk; }); child.stderr.on('data', chunk => { output += chunk; });
    const timer = setTimeout(() => child.kill(), 8000);
    const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); }).finally(() => clearTimeout(timer));
    if (protocolError) throw protocolError;
    assert.equal(code, 0, `Endgame failed or waited for the unresponsive seed:\n${output}`);
    assert.deepEqual(fs.readFileSync(path.join(work, 'files', 'payload.bin')), data);
    assert.deepEqual(stopped, { left: 0, downloaded: data.length }, 'Duplicate bytes must not inflate progress/statistics');
    const grouped = new Map();
    for (const request of requests) { if (!grouped.has(request.key)) grouped.set(request.key, []); grouped.get(request.key).push(request); }
    for (const group of grouped.values()) {
      if (group[0].index !== 0) assert.equal(group.length, 1, 'Ordinary requests must stay unique');
      assert.ok(group.length <= 2, 'At most two peers may hold copies of one block');
      assert.equal(new Set(group.map(r => r.id)).size, group.length, 'Do not request the same block twice from one peer');
    }
    assert.ok(requests.some(r => r.id !== 0 && r.index === 0), 'A fast peer must rescue the straggling piece');
    if (scenario !== 'disconnect') assert.ok(cancels.some(r => r.id === 0 && r.key === '0:16384'), 'Cancel the slow outstanding block');
    if (scenario === 'late') {
      assert.ok(lateSent, 'Fixture must deliver a late cancelled block');
      assert.ok(!output.includes('hash verification FAILED'), 'Late cancelled data must not contaminate the assembly');
    }
    if (scenario === 'disconnect') assert.equal(grouped.get('0:0').length, 1, 'Keep valid partial data while another peer owns the remaining block');
    console.log(`PASS endgame ${scenario}: ${requests.length} requests; ${cancels.length} cancels; ${data.length} verified bytes.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const timer of timers) clearTimeout(timer);
    for (const socket of sockets) socket.destroy();
    for (const peer of peers) peer.close(); tracker.close();
    fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
