// Private loopback router fixtures. No discovery or mappings on the real gateway.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const dgram = require('node:dgram');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { availablePort } = require('./fixture_helpers.cjs');
const { encode, createDhtFixture } = require('./dht_fixture.cjs');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest();
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}
const xml = value => String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
const service = 'urn:schemas-upnp-org:service:WANIPConnection:2';
function envelope(body) { return `<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>${body}</s:Body></s:Envelope>`; }
function success(action, values = {}) {
  return envelope(`<n:${action}Response xmlns:n="${service}">${Object.entries(values).map(([key, value]) => `<${key}>${xml(value)}</${key}>`).join('')}</n:${action}Response>`);
}
function fault(response, code) {
  response.writeHead(500); response.end(envelope(`<s:Fault><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>${code}</errorCode></UPnPError></detail></s:Fault>`));
}

async function main() {
  const executable = path.resolve(process.argv[2]), scenario = process.argv[3];
  const scenarios = ['disabled', 'natpmp', 'udp', 'upnp', 'foreign', 'replaced', 'timeout', 'cancel', 'cancel-upnp',
    'toggle', 'concurrent', 'renew-failure', 'permanent', 'unsafe-url', 'partial', 'expiry', 'ui'];
  assert.ok(scenarios.includes(scenario), scenario);
  const upnp = ['upnp', 'foreign', 'replaced', 'cancel-upnp', 'permanent', 'unsafe-url', 'ui'].includes(scenario);
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `port-mapping-${scenario}-`));
  const sockets = new Set(), natRequests = [], soapRequests = [], discoveries = [], announces = [], held = [], lines = [];
  const rules = new Map(); let child, code, output = '', processError, protocolError, pendingCreate = false, failRenew = false, full = false, dht;
  let uiRelease, reservedDuringDelete = false;
  const torrents = [0, 1].map(id => {
    const data = Buffer.alloc(32768); for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + id) % 251;
    const info = { name: `${id}.bin`, length: data.length, 'piece length': 16384,
      pieces: Buffer.concat([sha1(data.subarray(0, 16384)), sha1(data.subarray(16384))]) };
    return { data, info, hash: sha1(encode(info)), connections: 0 };
  });
  function track(socket) { sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket)); return socket; }
  function answer({ socket, header, data }) { if (!socket.destroyed) socket.write(packet(7, Buffer.concat([header, data]))); }
  const seed = net.createServer(socket => {
    track(socket); let buffer = Buffer.alloc(0), torrent;
    socket.on('data', data => {
      try {
        buffer = Buffer.concat([buffer, data]);
        if (!torrent) {
          if (buffer.length < 68) return;
          torrent = torrents.find(t => t.hash.equals(buffer.subarray(28, 48))); assert.ok(torrent); ++torrent.connections;
          buffer = buffer.subarray(68);
          socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8), torrent.hash,
            Buffer.from('-ROUTER-123456789012'), packet(5, Buffer.from([0xc0])), packet(1)]));
        }
        while (buffer.length >= 4) {
          const length = buffer.readUInt32BE(); if (buffer.length < length + 4) break;
          const message = buffer.subarray(4, length + 4); buffer = buffer.subarray(length + 4);
          if (message[0] !== 6) continue;
          const piece = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
          assert.ok(piece < 2 && offset + size <= 16384);
          const block = { socket, header: message.subarray(1, 9), data: torrent.data.subarray(piece * 16384 + offset, piece * 16384 + offset + size) };
          if (full) answer(block); else held.push(block);
        }
      } catch (error) { protocolError = error; socket.destroy(); }
    });
  });
  const tracker = http.createServer((request, response) => {
    const query = new URL(request.url, 'http://localhost').searchParams;
    announces.push({ event: query.get('event') || '', port: Number(query.get('port')), time: Date.now() });
    const compact = Buffer.from([127, 0, 0, 1, seed.address().port >> 8, seed.address().port & 255]);
    response.end(encode({ interval: 60, peers: compact }));
  });
  const nat = dgram.createSocket('udp4');
  nat.on('message', (message, peer) => {
    try {
      assert.equal(message[0], 0); const opcode = message[1]; assert.ok([0, 1, 2].includes(opcode));
      const entry = { opcode, time: Date.now() };
      if (opcode) Object.assign(entry, { internal: message.readUInt16BE(4), external: message.readUInt16BE(6), lease: message.readUInt32BE(8) });
      natRequests.push(entry);
      if (scenario === 'timeout') return;
      if (scenario === 'expiry' && failRenew && opcode && entry.lease) return;
      const result = upnp ? 1 : (failRenew && opcode && entry.lease || scenario === 'partial' && opcode === 1 && entry.lease) ? 2 : 0;
      const response = Buffer.alloc(result ? 8 : opcode ? 16 : 12);
      response[1] = opcode + 128; response.writeUInt16BE(result, 2); response.writeUInt32BE(100, 4);
      if (!result && !opcode) Buffer.from([203, 0, 113, 7]).copy(response, 8);
      if (!result && opcode) {
        assert.equal(message.length, 12); assert.ok(entry.internal > 0, 'Wildcard mapping/deletion attempted');
        const key = `${opcode}:${entry.internal}`;
        const external = scenario === 'cancel' ? entry.internal : 40000 + entry.internal % 10000;
        response.writeUInt16BE(entry.internal, 8); response.writeUInt16BE(entry.lease ? external : 0, 10); response.writeUInt32BE(entry.lease ? 2 : 0, 12);
        if (entry.lease) {
          rules.set(key, { external, internal: entry.internal, transport: opcode === 2 ? 'TCP' : 'UDP' });
          if (scenario === 'cancel') { pendingCreate = true; return; }
        } else { assert.equal(entry.external, 0, 'NAT-PMP deletion must use zero suggested external port'); rules.delete(key); }
      }
      nat.send(response, peer.port, peer.address);
    } catch (error) { protocolError = error; }
  });
  const router = http.createServer((request, response) => {
    if (request.method === 'GET') {
      const control = scenario === 'unsafe-url' ? 'http://192.0.2.123/control' : 'control';
      response.end(`<root xmlns="urn:schemas-upnp-org:device-1-0"><device><deviceList><device><serviceList><service><serviceType>${service}</serviceType><controlURL>${control}</controlURL></service></serviceList></device></deviceList></device></root>`);
      return;
    }
    let body = ''; request.on('data', data => { body += data; }); request.on('end', async () => {
      try {
        const action = String(request.headers.soapaction).match(/#(\w+)"$/)?.[1]; assert.ok(action);
        assert.ok(body.includes(`<u:${action}`));
        const field = name => body.match(new RegExp(`<${name}>(.*?)</${name}>`, 's'))?.[1] || '';
        const entry = { action, time: Date.now(), external: Number(field('NewExternalPort')), internal: Number(field('NewInternalPort')),
          transport: field('NewProtocol'), owner: field('NewPortMappingDescription'), lease: Number(field('NewLeaseDuration')) };
        soapRequests.push(entry);
        if (action === 'GetExternalIPAddress') { response.end(success(action, { NewExternalIPAddress: '203.0.113.9' })); return; }
        const key = `${entry.transport}:${entry.external}`, existing = rules.get(key);
        if (action === 'GetSpecificPortMappingEntry') {
          if (!existing) { fault(response, 714); return; }
          response.end(success(action, { NewInternalClient: existing.client, NewInternalPort: existing.internal,
            NewPortMappingDescription: existing.owner, NewEnabled: 'true', NewLeaseDuration: 2 })); return;
        }
        if (action === 'AddPortMapping') {
          assert.ok(entry.internal > 0 && entry.external > 0 && entry.lease > 0, 'Permanent/wildcard creation attempted');
          assert.equal(field('NewInternalClient'), '127.0.0.1'); assert.match(entry.owner, /^TorrentClient-[a-f0-9]+$/);
          if (scenario === 'permanent') { fault(response, 725); return; }
          assert.ok(!existing || existing.owner === entry.owner, 'Foreign UPnP mapping overwritten');
          rules.set(key, { ...entry, client: '127.0.0.1' });
          if (scenario === 'ui' && !uiRelease) uiRelease = setTimeout(() => {
            full = true; for (const block of held.splice(0)) answer(block);
          }, 1500);
          if (scenario === 'cancel-upnp') { pendingCreate = true; return; }
          response.end(success(action)); return;
        }
        if (action === 'DeletePortMapping') {
          assert.ok(existing, 'Deleting an unknown rule'); assert.notEqual(existing.owner, 'foreign', 'Foreign rule deleted');
          if (scenario === 'upnp') {
            await new Promise((resolve, reject) => {
              const probe = track(net.createConnection(existing.internal, '127.0.0.1'));
              const timer = setTimeout(() => { probe.destroy(); reject(new Error('Local socket was released before router cleanup')); }, 500);
              probe.once('error', reject); probe.once('connect', () => { clearTimeout(timer); reservedDuringDelete = true; probe.destroy(); resolve(); });
            });
          }
          rules.delete(key); response.end(success(action)); return;
        }
        throw new Error(`Unexpected SOAP action ${action}`);
      } catch (error) { protocolError = error; response.writeHead(500); response.end(); }
    });
  });
  router.on('connection', track); tracker.on('connection', track);
  const ssdp = dgram.createSocket('udp4');
  ssdp.on('message', (message, peer) => {
    try {
      assert.match(message.toString(), /M-SEARCH \* HTTP\/1.1/); discoveries.push(Date.now());
      if (!upnp) return;
      ssdp.send(`HTTP/1.1 200 OK\r\nLOCATION: http://127.0.0.1:${router.address().port}/device.xml\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n`, peer.port, peer.address);
    } catch (error) { protocolError = error; }
  });
  const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const bind = socket => new Promise(resolve => socket.bind(0, '127.0.0.1', resolve));
  async function until(check, description, timeout = 9000) {
    const end = Date.now() + timeout;
    while (!check()) {
      if (protocolError) throw protocolError; if (processError) throw processError; if (dht) dht.check();
      if (code !== undefined || Date.now() > end) throw new Error(`${description}: ${output}`);
      await delay(15);
    }
  }
  async function command(text, marker = 'OK') {
    child.stdin.write(text + '\n'); await until(() => lines.some(line => line.startsWith(marker)), marker);
    return lines.splice(lines.findIndex(line => line.startsWith(marker)), 1)[0];
  }
  const status = async () => JSON.parse((await command('status', 'STATUS ')).slice(7));
  async function waitStatus(check, description) {
    let states; const end = Date.now() + 9000;
    do {
      states = await status(); for (const job of states) assert.equal(job.error, '', JSON.stringify(job));
      if (check(states)) return states; await delay(25);
    } while (Date.now() < end);
    throw new Error(`${description}: ${JSON.stringify(states)}`);
  }
  const stop = index => command(`stop ${index}`);
  const created = () => natRequests.filter(r => r.opcode && r.lease > 0);
  try {
    await listen(seed); await listen(tracker); await listen(router); await bind(nat); await bind(ssdp);
    const firstPort = await availablePort(scenario === 'concurrent' ? 2 : 1);
    if (scenario === 'foreign') rules.set(`TCP:${firstPort}`, { owner: 'foreign', client: '192.168.1.42', internal: 1234 });
    if (scenario === 'udp' || scenario === 'partial') dht = await createDhtFixture(torrents[0].hash, seed.address().port);
    for (let i = 0; i < 2; ++i) fs.writeFileSync(path.join(work, `${i}.torrent`), encode({ info: torrents[i].info,
      announce: `http://127.0.0.1:${tracker.address().port}/announce` }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_port_mapping: !['disabled', 'toggle', 'ui'].includes(scenario),
      listen_port: firstPort, enable_dht: !!dht, dht_port: firstPort + 1, dht_bootstrap_nodes: dht?.routers || [],
      enable_lsd: false, enable_pex: false, enable_ipv6: false, enable_utp: false, enable_webseeds: false, sequential_download: true }));
    fs.writeFileSync(path.join(work, 'options.json'), JSON.stringify({ gateway: '127.0.0.1', natpmp_port: nat.address().port,
      discovery_port: ssdp.address().port, request_timeout_ms: scenario.startsWith('cancel') || scenario === 'expiry' ? 3000 : 200, discovery_timeout_ms: 500,
      retry_delay_ms: 1000, lease: 2 }));
    const args = scenario === 'ui' ? ['--ui-test-port-mapping', path.join(work, '0.torrent'), path.join(work, 'files-0'),
      path.join(work, 'config.json'), String(nat.address().port), String(ssdp.address().port)] : ['config.json', 'options.json', '0.torrent'];
    if (scenario === 'concurrent') args.push('1.torrent');
    child = spawn(executable, args, { cwd: work, windowsHide: true }); let partial = '';
    child.stdout.on('data', bytes => {
      output += bytes; partial += bytes; let end;
      while ((end = partial.indexOf('\n')) >= 0) { lines.push(partial.slice(0, end).trim()); partial = partial.slice(end + 1); }
    });
    child.stderr.on('data', bytes => { output += bytes; }); child.stdin.on('error', () => {});
    child.once('error', error => { processError = error; }); child.once('close', result => { code = result; });
    if (scenario === 'ui') {
      await until(() => code !== undefined, 'native UI exit', 25000); assert.equal(code, 0, output);
      assert.equal(JSON.parse(fs.readFileSync(path.join(work, 'config.json'))).enable_port_mapping, true);
      assert.deepEqual(fs.readFileSync(path.join(work, 'files-0/0.bin')), torrents[0].data);
      assert.ok(soapRequests.some(r => r.action === 'AddPortMapping')); assert.ok(soapRequests.some(r => r.action === 'DeletePortMapping'));
      assert.equal(rules.size, 0); console.log('PASS port mapping ui: opt-in setting and router diagnosis verified.'); return;
    }
    await until(() => lines.includes('READY'), 'ready'); await command('start 0');
    if (scenario === 'concurrent') await command('start 1');
    await waitStatus(jobs => jobs[0].listener, 'local listener');
    if (scenario === 'disabled' || scenario === 'toggle') {
      await delay(450); assert.equal(natRequests.length + discoveries.length + soapRequests.length, 0, 'Disabled feature sent network traffic');
      if (scenario === 'toggle') {
        await command('mapping 1'); await waitStatus(jobs => jobs[0].mapping.state === 'active', 'live enable');
        await command('mapping 0'); await waitStatus(jobs => jobs[0].mapping.state === 'disabled', 'live disable');
        assert.equal(rules.size, 0); assert.equal(JSON.parse(fs.readFileSync(path.join(work, 'saved-settings.json'))).enable_port_mapping, false);
        await command('mapping 1'); await waitStatus(jobs => jobs[0].mapping.state === 'active', 'second enable');
      }
      await stop(0); assert.equal(rules.size, 0);
    } else if (scenario === 'timeout' || scenario === 'unsafe-url' || scenario === 'foreign' || scenario === 'permanent') {
      const states = await waitStatus(jobs => jobs[0].mapping.state === 'error', 'router failure');
      assert.ok(states[0].mapping.error); assert.equal(states[0].listener, true);
      if (scenario === 'foreign') assert.equal(soapRequests.filter(r => r.action === 'AddPortMapping').length, 0);
      if (scenario === 'unsafe-url') assert.equal(soapRequests.length, 0, 'Contacted a foreign control URL');
      if (scenario === 'timeout') {
        full = true; for (const block of held.splice(0)) answer(block);
        await waitStatus(jobs => !jobs[0].busy && jobs[0].progress === 100, 'download despite unsupported router');
        assert.deepEqual(fs.readFileSync(path.join(work, 'files-0/0.bin')), torrents[0].data);
      } else await stop(0);
      assert.equal(soapRequests.filter(r => r.action === 'DeletePortMapping').length, 0);
      assert.equal(rules.size, scenario === 'foreign' ? 1 : 0);
    } else if (scenario.startsWith('cancel')) {
      await until(() => pendingCreate, 'pending mapping creation');
      const started = Date.now(); await stop(0);
      assert.ok(Date.now() - started < 4000, 'Stop waited for the normal request timeout');
      assert.equal(rules.size, 0, 'Lost create acknowledgement left an owned rule behind');
    } else {
      let states = await waitStatus(jobs => jobs.every(job => job.mapping.state === (scenario === 'partial' ? 'partial' : 'active')), 'mappings active');
      assert.equal(states[0].mapping.protocol, upnp ? 'UPnP' : 'NAT-PMP');
      assert.match(states[0].report, /Доступность из интернета: не проверена/);
      if (scenario === 'replaced') {
        for (const rule of rules.values()) { rule.owner = 'foreign'; rule.client = '192.168.1.42'; }
        await waitStatus(jobs => jobs[0].mapping.state === 'error', 'replaced owner detected');
        await stop(0); assert.equal(soapRequests.filter(r => r.action === 'DeletePortMapping').length, 0); assert.equal(rules.size, 1);
      } else {
        const tcp = states[0].mapping.ports.find(p => p.transport === 'TCP');
        await until(() => announces.some(a => a.event === 'started' && a.port === tcp.external), 'external port announced');
        if (scenario === 'renew-failure' || scenario === 'expiry') {
          const changedAt = Date.now();
          failRenew = true; await waitStatus(jobs => jobs[0].mapping.state === 'error' && !jobs[0].mapping.ports[0].active, 'failed renewal');
          await until(() => announces.some(a => a.event === 'started' && a.port === states[0].port && a.time >= changedAt), 'local port restored after renewal failure');
        } else if (scenario === 'upnp') await until(() => soapRequests.filter(r => r.action === 'AddPortMapping').length >= 2, 'UPnP renewal');
        else if (scenario !== 'concurrent') await until(() => created().filter(r => r.opcode === 2).length >= 2, 'NAT-PMP renewal');
        if (scenario === 'udp') {
          assert.equal(states[0].mapping.ports.filter(p => p.active).length, 2);
          assert.ok(created().some(r => r.opcode === 1 && r.internal === firstPort + 1), 'DHT UDP port was not mapped');
          await until(() => dht.announces.includes(tcp.external), 'mapped TCP port announced through DHT');
        }
        if (scenario === 'partial') {
          assert.ok(states[0].mapping.ports.find(p => p.transport === 'TCP').active);
          assert.equal(states[0].mapping.ports.find(p => p.transport === 'UDP').active, false);
        }
        await stop(0);
        if (scenario === 'concurrent') {
          assert.equal(rules.size, 1, 'Stopping one torrent removed another torrent mapping');
          states = await status(); assert.equal(states[1].mapping.state, 'active'); await stop(1);
        }
        assert.equal(rules.size, 0);
        if (scenario === 'upnp') assert.equal(reservedDuringDelete, true);
        if (scenario === 'natpmp') assert.equal(announces.at(-1).port, tcp.external, 'Stopped announce used a different port');
      }
    }
    await command('restore');
    const count = natRequests.length + discoveries.length + soapRequests.length;
    await delay(250); assert.equal(natRequests.length + discoveries.length + soapRequests.length, count, 'Restoring idle entries created mappings');
    const final = await status(); assert.ok(final.every(job => !job.busy));
    child.stdin.end('quit\n'); await until(() => code !== undefined, 'exit'); assert.equal(code, 0, output);
    if (protocolError) throw protocolError;
    console.log(`PASS port mapping ${scenario}: router protocol and lease ownership verified.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    if (uiRelease) clearTimeout(uiRelease);
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ scenario, natRequests, soapRequests, discoveries, announces, rules: [...rules], output }, null, 2));
    for (const socket of sockets) socket.destroy();
    await Promise.all([seed, tracker, router].filter(s => s.listening).map(s => new Promise(resolve => s.close(resolve))));
    nat.close(); ssdp.close(); if (dht) await dht.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
