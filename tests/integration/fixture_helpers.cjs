const net = require('node:net');
const dgram = require('node:dgram');
const { randomInt } = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const leases = [];
process.once('exit', () => {
  for (const file of leases) { try { fs.unlinkSync(file); } catch { /* Already removed. */ } }
});

// Reserve adjacent task ports while finding a free range for parallel CTest runs.
async function availablePort(count = 1) {
  const directory = path.resolve(__dirname, '../../build/tests/ports');
  fs.mkdirSync(directory, { recursive: true });
  for (let attempt = 0; attempt < 32; ++attempt) {
    const servers = [], datagrams = [], acquired = [];
    let reserved = false;
    try {
      // Keep listeners out of the ephemeral range used by outbound connections.
      const base = randomInt(20000, 30000);
      // The child needs the sockets released before it can bind. Keep exclusive
      // file leases until this fixture exits so another fixture cannot choose
      // the same adjacent range between the probe and the child's startup.
      for (let i = 0; i < count * 2; ++i) {
        const file = path.join(directory, `${base + i}.lock`);
        const descriptor = fs.openSync(file, 'wx'); acquired.push(file);
        fs.writeSync(descriptor, String(process.pid)); fs.closeSync(descriptor);
      }
      for (let i = 0; i < count; ++i) {
        const server = net.createServer(); servers.push(server);
        await new Promise((resolve, reject) => {
          server.once('error', reject); server.listen(base + i * 2, '0.0.0.0', resolve);
        });
        const udp = dgram.createSocket('udp4'); datagrams.push(udp);
        await new Promise((resolve, reject) => {
          udp.once('error', reject); udp.bind(base + i * 2 + 1, '0.0.0.0', resolve);
        });
        if (base + (count - 1) * 2 > 65535) throw new Error('Port range exceeds uint16');
      }
      leases.push(...acquired); reserved = true; return base;
    } catch (error) {
      if (attempt === 31) throw error;
    } finally {
      await Promise.all(servers.map(server => server.listening ? new Promise(resolve => server.close(resolve)) : Promise.resolve()));
      await Promise.all(datagrams.map(socket => new Promise(resolve => {
        try { socket.close(resolve); } catch { resolve(); }
      })));
      if (!reserved) for (const file of acquired) fs.unlinkSync(file);
    }
  }
}

module.exports = { availablePort };
