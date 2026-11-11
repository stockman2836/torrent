const assert = require('node:assert/strict');
const dgram = require('node:dgram');
const crypto = require('node:crypto');

function encode(value) {
  if (Buffer.isBuffer(value)) return Buffer.concat([Buffer.from(`${value.length}:`), value]);
  if (typeof value === 'string') return encode(Buffer.from(value));
  if (typeof value === 'number') return Buffer.from(`i${value}e`);
  if (Array.isArray(value)) return Buffer.concat([Buffer.from('l'), ...value.map(encode), Buffer.from('e')]);
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(key => [encode(key), encode(value[key])]), Buffer.from('e')]);
}
function decode(bytes, at = 0) {
  const start = bytes[at++];
  if (start === 105) {
    const end = bytes.indexOf(101, at); assert.ok(end >= at);
    return [Number(bytes.subarray(at, end)), end + 1];
  }
  if (start === 108 || start === 100) {
    const value = start === 108 ? [] : {};
    while (bytes[at] !== 101) {
      let key, item;
      [key, at] = decode(bytes, at);
      if (start === 100) { [item, at] = decode(bytes, at); value[key.toString()] = item; }
      else value.push(key);
    }
    return [value, at + 1];
  }
  assert.ok(start >= 48 && start <= 57, 'Invalid Bencode');
  const colon = bytes.indexOf(58, at - 1), size = Number(bytes.subarray(at - 1, colon));
  assert.ok(colon >= 0 && Number.isSafeInteger(size) && size >= 0 && colon + 1 + size <= bytes.length);
  return [bytes.subarray(colon + 1, colon + 1 + size), colon + 1 + size];
}
function peer(port) { return Buffer.from([127, 0, 0, 1, port >> 8, port & 255]); }

// Independent loopback KRPC nodes used for deterministic protocol failures.
async function createDhtFixture(hash, peerPort, mode = 'direct') {
  const nodes = [], timers = new Set(), queries = [], announces = [];
  let error;
  async function node(role) {
    const socket = dgram.createSocket('udp4'), id = crypto.randomBytes(20);
    const result = { socket, id, role, queries: 0, port: 0 };
    nodes.push(result);
    socket.on('error', caught => { error = caught; });
    await new Promise(resolve => socket.bind(0, '127.0.0.1', resolve)); result.port = socket.address().port;
    result.compact = () => Buffer.concat([id, peer(result.port)]);
    socket.on('message', (bytes, remote) => {
      try {
        const [query, end] = decode(bytes); assert.equal(end, bytes.length);
        assert.equal(query.y.toString(), 'q'); assert.equal(query.a.id.length, 20);
        const method = query.q.toString(); queries.push({ role, method, from: remote.port });
        if (role === 'silent') return;
        const reply = fields => socket.send(encode({ t: query.t, y: 'r', r: { id, ...fields } }), remote.port, remote.address);
        if (method === 'ping') return reply({});
        if (method === 'find_node') {
          if (mode === 'error' && role === 'root') {
            const timer = setTimeout(() => { timers.delete(timer); reply({ nodes: Buffer.alloc(0) }); }, 120);
            timers.add(timer); return;
          }
          return reply({ nodes: Buffer.alloc(0) });
        }
        assert.deepEqual(query.a.info_hash, hash);
        if (method === 'announce_peer') {
          assert.deepEqual(query.a.token, Buffer.from(`token-${role}`));
          assert.equal(query.a.implied_port, 0);
          announces.push(query.a.port); return reply({});
        }
        assert.equal(method, 'get_peers'); ++result.queries;
        if (role === 'error') return socket.send(encode({ t: query.t, y: 'e', e: [202, 'fixture error'] }), remote.port, remote.address);
        if (role === 'root' && mode === 'chain') return reply({ nodes: leaf.compact(), token: `token-${role}` });
        if (mode === 'late' && result.queries === 1) {
          const timer = setTimeout(() => { timers.delete(timer); reply({ values: [peer(peerPort)], token: `token-${role}` }); }, 1400);
          timers.add(timer); return;
        }
        if (mode === 'cancel' || mode === 'metadata-cancel') return;
        if (mode === 'spoof') {
          const invalid = { t: query.t, y: 'r', r: { id, values: [peer(1)], token: 'forged' } };
          leaf.socket.send(encode(invalid), remote.port, remote.address);
          socket.send(encode({ ...invalid, t: Buffer.from('wrong') }), remote.port, remote.address);
          const timer = setTimeout(() => { timers.delete(timer); reply({ values: [peer(peerPort)], token: `token-${role}` }); }, 120);
          timers.add(timer); return;
        }
        reply({ values: [Buffer.alloc(3), peer(0), peer(peerPort), peer(peerPort)], token: `token-${role}` });
      } catch (caught) { error = caught; }
    });
    return result;
  }
  const root = await node('root');
  const leaf = ['chain', 'spoof'].includes(mode) ? await node('leaf') : undefined;
  const fallback = ['fallback', 'error'].includes(mode) ? await node(mode === 'error' ? 'error' : 'silent') : undefined;
  return { queries, announces, root,
    routers: [...(fallback ? [fallback] : []), root].map(item => ({ host: '127.0.0.1', port: item.port })),
    check() { if (error) throw error; },
    async close() { for (const timer of timers) clearTimeout(timer); await Promise.all(nodes.map(item => new Promise(resolve => item.socket.close(resolve)))); }
  };
}

module.exports = { encode, decode, createDhtFixture };
