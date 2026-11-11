// Opt-in interoperability checks against an independently installed qBittorrent.
// All profiles, torrents and payloads are disposable artifacts under build/tests.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { availablePort } = require('./fixture_helpers.cjs');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest();
function encode(value) {
  if (Buffer.isBuffer(value)) return Buffer.concat([Buffer.from(`${value.length}:`), value]);
  if (typeof value === 'string') return encode(Buffer.from(value));
  if (typeof value === 'number') return Buffer.from(`i${value}e`);
  if (Array.isArray(value)) return Buffer.concat([Buffer.from('l'), ...value.map(encode), Buffer.from('e')]);
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(key => [encode(key), encode(value[key])]), Buffer.from('e')]);
}
async function until(check, description, timeout = 30000) {
  const deadline = Date.now() + timeout;
  do {
    const result = await check();
    if (result) return result;
    await delay(50);
  } while (Date.now() < deadline);
  throw new Error(`Timed out waiting for ${description}`);
}
function run(program, args, cwd, options = {}) {
  const child = spawn(program, args, { cwd, windowsHide: true, ...options });
  child.output = ''; child.code = undefined; child.error = undefined;
  for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => { child.output += bytes; });
  child.on('error', error => { child.error = error; });
  child.stdin.on('error', () => {});
  child.on('close', code => { child.code = code; });
  return child;
}
async function exited(child, description) {
  await until(() => child.code !== undefined || child.error, description);
  if (child.error) throw child.error;
  assert.equal(child.code, 0, child.output);
}
function request(port, endpoint, fields, raw, contentType) {
  const body = raw || (fields ? Buffer.from(new URLSearchParams(fields).toString()) : undefined);
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, path: `/api/v2/${endpoint}`,
      method: body ? 'POST' : 'GET', headers: { Referer: `http://127.0.0.1:${port}/`,
        ...(body ? { 'Content-Type': contentType || 'application/x-www-form-urlencoded', 'Content-Length': body.length } : {}) }
    }, response => {
      const bytes = []; response.on('data', chunk => bytes.push(chunk));
      response.on('end', () => response.statusCode === 200 ? resolve(Buffer.concat(bytes).toString())
        : reject(new Error(`qBittorrent ${endpoint}: HTTP ${response.statusCode}`)));
      response.on('error', reject);
    });
    req.setTimeout(2000, () => req.destroy(new Error(`qBittorrent ${endpoint} timed out`)));
    req.on('error', reject); req.end(body);
  });
}

