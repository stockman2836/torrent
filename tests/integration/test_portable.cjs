// Validate the archive itself, then run its real window with no toolchain on PATH.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
function run(program, args, options = {}) {
  const result = spawnSync(program, args, { encoding: 'utf8', windowsHide: true, ...options });
  if (result.error) throw result.error;
  assert.equal(result.status, 0, result.stdout + result.stderr);
  return result.stdout;
}
function files(directory, prefix = '') {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    const relative = prefix + entry.name;
    return entry.isDirectory() ? files(path.join(directory, entry.name), relative + '/') : [relative];
  });
}
async function fixture(script, executable, mode, cwd, env) {
  const child = spawn(process.execPath, [path.join(__dirname, script), executable, mode], { cwd, env, windowsHide: true });
  let output = '';
  for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => { output += bytes; });
  const result = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); });
  assert.equal(result, 0, output); process.stdout.write(output);
}
async function main() {
  assert.equal(process.platform, 'win32', 'The portable desktop test requires Windows');
  assert.ok(process.argv[2], 'Usage: test_portable.cjs <archive.zip> [cmake.exe]');
  const archive = path.resolve(process.argv[2]), cmake = process.argv[3] || 'cmake';
  const checksum = fs.readFileSync(archive + '.sha256', 'utf8').match(/[a-f0-9]{64}/i);
  assert.ok(checksum, 'Missing archive SHA256'); assert.equal(hash(archive), checksum[0].toLowerCase());
  const entries = run(cmake, ['-E', 'tar', 'tf', archive]).trim().split(/\r?\n/).map(name => name.replace(/\\/g, '/'));
  for (const entry of entries) assert.ok(!/^([a-z]:|\/)/i.test(entry) && !entry.split('/').includes('..'), `Unsafe archive path: ${entry}`);
  const roots = new Set(entries.map(entry => entry.split('/')[0]));
  assert.equal(roots.size, 1, 'Archive must contain one application folder');
  const folder = [...roots][0]; assert.match(folder, /^TorrentClient-[\d.]+-windows-(x64|x86|arm64)$/);
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, 'portable-'));
  const unpacked = path.join(work, 'Распакованное приложение'); fs.mkdirSync(unpacked);
  run(cmake, ['-E', 'tar', 'xf', archive], { cwd: unpacked });
  const root = path.join(unpacked, folder), executable = path.join(root, 'TorrentClient.exe');
  assert.ok(fs.existsSync(executable), 'Missing desktop executable');
  const packaged = files(root).sort(), recorded = new Set();
  for (const line of fs.readFileSync(path.join(root, 'SHA256SUMS.txt'), 'utf8').trim().split(/\r?\n/)) {
    const match = line.match(/^([a-f0-9]{64})  (.+)$/); assert.ok(match, `Invalid manifest line: ${line}`);
    const relative = match[2], target = path.resolve(root, relative);
    assert.ok(!path.relative(root, target).startsWith('..') && !path.isAbsolute(relative), 'Manifest path escapes the application folder');
    assert.ok(!recorded.has(relative), 'Duplicate manifest entry'); recorded.add(relative);
    assert.equal(hash(target), match[1], `Checksum mismatch: ${relative}`);
  }
  assert.deepEqual([...recorded].sort(), packaged.filter(file => file !== 'SHA256SUMS.txt'), 'Manifest does not match archive contents');
  assert.ok(!packaged.some(file => /\.(a|lib|obj|pdb|cmake|torrent|log)$/i.test(file)), 'Archive contains development/runtime artifacts');
  assert.deepEqual(packaged.filter(file => file.endsWith('.exe')), ['TorrentClient.exe']);
  assert.match(fs.readFileSync(path.join(root, 'BUILD-INFO.txt'), 'utf8'), /Configuration: Release/);
  const binary = fs.readFileSync(executable), pe = binary.readUInt32LE(0x3c);
  assert.equal(binary.toString('ascii', pe, pe + 4), 'PE\0\0');
  assert.equal(binary.readUInt16LE(pe + 24 + 68), 2, 'The application must use the Windows GUI subsystem');

  const windows = process.env.SystemRoot || process.env.windir;
  assert.ok(windows, 'Windows system directory is not defined');
  const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => key.toLowerCase() !== 'path'));
  env.Path = [path.join(windows, 'System32'), windows].join(path.delimiter);
  const working = path.join(work, 'Отдельная рабочая папка'); fs.mkdirSync(working);
  await fixture('test_download.cjs', executable, '--desktop-ui', working, env);
  await fixture('test_queue.cjs', executable, 'ui', working, env);
  await fixture('test_live_priorities.cjs', executable, 'ui', working, env);
  await fixture('test_download.cjs', executable, '--desktop-global-ui', working, env);
  await fixture('test_download.cjs', executable, '--desktop-fallback-ui', working, env);
  fs.writeFileSync(path.join(work, 'result.json'), JSON.stringify({ archive, sha256: hash(archive),
    files: packaged, isolatedPath: env.Path, passed: true }, null, 2));
  console.log(`PASS portable: ${packaged.length} files verified; Release window and transfers work with a system-only PATH.`);
  console.log(`Extracted application: ${root}`);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
