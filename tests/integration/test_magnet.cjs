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
// Independent fixture decoder: return one value and the position after it.
function decode(data, position = 0) {
  const first = data[position++];
  if (first === 105) {
    const end = data.indexOf(101, position);
    assert.ok(end >= position);
    return [Number(data.subarray(position, end).toString()), end + 1];
  }
  if (first === 100) {
    const result = {};
    while (data[position] !== 101) {
      let key, value;
      [key, position] = decode(data, position);
      [value, position] = decode(data, position);
      result[key.toString()] = value;
    }
    return [result, position + 1];
  }
  assert.ok(first >= 48 && first <= 57, `Invalid Bencode byte: ${first}`);
  const colon = data.indexOf(58, position - 1);
  const size = Number(data.subarray(position - 1, colon).toString());
  assert.ok(colon >= 0 && Number.isSafeInteger(size) && size >= 0 && colon + size < data.length);
  return [data.subarray(colon + 1, colon + 1 + size), colon + 1 + size];
}
function packet(id, payload = Buffer.alloc(0)) {
  const head = Buffer.alloc(5);
  head.writeUInt32BE(payload.length + 1);
  head[4] = id;
  return Buffer.concat([head, payload]);
}
const hash = data => crypto.createHash('sha1').update(data).digest();
const listen = server => new Promise((resolve, reject) => {
  server.once('error', reject);
  server.listen(0, '127.0.0.1', resolve);
});

