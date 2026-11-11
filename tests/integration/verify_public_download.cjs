// Independent, offline verification of a single-file public download or its resume state.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { decode, encode } = require('./dht_fixture.cjs');

async function main() {
  const [torrent, directory, resultPath, expectedSha256] = process.argv.slice(2);
  if (!resultPath) throw new Error('Usage: verify_public_download.cjs <torrent> <directory> <result.json> [official-sha256]');
  if (expectedSha256) assert.match(expectedSha256, /^[a-f0-9]{64}$/i);
  const [metadata] = decode(fs.readFileSync(torrent)), info = metadata.info;
  assert.ok(!info.files, 'This manual verifier supports single-file images only');
  const name = info.name.toString();
  assert.ok(name && name !== '.' && name !== '..' && !/[\\/:]/.test(name), 'Unsafe image name');
  const hash = crypto.createHash('sha1').update(encode(info)).digest('hex');
  const pieceLength = info['piece length'], count = info.pieces.length / 20;
  assert.ok(Number.isSafeInteger(pieceLength) && pieceLength > 0 && pieceLength <= 64 * 1024 * 1024);
  assert.equal(count, Math.ceil(info.length / pieceLength));
  const file = path.join(directory, name);
  assert.equal(fs.statSync(file).size, info.length, 'Image length differs from metadata');
  const resumeFile = path.join(directory, `.resume_${hash.slice(0, 16)}.dat`);
  let bitfield;
  if (!expectedSha256) {
    const resume = fs.readFileSync(resumeFile);
    assert.equal(resume.subarray(0, 8).toString(), 'BTRESUME');
    assert.equal(resume[8], 1); assert.equal(resume.readUInt32BE(9), count);
    assert.equal(resume.length, 13 + Math.ceil(count / 8) + 8);
    bitfield = resume.subarray(13, 13 + Math.ceil(count / 8));
  }
  let claimed = 0, verified = 0, verifiedBytes = 0;
  const failed = [], fd = fs.openSync(file, 'r'), buffer = Buffer.alloc(pieceLength);
  try {
    for (let index = 0; index < count; ++index) {
      if (bitfield && !(bitfield[index >> 3] & (0x80 >> (index & 7)))) continue;
      ++claimed;
      const length = Math.min(pieceLength, info.length - index * pieceLength);
      let read = 0;
      while (read < length) {
        const bytes = fs.readSync(fd, buffer, read, length - read, index * pieceLength + read);
        if (!bytes) break;
        read += bytes;
      }
      const digest = crypto.createHash('sha1').update(buffer.subarray(0, read)).digest();
      if (read === length && digest.equals(info.pieces.subarray(index * 20, index * 20 + 20))) {
        ++verified; verifiedBytes += length;
      } else failed.push(index);
    }
  } finally { fs.closeSync(fd); }
  let sha256;
  if (expectedSha256) {
    const digest = crypto.createHash('sha256');
    for await (const bytes of fs.createReadStream(file)) digest.update(bytes);
    sha256 = digest.digest('hex');
  }
  const result = { name, infoHash: hash, totalPieces: count, claimedPieces: claimed,
    verifiedPieces: verified, verifiedBytes, failedPieces: failed,
    sha256, expectedSha256, sha256Matches: expectedSha256 ? sha256 === expectedSha256.toLowerCase() : undefined };
  fs.mkdirSync(path.dirname(path.resolve(resultPath)), { recursive: true });
  fs.writeFileSync(resultPath, JSON.stringify(result, null, 2) + '\n');
  console.log(JSON.stringify(result));
  assert.ok(verified > 0 && failed.length === 0, 'Saved pieces failed independent SHA1 verification');
  if (expectedSha256) assert.ok(result.sha256Matches, 'Image SHA256 differs from the official checksum');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
