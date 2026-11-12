const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const http = require('node:http');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const { availablePort } = require('./fixture_helpers.cjs');
const { encode, createDhtFixture } = require('./dht_fixture.cjs');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest();
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}
function handshake(hash) {
  return Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8), hash,
    Buffer.from('-STATUS-123456789012')]);
}
async function main() {
  const executable = path.resolve(process.argv[2]), scenario = process.argv[3];
  assert.ok(['transfer', 'error', 'none', 'dht', 'occupied', 'cap'].includes(scenario));
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `network-status-${scenario}-`));
  const data = Buffer.alloc(65536);
  for (let i = 0; i < data.length; ++i) data[i] = i % 251;
  const pieceLength = 16384;
  const info = { name: 'payload.bin', length: data.length, 'piece length': pieceLength,
    pieces: Buffer.concat([0, 1, 2, 3].map(index => sha1(data.subarray(index * pieceLength, (index + 1) * pieceLength)))) };
  const hash = sha1(encode(info)), sockets = new Set(), requests = [], announces = [];
  let child, output = '', code, processError, protocolError, seedSocket, full = false, healthy = false, blockedReply, dht;
  const lines = [];
  function serve(socket) {
    sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket));
    return socket;
  }
  const seed = net.createServer(socket => {
    serve(socket); seedSocket = socket;
    let pending = Buffer.alloc(0), greeted = false;
    socket.on('data', chunk => {
      try {
        pending = Buffer.concat([pending, chunk]);
        if (!greeted) {
          if (pending.length < 68) return;
          assert.deepEqual(pending.subarray(28, 48), hash);
          pending = pending.subarray(68); greeted = true;
          socket.write(Buffer.concat([handshake(hash), packet(5, Buffer.from([0xf0]))]));
          if (full) socket.write(packet(1));
        }
        while (pending.length >= 4) {
          const length = pending.readUInt32BE();
          if (pending.length < length + 4) break;
          const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
          if (!message.length || message[0] !== 6) continue;
          const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
          assert.ok(index < 4 && offset + size <= pieceLength);
          const request = { index, offset, size, socket }; requests.push(request);
          if (full) answer(request);
        }
      } catch (error) { protocolError = error; socket.destroy(); }
    });
  });
  function answer({ index, offset, size, socket }) {
    const header = Buffer.alloc(8); header.writeUInt32BE(index); header.writeUInt32BE(offset, 4);
    socket.write(packet(7, Buffer.concat([header, data.subarray(index * pieceLength + offset, index * pieceLength + offset + size)])));
  }
  const bad = net.createServer(socket => {
    serve(socket); socket.once('data', () => socket.end(Buffer.alloc(68)));
  });
  const occupied = net.createServer(socket => socket.destroy());
  const compact = port => Buffer.from([127, 0, 0, 1, port >> 8, port & 255]);
  const tracker = http.createServer((request, response) => {
    const params = new URL(request.url, 'http://localhost').searchParams;
    announces.push({ event: params.get('event'), left: Number(params.get('left')) });
    if (params.get('event') === 'stopped') return response.end(encode({ peers: Buffer.alloc(0), interval: 60 }));
    if (scenario === 'error' && !healthy) {
      blockedReply = () => {
        blockedReply = undefined; response.end(encode({ 'failure reason': 'fixture tracker unavailable' }));
      }; return;
    }
    response.end(encode({ interval: 60, peers: scenario === 'transfer'
      ? Buffer.concat([compact(seed.address().port), compact(bad.address().port)]) : Buffer.alloc(0) }));
  });
  const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  async function until(check, description, timeout = 6000) {
    const deadline = Date.now() + timeout;
    while (!check()) {
      if (processError) throw processError;
      if (protocolError) throw protocolError;
      if (dht) dht.check();
      if (code !== undefined || Date.now() >= deadline) throw new Error(`${description}: ${output}`);
      await delay(10);
    }
  }
  async function command(text, marker) {
    child.stdin.write(text + '\n');
    await until(() => lines.some(line => line.startsWith(marker)), marker);
    const index = lines.findIndex(line => line.startsWith(marker)); return lines.splice(index, 1)[0];
  }
  const status = async () => JSON.parse((await command('status', 'STATUS ')).slice(7));
  async function waitStatus(check, description, allowError = false) {
    let snapshot;
    const deadline = Date.now() + 6000;
    do {
      snapshot = await status();
      if (!allowError) assert.equal(snapshot.error, '', JSON.stringify(snapshot));
      if (check(snapshot)) return snapshot;
      await delay(20);
    } while (Date.now() < deadline);
    throw new Error(`${description}: ${JSON.stringify(snapshot)}`);
  }
  try {
    await listen(seed); await listen(bad); await listen(tracker);
    const listenPort = await availablePort();
    if (scenario === 'occupied') await new Promise(resolve => occupied.listen(listenPort, '0.0.0.0', resolve));
    if (scenario === 'dht') dht = await createDhtFixture(hash, seed.address().port);
    const url = `http://127.0.0.1:${tracker.address().port}/announce`;
    const metadata = { info };
    if (['transfer', 'error'].includes(scenario)) metadata.announce = url;
    fs.writeFileSync(path.join(work, 'test.torrent'), encode(metadata));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ download_dir: './files',
      listen_port: listenPort, enable_dht: !!dht, dht_port: 0,
      dht_bootstrap_nodes: dht?.routers || [], enable_pex: false, enable_lsd: false,
      enable_webseeds: false, enable_utp: false, max_peers: scenario === 'cap' ? 1 : 8,
      enable_ipv6: scenario === 'cap', sequential_download: true }));
    child = spawn(executable, ['test.torrent', 'config.json'], { cwd: work });
    let partial = '';
    child.stdout.on('data', bytes => {
      output += bytes; partial += bytes;
      let end;
      while ((end = partial.indexOf('\n')) >= 0) { lines.push(partial.slice(0, end).trim()); partial = partial.slice(end + 1); }
    });
    child.stderr.on('data', bytes => { output += bytes; });
    child.on('error', error => { processError = error; }); child.on('close', value => { code = value; });
    child.stdin.on('error', () => {});
    await until(() => lines.includes('READY'), 'driver ready');
    const initial = await status();
    assert.equal(initial.tracker.state, metadata.announce ? 'waiting' : 'none');
    assert.equal(initial.peers.length, 0);
    assert.equal(initial.listener.state, 'not-started');
    await command('start', 'STARTED');
    if (scenario === 'transfer' || scenario === 'dht') {
      let snapshot = await waitStatus(state => state.peers.some(peer => !peer.incoming && peer.state === 'connected'), 'connected peer');
      const outgoing = snapshot.peers.find(peer => !peer.incoming && peer.state === 'connected');
      assert.equal(snapshot.listener.state, 'listening'); assert.equal(snapshot.listener.port, listenPort);
      assert.equal(snapshot.listener.ipv6, false); assert.equal(snapshot.outgoing, 1);
      assert.equal(outgoing.port, seed.address().port); assert.equal(outgoing.choked, true);
      if (scenario === 'dht') {
        assert.equal(snapshot.dht, true); assert.ok(snapshot.nodes >= 1); assert.equal(snapshot.tracker.state, 'none');
      } else {
        assert.equal(snapshot.tracker.state, 'working'); assert.equal(snapshot.tracker.peers, 2);
        assert.equal(snapshot.tracker.url, url); assert.ok(snapshot.failed >= 1); assert.ok(snapshot.peer_error);
        seedSocket.write(packet(1));
        await until(() => requests.some(request => request.index === 0), 'wire request');
        snapshot = await waitStatus(state => state.peers.some(peer => peer.pending > 0 && !peer.choked), 'pending download');
        assert.equal(snapshot.peers.find(peer => !peer.incoming).downloaded, 0);
        answer(requests.find(request => request.index === 0));
        await waitStatus(state => state.peers.some(peer => peer.downloaded === pieceLength), 'download byte count');
        await waitStatus(state => state.progress === 25, 'verified piece');
        const incoming = serve(net.createConnection(snapshot.port, '127.0.0.1'));
        let received = Buffer.alloc(0);
        incoming.on('data', bytes => { received = Buffer.concat([received, bytes]); });
        await until(() => !incoming.connecting, 'incoming TCP connection');
        await waitStatus(state => state.peers.some(peer => peer.incoming && peer.state === 'handshaking'), 'incoming handshake state');
        incoming.write(handshake(hash));
        await until(() => received.length >= 68, 'incoming handshake response');
        const request = Buffer.alloc(12); request.writeUInt32BE(4096, 8);
        incoming.write(Buffer.concat([packet(2), packet(6, request)]));
        await waitStatus(state => state.peers.some(peer => peer.incoming && peer.uploaded === 4096), 'upload byte count');
        snapshot = await status(); assert.equal(snapshot.incoming, 1); assert.equal(snapshot.local_incoming, 1);
        assert.match(snapshot.report, /Доступность из интернета: не проверена/);
        await until(() => received.includes(data.subarray(0, 4096)), 'actual uploaded bytes');
        incoming.destroy();
        await waitStatus(state => !state.peers.some(peer => peer.incoming), 'disconnected incoming peer removed');
        assert.equal((await status()).incoming, 1, 'Disconnected incoming evidence was lost');
      }
      await command('stop', 'STOPPED');
      snapshot = await status(); assert.equal(snapshot.peers.length, 0); assert.equal(snapshot.queued, 0);
      assert.equal(snapshot.listener.state, 'stopped');
      if (scenario === 'transfer') {
        full = true;
        await command('start', 'STARTED');
        await waitStatus(state => !state.busy && state.progress === 100, 'resumed completion');
        assert.deepEqual(fs.readFileSync(path.join(work, 'files/payload.bin')), data);
        assert.deepEqual(announces.filter(item => item.event === 'started').map(item => item.left), [data.length, data.length - pieceLength]);
      }
    } else if (scenario === 'error') {
      await until(() => blockedReply, 'pending tracker request');
      const pending = await waitStatus(state => state.tracker.state === 'announcing', 'visible initial announce');
      assert.equal(pending.listener.state, 'listening');
      assert.ok(pending.port, 'Initial tracker query must not block publishing listener state');
      blockedReply();
      let snapshot = await waitStatus(state => state.tracker.state === 'error', 'tracker error');
      assert.equal(snapshot.tracker.error, 'fixture tracker unavailable'); assert.equal(snapshot.tracker.failures, 1);
      await command('stop', 'STOPPED'); snapshot = await status();
      assert.equal(snapshot.tracker.state, 'error', 'Stopped announce must not erase the useful error');
      healthy = true;
      await command('reload', 'RELOADED'); snapshot = await status();
      assert.equal(snapshot.tracker.state, 'waiting'); assert.equal(snapshot.tracker.error, '');
      await command('start', 'STARTED');
      snapshot = await waitStatus(state => state.tracker.state === 'working', 'tracker recovery');
      assert.equal(snapshot.tracker.error, ''); assert.equal(snapshot.tracker.failures, 0);
      assert.equal(snapshot.tracker.peers, 0); assert.equal(snapshot.peers.length, 0);
      await command('stop', 'STOPPED');
    } else if (scenario === 'occupied') {
      const snapshot = await waitStatus(state => !state.busy && state.listener.state === 'error', 'listener error', true);
      assert.equal(snapshot.listener.port, listenPort); assert.ok(snapshot.error); assert.ok(snapshot.listener.error);
      assert.match(snapshot.hint, /TCP-порт/); assert.match(snapshot.report, /начальный порт/);
      await command('stop', 'STOPPED');
    } else if (scenario === 'cap') {
      let snapshot = await waitStatus(state => state.listener.state === 'listening', 'listener started');
      async function inbound(host) {
        const socket = serve(net.createConnection(snapshot.port, host));
        await until(() => !socket.connecting, 'inbound connected');
        socket.write(handshake(hash)); return socket;
      }
      const first = await inbound('127.0.0.1');
      snapshot = await waitStatus(state => state.incoming === 1, 'first incoming handshake');
      assert.equal(snapshot.local_incoming, 1); assert.match(snapshot.report, /Достигнут лимит пиров: 1/);
      const rejected = serve(net.createConnection(snapshot.port, '127.0.0.1'));
      await until(() => rejected.destroyed, 'excess incoming rejected');
      snapshot = await waitStatus(state => state.rejected === 1, 'cap diagnostics');
      assert.equal(snapshot.incoming_failed, 0);
      first.destroy(); await waitStatus(state => state.peers.length === 0, 'first peer gone');
      const second = await inbound(snapshot.listener.ipv6 ? '::1' : '127.0.0.1');
      snapshot = await waitStatus(state => state.incoming === 2, 'second incoming handshake');
      assert.equal(snapshot.local_incoming, 2, 'IPv6 or mapped IPv4 was misclassified as remote evidence');
      second.destroy(); await waitStatus(state => state.peers.length === 0, 'second peer gone');
      const invalid = serve(net.createConnection(snapshot.port, '127.0.0.1'));
      await until(() => !invalid.connecting, 'invalid peer connected'); invalid.end(Buffer.alloc(68));
      snapshot = await waitStatus(state => state.incoming_failed === 1 && state.peers.length === 0, 'invalid handshake diagnostics');
      assert.equal(snapshot.incoming, 2); assert.equal(snapshot.local_incoming, 2);
      assert.equal(snapshot.outgoing, 0); assert.match(snapshot.report, /с других адресов: 0/);
      await command('stop', 'STOPPED');
      snapshot = await status(); assert.equal(snapshot.listener.state, 'stopped'); assert.equal(snapshot.incoming, 2);
    } else {
      const snapshot = await waitStatus(state => state.port > 0, 'trackerless session');
      assert.equal(snapshot.tracker.state, 'none'); assert.equal(snapshot.tracker.url, '');
      assert.equal(snapshot.dht, false); assert.equal(snapshot.nodes, 0); assert.equal(snapshot.peers.length, 0);
      await command('stop', 'STOPPED');
    }
    const final = await status(); assert.equal(final.peers.length, 0); assert.equal(final.busy, false);
    child.stdin.end('quit\n'); await until(() => code !== undefined, 'driver exit'); assert.equal(code, 0, output);
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ scenario, passed: true, final }, null, 2));
    console.log(`PASS network status ${scenario}: live snapshots and lifecycle verified.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const socket of sockets) socket.destroy();
    if (blockedReply && !healthy) blockedReply();
    await Promise.all([seed, bad, tracker, occupied].filter(server => server.listening).map(server => new Promise(resolve => server.close(resolve))));
    if (dht) await dht.close();
    fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
