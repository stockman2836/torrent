const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');

function encode(value) {
  if (Buffer.isBuffer(value)) return Buffer.concat([Buffer.from(`${value.length}:`), value]);
  if (typeof value === 'string') return encode(Buffer.from(value));
  if (typeof value === 'number') return Buffer.from(`i${value}e`);
  if (Array.isArray(value)) return Buffer.concat([Buffer.from('l'), ...value.map(encode), Buffer.from('e')]);
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(k => [encode(k), encode(value[k])]), Buffer.from('e')]);
}
function packet(id, payload = Buffer.alloc(0)) {
  const header = Buffer.alloc(5); header.writeUInt32BE(payload.length + 1); header[4] = id;
  return Buffer.concat([header, payload]);
}
const hash = data => crypto.createHash('sha1').update(data).digest();
const listen = server => new Promise((resolve, reject) => {
  server.once('error', reject); server.listen(0, '127.0.0.1', resolve);
});

async function main() {
  const [scenario, program] = process.argv.slice(2);
  assert.ok(['list', 'single', 'multiple', 'zero', 'resume', 'corrupt', 'changed', 'invalid', 'preserve', 'seed'].includes(scenario));
  const work = fs.mkdtempSync(path.join(process.cwd(), `selection-${scenario}-`));
  const pieceLength = 16384, lengths = [10007, 40009, 65536, 777, 0];
  const names = ['first.bin', 'nested/middle.bin', 'skipped.bin', 'tail.bin', 'empty.bin'];
  const data = Buffer.alloc(lengths.reduce((a, b) => a + b, 0));
  for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + Math.floor(i / pieceLength) * 19) % 256;
  const pieces = Array.from({ length: 8 }, (_, i) => hash(data.subarray(i * pieceLength, (i + 1) * pieceLength)));
  const info = { name: 'payload', 'piece length': pieceLength, pieces: Buffer.concat(pieces),
    files: lengths.map((length, i) => ({ length, path: names[i].split('/') })) };
  const infoHash = hash(encode(info)), sockets = new Set(), requests = [], announces = [];
  let child, output = '', phase = 'first', protocolError;
  const peer = net.createServer(socket => {
    sockets.add(socket); socket.on('close', () => sockets.delete(socket)); socket.on('error', () => {});
    let pending = Buffer.alloc(0), handshaken = false;
    socket.on('data', chunk => {
      try {
        pending = Buffer.concat([pending, chunk]);
        if (!handshaken) {
          if (pending.length < 68) return;
          assert.deepEqual(pending.subarray(28, 48), infoHash);
          pending = pending.subarray(68); handshaken = true;
          socket.write(Buffer.concat([Buffer.from([19]), Buffer.from('BitTorrent protocol'), Buffer.alloc(8), infoHash,
            Buffer.from('-SELECT-123456789012')]));
          socket.write(packet(5, Buffer.from([0xff]))); socket.write(packet(1));
        }
        while (pending.length >= 4) {
          const length = pending.readUInt32BE();
          if (pending.length < length + 4) break;
          const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
          if (message[0] !== 6) continue;
          const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
          assert.ok(index < 8 && offset === 0 && size === Math.min(pieceLength, data.length - index * pieceLength));
          requests.push({ phase, index });
          socket.write(packet(7, Buffer.concat([message.subarray(1, 9), data.subarray(index * pieceLength, index * pieceLength + size)])));
        }
      } catch (error) { protocolError = error; socket.destroy(); }
    });
  });
  const tracker = http.createServer((req, res) => {
    const query = new URL(req.url, 'http://localhost').searchParams;
    announces.push({ phase, event: query.get('event'), left: Number(query.get('left')) });
    const compact = Buffer.from([127, 0, 0, 1, 0, 0]); compact.writeUInt16BE(peer.address().port, 4);
    res.end(encode({ interval: 60, peers: compact }));
  });
  async function run(flags, expectedCode = 0) {
    output = '';
    child = spawn(path.resolve(program), ['test.torrent', '--config', 'config.json', ...flags], { cwd: work });
    child.stdout.on('data', chunk => { output += chunk; }); child.stderr.on('data', chunk => { output += chunk; });
    const timer = setTimeout(() => child.kill(), 12000);
    const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); }).finally(() => clearTimeout(timer));
    fs.writeFileSync(path.join(work, `${phase}.log`), output);
    if (protocolError) throw protocolError;
    assert.equal(code, expectedCode, output);
  }
  function checkMiddle() {
    assert.deepEqual(fs.readFileSync(path.join(work, 'files/payload/nested/middle.bin')), data.subarray(10007, 50016));
  }
  try {
    await listen(peer); await listen(tracker);
    fs.writeFileSync(path.join(work, 'test.torrent'), encode({ announce: `http://127.0.0.1:${tracker.address().port}/announce`, info }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_pex: false,
      enable_lsd: false, enable_webseeds: false, enable_utp: false, enable_encryption: false,
      seed_after_download: scenario === 'seed', download_dir: './files', log_file: './client.log', listen_port: 16881 }));
    const sentinel = Buffer.from('keep this existing unselected file');
    if (scenario === 'preserve') {
      fs.mkdirSync(path.join(work, 'files/payload'), { recursive: true });
      fs.writeFileSync(path.join(work, 'files/payload/first.bin'), sentinel);
    }
    if (scenario === 'list') {
      await run(['--list-files']);
      assert.ok(output.includes('ID\tBytes\tPath'));
      names.forEach((name, i) => assert.ok(output.includes(`${i + 1}\t${lengths[i]}\tpayload/${name}`), output));
      assert.ok(!fs.existsSync(path.join(work, 'files')));
      assert.equal(announces.length, 0);
    } else if (scenario === 'invalid') {
      await run(['--files', '99'], 1);
      assert.ok(!fs.existsSync(path.join(work, 'files')));
      assert.equal(announces.length, 0);
    } else if (scenario === 'zero') {
      await run(['--files', '5']);
      assert.equal(fs.statSync(path.join(work, 'files/payload/empty.bin')).size, 0);
      assert.equal(requests.length, 0);
      assert.equal(announces.length, 0);
      assert.ok(!fs.existsSync(path.join(work, 'files/payload/first.bin')));
    } else {
      await run(['--files', scenario === 'multiple' ? '2,4' : '2']);
      checkMiddle();
      const expected = scenario === 'multiple' ? [0, 1, 2, 3, 7] : [0, 1, 2, 3];
      assert.deepEqual(requests.map(r => r.index).sort((a, b) => a - b), expected);
      assert.equal(announces.find(a => a.event === 'stopped').left, scenario === 'multiple' ? 49152 : 50793);
      assert.ok(!announces.some(a => a.event === 'completed'), 'A selected subset is not a complete seed');
      assert.ok(!fs.existsSync(path.join(work, 'files/payload/skipped.bin')));
      assert.ok(!fs.existsSync(path.join(work, 'files/payload/empty.bin')));
      if (scenario === 'multiple') assert.deepEqual(fs.readFileSync(path.join(work, 'files/payload/tail.bin')), data.subarray(115552));
      if (['resume', 'corrupt', 'changed', 'preserve'].includes(scenario)) {
        if (scenario === 'corrupt') {
          const file = fs.openSync(path.join(work, 'files/payload/nested/middle.bin'), 'r+');
          fs.writeSync(file, Buffer.from([data[10007] ^ 0xff]), 0, 1, 0); fs.closeSync(file);
        }
        phase = 'second';
        await run(['--files', scenario === 'changed' ? '4' : '2']);
        const second = requests.filter(r => r.phase === 'second').map(r => r.index);
        assert.deepEqual(second, scenario === 'corrupt' ? [0] : scenario === 'changed' ? [7] : []);
        checkMiddle();
        if (scenario === 'changed') assert.deepEqual(fs.readFileSync(path.join(work, 'files/payload/tail.bin')), data.subarray(115552));
      }
      if (scenario === 'preserve') assert.deepEqual(fs.readFileSync(path.join(work, 'files/payload/first.bin')), sentinel);
      else assert.ok(!fs.existsSync(path.join(work, 'files/payload/first.bin')));
      assert.ok(output.includes('Selected files complete!'), output);
    }
    console.log(`PASS selection ${scenario}: ${requests.length} piece requests; skipped payload files preserved.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const socket of sockets) socket.destroy(); peer.close(); tracker.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
