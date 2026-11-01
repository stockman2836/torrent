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
const listen = server => new Promise((resolve, reject) => {
  server.once('error', reject); server.listen(0, '127.0.0.1', resolve);
});
const hash = data => crypto.createHash('sha1').update(data).digest();

async function main() {
  const [scenario, program] = process.argv.slice(2);
  assert.ok(['order', 'default', 'boundary', 'allskip', 'invalid', 'outside', 'resume', 'list'].includes(scenario));
  const work = fs.mkdtempSync(path.join(process.cwd(), `priority-${scenario}-`));
  const lengths = scenario === 'boundary' ? [10007, 40009, 65536, 777, 0] : [16384, 16384, 16384, 16384, 16384, 16384, 123, 0];
  const data = Buffer.alloc(lengths.reduce((a, b) => a + b, 0));
  for (let i = 0; i < data.length; ++i) data[i] = (i * 37 + Math.floor(i / 16384) * 19) % 256;
  const pieces = Array.from({ length: Math.ceil(data.length / 16384) }, (_, i) => hash(data.subarray(i * 16384, (i + 1) * 16384)));
  const info = { name: 'payload', 'piece length': 16384, pieces: Buffer.concat(pieces),
    files: lengths.map((length, i) => ({ length, path: [`file${i + 1}.bin`] })) };
  const infoHash = hash(encode(info)), requests = [], events = [], sockets = new Set();
  let phase = 'first', child, output = '', protocolError;
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
            Buffer.from('-PRIORI-123456789012')]));
          socket.write(packet(5, Buffer.from([pieces.length === 8 ? 0xff : 0xfe]))); socket.write(packet(1));
        }
        while (pending.length >= 4) {
          const length = pending.readUInt32BE();
          if (pending.length < length + 4) break;
          const message = pending.subarray(4, length + 4); pending = pending.subarray(length + 4);
          if (message[0] !== 6) continue;
          const index = message.readUInt32BE(1), offset = message.readUInt32BE(5), size = message.readUInt32BE(9);
          assert.ok(index < pieces.length && offset === 0 && size === Math.min(16384, data.length - index * 16384));
          requests.push({ phase, index });
          socket.write(packet(7, Buffer.concat([message.subarray(1, 9), data.subarray(index * 16384, index * 16384 + size)])));
        }
      } catch (error) { protocolError = error; socket.destroy(); }
    });
  });
  const tracker = http.createServer((req, res) => {
    const query = new URL(req.url, 'http://localhost').searchParams;
    events.push({ phase, event: query.get('event'), left: Number(query.get('left')) });
    const compact = Buffer.from([127, 0, 0, 1, 0, 0]); compact.writeUInt16BE(peer.address().port, 4);
    res.end(encode({ interval: 60, peers: compact }));
  });
  async function run(flags, code = 0) {
    output = '';
    child = spawn(path.resolve(program), ['test.torrent', '--config', 'config.json', ...flags], { cwd: work });
    child.stdout.on('data', chunk => { output += chunk; }); child.stderr.on('data', chunk => { output += chunk; });
    const timer = setTimeout(() => child.kill(), 12000);
    const actual = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); }).finally(() => clearTimeout(timer));
    fs.writeFileSync(path.join(work, `${phase}.log`), output);
    if (protocolError) throw protocolError;
    assert.equal(actual, code, output);
  }
  try {
    await listen(peer); await listen(tracker);
    fs.writeFileSync(path.join(work, 'test.torrent'), encode({ announce: `http://127.0.0.1:${tracker.address().port}/announce`, info }));
    fs.writeFileSync(path.join(work, 'config.json'), JSON.stringify({ enable_dht: false, enable_pex: false,
      enable_lsd: false, enable_webseeds: false, enable_utp: false, enable_encryption: false,
      sequential_download: scenario !== 'default', download_dir: './files', log_file: './client.log', listen_port: 16881 }));
    const priorities = '1:low,3:high,4:high,5:skip,6:low,8:high';
    if (scenario === 'invalid' || scenario === 'outside') {
      await run(scenario === 'invalid' ? ['--priority', '99:high'] : ['--files', '2', '--priority', '3:high'], 1);
      assert.equal(events.length, 0); assert.ok(!fs.existsSync(path.join(work, 'files')));
    } else if (scenario === 'list') {
      await run(['--list-files', '--priority', priorities]);
      assert.ok(output.includes('Priority'));
      assert.ok(output.includes('3\t16384\tpayload/file3.bin\thigh'));
      assert.ok(output.includes('5\t16384\tpayload/file5.bin\tskip'));
      assert.equal(events.length, 0); assert.ok(!fs.existsSync(path.join(work, 'files')));
    } else if (scenario === 'allskip') {
      await run(['--priority', lengths.map((_, i) => `${i + 1}:skip`).join(',')]);
      assert.equal(events.length, 0); assert.equal(requests.length, 0);
      assert.ok(!fs.existsSync(path.join(work, 'files/payload')));
    } else {
      const boundary = scenario === 'boundary';
      await run(['--priority', boundary ? '1:skip,2:low,3:skip,4:high,5:skip' : priorities]);
      const order = requests.map(r => r.index);
      if (boundary) assert.deepEqual(order, [7, 0, 1, 2, 3]);
      else if (scenario === 'default') {
        assert.deepEqual([...order].sort((a, b) => a - b), [0, 1, 2, 3, 5, 6]);
        const ranks = [1, 2, 3, 3, 0, 1, 2];
        for (let i = 1; i < order.length; ++i) assert.ok(ranks[order[i - 1]] >= ranks[order[i]], `Priority inversion: ${order}`);
      } else assert.deepEqual(order, [2, 3, 1, 6, 0, 5]);
      let offset = 0;
      lengths.forEach((length, i) => {
        const selected = boundary ? i === 1 || i === 3 : i !== 4;
        const file = path.join(work, 'files/payload', `file${i + 1}.bin`);
        if (selected) assert.deepEqual(fs.readFileSync(file), data.subarray(offset, offset + length));
        else assert.ok(!fs.existsSync(file));
        offset += length;
      });
      assert.equal(events.find(e => e.event === 'stopped').left, boundary ? 49152 : 16384);
      assert.ok(!events.some(e => e.event === 'completed'));
      if (scenario === 'resume') {
        phase = 'second';
        await run(['--priority', '1:high,3:low,4:normal,5:skip,6:high']);
        assert.equal(requests.filter(r => r.phase === 'second').length, 0);
        assert.equal(events.filter(e => e.phase === 'second').length, 0);
        assert.ok(output.includes('Resume data loaded successfully'));
      }
    }
    console.log(`PASS priority ${scenario}: request order ${requests.map(r => r.index).join(',') || '(none)'}.`);
  } finally {
    if (child && child.exitCode === null) child.kill();
    for (const socket of sockets) socket.destroy(); peer.close(); tracker.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
