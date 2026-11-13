const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { availablePort } = require('./fixture_helpers.cjs');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest();
function encode(value) {
  if (Buffer.isBuffer(value)) return Buffer.concat([Buffer.from(`${value.length}:`), value]);
  if (typeof value === 'string') return encode(Buffer.from(value));
  if (typeof value === 'number') return Buffer.from(`i${value}e`);
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(key => [encode(key), encode(value[key])]), Buffer.from('e')]);
}
function decode(data, position = 0) {
  const first = data[position++];
  if (first === 105) {
    const end = data.indexOf(101, position); assert.ok(end >= position);
    return [Number(data.subarray(position, end).toString()), end + 1];
  }
  if (first === 100) {
    const result = {};
    while (data[position] !== 101) {
      let key, value; [key, position] = decode(data, position); [value, position] = decode(data, position);
      result[key.toString()] = value;
    }
    return [result, position + 1];
  }
  assert.ok(first >= 48 && first <= 57);
  const colon = data.indexOf(58, position - 1), size = Number(data.subarray(position - 1, colon).toString());
  assert.ok(colon >= 0 && size >= 0 && colon + size < data.length);
  return [data.subarray(colon + 1, colon + 1 + size), colon + 1 + size];
}
function packet(id, body = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(body.length + 1); header[4] = id;
  return Buffer.concat([header, body]);
}
function extended(id, value, suffix = Buffer.alloc(0)) {
  return packet(20, Buffer.concat([Buffer.from([id]), encode(value), suffix]));
}
const listen = server => new Promise((resolve, reject) => {
  server.once('error', reject); server.listen(0, '127.0.0.1', resolve);
});

