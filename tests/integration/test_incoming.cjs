// A separate TCP client initiates every tested connection to the torrent engine.
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
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}
function request(index, offset = 0, length = 16384) {
  const payload = Buffer.alloc(12);
  payload.writeUInt32BE(index); payload.writeUInt32BE(offset, 4); payload.writeUInt32BE(length, 8);
  return packet(6, payload);
}
function handshake(hash, id = Buffer.from('-TESTIN-123456789012')) {
  return Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8), hash, id]);
}
async function waitUntil(test, timeout = 6000, description = 'condition') {
  const deadline = Date.now() + timeout;
  while (!test()) {
    if (Date.now() >= deadline) throw new Error(`Timed out waiting for ${description}`);
    await delay(10);
  }
}

class Client {
  constructor(port, host = '127.0.0.1') {
    this.socket = net.createConnection({ port, host });
    this.messages = []; this.pending = Buffer.alloc(0); this.rawBytes = 0;
    this.closed = false; this.response = undefined; this.error = undefined;
    this.socket.on('error', error => { this.error = error; });
    this.socket.on('close', () => { this.closed = true; });
    this.socket.on('data', bytes => {
      this.rawBytes += bytes.length;
      this.pending = Buffer.concat([this.pending, bytes]);
      if (!this.response) {
        if (this.pending.length < 68) return;
        this.response = this.pending.subarray(0, 68); this.pending = this.pending.subarray(68);
      }
      while (this.pending.length >= 4) {
        const length = this.pending.readUInt32BE();
        if (this.pending.length < length + 4) break;
        const message = { id: length ? this.pending[4] : -1,
          payload: this.pending.subarray(5, 4 + length), at: Date.now() };
        this.pending = this.pending.subarray(4 + length);
        this.messages.push(message);
        if (this.onMessage) this.onMessage(message);
      }
    });
  }
  async connected() {
    await waitUntil(() => !this.socket.connecting || this.error, 3000, 'TCP connection');
    if (this.error) throw this.error;
    return this;
  }
  async greeted(hash, fragmented = false) {
    await this.connected();
    const bytes = handshake(hash);
    if (fragmented) {
      for (const segment of [bytes.subarray(0, 1), bytes.subarray(1, 27), bytes.subarray(27)]) {
        this.socket.write(segment); await delay(10);
      }
    } else this.socket.write(bytes);
    await waitUntil(() => this.response || this.closed, 3000, 'handshake response');
    assert.ok(this.response, `Handshake rejected: ${this.error || ''}`);
    assert.equal(this.response[0], 19);
    assert.equal(this.response.subarray(1, 20).toString(), 'BitTorrent protocol');
    assert.deepEqual(this.response.subarray(28, 48), hash);
    await this.next(1);
    return this;
  }
  async next(id, timeout = 6000) {
    await waitUntil(() => this.messages.some(message => message.id === id) || this.closed, timeout, `message ${id}`);
    const index = this.messages.findIndex(message => message.id === id);
    assert.ok(index >= 0, `Connection closed before message ${id}`);
    return this.messages.splice(index, 1)[0];
  }
  async block(index, offset, length, expected) {
    this.socket.write(request(index, offset, length));
    const result = await this.next(7);
    assert.equal(result.payload.readUInt32BE(), index);
    assert.equal(result.payload.readUInt32BE(4), offset);
    assert.deepEqual(result.payload.subarray(8), expected);
  }
  close() { this.socket.end(); }
}

