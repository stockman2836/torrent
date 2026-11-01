const net = require('node:net');
const { randomInt } = require('node:crypto');

// Reserve adjacent task ports while finding a free range for parallel CTest runs.
async function availablePort(count = 1) {
  for (let attempt = 0; attempt < 32; ++attempt) {
    const servers = [];
    try {
      // Keep listeners out of the ephemeral range used by outbound connections.
      const base = randomInt(20000, 30000);
      for (let i = 0; i < count; ++i) {
        const server = net.createServer(); servers.push(server);
        await new Promise((resolve, reject) => {
          server.once('error', reject); server.listen(base + i * 2, '127.0.0.1', resolve);
        });
        if (base + (count - 1) * 2 > 65535) throw new Error('Port range exceeds uint16');
      }
      return base;
    } catch (error) {
      if (attempt === 31) throw error;
    } finally {
      await Promise.all(servers.map(server => server.listening ? new Promise(resolve => server.close(resolve)) : Promise.resolve()));
    }
  }
}

module.exports = { availablePort };
