// Local tracker tiers, wire events and exact payloads. No public discovery.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const dgram = require('node:dgram');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { availablePort } = require('./fixture_helpers.cjs');
const { encode } = require('./dht_fixture.cjs');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest();
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}

async function main() {
  const executable = path.resolve(process.argv[2]), scenario = process.argv[3];
  const scenarios = ['failure', 'http', 'malformed', 'empty', 'timeout', 'list-only', 'udp-timeout',
    'udp-backup', 'promotion', 'mid-download', 'recover', 'completion-timeout', 'all-fail', 'cancel-http', 'cancel-udp', 'empty-peers', 'stop-bounded', 'shutdown-race'];
  assert.ok(scenarios.includes(scenario), scenario);
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `tracker-fallback-${scenario}-`));
  const data = Buffer.alloc(65536); for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + 13) % 251;
  const info = { name: 'payload.bin', length: data.length, 'piece length': 16384,
    pieces: Buffer.concat([0, 1, 2, 3].map(i => sha1(data.subarray(i * 16384, (i + 1) * 16384)))) };
  const hash = sha1(encode(info)), sockets = new Set(), announces = [], requests = [], held = [], lines = [];
  let child, code, processError, protocolError, output = '', full = false, chosen, connections = 0;
  function socketTrack(socket) {
    sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket)); return socket;
  }
  function answer(request) {
    if (request.socket.destroyed) return;
    const header = Buffer.alloc(8); header.writeUInt32BE(request.index); header.writeUInt32BE(request.offset, 4);
    request.socket.write(packet(7, Buffer.concat([header,
      data.subarray(request.index * 16384 + request.offset, request.index * 16384 + request.offset + request.size)])));
  }
  const seed = net.createServer(socket => {
    ++connections;
    socketTrack(socket); let pending = Buffer.alloc(0), greeted = false;
    socket.on('data', chunk => {
      try {
        pending = Buffer.concat([pending, chunk]);
        if (!greeted) {
          if (pending.length < 68) return;
          assert.deepEqual(pending.subarray(28, 48), hash); pending = pending.subarray(68); greeted = true;
          socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8),
            hash, Buffer.from('-FALLBK-123456789012'), packet(5, Buffer.from([0xf0])), packet(1)]));
        }
        while (pending.length >= 4) {
          const length = pending.readUInt32BE(); if (pending.length < length + 4) break;
          const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
          if (message[0] !== 6) continue;
          const request = { index: message.readUInt32BE(1), offset: message.readUInt32BE(5), size: message.readUInt32BE(9), socket };
          assert.ok(request.index < 4 && request.offset + request.size <= 16384);
          requests.push(request);
          if (full || scenario === 'mid-download' && request.index === 0) answer(request); else held.push(request);
        }
      } catch (error) { protocolError = error; socket.destroy(); }
    });
  });
  const compact = () => Buffer.from([127, 0, 0, 1, seed.address().port >> 8, seed.address().port & 255]);
  const tracker = http.createServer((request, response) => {
    try {
      const url = new URL(request.url, 'http://localhost'), name = url.pathname, event = url.searchParams.get('event') || '';
      announces.push({ name, event, left: Number(url.searchParams.get('left')), time: Date.now() });
      assert.ok(!['/legacy', '/unused'].includes(name), `Unexpected tracker: ${name}`);
      if (event === 'stopped') {
        if (scenario === 'stop-bounded') return;
        response.end(encode({ peers: Buffer.alloc(0), interval: 60 })); return;
      }
      if (scenario === 'completion-timeout' && event === 'completed') return;
      let failure = name === '/first';
      if (scenario === 'mid-download') failure = name === '/first' && event === '';
      if (scenario === 'recover') failure = name === '/first' && announces.filter(a => a.name === '/first').length === 1;
      if (['empty-peers', 'stop-bounded', 'shutdown-race'].includes(scenario)) failure = false;
      if (scenario === 'promotion') {
        if (!chosen && announces.length === 2) chosen = name;
        failure = name !== chosen;
      }
      if (scenario === 'all-fail') failure = true;
      if (failure) {
        if (['timeout', 'cancel-http'].includes(scenario)) return;
        if (scenario === 'http') { response.writeHead(503); response.end('unavailable'); return; }
        if (scenario === 'malformed') { response.end('not bencode'); return; }
        if (scenario === 'empty') { response.end(); return; }
        response.end(encode({ 'failure reason': `unavailable ${name}` })); return;
      }
      response.end(encode({ interval: ['promotion', 'mid-download', 'recover'].includes(scenario) ? 1 : 60,
        peers: ['promotion', 'empty-peers', 'stop-bounded', 'shutdown-race'].includes(scenario) ? Buffer.alloc(0) : compact() }));
    } catch (error) { protocolError = error; response.writeHead(500); response.end(); }
  });
  tracker.on('connection', socketTrack);
  const udp = dgram.createSocket('udp4');
  udp.on('message', (message, remote) => {
    try {
      const action = message.readUInt32BE(8), transaction = message.readUInt32BE(12);
      if (scenario !== 'udp-backup') { announces.push({ name: 'udp', action, time: Date.now() }); return; }
      if (action === 0) {
        const reply = Buffer.alloc(16); reply.writeUInt32BE(transaction, 4); reply.writeBigUInt64BE(12345n, 8);
        udp.send(reply, remote.port, remote.address);
      } else {
        assert.equal(message.length, 98); assert.equal(action, 1); assert.deepEqual(message.subarray(16, 36), hash);
        const event = ['', 'completed', 'started', 'stopped'][message.readUInt32BE(80)];
        announces.push({ name: 'udp', event, left: Number(message.readBigUInt64BE(64)), time: Date.now() });
        const reply = Buffer.alloc(20); reply.writeUInt32BE(1); reply.writeUInt32BE(transaction, 4); reply.writeUInt32BE(60, 8);
        udp.send(Buffer.concat([reply, event === 'stopped' ? Buffer.alloc(0) : compact()]), remote.port, remote.address);
      }
    } catch (error) { protocolError = error; }
  });
  const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  async function until(check, description, timeout = 8000) {
    const deadline = Date.now() + timeout;
    while (!check()) {
      if (processError) throw processError; if (protocolError) throw protocolError;
      if (code !== undefined || Date.now() >= deadline) throw new Error(`${description}: ${output}`);
      await delay(10);
    }
  }
  async function command(text, marker) {
    child.stdin.write(text + '\n'); await until(() => lines.some(line => line.startsWith(marker)), marker);
    return lines.splice(lines.findIndex(line => line.startsWith(marker)), 1)[0];
  }
  const status = async () => JSON.parse((await command('status', 'STATUS ')).slice(7));
  async function waitStatus(check, description) {
    const deadline = Date.now() + 8000; let snapshot;
    do {
      snapshot = await status(); assert.equal(snapshot.error, '', JSON.stringify(snapshot));
      if (check(snapshot)) return snapshot; await delay(20);
    } while (Date.now() < deadline);
    throw new Error(`${description}: ${JSON.stringify(snapshot)}`);
  }
  try {
    await listen(seed); await listen(tracker); await new Promise(resolve => udp.bind(0, '127.0.0.1', resolve));
    const url = name => `http://127.0.0.1:${tracker.address().port}${name}`;
    const udpUrl = `udp://127.0.0.1:${udp.address().port}/announce`;
    const first = ['udp-timeout', 'cancel-udp'].includes(scenario) ? udpUrl : url('/first');
    const backup = scenario === 'udp-backup' ? udpUrl : url('/backup');
    const tiers = scenario === 'promotion' ? [[url('/a'), url('/b'), url('/c')], [url('/unused')]] :
      [[first], ['', first, backup], [url('/unused')]];
    if (scenario === 'all-fail') tiers.pop();
    const metadata = { info, 'announce-list': tiers }; if (scenario !== 'list-only') metadata.announce = url('/legacy');
    fs.writeFileSync(path.join(work, 'test.torrent'), encode(metadata));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ download_dir: './files', log_file: 'client.log',
      listen_port: await availablePort(), enable_dht: false, enable_lsd: false, enable_pex: false,
      enable_webseeds: false, enable_utp: false, enable_ipv6: false, sequential_download: true,
      tracker_timeout_seconds: ['timeout', 'udp-timeout'].includes(scenario) ? 1 : 10 }));
    child = spawn(executable, ['test.torrent', 'config.json'], { cwd: work, windowsHide: true });
    let partial = '';
    child.stdout.on('data', bytes => {
      output += bytes; partial += bytes; let end;
      while ((end = partial.indexOf('\n')) >= 0) { lines.push(partial.slice(0, end).trim()); partial = partial.slice(end + 1); }
    });
    child.stderr.on('data', bytes => { output += bytes; }); child.stdin.on('error', () => {});
    child.once('error', error => { processError = error; }); child.once('close', value => { code = value; });
    await until(() => lines.includes('READY'), 'ready');
    const initial = await status(); assert.equal(initial.tracker.total, scenario === 'all-fail' ? 2 : scenario === 'promotion' ? 4 : 3);
    assert.equal(initial.tracker.state, 'waiting');
    await command('start', 'STARTED');
    if (scenario === 'shutdown-race') {
      for (let iteration = 0; iteration < 32; ++iteration) {
        await waitStatus(s => s.port > 0, 'workers started');
        const start = Date.now(); await command('stop', 'STOPPED');
        assert.ok(Date.now() - start < 2500, 'Worker missed its shutdown wake-up');
        if (iteration < 31) await command('start', 'STARTED');
      }
    } else if (['cancel-http', 'cancel-udp'].includes(scenario)) {
      await until(() => announces.length > 0, 'pending announce');
      await waitStatus(s => s.tracker.state === 'announcing' && s.port > 0, 'responsive status during announce');
      const started = Date.now(); await command('stop', 'STOPPED');
      assert.ok(Date.now() - started < 3000, 'Cancellation waited for tracker timeout');
      assert.ok(!announces.some(a => a.name === '/backup'), 'Fallback continued after stop');
      assert.ok(!announces.some(a => a.event === 'stopped'), 'An unacknowledged tracker received stopped');
    } else if (scenario === 'all-fail') {
      const failed = await waitStatus(s => s.tracker.state === 'error' && s.tracker.failures === 2, 'all failed');
      assert.equal(failed.tracker.url, backup); assert.equal(failed.tracker.error, 'unavailable /backup');
      await delay(1200); assert.equal(announces.length, 2, 'Failed trackers retried without backoff');
      await command('stop', 'STOPPED'); assert.equal((await status()).tracker.error, failed.tracker.error);
    } else if (['promotion', 'empty-peers', 'stop-bounded'].includes(scenario)) {
      const working = await waitStatus(s => s.tracker.state === 'working', 'working empty tracker');
      assert.equal(working.tracker.peers, 0);
      if (scenario === 'promotion') {
        await until(() => announces.length >= 3, 'periodic announce');
        assert.equal(announces[2].name, chosen, 'Successful tracker was not promoted within its tier');
        assert.equal(announces[2].event, ''); assert.ok(announces[2].time - announces[1].time >= 900, 'Ignored tracker interval');
      } else assert.equal(working.tracker.url, first);
      const start = Date.now(); await command('stop', 'STOPPED');
      assert.ok(Date.now() - start < 3500, 'Stopped announce exceeded its shutdown budget');
      assert.ok(!announces.some(a => a.name === '/backup'), 'Empty success or shutdown contacted a backup');
    } else {
      if (scenario === 'mid-download') await waitStatus(s => s.progress === 25, 'first verified piece');
      const working = await waitStatus(s => s.tracker.state === 'working' && s.tracker.url === backup, 'backup active');
      assert.equal(working.tracker.error, ''); assert.equal(working.tracker.peers, 1);
      if (scenario !== 'mid-download') assert.equal(working.tracker.attempt, 2);
      if (scenario === 'recover') await waitStatus(s => s.tracker.state === 'working' && s.tracker.url === first, 'higher tier recovered');
      const completedAt = Date.now();
      full = true; for (const request of held.splice(0)) answer(request);
      await waitStatus(s => !s.busy && s.progress === 100, 'completed fallback download');
      assert.deepEqual(fs.readFileSync(path.join(work, 'files/payload.bin')), data);
      const events = announces.filter(a => a.name === (scenario === 'udp-backup' ? 'udp' : '/backup')).map(a => a.event);
      if (scenario === 'completion-timeout') {
        assert.ok(Date.now() - completedAt < 3500, 'Completion query blocked stopping the task');
        assert.equal(events.filter(event => event === 'completed').length, 1);
      } else assert.deepEqual(events, ['started', 'completed', 'stopped']);
      if (scenario === 'recover') {
        assert.equal(connections, 1, 'Tracker recovery duplicated an existing connection');
        assert.equal((await status()).tracker.url, first, 'Older tracker replaced active tracker diagnostics');
        assert.deepEqual(announces.filter(a => a.name === '/first').map(a => a.event), ['started', 'started', 'completed', 'stopped']);
      }
      if (scenario === 'mid-download') {
        assert.equal(connections, 1, 'Fallback duplicated an existing connection');
        assert.equal(requests.filter(r => r.index === 0).length, 1, 'Fallback discarded a verified piece');
        assert.deepEqual(announces.filter(a => a.name === '/first').map(a => a.event), ['started', '', 'completed', 'stopped']);
      }
    }
    const final = await status(); assert.equal(final.busy, false); assert.equal(final.peers.length, 0);
    child.stdin.end('quit\n'); await until(() => code !== undefined, 'exit'); assert.equal(code, 0, output);
    if (protocolError) throw protocolError;
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ scenario, passed: true, final, announces }, null, 2));
    console.log(`PASS tracker fallback ${scenario}: tracker order and session lifecycle verified.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    fs.writeFileSync(path.join(work, 'trace.json'), JSON.stringify({ scenario, announces,
      requests: requests.map(({ index, offset, size }) => ({ index, offset, size })) }, null, 2));
    for (const socket of sockets) socket.destroy();
    await Promise.all([seed, tracker].filter(s => s.listening).map(s => new Promise(resolve => s.close(resolve))));
    udp.close(); fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
