const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { createDhtFixture } = require('./dht_fixture.cjs');

async function main() {
  const executable = path.resolve(process.argv[2]), mode = process.argv[3];
  const hash = Buffer.alloc(20, 77), peerPort = 42123;
  const fixture = mode === 'reference' ? await require('./reference_dht.cjs').createReferenceDht(hash, peerPort)
    : await createDhtFixture(hash, peerPort, mode);
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, `dht-${mode}-`));
  const config = { enable_dht: true, dht_port: mode === 'occupied' ? fixture.root.port : 0,
    dht_bootstrap_nodes: fixture.routers, listen_port: 24123, download_dir: './files' };
  fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify(config));
  let output = '', code, processError;
  const child = spawn(executable, ['config.json', mode, mode === 'metadata-cancel' ? `magnet:?xt=urn:btih:${hash.toString('hex')}` : hash.toString('hex')], { cwd: work });
  child.stdout.on('data', bytes => { output += bytes; }); child.stderr.on('data', bytes => { output += bytes; });
  child.on('error', error => { processError = error; }); child.on('close', result => { code = result; }); child.stdin.on('error', () => {});
  const until = async (check, description, timeout = 6000) => {
    const deadline = Date.now() + timeout;
    while (!check()) {
      fixture.check(); if (processError) throw processError;
      if (Date.now() >= deadline || code !== undefined) throw new Error(`${description}: ${output}`);
      await new Promise(resolve => setTimeout(resolve, 10));
    }
  };
  try {
    if (mode !== 'occupied') {
      await until(() => output.includes('DHT_READY'), 'driver startup');
      if (mode === 'cancel' || mode === 'metadata-cancel') {
        await until(() => fixture.queries.some(query => query.method === 'get_peers'), 'pending UDP lookup');
      } else {
        await until(() => output.includes('DHT_FOUND'), 'peer discovery');
        assert.ok(output.includes(`DHT_PEER 127.0.0.1 ${peerPort}`), output);
        assert.ok(!output.includes('DHT_PEER 127.0.0.1 1\n'), 'Accepted forged response');
        await until(() => fixture.announces.includes(config.listen_port), 'announce token round trip');
        if (mode === 'late') {
          assert.ok(output.includes('DHT_TIMEOUT')); await new Promise(resolve => setTimeout(resolve, 1500));
        }
      }
    }
    const stopped = Date.now(); child.stdin.end('stop\n');
    await until(() => code !== undefined, 'DHT shutdown', 2000);
    fixture.check();
    assert.equal(code, mode === 'occupied' ? 1 : 0, output);
    assert.ok(Date.now() - stopped < 1500, 'DHT stop was not responsive');
    if (mode === 'occupied') assert.ok(output.includes('Cannot bind DHT UDP port'), output);
    if (mode === 'metadata-cancel') assert.ok(!fs.existsSync(path.join(work, 'files')), 'Cancelled metadata created payload files');
    console.log(`PASS DHT ${mode}: peer lookup and bounded shutdown verified.`);
  } finally {
    if (child.exitCode === null) child.kill(); await fixture.close();
    fs.writeFileSync(path.join(work, 'output.log'), output);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