async function main() {
  const executable = path.resolve(process.argv[2]), scenario = process.argv[3];
  assert.ok(['incoming', 'outgoing', 'invalid', 'budget', 'remap', 'disabled', 'magnet', 'limited-stop'].includes(scenario));
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `metadata-serving-${scenario}-`));
  const data = Buffer.alloc(50003);
  for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + (i >>> 8) * 11) % 256;
  const pieceLength = 16384, hashes = [];
  for (let i = 0; i < data.length; i += pieceLength) hashes.push(sha1(data.subarray(i, i + pieceLength)));
  // Preserve deliberately unsorted keys and an unknown binary field. Re-encoding
  // this dictionary changes its info hash and is not a valid metadata response.
  const fields = { source: Buffer.concat([Buffer.alloc(20000, 255), Buffer.from('ed:i123e\0')]), pieces: Buffer.concat(hashes),
    'piece length': pieceLength, name: 'payload.bin', length: data.length };
  const metadata = Buffer.concat([Buffer.from('d'), ...Object.entries(fields).flatMap(([key, value]) => [encode(key), encode(value)]), Buffer.from('e')]);
  const hash = sha1(metadata), count = Math.ceil(metadata.length / 16384), port = await availablePort();
  let child, output = '', code, failure, server;
  const clients = [], replies = [], upstreamRequests = [];
  async function until(check, description, timeout = 8000) {
    const end = Date.now() + timeout;
    do { if (failure) throw failure; if (check()) return; await delay(10); } while (Date.now() < end);
    throw new Error(`Timed out waiting for ${description}\n${output}`);
  }
  class Client {
    constructor(socket) {
      this.socket = socket; this.pending = Buffer.alloc(0); this.messages = []; this.closed = false;
      clients.push(this); socket.on('error', () => {}); socket.on('close', () => { this.closed = true; });
      socket.on('data', bytes => {
        try {
          this.pending = Buffer.concat([this.pending, bytes]);
          if (!this.handshake && this.pending.length >= 68) {
            this.handshake = this.pending.subarray(0, 68); this.pending = this.pending.subarray(68);
          }
          if (!this.handshake) return;
          while (this.pending.length >= 4) {
            const length = this.pending.readUInt32BE(); if (this.pending.length < length + 4) break;
            const message = { id: length ? this.pending[4] : -1, body: this.pending.subarray(5, length + 4) };
            this.pending = this.pending.subarray(length + 4); this.messages.push(message);
            if (this.onMessage) this.onMessage(message);
          }
        } catch (error) { failure = error; socket.destroy(); }
      });
    }
    async greet(extensions = true) {
      const reserved = Buffer.alloc(8); reserved[5] = extensions ? 0x10 : 0;
      this.socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), reserved, hash, Buffer.from('-MDSRV-1234567890123')]));
      await until(() => this.handshake || this.closed, 'handshake');
      assert.ok(this.handshake); assert.deepEqual(this.handshake.subarray(28, 48), hash);
      await this.next(message => message.id === 1);
      if (extensions) {
        const hello = await this.next(message => message.id === 20 && message.body[0] === 0);
        const [header, end] = decode(hello.body, 1); assert.equal(end, hello.body.length);
        this.localId = header.m.ut_metadata; assert.ok(this.localId > 0);
        assert.equal(header.metadata_size, metadata.length);
        assert.equal(header.m.ut_pex, undefined, 'Metadata serving must work with PEX disabled');
        this.mapping(37);
      }
    }
    mapping(id) { this.remoteId = id; this.socket.write(extended(0, { m: { ut_metadata: id } })); }
    async next(predicate) {
      await until(() => this.messages.some(predicate) || this.closed, 'peer response');
      const index = this.messages.findIndex(predicate); assert.ok(index >= 0, 'Peer closed before response');
      return this.messages.splice(index, 1)[0];
    }
    async request(index, expected = 1) {
      this.socket.write(extended(this.localId, { msg_type: 0, piece: index }));
      const message = await this.next(message => message.id === 20 && message.body[0] !== 0);
      assert.equal(message.body[0], this.remoteId, 'Replies must use the requesting peer\'s extension ID');
      const [header, end] = decode(message.body, 1);
      assert.equal(header.msg_type, expected); assert.equal(header.piece, index);
      const bytes = message.body.subarray(end); replies.push({ type: expected, piece: index, bytes: bytes.length });
      if (expected === 1) {
        assert.equal(header.total_size, metadata.length);
        assert.deepEqual(bytes, metadata.subarray(index * 16384, (index + 1) * 16384));
      } else { assert.equal(header.total_size, undefined); assert.equal(bytes.length, 0); }
      return bytes;
    }
    async payloadBlock() {
      const body = Buffer.alloc(12); body.writeUInt32BE(1024, 8); this.socket.write(packet(6, body));
      const response = await this.next(message => message.id === 7);
      assert.deepEqual(response.body.subarray(8), data.subarray(0, 1024));
    }
  }
  let requester;
  if (['outgoing', 'magnet'].includes(scenario)) {
    server = net.createServer(socket => {
      const client = new Client(socket);
      if (scenario === 'outgoing') { requester = client; return; }
      const reserved = Buffer.alloc(8); reserved[5] = 0x10;
      socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), reserved, hash, Buffer.from('-UPSTM-1234567890123'),
        packet(5, Buffer.from([0xf0])), packet(1)]));
      let localId;
      client.onMessage = message => {
        if (message.id === 20) {
          const [header] = decode(message.body, 1);
          if (message.body[0] === 0) {
            localId = header.m.ut_metadata;
            socket.write(extended(0, { m: { ut_metadata: 77 }, metadata_size: metadata.length }));
          } else if (header.msg_type === 0) {
            assert.equal(message.body[0], 77); upstreamRequests.push(header.piece);
            socket.write(extended(localId, { msg_type: 1, piece: header.piece, total_size: metadata.length },
              metadata.subarray(header.piece * 16384, (header.piece + 1) * 16384)));
          }
        } else if (message.id === 6) {
          const index = message.body.readUInt32BE(), offset = message.body.readUInt32BE(4), length = message.body.readUInt32BE(8);
          const start = index * pieceLength + offset;
          assert.ok(length > 0 && length <= 16384 && start + length <= data.length);
          socket.write(packet(7, Buffer.concat([message.body.subarray(0, 8), data.subarray(start, start + length)])));
        }
      };
    });
    await listen(server);
  }
  const tracker = http.createServer((req, res) => {
    const peers = server ? Buffer.from([127, 0, 0, 1, server.address().port >> 8, server.address().port & 255]) : Buffer.alloc(0);
    res.end(encode({ interval: 60, peers }));
  });
  await listen(tracker);
  const announce = `http://127.0.0.1:${tracker.address().port}/announce`;
  fs.writeFileSync(path.join(work, 'fixture.torrent'), Buffer.concat([Buffer.from('d8:announce'), encode(announce), Buffer.from('4:info'), metadata, Buffer.from('e')]));
  fs.mkdirSync(path.join(work, 'files'));
  fs.writeFileSync(path.join(work, 'files', 'payload.bin'), scenario === 'incoming' ? data.subarray(0, pieceLength) : data);
  fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_pex: false, enable_lsd: false,
    enable_port_mapping: false, enable_webseeds: false, enable_utp: false, enable_encryption: false,
    download_dir: './files', listen_port: port, max_upload_speed: scenario === 'limited-stop' ? 128 : 0 }));
  try {
    const source = scenario === 'magnet' ? `magnet:?xt=urn:btih:${hash.toString('hex')}&tr=${encodeURIComponent(announce)}` : 'fixture.torrent';
    child = spawn(executable, [source, 'config.json'], { cwd: work, windowsHide: true });
    child.on('error', error => { failure = error; }); child.on('close', value => { code = value; });
    child.stdin.on('error', () => {});
    for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => { output += bytes; });
    await until(() => output.includes('INCOMING_READY') || code !== undefined, 'running session');
    assert.equal(code, undefined, output);
    if (scenario === 'magnet') {
      child.stdin.write('complete\n'); await until(() => output.includes('INCOMING_COMPLETE'), 'magnet payload completion');
      assert.ok(upstreamRequests.length >= count); assert.deepEqual(fs.readFileSync(path.join(work, 'files', 'payload.bin')), data);
    }
    if (scenario === 'outgoing') await until(() => requester, 'outgoing connection');
    else requester = new Client(net.createConnection({ host: '127.0.0.1', port }));
    await requester.greet(scenario !== 'disabled');
    if (scenario === 'incoming') {
      const availability = await requester.next(message => message.id === 5);
      assert.deepEqual(availability.body, Buffer.from([0x80]), 'Metadata must be served while the payload is incomplete');
    }
    if (scenario === 'disabled') {
      await delay(200); assert.ok(!requester.messages.some(message => message.id === 20)); await requester.payloadBlock();
    } else if (scenario === 'limited-stop') {
      requester.socket.write(extended(requester.localId, { msg_type: 0, piece: 0 }));
      await delay(150);
      assert.ok(!requester.messages.some(message => message.id === 20 && message.body[0] !== 0));
    } else if (scenario === 'budget') {
      for (let i = 0; i < count * 4 + 2; ++i) await requester.request(i % count, i < count * 4 ? 1 : 2);
      requester.mapping(83); await requester.request(0, 2); // Renegotiation cannot reset the budget.
      const other = new Client(net.createConnection({ host: '127.0.0.1', port })); await other.greet(); await other.request(0);
      await requester.payloadBlock();
    } else if (scenario === 'remap') {
      await requester.request(0); requester.mapping(83); await requester.request(1);
      requester.mapping(0); requester.socket.write(extended(requester.localId, { msg_type: 0, piece: 0 }));
      await delay(150); assert.ok(!requester.messages.some(message => message.id === 20 && message.body[0] !== 0));
      requester.mapping(91); await requester.request(0);
    } else {
      if (scenario === 'invalid') {
        await requester.request(-1, 2); await requester.request(count, 2);
        for (const bytes of [Buffer.from('de'), Buffer.from('d8:msg_typei0e5:piece1:xe'), Buffer.alloc(512, 108)])
          requester.socket.write(packet(20, Buffer.concat([Buffer.from([requester.localId]), bytes])));
        requester.socket.write(extended(requester.localId, { msg_type: 99, piece: 0 }));
        requester.socket.write(extended(requester.localId + 50, { msg_type: 0, piece: 0 }));
        await delay(150); assert.ok(!requester.messages.some(message => message.id === 20 && message.body[0] !== 0));
      }
      const blocks = [];
      for (let index = count - 1; index >= 0; --index) blocks[index] = await requester.request(index);
      const actual = Buffer.concat(blocks); assert.deepEqual(actual, metadata); assert.deepEqual(sha1(actual), hash);
      await requester.payloadBlock();
    }
    const stopped = Date.now(); child.stdin.end('stop\n');
    await until(() => code !== undefined, 'session shutdown'); assert.equal(code, 0, output);
    if (scenario === 'limited-stop') assert.ok(Date.now() - stopped < 2000, 'Cancellation must interrupt metadata upload throttling');
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ scenario, infoHash: hash.toString('hex'), metadataBytes: metadata.length,
      replies, upstreamRequests, passed: true }, null, 2));
    console.log(`PASS metadata serving ${scenario}: original info bytes and protocol behavior verified.`);
  } finally {
    for (const client of clients) client.socket.destroy();
    if (child && code === undefined) {
      child.stdin.end('stop\n'); try { await until(() => code !== undefined, 'cleanup', 2000); } catch { child.kill(); }
    }
    tracker.closeAllConnections(); await new Promise(resolve => tracker.close(resolve));
    if (server) await new Promise(resolve => server.close(resolve));
    fs.writeFileSync(path.join(work, 'engine-output.log'), output); console.log(`Artifacts: ${work}`);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
