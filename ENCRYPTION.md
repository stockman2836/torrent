# Protocol Encryption (MSE/PE) Support

## Overview

This BitTorrent client implements Message Stream Encryption (MSE) / Protocol Encryption (PE) to obfuscate BitTorrent traffic and prevent ISP throttling and deep packet inspection.

## Status: ✅ Implemented

### Components

1. **Crypto Utilities** (`crypto.h/cpp`)
   - Diffie-Hellman key exchange (768-bit prime)
   - RC4 stream cipher
   - SHA1 hashing (reuse existing utils)
   - HMAC-SHA1 for key derivation

2. **MSE Protocol Handler** (`mse_handshake.h/cpp`)
   - 4-packet handshake implementation
   - Key negotiation and derivation
   - Verification constant (VC) handling
   - crypto_provide/crypto_select flags

3. **PeerConnection Integration**
   - Optional MSE handshake before BitTorrent handshake
   - Transparent encryption/decryption layer
   - Fallback to plaintext when needed

4. **Configuration**
   - Encryption modes: prefer_plaintext, prefer_encrypted, require_encrypted
   - Per-torrent encryption settings

## Protocol Specification

### Diffie-Hellman Parameters

```
P = 0xFFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A63A36210000000000090563
G = 2
```

- Each peer generates random private key (128-160 bits)
- Public key: `Ya = G^Xa mod P` (96 bytes)
- Shared secret: `S = Yb^Xa mod P = Ya^Xb mod P`

### Key Derivation

```
SKEY = torrent info_hash

For initiator (client):
- Outgoing key: SHA1("keyA" + S + SKEY)
- Incoming key: SHA1("keyB" + S + SKEY)

For responder (server):
- Outgoing key: SHA1("keyB" + S + SKEY)
- Incoming key: SHA1("keyA" + S + SKEY)
```

### RC4 Keystream

- Initialize RC4 with derived keys
- Discard first 1024 bytes (protection against Fluhrer-Mantin-Shamir attack)
- Use same keystream for all subsequent data (not reinitialized)

### 4-Packet Handshake

**Packet 1 (Initiator → Responder):**
```
Ya (96 bytes) + PadA (0-512 random bytes)
```

**Packet 2 (Responder → Initiator):**
```
Yb (96 bytes) + PadB (0-512 random bytes)
```

**Packet 3 (Initiator → Responder, encrypted):**
```
HASH("req1", S) (20 bytes)           // Verification hash
HASH("req2", SKEY) xor HASH("req3", S) (20 bytes)  // SKEY verification
VC (8 zero bytes)
crypto_provide (4 bytes, big-endian)  // 0x01 = plaintext, 0x02 = RC4
len(PadC) (2 bytes)
PadC (0-512 bytes)
len(IA) (2 bytes)                     // Initial Application data
IA (variable)                         // Can be empty or contain BT handshake
```

**Packet 4 (Responder → Initiator, encrypted):**
```
VC (8 zero bytes)
crypto_select (4 bytes)               // Selected encryption method
len(PadD) (2 bytes)
PadD (0-512 bytes)
```

### Encryption Flags

**crypto_provide / crypto_select:**
- Bit 0 (0x01): Plaintext
- Bit 1 (0x02): RC4

Multiple bits can be set in crypto_provide. Exactly one bit must be set in crypto_select.

### Post-Handshake Flow

After handshake completes:
- If RC4 selected: All data encrypted with established keystreams
- If plaintext selected: Switch to unencrypted communication
- Standard BitTorrent handshake follows (if not sent in IA)

## Implementation Architecture

### Class Structure

