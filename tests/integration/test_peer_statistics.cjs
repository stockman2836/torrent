const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { availablePort } = require('./fixture_helpers.cjs');
const { encode } = require('./dht_fixture.cjs');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest();
function packet(id, body = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(body.length + 1); header[4] = id;
  return Buffer.concat([header, body]);
}
function decode(bytes, position = 0) {
  const first = bytes[position++];
  if (first === 105) { const end = bytes.indexOf(101, position); return [Number(bytes.subarray(position, end)), end + 1]; }
  if (first === 100) {
    const result = {};
    while (bytes[position] !== 101) {
      let key, value; [key, position] = decode(bytes, position); [value, position] = decode(bytes, position); result[key.toString()] = value;
    }
    return [result, position + 1];
  }
  assert.ok(first >= 48 && first <= 57);
  const colon = bytes.indexOf(58, position - 1), size = Number(bytes.subarray(position - 1, colon));
  return [bytes.subarray(colon + 1, colon + 1 + size), colon + 1 + size];
}
function extended(id, value) { return packet(20, Buffer.concat([Buffer.from([id]), encode(value)])); }

async function main() {
  const executable = path.resolve(process.argv[2]), scenario = process.argv[3];
  assert.ok(['download', 'upload', 'idle', 'unknown', 'connect-failure', 'handshake', 'handshake-timeout', 'bad-frame', 'cancel', 'history'].includes(scenario));
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `peer-statistics-${scenario}-`));
  const data = Buffer.alloc(131072); for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + (i >>> 8) * 13) % 251;
  const hashes = []; for (let i = 0; i < data.length; i += 16384) hashes.push(sha1(data.subarray(i, i + 16384)));
  const metadata = encode({ name: 'payload.bin', length: data.length, 'piece length': 16384, pieces: Buffer.concat(hashes) });
  const hash = sha1(metadata), port = await availablePort(2), wires = [], seeds = [], timers = new Set(), lines = [];
  let child, code, failure, output = '', fakeConnections = 0;
  const hello = (peerId, infoHash = hash, extensions = true) => {
    const reserved = Buffer.alloc(8); if (extensions) reserved[5] = 0x10;
    return Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), reserved, infoHash, peerId]);
  };
  async function until(check, description, timeout = 8000) {
    const end = Date.now() + timeout;
    do { if (failure) throw failure; if (check()) return; if (code !== undefined) throw new Error(output); await delay(10); } while (Date.now() < end);
    throw new Error(`${description}\n${output}`);
  }
  class Wire {
    constructor(socket) {
      this.socket = socket; this.pending = Buffer.alloc(0); this.messages = []; this.closed = false; wires.push(this);
      socket.on('error', () => {}); socket.on('close', () => { this.closed = true; });
      socket.on('data', bytes => {
        try {
          this.pending = Buffer.concat([this.pending, bytes]);
          if (!this.greeting && this.pending.length >= 68) {
            this.greeting = this.pending.subarray(0, 68); this.pending = this.pending.subarray(68);
            if (this.onHello) this.onHello(this.greeting);
          }
          if (!this.greeting) return;
          while (this.pending.length >= 4) {
            const length = this.pending.readUInt32BE(); if (this.pending.length < length + 4) break;
            const frame = { id: length ? this.pending[4] : -1, body: this.pending.subarray(5, length + 4) };
            this.pending = this.pending.subarray(length + 4); this.messages.push(frame);
            if (this.onFrame) this.onFrame(frame);
          }
        } catch (error) { failure = error; socket.destroy(); }
      });
    }
    async next(predicate) {
      await until(() => this.messages.some(predicate) || this.closed, 'wire response');
      const index = this.messages.findIndex(predicate); assert.ok(index >= 0, 'Connection closed before expected response');
      return this.messages.splice(index, 1)[0];
    }
    async greet(extensions = true, name = 'IncomingFixture/2.3') {
      this.socket.write(hello(Buffer.from('-qB4620-123456789012'), hash, extensions));
      await until(() => this.greeting || this.closed, 'incoming handshake'); assert.ok(this.greeting);
      assert.deepEqual(this.greeting.subarray(28, 48), hash);
      await this.next(frame => frame.id === 1);
      if (extensions) {
        const message = await this.next(frame => frame.id === 20 && frame.body[0] === 0);
        this.localMetadataId = decode(message.body, 1)[0].m.ut_metadata;
        this.socket.write(extended(0, { m: { ut_metadata: 73 }, v: name }));
      }
    }
    async block() {
      const request = Buffer.alloc(12); request.writeUInt32BE(16384, 8); this.socket.write(packet(6, request));
      const frame = await this.next(frame => frame.id === 7);
      assert.deepEqual(frame.body.subarray(8), data.subarray(0, 16384));
    }
  }
  function fakeSeed(name) {
    const server = net.createServer(socket => {
      ++fakeConnections; const attempt = fakeConnections; const wire = new Wire(socket); wire.sent = 0;
      wire.onHello = greeting => {
        assert.deepEqual(greeting.subarray(28, 48), hash);
        if (['cancel', 'handshake-timeout'].includes(scenario)) return;
        if (scenario === 'handshake') {
          if (attempt === 1) { const wrong = Buffer.from(hash); wrong[0] ^= 1; socket.write(hello(Buffer.from('-SEEDST-123456789012'), wrong)); }
          else if (attempt === 2) { const wrong = hello(Buffer.from('-SEEDST-123456789012')); wrong[0] = 18; socket.write(wrong); }
          else socket.write(hello(greeting.subarray(48, 68)));
          return;
        }
        socket.write(Buffer.concat([hello(Buffer.from('-SEEDST-123456789012')), packet(5, Buffer.from([0xff])), packet(1)]));
        const client = scenario === 'bad-frame' ? Buffer.concat([Buffer.from('Fixture\0bad\n'), Buffer.from([255]), Buffer.from('я'.repeat(300))]) : name;
        socket.write(extended(0, { m: {}, v: client }));
        if (scenario === 'bad-frame') {
          const timer = setTimeout(() => {
            timers.delete(timer); if (socket.destroyed) return;
            if (attempt === 1) socket.write(Buffer.from([255, 255, 255, 255]));
            else { const index = Buffer.alloc(4); index.writeUInt32BE(999); socket.write(packet(4, index)); }
          }, 80); timers.add(timer);
        }
      };
      wire.onFrame = frame => {
        if (frame.id !== 6 || ['bad-frame', 'handshake', 'cancel', 'handshake-timeout'].includes(scenario)) return;
        if (scenario === 'download' && wire.sent) return;
        const index = frame.body.readUInt32BE(), offset = frame.body.readUInt32BE(4), size = frame.body.readUInt32BE(8);
        const start = index * 16384 + offset;
        assert.ok(index < 8 && size > 0 && size <= 16384 && start + size <= data.length);
        socket.write(packet(7, Buffer.concat([frame.body.subarray(0, 8), data.subarray(start, start + size)]))); wire.sent += size;
      };
    }); seeds.push(server); return server;
  }
  const tracker = http.createServer((req, res) => {
    const query = new URL(req.url, 'http://localhost').searchParams;
    let peers = seeds.map(server => server.address().port);
    if (scenario === 'connect-failure') peers = [port + 2];
    if (scenario === 'handshake' && fakeConnections >= 3 || scenario === 'bad-frame' && fakeConnections >= 2 || query.get('event') === 'stopped') peers = [];
    const compact = Buffer.concat(peers.map(value => Buffer.from([127, 0, 0, 1, value >> 8, value & 255])));
    res.end(encode({ interval: ['handshake', 'bad-frame'].includes(scenario) ? 1 : 60, peers: compact }));
  });
  const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  async function command(value, marker) {
    child.stdin.write(value + '\n'); await until(() => lines.some(line => line.startsWith(marker)), marker);
    return lines.splice(lines.findIndex(line => line.startsWith(marker)), 1)[0];
  }
  const status = async () => JSON.parse((await command('status', 'STATUS ')).slice(7));
  async function waitStatus(check, description, timeout = 8000) {
    const end = Date.now() + timeout; let state;
    do { state = await status(); assert.equal(state.error, ''); if (check(state)) return state; await delay(25); } while (Date.now() < end);
    throw new Error(`${description}: ${JSON.stringify(state)}`);
  }
  const incoming = () => new Wire(net.createConnection({ host: '127.0.0.1', port }));
  let observed, final;
  try {
    if (!['history', 'connect-failure'].includes(scenario)) await listen(fakeSeed('FixtureSeed/A-1.0'));
    if (scenario === 'download') await listen(fakeSeed('FixtureSeed/B-2.0'));
    await listen(tracker);
    fs.writeFileSync(path.join(work, 'test.torrent'), Buffer.concat([Buffer.from('d8:announce'), encode(`http://127.0.0.1:${tracker.address().port}/announce`), Buffer.from('4:info'), metadata, Buffer.from('e')]));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_pex: false, enable_lsd: false,
      enable_ipv6: false, enable_port_mapping: false, enable_webseeds: false, enable_utp: false, enable_encryption: false,
      download_dir: './files', listen_port: port, max_peers: 4, seed_after_download: true, seed_ratio_limit: 0, seed_time_limit_hours: 0 }));
    child = spawn(executable, ['test.torrent', 'config.json'], { cwd: work, windowsHide: true });
    child.on('error', error => { failure = error; }); child.on('close', value => { code = value; }); child.stdin.on('error', () => {});
    let pending = ''; for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => {
      output += bytes; if (stream !== child.stdout) return; pending += bytes;
      let end; while ((end = pending.indexOf('\n')) >= 0) { lines.push(pending.slice(0, end).trim()); pending = pending.slice(end + 1); }
    });
    await until(() => lines.includes('READY'), 'ready'); assert.equal((await status()).peer_history.length, 0);
    await command('start', 'STARTED'); await waitStatus(state => state.listener.state === 'listening', 'listener');
    if (scenario === 'download') {
      observed = await waitStatus(state => state.peers.length === 2 && state.peers.every(peer => peer.downloaded === 16384 && peer.download_speed > 0 && peer.client), 'two independent peer rates');
      assert.deepEqual(observed.peers.map(peer => peer.client).sort(), ['FixtureSeed/A-1.0', 'FixtureSeed/B-2.0']);
      assert.equal(new Set(observed.peers.map(peer => peer.id)).size, 2);
      await delay(5200); await waitStatus(state => state.peers.every(peer => peer.download_speed === 0), 'idle download rates');
      const first = wires.find(wire => wire.sent === 16384); first.socket.end();
      const state = await waitStatus(state => state.peer_history.length === 1 && state.peers.length === 1, 'closed source history');
      assert.equal(state.peer_history[0].reason, 'remote-closed'); assert.equal(state.peer_history[0].downloaded, 16384);
      assert.equal(state.peers[0].downloaded, 16384);
    } else if (['upload', 'idle', 'unknown'].includes(scenario)) {
      await waitStatus(state => state.progress === 100, 'verified seed');
      assert.deepEqual(fs.readFileSync(path.join(work, 'files', 'payload.bin')), data);
      const wire = incoming(); await wire.greet(scenario !== 'unknown');
      let state = await waitStatus(state => state.peers.some(peer => peer.incoming && peer.state === 'connected' &&
        (scenario === 'unknown' || peer.client === 'IncomingFixture/2.3')), 'incoming published client');
      if (scenario === 'idle') {
        wire.socket.write(extended(wire.localMetadataId, { msg_type: 0, piece: 0 }));
        const response = await wire.next(frame => frame.id === 20 && frame.body[0] === 73);
        const [header, end] = decode(response.body, 1); assert.equal(header.msg_type, 1); assert.deepEqual(response.body.subarray(end), metadata);
        state = await status(); const peer = state.peers.find(peer => peer.incoming);
        assert.equal(peer.uploaded, 0); assert.equal(peer.upload_speed, 0); assert.equal(state.uploaded, 0);
      }
      await wire.block();
      observed = await waitStatus(state => state.peers.some(peer => peer.incoming && peer.uploaded === 16384 && peer.upload_speed > 0), 'incoming upload rate');
      const peer = observed.peers.find(peer => peer.incoming);
      assert.equal(peer.downloaded, 0); assert.equal(peer.download_speed, 0);
      assert.equal(peer.client, scenario === 'unknown' ? '' : 'IncomingFixture/2.3');
      if (scenario === 'idle') { await delay(5200); await waitStatus(state => state.peers.find(peer => peer.incoming)?.upload_speed === 0, 'idle upload rate'); }
      wire.socket.end();
      const stateAfter = await waitStatus(state => state.peer_history.some(item => item.id === peer.id), 'incoming closure');
      assert.ok(stateAfter.peer_history.some(item => item.incoming && item.reason === 'remote-closed' && item.uploaded === 16384));
    } else if (scenario === 'connect-failure') {
      observed = await waitStatus(state => state.peer_history.length === 1, 'refused TCP connection');
      assert.equal(observed.peer_history[0].reason, 'connect-failed'); assert.ok(observed.peer_history[0].system_error > 0);
      assert.equal(observed.failed, 1);
    } else if (scenario === 'handshake') {
      observed = await waitStatus(state => state.peer_history.length === 3, 'handshake failure reasons');
      assert.deepEqual(observed.peer_history.map(peer => peer.reason), ['wrong-torrent', 'invalid-handshake', 'self-connection']);
      assert.equal(new Set(observed.peer_history.map(peer => peer.id)).size, 3);
    } else if (scenario === 'handshake-timeout' || scenario === 'cancel') {
      incoming();
      await waitStatus(state => state.peers.length === 2 && state.peers.every(peer => peer.state === 'handshaking'), 'pending handshakes');
      if (scenario === 'handshake-timeout') {
        observed = await waitStatus(state => state.peer_history.length === 2, 'handshake deadlines');
        assert.ok(observed.peer_history.every(peer => peer.reason === 'handshake-timeout' && peer.duration >= 4500));
      } else {
        const began = Date.now(); await command('stop', 'STOPPED'); assert.ok(Date.now() - began < 1500);
        observed = await status(); assert.equal(observed.failed, 0);
        assert.equal(observed.peer_history.length, 2); assert.ok(observed.peer_history.every(peer => peer.reason === 'stopped'));
      }
    } else if (scenario === 'bad-frame') {
      observed = await waitStatus(state => state.peer_history.length === 2, 'invalid message reasons');
      for (const peer of observed.peer_history) {
        assert.equal(peer.reason, 'invalid-message'); assert.ok(Buffer.byteLength(peer.client) <= 256);
        assert.ok(peer.client.includes('?')); assert.ok(!/[\x00-\x1f\x7f]/.test(peer.client));
      }
    } else if (scenario === 'history') {
      const kept = incoming(); await kept.greet();
      for (let i = 0; i < 70; ++i) {
        const wire = incoming(); await wire.greet(); wire.socket.end(); await until(() => wire.closed, 'peer closure'); await delay(5);
      }
      observed = await waitStatus(state => state.peer_history.length === 64 && state.peers.length === 1, 'bounded history');
      const ids = observed.peer_history.map(peer => peer.id); assert.deepEqual(ids, [...ids].sort((a, b) => a - b));
      assert.ok(observed.peer_history.every(peer => peer.reason === 'remote-closed'));
      for (let i = 0; i < 3; ++i) incoming();
      await waitStatus(state => state.peers.length === 4, 'full peer cap');
      const rejected = incoming(); await until(() => rejected.closed, 'cap rejection');
      await waitStatus(state => state.peer_history.some(peer => peer.reason === 'peer-limit'), 'cap reason');
      await command('stop', 'STOPPED'); await command('start', 'STARTED');
      await waitStatus(state => state.listener.state === 'listening' && state.peer_history.length === 0, 'fresh history after restart');
    }
    await command('stop', 'STOPPED'); final = await status();
    assert.equal(final.peers.length, 0);
    assert.ok(final.peer_history.every(peer => peer.finished > 0 && peer.duration >= 0 && peer.reason !== 'none' &&
      peer.download_speed === 0 && peer.upload_speed === 0));
    if (['download', 'upload', 'idle', 'unknown'].includes(scenario)) assert.ok(final.peer_history.some(peer => peer.reason === 'stopped'));
    child.stdin.end('quit\n'); await until(() => code !== undefined, 'shutdown'); assert.equal(code, 0, output);
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ scenario, observed, final, passed: true }, null, 2));
    console.log(`PASS peer statistics ${scenario}: rates, client text and connection history verified.`);
  } finally {
    if (child && code === undefined) { child.stdin.end('quit\n'); try { await until(() => code !== undefined, 'cleanup', 2000); } catch { child.kill(); } }
    for (const timer of timers) clearTimeout(timer);
    for (const wire of wires) wire.socket.destroy();
    tracker.closeAllConnections(); await new Promise(resolve => tracker.close(resolve));
    for (const server of seeds) await new Promise(resolve => server.close(resolve));
    fs.writeFileSync(path.join(work, 'engine-output.log'), output); console.log(`Artifacts: ${work}`);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
