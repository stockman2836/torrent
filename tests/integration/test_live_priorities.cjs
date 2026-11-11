const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const http = require('node:http');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const { encode } = require('./dht_fixture.cjs');
const { availablePort } = require('./fixture_helpers.cjs');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha1 = data => crypto.createHash('sha1').update(data).digest();
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}
async function main() {
  const executable = path.resolve(process.argv[2]), scenario = process.argv[3];
  assert.ok(['order', 'skip', 'boundary', 'ownership', 'allskip', 'throttled', 'ui'].includes(scenario));
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `live-priorities-${scenario}-`));
  const pieceLength = ['skip', 'allskip', 'ownership', 'throttled'].includes(scenario) ? 32768 : 16384;
  const layouts = scenario === 'boundary' ? [[8000, 32000, 58304, 0]] : scenario === 'ownership'
    ? [[65536, 65536], [65536, 65536]] : scenario === 'allskip' ? [[131072]]
    : [Array(3).fill(pieceLength * 2)];
  const torrents = layouts.map((lengths, id) => {
    const data = Buffer.alloc(lengths.reduce((sum, length) => sum + length, 0));
    for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + id * 19) % 251;
    const names = scenario === 'ownership' ? ['shared.bin', id ? 'other.bin' : 'keep.bin'] : lengths.map((_, index) => `file${index}.bin`);
    const info = { name: 'payload', 'piece length': pieceLength,
      pieces: Buffer.concat(Array.from({ length: Math.ceil(data.length / pieceLength) }, (_, i) => sha1(data.subarray(i * pieceLength, (i + 1) * pieceLength)))),
      files: lengths.map((length, index) => ({ length, path: [names[index]] })) };
    return { id, lengths, names, data, info, hash: sha1(encode(info)) };
  });
  const sockets = new Set(), requests = [], cancels = [], announces = [], sessions = [], timers = new Set();
  const peers = []; let child, code, output = '', partial = '', processError, protocolError, phase = 'initial', releaseOwnership = false;
  const lines = [];
  const file = (index, torrent = 0) => path.join(work, 'files', 'payload', torrents[torrent].names[index]);
  const expected = (index, torrent = 0) => {
    const item = torrents[torrent], offset = item.lengths.slice(0, index).reduce((sum, length) => sum + length, 0);
    return item.data.subarray(offset, offset + item.lengths[index]);
  };
  function answer(request, late = false) {
    if (request.answered || (request.canceled && !late) || request.socket.destroyed) return;
    request.answered = true;
    const start = request.index * pieceLength + request.offset;
    request.socket.write(packet(7, Buffer.concat([request.header, torrents[request.torrent].data.subarray(start, start + request.length)])));
  }
  function seed() {
    const server = net.createServer(socket => {
      sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket));
      const session = sessions.length; sessions.push({ socket });
      let pending = Buffer.alloc(0), greeted = false, torrent;
      socket.on('data', bytes => {
        try {
          pending = Buffer.concat([pending, bytes]);
          if (!greeted) {
            if (pending.length < 68) return;
            torrent = torrents.find(item => item.hash.equals(pending.subarray(28, 48)));
            assert.ok(torrent); pending = pending.subarray(68); greeted = true;
            const bitfield = Buffer.alloc(Math.ceil(torrent.info.pieces.length / 20 / 8));
            for (let i = 0; i < torrent.info.pieces.length / 20; ++i) bitfield[i >> 3] |= 0x80 >> (i & 7);
            socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8), torrent.hash,
              Buffer.from('-LIVEPR-123456789012'), packet(5, bitfield), packet(1)]));
          }
          while (pending.length >= 4) {
            const length = pending.readUInt32BE(); if (pending.length < length + 4) break;
            const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
            if (message[0] !== 6 && message[0] !== 8) continue;
            const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
            assert.ok(index * pieceLength + offset + size <= torrent.data.length);
            if (message[0] === 8) {
              cancels.push({ session, index, offset });
              for (const request of requests) if (request.session === session && request.index === index && request.offset === offset && !request.answered)
                request.canceled = true;
              continue;
            }
            const request = { session, torrent: torrent.id, index, offset, length: size, header: message.subarray(1, 9),
              socket, phase, answered: false, canceled: false };
            requests.push(request);
            if (scenario === 'allskip' && (phase === 'resumed' || index !== 3)) answer(request);
            if (scenario === 'ownership' && releaseOwnership && torrent.id === 1) answer(request);
            if (scenario === 'ui') {
              if (index === 1) {
                const timer = setTimeout(() => { timers.delete(timer); answer(request); }, 1200); timers.add(timer);
              } else answer(request);
            }
          }
        } catch (error) { protocolError = error; socket.destroy(); }
      });
    });
    peers.push(server); return server;
  }
  const tracker = http.createServer((request, response) => {
    const url = new URL(request.url, 'http://localhost'), torrent = Number(url.pathname.slice(1));
    announces.push({ torrent, event: url.searchParams.get('event'), left: Number(url.searchParams.get('left')), phase });
    const compact = peers.map(server => {
      const value = Buffer.from([127, 0, 0, 1, 0, 0]); value.writeUInt16BE(server.address().port, 4); return value;
    });
    response.end(encode({ interval: 60, peers: url.searchParams.get('event') === 'stopped' ? Buffer.alloc(0) : Buffer.concat(compact) }));
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
  async function command(value, marker = 'OK') {
    child.stdin.write(value + '\n'); await until(() => lines.some(line => line.startsWith(marker)), marker);
    return lines.splice(lines.findIndex(line => line.startsWith(marker)), 1)[0];
  }
  const status = async () => JSON.parse((await command('status', 'STATUS ')).slice(7));
  async function waitStatus(check, description) {
    const deadline = Date.now() + 8000; let states;
    do {
      states = await status(); states.forEach(state => assert.equal(state.error, '', JSON.stringify(state)));
      if (check(states)) return states;
      await delay(20);
    } while (Date.now() < deadline);
    throw new Error(`${description}: ${JSON.stringify(states)}`);
  }
  async function priorities(values, index = 0) {
    await command(`priority ${index} ${values.join(' ')}`);
    return waitStatus(states => !states[index].pending && JSON.stringify(states[index].priorities) === JSON.stringify(values), 'applied priorities');
  }
  const active = (index, torrent = 0) => requests.filter(request => request.phase === phase && request.torrent === torrent && request.index === index && !request.answered && !request.canceled);
  async function piece(index, torrent = 0) {
    const size = Math.min(pieceLength, torrents[torrent].data.length - index * pieceLength);
    await until(() => active(index, torrent).length >= Math.ceil(size / 16384), `piece ${index} requests`);
    return active(index, torrent);
  }
  const reply = async (index, torrent = 0) => (await piece(index, torrent)).forEach(request => answer(request));
  try {
    await listen(seed()); if (scenario === 'allskip') await listen(seed()); await listen(tracker);
    for (const torrent of torrents) fs.writeFileSync(path.join(work, `${torrent.id}.torrent`), encode({ info: torrent.info,
      announce: `http://127.0.0.1:${tracker.address().port}/${torrent.id}` }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ download_dir: './files', listen_port: await availablePort(4),
      enable_dht: false, enable_pex: false, enable_lsd: false, enable_utp: false, enable_webseeds: false, enable_ipv6: false,
      sequential_download: true, max_peers: 8, max_download_speed: scenario === 'throttled' ? 1 : 0 }));
    const args = scenario === 'ui' ? ['--ui-test-priorities', path.join(work, '0.torrent'), path.join(work, 'files'), path.join(work, 'config.json')]
      : ['config.json', ...torrents.map(torrent => `${torrent.id}.torrent`)];
    child = spawn(executable, args, { cwd: work, windowsHide: true });
    child.stdout.on('data', bytes => {
      output += bytes; partial += bytes; let end;
      while ((end = partial.indexOf('\n')) >= 0) { lines.push(partial.slice(0, end).trim()); partial = partial.slice(end + 1); }
    });
    child.stderr.on('data', bytes => { output += bytes; }); child.stdin.on('error', () => {});
    child.on('error', error => { processError = error; }); child.on('close', result => { code = result; });
    if (scenario === 'ui') {
      await until(() => code !== undefined, 'native window transfer', 30000); assert.equal(code, 0, output);
      assert.deepEqual(requests.map(request => request.index), [0, 1, 4, 5]);
      assert.deepEqual(fs.readFileSync(file(0)), expected(0)); assert.deepEqual(fs.readFileSync(file(2)), expected(2));
      assert.deepEqual(fs.readFileSync(file(1)), Buffer.alloc(expected(1).length));
    } else {
      await until(() => lines.includes('READY'), 'driver ready');
      if (scenario === 'boundary') await priorities([2, 0, 2, 0]);
      if (scenario === 'ownership') { await priorities([0, 2]); await priorities([2, 0], 1); }
      await command('start 0');
      if (scenario === 'order') {
        await reply(0); await piece(1);
        await command('priority 0 2 2 2 2', 'REJECTED');
        assert.deepEqual((await status())[0].priorities, [2, 2, 2]);
        await priorities([1, 2, 3]); await command('save');
        assert.equal(JSON.parse(fs.readFileSync(path.join(work, 'list.json'))).downloads[0].priorities.find(item => item.index === 2).priority, 'high');
        await reply(1); await reply(4); await reply(5); await reply(2); await reply(3);
        await waitStatus(states => !states[0].busy && states[0].progress === 100, 'priority completion');
        assert.deepEqual(requests.map(request => request.index), [0, 1, 4, 5, 2, 3]);
        assert.equal(sessions.length, 1, 'Changing priority restarted the peer session');
      } else if (scenario === 'skip') {
        await reply(0); const delayed = await piece(1);
        await priorities([0, 3, 1]);
        await until(() => cancels.filter(item => item.index === 1).length === 2, 'wire CANCEL for skipped piece');
        delayed.forEach(request => answer(request, true)); await delay(150);
        const stoppedFile = fs.readFileSync(file(0));
        assert.deepEqual(stoppedFile.subarray(0, pieceLength), expected(0).subarray(0, pieceLength));
        assert.deepEqual(stoppedFile.subarray(pieceLength), Buffer.alloc(pieceLength));
        await reply(2); await piece(3);
        assert.deepEqual(fs.readFileSync(file(0)).subarray(pieceLength), Buffer.alloc(pieceLength));
        await priorities([2, 3, 1]);
        await reply(3); await reply(1); await reply(4); await reply(5);
        await waitStatus(states => !states[0].busy && states[0].progress === 100, 're-enabled completion');
        assert.equal(requests.filter(request => request.index === 0).length, 2, 'A verified piece was requested again');
      } else if (scenario === 'boundary') {
        await reply(0); await piece(2); assert.ok(!fs.existsSync(file(1)));
        await priorities([2, 3, 2, 2]);
        assert.deepEqual(fs.readFileSync(file(1)).subarray(0, 8384), expected(1).subarray(0, 8384));
        assert.equal(fs.statSync(file(3)).size, 0);
        await reply(2); await piece(1); await priorities([0, 2, 2, 2]); await reply(1);
        await waitStatus(states => states[0].progress === 50, 'three verified boundary pieces');
        await command('stop 0'); await command('restore');
        const restored = await status();
        assert.ok(restored.every(state => !state.busy && !state.queued));
        assert.deepEqual(restored[0].priorities, [0, 2, 2, 2]);
        phase = 'resumed'; await command('start 0');
        await reply(3); await reply(4); await reply(5);
        await waitStatus(states => !states[0].busy && states[0].progress === 100, 'boundary resume');
        assert.ok(!requests.some(request => request.phase === phase && request.index < 3), 'Cached verified pieces were downloaded again');
        assert.deepEqual(fs.readFileSync(file(0)), expected(0));
      } else if (scenario === 'throttled') {
        const delayed = await piece(0);
        answer(delayed[0]); await delay(250); // The complete wire block waits in the download limiter.
        await priorities([0, 3, 2]);
        await until(() => cancels.some(item => item.index === 0), 'cancel throttled excluded data', 2000);
        await piece(2); // A new piece must be requested before removing the 1 B/s limit.
        await command('limits 0 0 0');
        await reply(2); await reply(3); await reply(4); await reply(5);
        await waitStatus(states => !states[0].busy && states[0].progress === 100, 'throttled selection completion');
        assert.deepEqual(fs.readFileSync(file(0)), Buffer.alloc(expected(0).length));
      } else if (scenario === 'ownership') {
        await piece(2); await command('start 1'); await piece(0, 1);
        await command('priority 0 2 2', 'REJECTED');
        assert.deepEqual((await status())[0].priorities, [0, 2]);
        await command('stop 1'); await priorities([2, 2]);
        await command('start 1');
        await waitStatus(states => states[1].queued && !states[1].busy, 'conflicting queued torrent');
        await priorities([0, 2]);
        const waiting = await status(); assert.equal(waiting[1].queued, true);
        assert.equal(announces.filter(item => item.torrent === 1 && item.event === 'started').length, 1);
        releaseOwnership = true; await command('stop 0');
        await waitStatus(states => !states[1].busy && states[1].progress === 100, 'released file ownership');
        assert.deepEqual(fs.readFileSync(file(0, 1)), expected(0, 1));
      } else {
        await until(() => new Set(requests.filter(request => request.index === 3).map(request => request.session)).size === 2, 'endgame duplicate requests');
        await waitStatus(states => states[0].progress === 75, 'verified pieces before skip all');
        await priorities([0]);
        await waitStatus(states => !states[0].busy && states[0].progress === 100, 'skip-all completion');
        assert.equal(new Set(cancels.filter(item => item.index === 3).map(item => item.session)).size, 2);
        assert.deepEqual(fs.readFileSync(file(0)).subarray(3 * pieceLength), Buffer.alloc(pieceLength));
        await priorities([2]); phase = 'resumed'; await command('start 0');
        await waitStatus(states => !states[0].busy && states[0].progress === 100, 'resume after skip all');
        assert.ok(!requests.some(request => request.phase === phase && request.index !== 3), 'Verified pieces were requested again');
      }
      if (scenario !== 'ownership') for (let i = scenario === 'throttled' ? 1 : 0; i < torrents[0].lengths.length; ++i)
        assert.deepEqual(fs.readFileSync(file(i)), expected(i));
      child.stdin.end('quit\n'); await until(() => code !== undefined, 'driver exit'); assert.equal(code, 0, output);
    }
    if (protocolError) throw protocolError;
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ scenario, passed: true,
      requests: requests.map(({ session, torrent, index, offset, phase }) => ({ session, torrent, index, offset, phase })), cancels, announces }, null, 2));
    console.log(`PASS live priorities ${scenario}: wire order/cancellation and saved payloads verified.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const timer of timers) clearTimeout(timer);
    for (const socket of sockets) socket.destroy();
    await Promise.all([...peers, tracker].filter(server => server.listening).map(server => new Promise(resolve => server.close(resolve))));
    fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