```cpp
// Crypto utilities
class BigInteger {
    // Arbitrary precision integer for DH
};

class RC4 {
    void init(const std::vector<uint8_t>& key);
    void crypt(uint8_t* data, size_t len);
};

// MSE Protocol
class MSEHandshake {
    enum class Mode {
        PREFER_PLAINTEXT,
        PREFER_ENCRYPTED,
        REQUIRE_ENCRYPTED
    };

    enum class CryptoMethod {
        PLAINTEXT = 0x01,
        RC4 = 0x02
    };

    bool performHandshake(PeerConnection& conn, bool is_initiator);

private:
    // DH key exchange
    std::vector<uint8_t> generatePrivateKey();
    std::vector<uint8_t> computePublicKey(const std::vector<uint8_t>& private_key);
    std::vector<uint8_t> computeSharedSecret(const std::vector<uint8_t>& their_public);

    // Key derivation
    std::vector<uint8_t> deriveKey(const std::string& salt);

    // Handshake packets
    bool sendPacket1(PeerConnection& conn);
    bool receivePacket2(PeerConnection& conn);
    bool sendPacket3(PeerConnection& conn);
    bool receivePacket4(PeerConnection& conn);
};

// Encrypted stream wrapper
class EncryptedStream {
    void encrypt(std::vector<uint8_t>& data);
    void decrypt(std::vector<uint8_t>& data);

private:
    RC4 outgoing_cipher_;
    RC4 incoming_cipher_;
    bool encryption_enabled_;
};
```

### PeerConnection Integration

```cpp
class PeerConnection {
    // ...existing members...

    std::unique_ptr<EncryptedStream> encrypted_stream_;
    MSEHandshake::Mode encryption_mode_;

    bool performEncryptedHandshake();

private:
    bool sendDataEncrypted(const void* data, size_t length);
    bool receiveDataEncrypted(void* buffer, size_t length);
};
```

## Configuration

Add to `config.json`:

```json
{
    "encryption": {
        "mode": "prefer_encrypted",  // prefer_plaintext, prefer_encrypted, require_encrypted
        "allow_legacy_peers": true,  // Allow plaintext fallback
        "enable_mse": true           // Enable MSE/PE support
    }
}
```

## Security Considerations

### Known Limitations

1. **RC4 Weakness**: RC4 has known cryptographic weaknesses
   - We mitigate by discarding first 1024 bytes
   - This is for obfuscation, not strong cryptography

2. **No Authentication**: MSE/PE doesn't authenticate peers
   - Vulnerable to man-in-the-middle attacks
   - Info hash in handshake provides weak identity verification

3. **Traffic Analysis**: Packet sizes and timing still leak information
   - Padding helps but doesn't eliminate patterns

### Design Goals

MSE/PE is designed for:
- ✅ Preventing automated DPI detection
- ✅ Avoiding BitTorrent protocol fingerprinting
- ✅ Bypassing ISP throttling
- ❌ NOT for strong cryptographic security
- ❌ NOT for anonymity

## Testing

### Unit Tests

```cpp
// Test DH key exchange
TEST(MSETest, DiffieHellmanKeyExchange)

// Test RC4 encryption/decryption
TEST(CryptoTest, RC4SymmetricEncryption)

// Test key derivation
TEST(MSETest, KeyDerivation)

// Test handshake packets
TEST(MSETest, HandshakePackets)
```

### Integration Tests

1. Encrypted connection with another MSE-capable client
2. Fallback to plaintext when peer doesn't support MSE
3. Mixed encrypted/plaintext peers in swarm
4. Rejection when require_encrypted mode encounters plaintext peer

## Performance Impact

Expected overhead:
- **Handshake**: +2 RTT (4 packets vs 1 for standard BT handshake)
- **CPU**: RC4 is very lightweight (~1-2% overhead)
- **Memory**: ~200 bytes per connection for cipher state
- **Network**: 192+ bytes for DH public keys + padding

## References

- [BEP 8: Tracker Peer Obfuscation](https://www.bittorrent.org/beps/bep_0008.html)
- [Knowledge Bits — The BitTorrent Encryption Protocol](https://jwodder.github.io/kbits/posts/bt-encrypt/)
- [BitTorrent Protocol Encryption - Wikipedia](https://en.wikipedia.org/wiki/BitTorrent_protocol_encryption)
- [Vuze Wiki MSE Specification](https://wiki.vuze.com/w/Message_Stream_Encryption) (historical)

## Implementation Status

- [x] Crypto utilities (RC4, Diffie-Hellman, key derivation) - **DONE**
- [x] MSE handshake protocol (4-packet flow) - **DONE**
- [x] PeerConnection integration - **DONE**
- [x] Configuration and modes - **DONE**
- [ ] Unit tests (recommended for future)
- [ ] Integration tests with real peers (recommended)
- [x] Documentation - **DONE**

---

**Last Updated:** 2025-12-11
**Version:** 0.1 (In Development)
