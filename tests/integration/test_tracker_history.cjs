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
function packet(id, body = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(body.length + 1); header[4] = id;
  return Buffer.concat([header, body]);
}

async function main() {
  const executable = path.resolve(process.argv[2]), scenario = process.argv[3];
  assert.ok(['fallback', 'manual', 'minimum', 'cancel-http', 'cancel-udp', 'history', 'lifecycle', 'reset', 'invalid'].includes(scenario));
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `tracker-history-${scenario}-`));
  const data = Buffer.alloc(50003); for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + 17) % 251;
  const pieces = []; for (let i = 0; i < data.length; i += 16384) pieces.push(sha1(data.subarray(i, i + 16384)));
  const info = { name: 'payload.bin', length: data.length, 'piece length': 16384, pieces: Buffer.concat(pieces) };
  const hash = sha1(encode(info)), sockets = new Set(), held = [], announces = [], datagrams = [], lines = [];
  let child, code, failure, output = '', release = false, connections = 0;
  const track = socket => { sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket)); return socket; };
  const seed = net.createServer(socket => {
    ++connections; track(socket); let pending = Buffer.alloc(0), greeted = false;
    socket.on('data', bytes => {
      try {
        pending = Buffer.concat([pending, bytes]);
        if (!greeted) {
          if (pending.length < 68) return;
          assert.deepEqual(pending.subarray(28, 48), hash); pending = pending.subarray(68); greeted = true;
          socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8), hash,
            Buffer.from('-HISTRY-123456789012'), packet(5, Buffer.from([0xf0])), packet(1)]));
        }
        while (pending.length >= 4) {
          const length = pending.readUInt32BE(); if (pending.length < length + 4) break;
          const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
          if (message[0] !== 6) continue;
          const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
          const start = index * 16384 + offset;
          assert.ok(index < 4 && size > 0 && size <= 16384 && start + size <= data.length);
          const reply = packet(7, Buffer.concat([message.subarray(1, 9), data.subarray(start, start + size)]));
          if (release) socket.write(reply); else held.push({ socket, reply });
        }
      } catch (error) { failure = error; socket.destroy(); }
    });
  });
  const tracker = http.createServer((request, response) => {
    const url = new URL(request.url, 'http://localhost'), event = url.searchParams.get('event') || 'update';
    announces.push({ path: url.pathname, event, time: Date.now() });
    if (url.pathname === '/unused') { failure = new Error('Unneeded backup was contacted'); response.writeHead(500); response.end(); return; }
    if (scenario === 'cancel-http') return;
    if (url.pathname === '/bad') {
      response.end(encode(scenario === 'invalid' ? { peers: Buffer.alloc(0), interval: 60, 'min interval': -7 }
        : { 'failure reason': 'a' + 'я'.repeat(600) })); return;
    }
    const ordinary = announces.filter(item => item.path === '/good' && !['completed', 'stopped'].includes(item.event)).length;
    const hasPeers = !['minimum', 'history', 'reset'].includes(scenario) && (scenario !== 'manual' || ordinary > 1);
    const compact = hasPeers ? Buffer.from([127, 0, 0, 1, seed.address().port >> 8, seed.address().port & 255]) : Buffer.alloc(0);
    response.end(encode({ peers: compact, interval: ['history', 'minimum'].includes(scenario) ? 1 : 60,
      'min interval': scenario === 'minimum' ? 3 : 1 }));
  });
  tracker.on('connection', track);
  const udp = dgram.createSocket('udp4');
  udp.on('message', (message, remote) => {
    try {
      const action = message.readUInt32BE(8), transaction = message.readUInt32BE(12);
      datagrams.push({ action, time: Date.now() });
      if (scenario !== 'minimum') return;
      const reply = Buffer.alloc(action === 0 ? 16 : 20); reply.writeUInt32BE(action); reply.writeUInt32BE(transaction, 4);
      if (action === 0) reply.writeBigUInt64BE(12345n, 8);
      else { assert.equal(action, 1); reply.writeUInt32BE(60, 8); }
      udp.send(reply, remote.port, remote.address);
    } catch (error) { failure = error; }
  });
  const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  async function until(check, description, timeout = 8000) {
    const end = Date.now() + timeout;
    do { if (failure) throw failure; if (check()) return; if (code !== undefined) throw new Error(output); await delay(10); } while (Date.now() < end);
    throw new Error(`${description}\n${output}`);
  }
  async function command(value, marker) {
    child.stdin.write(value + '\n'); await until(() => lines.some(line => line.startsWith(marker)), marker);
    return lines.splice(lines.findIndex(line => line.startsWith(marker)), 1)[0];
  }
  const status = async () => JSON.parse((await command('status', 'STATUS ')).slice(7));
  async function waitStatus(check, description, timeout = 8000) {
    const end = Date.now() + timeout; let state;
    do { state = await status(); assert.equal(state.error, ''); if (check(state)) return state; await delay(30); } while (Date.now() < end);
    throw new Error(`${description}: ${JSON.stringify(state)}`);
  }
  const updates = () => announces.filter(item => item.event !== 'stopped' && item.event !== 'completed');
  let final;
  try {
    await listen(seed); await listen(tracker); await new Promise(resolve => udp.bind(0, '127.0.0.1', resolve));
    const url = name => `http://127.0.0.1:${tracker.address().port}/${name}?passkey=fixture-secret`;
    const tiers = ['fallback', 'invalid'].includes(scenario) ? [[url('bad')], [url('good')], [url('unused')]]
      : [[scenario === 'cancel-udp' ? `udp://127.0.0.1:${udp.address().port}/announce` : url('good')]];
    fs.writeFileSync(path.join(work, 'test.torrent'), encode({ info, 'announce-list': tiers }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_pex: false, enable_lsd: false,
      enable_port_mapping: false, enable_webseeds: false, enable_utp: false, enable_encryption: false,
      download_dir: './files', listen_port: await availablePort(), tracker_timeout_seconds: 8,
      seed_after_download: true, seed_ratio_limit: 0, seed_time_limit_hours: 0 }));
    child = spawn(executable, ['test.torrent', 'config.json'], { cwd: work, windowsHide: true });
    child.on('error', error => { failure = error; }); child.on('close', value => { code = value; }); child.stdin.on('error', () => {});
    let pending = ''; for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => {
      output += bytes; if (stream !== child.stdout) return; pending += bytes;
      let end; while ((end = pending.indexOf('\n')) >= 0) { lines.push(pending.slice(0, end).trim()); pending = pending.slice(end + 1); }
    });
    await until(() => lines.includes('READY'), 'ready');
    let state = await status(); assert.equal(state.trackers.length, tiers.length);
    assert.ok(state.trackers.every(item => item.requests === 0 && item.history.length === 0));
    assert.equal(await command('update', 'UPDATED '), 'UPDATED 0'); assert.equal(announces.length, 0);
    await command('start', 'STARTED');
    if (scenario.startsWith('cancel-')) {
      await waitStatus(state => state.tracker.state === 'announcing' && state.trackers[0].requests === 1, 'pending request');
      const began = Date.now(); assert.equal(await command('update', 'UPDATED '), 'UPDATED 0'); await status();
      assert.ok(Date.now() - began < 500, 'Commands must not wait for tracker I/O');
      const stopped = Date.now(); await command('stop', 'STOPPED'); assert.ok(Date.now() - stopped < 2000);
      final = await status(); assert.equal(final.trackers[0].history.at(-1).result, 'cancelled');
      assert.equal(final.trackers[0].failures, 0); assert.equal(final.tracker.failures, 0);
      if (scenario === 'cancel-udp') assert.ok(datagrams.length > 0 && datagrams.every(item => item.action === 0));
    } else {
      state = await waitStatus(state => state.tracker.state === 'working' && state.trackers.some(item => item.history.length), 'first response');
      if (['fallback', 'invalid'].includes(scenario)) {
        const [bad, good, unused] = state.trackers;
        assert.deepEqual(state.trackers.map(item => item.tier), [1, 2, 3]);
        assert.equal(bad.state, 'error'); assert.equal(bad.failures, 1); assert.equal(bad.history[0].result, 'failure');
        assert.ok(Buffer.byteLength(bad.history[0].error) <= 1024);
        assert.ok(!bad.history[0].error.includes('\ufffd'), 'Truncation split a UTF-8 character');
        assert.equal(good.state, 'working'); assert.equal(good.peers, 1); assert.ok(good.last_response > 0);
        assert.equal(unused.state, 'waiting'); assert.equal(unused.requests, 0);
        assert.ok(!state.report.includes('fixture-secret'));
      } else if (scenario === 'manual') {
        assert.equal(connections, 0); assert.equal(await command('update', 'UPDATED '), 'UPDATED 0');
        await waitStatus(state => state.can_update, 'cooldown expiry');
        assert.equal(await command('update', 'UPDATED '), 'UPDATED 1');
        assert.equal(await command('update', 'UPDATED '), 'UPDATED 0');
        state = await waitStatus(state => state.trackers[0].history.length === 2, 'manual response');
        assert.deepEqual(state.trackers[0].history.map(item => item.event), ['started', 'update']);
        await until(() => connections > 0, 'new tracker peer'); assert.equal(updates().length, 2);
        assert.equal(await command('update', 'UPDATED '), 'UPDATED 0');
      } else if (scenario === 'minimum') {
        assert.ok(state.tracker.retry_in >= 2); assert.ok(state.tracker.next_in >= 2);
        await delay(1200); assert.equal(await command('update', 'UPDATED '), 'UPDATED 0'); assert.equal(updates().length, 1);
        await command('stop', 'STOPPED');
        fs.writeFileSync(path.join(work, 'test.torrent'), encode({ info, announce: `udp://127.0.0.1:${udp.address().port}/announce` }));
        await command('reload', 'RELOADED'); await command('start', 'STARTED');
        state = await waitStatus(state => state.tracker.state === 'working', 'UDP response');
        assert.ok(state.tracker.retry_in >= 59, 'UDP announce interval must also protect manual updates');
        assert.equal(await command('update', 'UPDATED '), 'UPDATED 0');
        assert.equal(datagrams.filter(item => item.action === 1).length, 1);
      } else if (scenario === 'history') {
        state = await waitStatus(state => state.trackers[0].requests >= 18 && state.tracker.state === 'working', 'bounded history', 26000);
        assert.equal(state.trackers[0].history.length, 16);
        assert.ok(state.trackers[0].history.every(item => item.event === 'update' && item.result === 'success'));
        const times = state.trackers[0].history.map(item => item.time); assert.deepEqual(times, [...times].sort((a, b) => a - b));
      } else if (scenario === 'lifecycle') {
        release = true; for (const item of held.splice(0)) item.socket.write(item.reply);
        state = await waitStatus(state => state.progress === 100 && state.trackers[0].history.some(item => item.event === 'completed'), 'completion history');
        assert.deepEqual(fs.readFileSync(path.join(work, 'files', 'payload.bin')), data);
      } else if (scenario === 'reset') {
        await command('stop', 'STOPPED'); state = await status(); assert.equal(state.trackers[0].history.length, 2);
        await command('start', 'STARTED');
        state = await waitStatus(state => state.tracker.state === 'working', 'new session');
        assert.equal(state.trackers[0].requests, 1); assert.equal(state.trackers[0].history.length, 1);
        await command('stop', 'STOPPED'); await command('reload', 'RELOADED');
        state = await status(); assert.equal(state.trackers[0].history.length, 0); assert.equal(state.trackers[0].requests, 0);
        assert.equal(await command('update', 'UPDATED '), 'UPDATED 0');
      }
      await command('stop', 'STOPPED'); final = await status();
      if (scenario === 'lifecycle') assert.deepEqual(final.trackers[0].history.map(item => item.event), ['started', 'completed', 'stopped']);
    }
    assert.equal(final.can_update, false); assert.equal(final.tracker.pending, false); assert.equal(final.tracker.next_in, 0);
    child.stdin.end('quit\n'); await until(() => code !== undefined, 'shutdown'); assert.equal(code, 0, output);
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ scenario, final, announces, datagrams, passed: true }, null, 2));
    console.log(`PASS tracker history ${scenario}: snapshots and actual requests verified.`);
  } finally {
    if (child && code === undefined) { child.stdin.end('quit\n'); try { await until(() => code !== undefined, 'cleanup', 2000); } catch { child.kill(); } }
    for (const socket of sockets) socket.destroy();
    tracker.closeAllConnections(); await new Promise(resolve => tracker.close(resolve));
    await new Promise(resolve => seed.close(resolve)); udp.close();
    fs.writeFileSync(path.join(work, 'engine-output.log'), output);
    fs.writeFileSync(path.join(work, 'announces.json'), JSON.stringify(announces, null, 2)); console.log(`Artifacts: ${work}`);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
