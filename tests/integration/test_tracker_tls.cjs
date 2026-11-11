const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

async function main() {
  const [driver, openssl] = process.argv.slice(2).map(file => path.resolve(file));
  const artifacts = path.resolve(__dirname, '../../build/tests'); fs.mkdirSync(artifacts, { recursive: true });
  const work = fs.mkdtempSync(path.join(artifacts, 'tracker-tls-'));
  fs.writeFileSync(path.join(work, 'certificate.conf'), '[req]\ndistinguished_name=dn\nx509_extensions=extensions\nprompt=no\n[dn]\nCN=localhost\n[extensions]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nextendedKeyUsage=serverAuth\n');
  const certificate = spawnSync(openssl, ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
    '-config', 'certificate.conf', '-keyout', 'key.pem', '-out', 'certificate.pem'], { cwd: work, encoding: 'utf8', windowsHide: true });
  if (certificate.error) throw certificate.error;
  assert.equal(certificate.status, 0, certificate.stderr);
  let plainHits = 0, tlsHits = 0;
  const body = Buffer.from('d8:intervali60e5:peers0:e');
  const plain = http.createServer((req, res) => { ++plainHits; res.end(body); });
  const encrypted = https.createServer({ key: fs.readFileSync(path.join(work, 'key.pem')),
    cert: fs.readFileSync(path.join(work, 'certificate.pem')) }, (req, res) => { ++tlsHits; res.end(body); });
  const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  async function check(url, expectation) {
    const child = spawn(driver, [url, expectation], { cwd: work, windowsHide: true });
    let output = '';
    for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => { output += bytes; });
    const timer = setTimeout(() => child.kill(), 6000);
    const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); }).finally(() => clearTimeout(timer));
    assert.equal(code, 0, output); return output;
  }
  try {
    await listen(plain); await listen(encrypted);
    await check(`http://127.0.0.1:${plain.address().port}/announce`, 'success');
    const rejected = await check(`https://127.0.0.1:${encrypted.address().port}/announce`, 'failure');
    assert.match(rejected, /certificate|SSL peer|SSL.*connect|schannel/i);
    assert.equal(plainHits, 1); assert.equal(tlsHits, 0, 'Untrusted HTTPS tracker received an application request');
    fs.writeFileSync(path.join(work, 'output.log'), rejected);
    console.log('PASS tracker TLS: HTTP succeeds and an untrusted HTTPS certificate is rejected.');
  } finally {
    for (const server of [plain, encrypted]) { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); }
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