async function main() {
  assert.equal(process.platform, 'win32', 'This fixture currently supports Windows qBittorrent 4.6.x');
  const [driverArg, cliArg, qbtArg, mode = 'download'] = process.argv.slice(2);
  assert.ok(driverArg && cliArg && qbtArg, 'Usage: test_interop.cjs <incoming_driver> <torrent_client> <qbittorrent> <scenario>');
  assert.ok(['download', 'upload-incoming', 'upload-outgoing', 'magnet', 'resume', 'dht'].includes(mode), 'Unknown scenario');
  const driver = path.resolve(driverArg), cli = path.resolve(cliArg), qbt = path.resolve(qbtArg);
  for (const executable of [driver, cli, qbt]) assert.ok(fs.existsSync(executable), `Missing executable: ${executable}`);
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `interop-${mode}-`));
  const profile = path.join(work, 'profile'), qbtFiles = path.join(work, 'qbt-files'), files = path.join(work, 'files');
  const ports = await availablePort(3), qbtPort = ports, webPort = ports + 2, enginePort = ports + 4;
  const uploads = mode.startsWith('upload');
  const payload = Buffer.alloc(1024 * 1024 + 137);
  for (let i = 0; i < payload.length; ++i) payload[i] = (i * 37 + (i >>> 8) * 13 + 11) % 256;
  const pieceLength = 32768, hashes = [];
  for (let offset = 0; offset < payload.length; offset += pieceLength) hashes.push(sha1(payload.subarray(offset, offset + pieceLength)));
  const parts = [{ path: ['alpha.bin'], length: 17003 }, { path: ['nested', 'beta.bin'], length: payload.length - 17003 }];
  const info = { files: parts, name: 'fixture', 'piece length': pieceLength, pieces: Buffer.concat(hashes) };
  const hash = sha1(encode(info)).toString('hex');
  const writePayload = root => {
    let offset = 0;
    for (const part of parts) {
      const target = path.join(root, 'fixture', ...part.path); fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.writeFileSync(target, payload.subarray(offset, offset + part.length)); offset += part.length;
    }
  };
  const verifyPayload = root => {
    const actual = Buffer.concat(parts.map(part => fs.readFileSync(path.join(root, 'fixture', ...part.path))));
    assert.deepEqual(actual, payload);
    for (let i = 0; i < hashes.length; ++i) assert.deepEqual(sha1(actual.subarray(i * pieceLength, (i + 1) * pieceLength)), hashes[i]);
  };
  writePayload(uploads ? files : qbtFiles);
  const announces = [];
  const tracker = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    const port = Number(url.searchParams.get('port'));
    const item = Object.fromEntries(url.searchParams); announces.push(item);
    const outgoing = mode !== 'upload-incoming';
    const target = outgoing ? (port === enginePort ? qbtPort : 0) : (port === qbtPort ? enginePort : 0);
    const peers = target ? Buffer.from([127, 0, 0, 1, target >> 8, target & 255]) : Buffer.alloc(0);
    res.end(encode({ interval: 1, 'min interval': 1, complete: 1, incomplete: 1, peers }));
  });
  await new Promise(resolve => tracker.listen(0, '127.0.0.1', resolve));
  const announce = `http://127.0.0.1:${tracker.address().port}/announce`;
  const torrentBytes = encode({ announce, info });
  fs.writeFileSync(path.join(work, 'fixture.torrent'), torrentBytes);
  const referenceDht = mode === 'dht' ? await require('./reference_dht.cjs').createReferenceDht(Buffer.from(hash, 'hex'), qbtPort) : undefined;
  const config = { enable_dht: mode === 'dht', dht_port: 0, dht_bootstrap_nodes: referenceDht?.routers || [],
    enable_pex: false, enable_lsd: false, enable_webseeds: false,
    enable_utp: false, enable_encryption: false, listen_port: enginePort, download_dir: './files', log_file: './client.log',
    max_download_speed: mode === 'resume' ? 65536 : 0, seed_after_download: uploads };
  fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify(config));
  const configDir = path.join(profile, 'qBittorrent', 'config'); fs.mkdirSync(configDir, { recursive: true });
  const salt = crypto.randomBytes(16);
  const passwordHash = crypto.pbkdf2Sync(crypto.randomBytes(32), salt, 100000, 64, 'sha512');
  fs.writeFileSync(path.join(configDir, 'qBittorrent.ini'), String.raw`[LegalNotice]
Accepted=true
[Preferences]
WebUI\Enabled=true
WebUI\Address=127.0.0.1
WebUI\Port=${webPort}
WebUI\LocalHostAuth=false
WebUI\Username=interop
WebUI\Password_PBKDF2="@ByteArray(${salt.toString('base64')}:${passwordHash.toString('base64')})"
WebUI\UseUPnP=false
Connection\UPnP=false
Connection\ResolvePeerCountries=false
General\CheckForProgramUpdates=false
General\ShowSplashScreen=false
General\StartMinimized=true
Win32\NeverCheckFileAssocation=true
[BitTorrent]
Session\DHTEnabled=false
Session\PeXEnabled=false
Session\LSDEnabled=false
Session\Port=${qbtPort}
Session\UPnP=false
Session\BTProtocol=TCP
Session\Encryption=2
Session\InterfaceAddress=127.0.0.1
[Network]
PortForwardingEnabled=false
`);
  let qbtProcess, engine, qbtPid, version, buildInfo;
  const engines = [];
  const startEngine = (program, args) => { engine = run(program, args, work); engines.push(engine); return engine; };
  const api = (endpoint, fields) => request(webPort, endpoint, fields);
  const torrentState = async () => JSON.parse(await api(`torrents/info?hashes=${hash}`))[0];
  try {
    // Start-Process hides the GUI window; windowsHide alone only hides consoles.
    const command = [
      '$qbtArgs = @("--profile=`"$env:TORRENT_TEST_QBT_PROFILE`"", "--webui-port=$env:TORRENT_TEST_QBT_WEB", "--torrenting-port=$env:TORRENT_TEST_QBT_PORT", "--no-splash")',
      '$qbtProcess = Start-Process -FilePath $env:TORRENT_TEST_QBT_EXE -ArgumentList $qbtArgs -WindowStyle Hidden -PassThru',
      'Write-Output "QBT_PID $($qbtProcess.Id)"',
      '$qbtProcess.WaitForExit()', 'exit $qbtProcess.ExitCode'
    ].join('\n');
    qbtProcess = run('powershell.exe', ['-NoProfile', '-Command', command], work, { env: { ...process.env,
      TORRENT_TEST_QBT_EXE: qbt, TORRENT_TEST_QBT_PROFILE: profile, TORRENT_TEST_QBT_WEB: String(webPort), TORRENT_TEST_QBT_PORT: String(qbtPort) } });
    qbtProcess.stdout.on('data', () => { const match = qbtProcess.output.match(/QBT_PID (\d+)/); if (match) qbtPid = Number(match[1]); });
    await until(async () => {
      if (qbtProcess.error || qbtProcess.code !== undefined) throw new Error(`qBittorrent exited: ${qbtProcess.error || qbtProcess.output}`);
      try { version = await api('app/version'); return true; } catch { return false; }
    }, 'isolated qBittorrent API', 30000);
    buildInfo = JSON.parse(await api('app/buildInfo'));
    assert.match(version, /^v4\.6\./, 'This fixture currently supports qBittorrent 4.6.x');
    assert.deepEqual(JSON.parse(await api('torrents/info')), [], 'Test profile must start empty');
    await api('app/setPreferences', { json: JSON.stringify({ dht: false, pex: false, lsd: false, upnp: false,
      random_port: false, listen_port: qbtPort, encryption: 2, bittorrent_protocol: 1, queueing_enabled: false,
      resolve_peer_countries: false, enable_multi_connections_from_same_ip: true,
      current_network_interface: '', current_interface_address: '127.0.0.1' }) });
    const boundary = `interop-${crypto.randomBytes(8).toString('hex')}`;
    const body = Buffer.concat([
      Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="torrents"; filename="fixture.torrent"\r\nContent-Type: application/x-bittorrent\r\n\r\n`), torrentBytes,
      Buffer.from(`\r\n--${boundary}\r\nContent-Disposition: form-data; name="savepath"\r\n\r\n${qbtFiles}\r\n--${boundary}--\r\n`)
    ]);
    assert.equal((await request(webPort, 'torrents/add', undefined, body, `multipart/form-data; boundary=${boundary}`)).trim(), 'Ok.');
    await until(async () => {
      const state = await torrentState();
      const readyStates = uploads ? ['downloading', 'stalledDL', 'forcedDL'] : ['uploading', 'stalledUP', 'forcedUP'];
      return state && readyStates.includes(state.state) && (uploads || state.progress === 1);
    }, 'qBittorrent hash check');
    await until(() => announces.some(item => Number(item.port) === qbtPort && item.event === 'started'), 'qBittorrent first announce');
    fs.writeFileSync(path.join(work, 'state-before.json'), JSON.stringify(await torrentState(), null, 2));
    if (uploads) {
      startEngine(driver, ['fixture.torrent', 'config.json']);
      await until(() => {
        if (engine.code !== undefined) throw new Error(engine.output);
        return engine.output.includes('INCOMING_READY');
      }, 'seeding session');
      if (mode === 'upload-incoming') await api('torrents/addPeers', { hashes: hash, peers: `127.0.0.1:${enginePort}` });
      await until(async () => (await torrentState())?.progress === 1, 'qBittorrent download');
      await api('torrents/pause', { hashes: hash });
      verifyPayload(qbtFiles);
      engine.stdin.end('stop\n'); await exited(engine, 'seeding shutdown');
    } else if (mode === 'resume') {
      startEngine(driver, ['fixture.torrent', 'config.json']);
      await until(async () => (await torrentState())?.uploaded > pieceLength * 2, 'partial transfer');
      engine.stdin.end('stop\n'); await exited(engine, 'partial session shutdown');
      assert.ok(fs.existsSync(path.join(files, `.resume_${hash.slice(0, 16)}.dat`)), 'Resume state must be saved');
      const partialAnnounce = announces.filter(item => Number(item.port) === enginePort && item.event === 'stopped').at(-1);
      assert.ok(partialAnnounce && Number(partialAnnounce.left) > 0 && Number(partialAnnounce.left) < payload.length,
        'Stopped session must contain verified pieces and missing pieces');
      config.max_download_speed = 0; fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify(config));
      startEngine(cli, ['fixture.torrent', '--config', 'config.json']);
      await exited(engine, 'resumed download'); verifyPayload(files);
      const starts = announces.filter(item => Number(item.port) === enginePort && item.event === 'started');
      assert.equal(starts.length, 2); assert.equal(starts[1].left, partialAnnounce.left, 'Restart must reuse verified pieces');
    } else {
      const source = mode === 'dht' ? `magnet:?xt=urn:btih:${hash}`
        : mode === 'magnet' ? `magnet:?xt=urn:btih:${hash}&tr=${encodeURIComponent(announce)}` : 'fixture.torrent';
      startEngine(cli, [source, '--config', 'config.json']);
      await exited(engine, `${mode} from qBittorrent`); verifyPayload(files);
      if (referenceDht) {
        referenceDht.check();
        assert.ok(referenceDht.queries.length > 0, 'No independent DHT lookup occurred');
        assert.ok(!announces.some(item => Number(item.port) === enginePort), 'Trackerless client contacted a tracker');
      }
    }
    const result = { scenario: mode, qBittorrent: version, buildInfo, bytes: payload.length, infoHash: hash, passed: true };
    fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify(result, null, 2));
    console.log(`PASS interop ${mode}: qBittorrent ${version}; ${payload.length} bytes verified.`);
  } finally {
    try {
      fs.writeFileSync(path.join(work, 'state-after.json'), JSON.stringify(await torrentState(), null, 2));
      fs.writeFileSync(path.join(work, 'peer-log.json'), await api('log/peers?last_known_id=-1'));
    } catch { /* The reference client may have failed to start. */ }
    for (const child of engines) if (child.code === undefined) {
      if (!child.stdin.destroyed) child.stdin.end('stop\n');
      try { await until(() => child.code !== undefined, 'engine cleanup', 4000); } catch { child.kill(); }
    }
    if (qbtProcess) {
      try { await api('app/shutdown', {}); } catch { /* API may not have started. */ }
      try { await until(() => qbtProcess.code !== undefined, 'qBittorrent cleanup', 4000); }
      catch { if (qbtPid) { try { process.kill(qbtPid); } catch { /* Already exited. */ } } qbtProcess.kill(); }
      fs.writeFileSync(path.join(work, 'qbittorrent-output.log'), qbtProcess.output);
    }
    tracker.closeAllConnections(); await new Promise(resolve => tracker.close(resolve));
    if (referenceDht) await referenceDht.close();
    fs.writeFileSync(path.join(work, 'engine-output.log'), engines.map(child => child.output).join('\n--- restart ---\n'));
    fs.writeFileSync(path.join(work, 'announces.json'), JSON.stringify(announces, null, 2));
    console.log(`Artifacts: ${work}`);
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
