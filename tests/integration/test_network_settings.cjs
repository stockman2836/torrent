const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const dgram = require('node:dgram');
const http = require('node:http');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const { encode, decode, createDhtFixture } = require('./dht_fixture.cjs');
const { availablePort } = require('./fixture_helpers.cjs');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest();
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}
function handshake(hash) {
  const reserved = Buffer.alloc(8); reserved[5] = 0x10;
  return Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), reserved, hash,
    Buffer.from('-NETSET-123456789012')]);
}
async function udpAvailable(port) {
  const socket = dgram.createSocket('udp4');
  try {
    await new Promise((resolve, reject) => { socket.once('error', reject); socket.bind(port, '127.0.0.1', resolve); });
  } finally { await new Promise(resolve => socket.close(resolve)); }
}
async function ping(port) {
  const socket = dgram.createSocket('udp4');
  try {
    await new Promise(resolve => socket.bind(0, '127.0.0.1', resolve));
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`No DHT reply on UDP ${port}`)), 1500);
      socket.once('message', bytes => {
        clearTimeout(timer);
        try { const [reply] = decode(bytes); assert.equal(reply.r.id.length, 20); resolve(); } catch (error) { reject(error); }
      });
      socket.once('error', error => { clearTimeout(timer); reject(error); });
      socket.send(encode({ t: 'ns', y: 'q', q: 'ping', a: { id: crypto.randomBytes(20) } }), port, '127.0.0.1');
    });
  } finally { await new Promise(resolve => socket.close(resolve)); }
}

