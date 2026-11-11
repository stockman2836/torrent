// Independent implementation, used only by explicitly enabled interop tests.
async function createReferenceDht(hash, peerPort) {
  const { default: DHT } = await import('bittorrent-dht');
  const node = new DHT({ bootstrap: false, host: '127.0.0.1' });
  const publisher = new DHT({ bootstrap: false, host: '127.0.0.1' });
  const queries = [], announces = [];
  let error;
  node.on('error', caught => { error = caught; });
  publisher.on('error', caught => { error = caught; });
  node.on('get_peers', () => queries.push({ method: 'get_peers' }));
  node.on('announce', peer => announces.push(peer.port));
  try {
    await new Promise(resolve => node.listen(0, '127.0.0.1', resolve));
    await new Promise(resolve => publisher.listen(0, '127.0.0.1', resolve));
    publisher.addNode({ host: '127.0.0.1', port: node.address().port, id: node.nodeId });
    await new Promise((resolve, reject) => publisher.announce(hash, peerPort, caught => caught ? reject(caught) : resolve()));
  } catch (caught) {
    await Promise.all([node, publisher].map(item => new Promise(resolve => item.destroy(resolve))));
    throw caught;
  }
  announces.length = 0;
  return { routers: [{ host: '127.0.0.1', port: node.address().port }], queries, announces,
    check() { if (error) throw error; },
    close() { return Promise.all([node, publisher].map(item => new Promise(resolve => item.destroy(resolve)))); }
  };
}
module.exports = { createReferenceDht };
