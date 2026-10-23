// Offline end-to-end check: local HTTP tracker -> TCP peer -> verified files.
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
  if (Array.isArray(value)) return Buffer.concat([Buffer.from('l'), ...value.map(encode), Buffer.from('e')]);
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(k => [encode(k), encode(value[k])]), Buffer.from('e')]);
}
const sha1 = data => crypto.createHash('sha1').update(data).digest();
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5);
  header.writeUInt32BE(payload.length + 1);
  header[4] = id;
  return Buffer.concat([header, payload]);
}

async function main() {
  const executable = path.resolve(process.argv[2]);
  const work = fs.mkdtempSync(path.join(process.cwd(), 'download-test-'));
  const data = Buffer.alloc(50003);
  for (let i = 0; i < data.length; i++) data[i] = (i * 37 + 11) % 256;
  const pieceLength = 32768;
  const info = { length: data.length, name: 'payload.bin', 'piece length': pieceLength,
    pieces: Buffer.concat([sha1(data.subarray(0, pieceLength)), sha1(data.subarray(pieceLength))]) };
  const infoHash = sha1(encode(info));
  const sockets = new Set();
  let requests = 0;
  let protocolError;
  const peer = net.createServer(socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    socket.on('error', () => {});
    let pending = Buffer.alloc(0);
    let handshaken = false;
    socket.on('data', chunk => {
      try {
        pending = Buffer.concat([pending, chunk]);
        if (!handshaken) {
          if (pending.length < 68) return;
          assert.equal(pending[0], 19);
          assert.deepEqual(pending.subarray(28, 48), infoHash);
          pending = pending.subarray(68);
          handshaken = true;
          socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'),
            Buffer.alloc(8), infoHash, Buffer.from('-TEST00-123456789012')]));
          socket.write(packet(5, Buffer.from([0xc0])));
          socket.write(packet(1));
        }
        while (pending.length >= 4) {
          const length = pending.readUInt32BE();
          if (pending.length < length + 4) break;
          const message = pending.subarray(4, 4 + length);
          pending = pending.subarray(4 + length);
          if (message[0] !== 6) continue;
          assert.equal(message.length, 13);
          const index = message.readUInt32BE(1);
          const offset = message.readUInt32BE(5);
          const size = message.readUInt32BE(9);
          assert.ok(index < 2 && size > 0 && size <= 16384);
          const start = index * pieceLength + offset;
          assert.ok(start + size <= Math.min((index + 1) * pieceLength, data.length));
          requests++;
          socket.write(packet(7, Buffer.concat([message.subarray(1, 9), data.subarray(start, start + size)])));
        }
      } catch (error) { protocolError = error; socket.destroy(); }
    });
  });
  const tracker = http.createServer((req, res) => {
    if (new URL(req.url, 'http://localhost').searchParams.get('event') === 'stopped') {
      for (const socket of sockets) socket.end();
    }
    const compact = Buffer.from([127, 0, 0, 1, 0, 0]);
    compact.writeUInt16BE(peer.address().port, 4);
    res.end(encode({ interval: 60, peers: compact }));
  });
  let child;
  let output = '';
  try {
    await listen(peer);
    await listen(tracker);
    fs.writeFileSync(path.join(work, 'test.torrent'), encode({
      announce: `http://127.0.0.1:${tracker.address().port}/announce`, info }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({
      enable_dht: false, enable_pex: false, enable_lsd: false,
      enable_webseeds: false, enable_utp: false, enable_encryption: false,
      download_dir: './files', log_file: './client.log', listen_port: 16881 }));
    child = spawn(executable, ['test.torrent', '--config', 'config.json'], { cwd: work });
    child.stdout.on('data', chunk => { output += chunk; });
    child.stderr.on('data', chunk => { output += chunk; });
    const timer = setTimeout(() => child.kill(), 45000);
    const code = await new Promise((resolve, reject) => {
      child.on('error', reject);
      child.on('exit', resolve);
    }).finally(() => clearTimeout(timer));
    if (protocolError) throw protocolError;
    assert.equal(code, 0, `Client failed or timed out:\n${output}`);
    assert.ok(requests >= 4, 'Expected requests spanning multiple pieces and blocks');
    assert.deepEqual(fs.readFileSync(path.join(work, 'files', 'payload.bin')), data);
    assert.ok(output.includes('Download complete!'), output);
    console.log(`PASS: ${data.length} bytes downloaded through local tracker and peer; exact contents verified (${requests} block requests).`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const socket of sockets) socket.destroy();
    peer.close();
    tracker.close();
    fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