async function main() {
  const executable = path.resolve(process.argv[2]), scenario = process.argv[3];
  assert.ok(['apply', 'magnet'].includes(scenario));
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `network-settings-${scenario}-`));
  const port = await availablePort(12), sockets = new Set(), sessions = [], announces = [];
  const data = Buffer.alloc(65536);
  for (let i = 0; i < data.length; ++i) data[i] = i % 251;
  const pieceLength = 16384;
  const info = { name: 'payload.bin', length: data.length, 'piece length': pieceLength,
    pieces: Buffer.concat([0, 1, 2, 3].map(index => sha1(data.subarray(index * pieceLength, (index + 1) * pieceLength)))) };
  const metadata = encode(info), hash = sha1(metadata);
  let child, code, processError, protocolError, dht, output = '', full = false;
  const lines = [];
  function track(socket) {
    sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket)); return socket;
  }
  const seed = net.createServer(socket => {
    track(socket);
    const session = { socket, id: '', pex: false, closed: false }; sessions.push(session);
    socket.on('close', () => { session.closed = true; });
    let pending = Buffer.alloc(0), greeted = false, metadataId;
    socket.on('data', bytes => {
      try {
        pending = Buffer.concat([pending, bytes]);
        if (!greeted) {
          if (pending.length < 68) return;
          assert.deepEqual(pending.subarray(28, 48), hash);
          session.id = pending.subarray(48, 68).toString(); pending = pending.subarray(68); greeted = true;
          socket.write(Buffer.concat([handshake(hash), packet(5, Buffer.from([0xf0])), packet(1),
            packet(20, Buffer.concat([Buffer.from([0]), encode({ m: { ut_metadata: 3 }, metadata_size: metadata.length })]))]));
        }
        while (pending.length >= 4) {
          const length = pending.readUInt32BE(); if (pending.length < length + 4) break;
          const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
          if (message[0] === 20) {
            const [header] = decode(message.subarray(2));
            if (message[1] === 0) {
              metadataId = header.m?.ut_metadata; session.pex = !!header.m?.ut_pex;
            } else if (message[1] === 3 && header.msg_type === 0) {
              assert.equal(header.piece, 0); assert.ok(metadataId);
              socket.write(packet(20, Buffer.concat([Buffer.from([metadataId]),
                encode({ msg_type: 1, piece: 0, total_size: metadata.length }), metadata])));
            }
          } else if (message[0] === 6) {
            const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
            assert.ok(index < 4 && offset + size <= pieceLength);
            if (full || index === 0) socket.write(packet(7, Buffer.concat([message.subarray(1, 9),
              data.subarray(index * pieceLength + offset, index * pieceLength + offset + size)])));
          }
        }
      } catch (error) { protocolError = error; socket.destroy(); }
    });
  });
  const tracker = http.createServer((request, response) => {
    const query = new URL(request.url, 'http://localhost').searchParams;
    const event = query.get('event'), id = query.get('peer_id');
    announces.push({ event, id, port: Number(query.get('port')) });
    if (event === 'stopped') for (const session of sessions) if (session.id === id) session.socket.end();
    const compact = Buffer.from([127, 0, 0, 1, 0, 0]); compact.writeUInt16BE(seed.address().port, 4);
    response.end(encode({ interval: 60, peers: event === 'stopped' ? Buffer.alloc(0) : compact }));
  });
  const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  async function until(check, description, timeout = 8000) {
    const deadline = Date.now() + timeout;
    while (!check()) {
      if (processError) throw processError; if (protocolError) throw protocolError; if (dht) dht.check();
      if (code !== undefined || Date.now() >= deadline) throw new Error(`${description}: ${output}`);
      await delay(10);
    }
  }
  async function command(text, marker = 'OK') {
    child.stdin.write(text + '\n');
    await until(() => lines.some(line => line.startsWith(marker)), marker);
    return lines.splice(lines.findIndex(line => line.startsWith(marker)), 1)[0];
  }
  const status = async () => JSON.parse((await command('status', 'STATUS ')).slice(7));
  async function waitStatus(check, description) {
    const deadline = Date.now() + 8000;
    let states;
    do {
      states = await status(); for (const state of states) assert.equal(state.error, '', JSON.stringify(state));
      if (check(states)) return states;
      await delay(20);
    } while (Date.now() < deadline);
    throw new Error(`${description}: ${JSON.stringify(states)}`);
  }
  function peerAt(listenPort) {
    const announce = announces.filter(item => item.event === 'started' && item.port === listenPort).at(-1);
    return sessions.find(session => session.id === announce?.id && !session.closed);
  }
  try {
    await listen(seed); await listen(tracker); dht = await createDhtFixture(hash, seed.address().port);
    const torrent = encode({ info, announce: `http://127.0.0.1:${tracker.address().port}/announce` });
    fs.writeFileSync(path.join(work, 'first.torrent'), torrent); fs.writeFileSync(path.join(work, 'second.torrent'), torrent);
    const initial = { listen_port: port, max_peers: 8, enable_dht: scenario === 'magnet', enable_pex: false,
      dht_bootstrap_nodes: dht.routers, enable_lsd: false, enable_utp: false, enable_webseeds: false,
      sequential_download: true, enable_ipv6: false, download_dir: './files' };
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify(initial));
    const sources = scenario === 'magnet' ? [`magnet:?xt=urn:btih:${hash.toString('hex')}`] : ['first.torrent', 'second.torrent'];
    child = spawn(executable, ['config.json', ...sources], { cwd: work });
    let partial = '';
    child.stdout.on('data', bytes => {
      output += bytes; partial += bytes; let end;
      while ((end = partial.indexOf('\n')) >= 0) { lines.push(partial.slice(0, end).trim()); partial = partial.slice(end + 1); }
    });
    child.stderr.on('data', bytes => { output += bytes; }); child.stdin.on('error', () => {});
    child.on('error', error => { processError = error; }); child.on('close', value => { code = value; });
    await until(() => lines.includes('READY'), 'loaded metadata');
    if (scenario === 'apply') {
      await command('start 0');
      await waitStatus(states => states[0].progress === 25 && states[0].port === port, 'first partial download');
      const firstPeer = peerAt(port); assert.ok(firstPeer); assert.equal(firstPeer.pex, false);
      await command(`network ${port + 4} 1 1 1`);
      let states = await status();
      assert.equal(states[0].port, port); assert.equal(states[0].dht, false); assert.equal(states[0].config.max_peers, 8);
      assert.equal(firstPeer.closed, false, 'Saving settings interrupted the active peer');
      await command('start 1');
      states = await waitStatus(values => values[1].port === port + 4 && values[1].progress === 25 && values[1].nodes > 0, 'new session settings');
      assert.equal(states[1].config.max_peers, 1); assert.equal(states[1].config.pex, true); assert.equal(states[1].dht, true);
      await until(() => peerAt(port + 4)?.pex, 'PEX extension handshake');
      await ping(port + 5);
      const extra = track(net.createConnection(port + 4, '127.0.0.1')); let closed = false, received = 0;
      extra.on('data', bytes => { received += bytes.length; }); extra.on('close', () => { closed = true; });
      extra.write(handshake(hash)); await until(() => closed, 'new peer cap', 2500); assert.equal(received, 0);
      await command('stop 0'); await command('start 0');
      states = await waitStatus(values => values[0].port === port + 6 && values[0].dht, 'resumed session settings');
      assert.notEqual(states[0].port, states[1].port); assert.equal(states[0].config.max_peers, 1);
      await command(`network ${port + 8} 2 0 0`);
      states = await status(); assert.equal(states[0].dht, true); assert.equal(states[1].dht, true);
      await command('stop 0'); await command('stop 1');
      await command('restore');
      states = await status();
      assert.deepEqual(states.map(state => state.config.port), [port + 8, port + 10]);
      assert.ok(states.every(state => !state.busy && !state.queued && !state.config.dht && !state.config.pex && state.config.max_peers === 2));
      const starts = announces.filter(item => item.event === 'started').length;
      await delay(100); assert.equal(announces.filter(item => item.event === 'started').length, starts, 'Restored jobs started automatically');
      full = true;
      await command('start 0'); await command('start 1');
      await waitStatus(values => values.every(state => !state.busy && state.progress === 100), 'restored transfers');
      for (const index of [0, 1]) assert.deepEqual(fs.readFileSync(path.join(work, `files-${index}/payload.bin`)), data);
    } else {
      await ping(port + 1);
      await command(`network ${port + 4} 1 1 1`);
      await ping(port + 1); // Ready metadata retains its old socket until the session starts.
      await command('start 0');
      await waitStatus(states => states[0].port === port + 4 && states[0].nodes > 0 && states[0].progress === 25, 'magnet rebind');
      await ping(port + 5); await udpAvailable(port + 1);
      await command('stop 0'); await command('restore');
      await ping(port + 5); // Metadata was loaded again with the saved enabled DHT.
      await command('network 65535 2 0 0', 'REJECTED');
      assert.equal(JSON.parse(fs.readFileSync(path.join(work, 'settings.json'))).listen_port, port + 4);
      await command(`network ${port + 8} 2 0 0`); await ping(port + 5);
      await command('start 0');
      await waitStatus(states => states[0].port === port + 8 && !states[0].dht, 'disabled metadata DHT');
      await udpAvailable(port + 5); await udpAvailable(port + 9);
      await command('stop 0');
    }
    const saved = JSON.parse(fs.readFileSync(path.join(work, 'settings.json')));
    assert.equal(saved.listen_port, port + 8); assert.equal(saved.max_peers, 2);
    assert.equal(saved.enable_dht, false); assert.equal(saved.enable_pex, false);
    assert.equal(saved.enable_lsd, false); assert.deepEqual(saved.dht_bootstrap_nodes, dht.routers);
    assert.ok(!fs.existsSync(path.join(work, 'settings.json.tmp')), 'Config replacement left a temporary file');
    const final = await status(); child.stdin.end('quit\n');
    await until(() => code !== undefined, 'driver exit'); assert.equal(code, 0, output); dht.check();
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ scenario, passed: true, saved, final }, null, 2));
    console.log(`PASS network settings ${scenario}: deferred ports/discovery/peer limits and persistence verified.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const socket of sockets) socket.destroy();
    await Promise.all([seed, tracker].filter(server => server.listening).map(server => new Promise(resolve => server.close(resolve))));
    if (dht) await dht.close();
    fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