async function main() {
  const [scenario, program] = process.argv.slice(2);
  assert.ok(['download', 'reject', 'hash', 'tracker', 'invalid', 'stall'].includes(scenario));
  const work = fs.mkdtempSync(path.join(process.cwd(), `magnet-${scenario}-`));
  const data = Buffer.alloc(50003);
  for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + Math.floor(i / 32768) * 19) % 256;
  const info = { length: data.length, name: 'payload.bin', 'piece length': 32768,
    pieces: Buffer.concat([hash(data.subarray(0, 32768)), hash(data.subarray(32768))]),
    // Force two metadata blocks. Strings include dictionary/integer delimiters.
    source: 'dictionary-end-123:ed:i'.repeat(900) };
  const metadata = encode(info), infoHash = hash(metadata);
  assert.ok(metadata.length > 16384);
  const peers = [], sockets = new Set(), requests = [], trackerHits = [];
  let child, output = '', protocolError;

  function seed(bad) {
    const server = net.createServer(socket => {
      sockets.add(socket);
      socket.on('close', () => sockets.delete(socket));
      socket.on('error', () => {});
      let pending = Buffer.alloc(0), handshaken = false, localId;
      socket.on('data', chunk => {
        try {
          pending = Buffer.concat([pending, chunk]);
          if (!handshaken) {
            if (pending.length < 68) return;
            assert.deepEqual(pending.subarray(28, 48), infoHash);
            pending = pending.subarray(68);
            handshaken = true;
            const reserved = Buffer.alloc(8); reserved[5] = 0x10;
            socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), reserved,
              infoHash, Buffer.from('-MAGNET-123456789012')]));
            socket.write(packet(5, Buffer.from([0xc0])));
            socket.write(packet(1));
          }
          while (pending.length >= 4) {
            const length = pending.readUInt32BE();
            if (pending.length < length + 4) break;
            const message = pending.subarray(4, length + 4);
            pending = pending.subarray(length + 4);
            if (message[0] === 20) {
              const [header, end] = decode(message, 2);
              assert.equal(end, message.length, 'Unexpected bytes or repeated extension headers');
              if (message[1] === 0) {
                localId = header.m?.ut_metadata;
                // Piece-download connections may negotiate other extensions only.
                if (!localId) continue;
                assert.ok(localId > 0 && localId < 256);
                socket.write(packet(20, Buffer.concat([Buffer.from([0]), encode({ m: { ut_metadata: 7 }, metadata_size: metadata.length })])));
                continue;
              }
              assert.equal(message[1], 7, 'Requests must use the remote extension ID');
              assert.equal(header.msg_type, 0);
              const index = header.piece;
              assert.ok(Number.isInteger(index) && index >= 0 && index < Math.ceil(metadata.length / 16384));
              requests.push({ kind: 'metadata', index, bad });
              if (bad && scenario === 'stall') continue;
              if (bad && scenario === 'reject') {
                socket.write(packet(20, Buffer.concat([Buffer.from([localId]), encode({ msg_type: 2, piece: index })])));
                continue;
              }
              const bytes = Buffer.from(metadata.subarray(index * 16384, (index + 1) * 16384));
              // Change a byte inside the long source string, keeping Bencode valid.
              if (bad && index === 0 && (scenario === 'hash' || scenario === 'invalid')) bytes[bytes.length - 1] ^= 1;
              socket.write(packet(20, Buffer.concat([Buffer.from([localId]),
                encode({ msg_type: 1, piece: index, total_size: metadata.length, x: 'ed:123i' }), bytes])));
            } else if (message[0] === 6) {
              const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
              const start = index * 32768 + offset;
              assert.ok(index < 2 && size > 0 && size <= 16384 && start + size <= Math.min((index + 1) * 32768, data.length));
              requests.push({ kind: 'data', index, bad });
              socket.write(packet(7, Buffer.concat([message.subarray(1, 9), data.subarray(start, start + size)])));
            }
          }
        } catch (error) { protocolError = error; socket.destroy(); }
      });
    });
    peers.push(server);
    return server;
  }

  const tracker = http.createServer((req, res) => {
    trackerHits.push(req.url);
    if (req.url.startsWith('/unavailable')) {
      res.writeHead(503); res.end('unavailable'); return;
    }
    const compact = peers.map(peer => {
      const entry = Buffer.from([127, 0, 0, 1, 0, 0]);
      entry.writeUInt16BE(peer.address().port, 4);
      return entry;
    });
    res.end(encode({ interval: 60, peers: Buffer.concat(compact) }));
  });
  try {
    if (['reject', 'hash', 'invalid', 'stall'].includes(scenario)) await listen(seed(true));
    if (scenario !== 'invalid') await listen(seed(false));
    await listen(tracker);
    const base = `http://127.0.0.1:${tracker.address().port}`;
    const trackers = scenario === 'tracker' ? [base + '/unavailable', base + '/announce'] : [base + '/announce'];
    const uri = `magnet:?xt=urn:btih:${infoHash.toString('hex')}&dn=fixture` + trackers.map(url => `&tr=${encodeURIComponent(url)}`).join('');
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_pex: false,
      enable_lsd: false, enable_webseeds: false, enable_utp: false, enable_encryption: false,
      download_dir: './files', log_file: './client.log', listen_port: 16881 }));
    child = spawn(path.resolve(program), [uri, '--config', 'config.json'], { cwd: work });
    child.stdout.on('data', chunk => { output += chunk; });
    child.stderr.on('data', chunk => { output += chunk; });
    const timer = setTimeout(() => child.kill(), 20000);
    const code = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('close', resolve);
    }).finally(() => clearTimeout(timer));
    if (protocolError) throw protocolError;
    if (scenario === 'invalid') {
      assert.equal(code, 1, output);
      assert.ok(!fs.existsSync(path.join(work, 'files')), 'Unverified metadata must not start file downloading');
      assert.ok(!requests.some(r => r.kind === 'data'));
      assert.ok(output.includes('Info hash mismatch'), output);
    } else {
      assert.equal(code, 0, output);
      assert.deepEqual(fs.readFileSync(path.join(work, 'files', 'payload.bin')), data);
      assert.ok(output.includes('Download complete!'));
      assert.equal(requests.filter(r => r.kind === 'metadata' && !r.bad).length, Math.ceil(metadata.length / 16384));
      if (['reject', 'hash', 'stall'].includes(scenario)) assert.ok(requests.some(r => r.kind === 'metadata' && r.bad));
      if (scenario === 'hash') assert.ok(output.includes('Info hash mismatch'), output);
      if (scenario === 'tracker') assert.ok(trackerHits.some(url => url.startsWith('/unavailable')));
    }
    console.log(`PASS magnet ${scenario}: ${metadata.length} metadata bytes; ${requests.filter(r => r.kind === 'data').length} file block requests.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const socket of sockets) socket.destroy();
    for (const peer of peers) peer.close();
    tracker.close();
    fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