async function main() {
  const executable = path.resolve(process.argv[2]), mode = process.argv[3];
  if (mode === 'ipv6') {
    const probe = net.createServer();
    try { await new Promise((resolve, reject) => { probe.once('error', reject); probe.listen(0, '::1', resolve); }); }
    catch { console.log('SKIP incoming ipv6: loopback IPv6 is unavailable'); return; }
    await new Promise(resolve => probe.close(resolve));
  }
  const work = fs.mkdtempSync(path.join(process.cwd(), `incoming-${mode}-`));
  const pieceLength = 32768, dataLength = pieceLength * 3 + 123;
  const data = [0, 1].map(id => Buffer.from(Array.from({ length: dataLength }, (_, i) => (i * 37 + id * 71 + 3) % 256)));
  const info = data.map((payload, i) => ({ name: `${i}.bin`, length: payload.length, 'piece length': pieceLength,
    pieces: Buffer.concat(Array.from({ length: 4 }, (_, piece) => sha1(payload.subarray(piece * pieceLength, (piece + 1) * pieceLength)))) }));
  const hashes = info.map(value => sha1(encode(value)));
  const port = await availablePort(2), clients = [], announces = [];
  const tracker = http.createServer((req, res) => {
    const query = new URL(req.url, 'http://localhost').searchParams;
    announces.push({ event: query.get('event'), port: Number(query.get('port')) });
    res.end(encode({ interval: 60, peers: Buffer.alloc(0) }));
  });
  await new Promise(resolve => tracker.listen(0, '127.0.0.1', resolve));
  let occupied, child, output = '', exitCode;
  const newClient = (task = 0, host) => { const client = new Client(port + task * 2, host); clients.push(client); return client; };
  const ready = async marker => {
    await waitUntil(() => output.includes(marker) || exitCode !== undefined, 8000, marker);
    assert.ok(output.includes(marker), `Driver exited ${exitCode}: ${output}`);
  };
  const command = async (value, marker) => { child.stdin.write(value + '\n'); await ready(marker); };
  const closed = async client => waitUntil(() => client.closed, 2000, 'peer closure');
  async function rejected(bytes) {
    const client = newClient(); await client.connected(); client.socket.write(bytes);
    await closed(client); assert.equal(client.rawBytes, 0, 'Invalid handshake received a response');
  }
  try {
    fs.mkdirSync(path.join(work, 'files'));
    for (let i = 0; i < (mode === 'multi' ? 2 : 1); ++i) {
      const saved = Buffer.from(data[i]);
      if (mode === 'missing') saved.fill(0, pieceLength);
      if (mode === 'receive') saved.fill(0);
      fs.writeFileSync(path.join(work, 'files', info[i].name), saved);
      fs.writeFileSync(path.join(work, `${i}.torrent`), encode({ info: info[i], announce: `http://127.0.0.1:${tracker.address().port}/announce` }));
    }
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ download_dir: './files', listen_port: port,
      max_peers: mode === 'cap' ? 2 : 8, enable_dht: false, enable_pex: false, enable_lsd: false,
      enable_utp: false, enable_webseeds: false, enable_ipv6: mode === 'ipv6',
      max_upload_speed: mode === 'limit' ? 8192 : mode === 'stop' ? 1 : 0 }));
    if (mode === 'occupied') {
      occupied = net.createServer(); await new Promise(resolve => occupied.listen(port, '127.0.0.1', resolve));
    }
    child = spawn(executable, ['0.torrent', 'config.json', ...(mode === 'multi' ? ['1.torrent'] : [])], { cwd: work });
    child.stdout.on('data', bytes => { output += bytes; }); child.stderr.on('data', bytes => { output += bytes; });
    child.stdin.on('error', () => {});
    const exited = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', code => { exitCode = code; resolve(code); }); });
    if (mode === 'occupied') {
      assert.equal(await exited, 1, output);
      assert.ok(output.includes('Cannot listen on TCP port'), output);
      assert.equal(announces.length, 0, 'Advertised a port without a listener');
      console.log('PASS incoming occupied: binding failure is reported without a tracker announce'); return;
    }
    await ready('INCOMING_READY');
    if (mode === 'validate') {
      const wrong = Buffer.from(hashes[0]); wrong[0] ^= 0xff;
      await rejected(handshake(wrong));
      const invalid = handshake(hashes[0]); invalid[1] = 88; await rejected(invalid);
    }
    let stalled;
    if (mode === 'stalled' || mode === 'stop') {
      stalled = newClient(); await stalled.connected(); stalled.socket.write(Buffer.from([19]));
    }
    if (mode === 'cap') {
      const first = newClient(), second = newClient();
      await first.connected(); await second.connected();
      first.socket.write(Buffer.from([19])); second.socket.write(Buffer.from([19]));
      await delay(150);
      await rejected(handshake(hashes[0]));
      first.close(); second.close(); await closed(first); await closed(second); await delay(100);
    }
    const client = await newClient(0, mode === 'ipv6' ? '::1' : undefined).greeted(hashes[0], mode === 'transfer');
    const bitfield = await client.next(5);
    assert.deepEqual(bitfield.payload, Buffer.from([mode === 'missing' ? 0x80 : mode === 'receive' ? 0 : 0xf0]));
    if (mode === 'validate') {
      await rejected(handshake(hashes[0], client.response.subarray(48)));
      for (const frame of [request(0xffffffff), request(0, 0xfffffff0, 64), request(0, 0, 0), request(0, 0, 16385),
        request(0, pieceLength - 1, 2), packet(6, Buffer.alloc(10))]) client.socket.write(frame);
      await client.block(1, 123, 1024, data[0].subarray(pieceLength + 123, pieceLength + 1147));
      assert.ok(!client.messages.some(message => message.id === 7), 'Invalid request returned data');
      const invalidHave = await newClient().greeted(hashes[0]);
      const index = Buffer.alloc(4, 0xff);
      invalidHave.socket.write(packet(4, index)); await closed(invalidHave);
      const invalidBits = await newClient().greeted(hashes[0]);
      invalidBits.socket.write(packet(5, Buffer.from([0xff]))); await closed(invalidBits);
      await client.block(0, 0, 100, data[0].subarray(0, 100));
    } else if (mode === 'limit') {
      const other = await newClient().greeted(hashes[0]);
      const start = Date.now();
      client.socket.write(Buffer.concat([request(0), request(0)])); other.socket.write(Buffer.concat([request(0), request(0)]));
      for (const peer of [client, other]) for (let i = 0; i < 2; ++i) {
        const block = await peer.next(7, 12000); assert.deepEqual(block.payload.subarray(8), data[0].subarray(0, 16384));
      }
      const elapsed = Date.now() - start;
      assert.ok(elapsed >= 5500 && elapsed < 12000, `Incoming peers did not share the upload limit: ${elapsed}ms`);
    } else if (mode === 'stop') {
      client.socket.write(request(0)); await delay(150);
    } else if (mode === 'receive') {
      client.onMessage = message => {
        if (message.id !== 6) return;
        const index = message.payload.readUInt32BE(), offset = message.payload.readUInt32BE(4), size = message.payload.readUInt32BE(8);
        client.socket.write(packet(7, Buffer.concat([message.payload.subarray(0, 8), data[0].subarray(index * pieceLength + offset, index * pieceLength + offset + size)])));
      };
      client.socket.write(Buffer.concat([packet(5, Buffer.from([0xf0])), packet(1)]));
      await command('complete', 'INCOMING_COMPLETE');
      assert.deepEqual(fs.readFileSync(path.join(work, 'files', '0.bin')), data[0]);
      await client.block(0, 0, 16384, data[0].subarray(0, 16384));
    } else {
      if (mode === 'missing') client.socket.write(request(1));
      if (mode === 'corrupt') {
        const fd = fs.openSync(path.join(work, 'files', '0.bin'), 'r+');
        fs.writeSync(fd, Buffer.from([data[0][0] ^ 0xff]), 0, 1, 0); fs.closeSync(fd);
        client.socket.write(request(0));
      }
      const piece = mode === 'corrupt' ? 1 : 0;
      await client.block(piece, 0, 16384, data[0].subarray(piece * pieceLength, piece * pieceLength + 16384));
      assert.ok(!client.messages.some(message => message.id === 7), 'Missing or corrupt piece was uploaded');
      if (mode === 'transfer') for (let i = 1; i < 4; ++i) {
        const size = Math.min(16384, dataLength - i * pieceLength);
        await client.block(i, 0, size, data[0].subarray(i * pieceLength, i * pieceLength + size));
      }
    }
    if (mode === 'stalled') await waitUntil(() => stalled.closed, 6000, 'handshake deadline');
    if (mode === 'cap') for (let i = 0; i < 8; ++i) {
      const peer = await newClient().greeted(hashes[0]);
      await peer.block(0, 0, 100, data[0].subarray(0, 100)); peer.close(); await closed(peer); await delay(30);
    }
    if (mode === 'restart') {
      await command('restart', 'INCOMING_RESTARTED'); await closed(client);
      const peer = await newClient().greeted(hashes[0]);
      await peer.block(0, 0, 100, data[0].subarray(0, 100));
    }
    if (mode === 'multi') {
      const other = await newClient(1).greeted(hashes[1]);
      await other.block(0, 0, 16384, data[1].subarray(0, 16384));
      await rejected(handshake(hashes[1]));
      await command('remove', 'INCOMING_REMOVED'); await closed(client);
      await other.block(1, 0, 100, data[1].subarray(pieceLength, pieceLength + 100));
      assert.ok(announces.some(event => event.event === 'started' && event.port === port + 2));
    }
    const stopAt = Date.now();
    await command('stop', 'INCOMING_STOPPED'); assert.equal(await exited, 0, output);
    assert.ok(Date.now() - stopAt < 2000, 'Stopping incoming connections took too long');
    for (const peer of clients) await closed(peer);
    const probe = newClient(); await probe.connected().then(() => assert.fail('Stopped TCP listener is still accepting'), error => assert.equal(error.code, 'ECONNREFUSED'));
    console.log(`PASS incoming ${mode}: correct payloads and closed sockets verified`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const peer of clients) peer.socket.destroy();
    if (occupied) occupied.close(); tracker.close();
    fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
const watchdog = setTimeout(() => { console.error('Incoming fixture exceeded 25 seconds'); process.exit(1); }, 25000);
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => clearTimeout(watchdog));
