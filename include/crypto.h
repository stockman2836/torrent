#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace torrent {
namespace crypto {

// ============================================================================
// RC4 Stream Cipher
// ============================================================================

class RC4 {
public:
    RC4() = default;

    // Initialize cipher with key
    void init(const std::vector<uint8_t>& key);
    void init(const uint8_t* key, size_t key_len);

    // Encrypt/decrypt data in place (RC4 is symmetric)
    void crypt(uint8_t* data, size_t len);
    void crypt(std::vector<uint8_t>& data);

    // Discard N bytes (used to discard first 1024 bytes for security)
    void discard(size_t n);

    // Reset cipher state
    void reset();

    // Check if initialized
    bool isInitialized() const { return initialized_; }

private:
    uint8_t S_[256];
    size_t i_ = 0;
    size_t j_ = 0;
    bool initialized_ = false;
};

// ============================================================================
// Diffie-Hellman Key Exchange
// ============================================================================

// DH parameters for MSE/PE (768-bit)
// P = prime modulus (96 bytes)
// G = generator (2)
extern const std::vector<uint8_t> DH_P;
extern const uint8_t DH_G;

class DiffieHellman {
public:
    DiffieHellman();

    // Generate random private key (128-160 bits recommended)
    std::vector<uint8_t> generatePrivateKey(size_t bits = 160);

    // Compute public key: Ya = G^Xa mod P
    std::vector<uint8_t> computePublicKey(const std::vector<uint8_t>& private_key);

    // Compute shared secret: S = Yb^Xa mod P
    std::vector<uint8_t> computeSharedSecret(
        const std::vector<uint8_t>& their_public_key,
        const std::vector<uint8_t>& our_private_key
    );

    // Get P and G
    static const std::vector<uint8_t>& getP() { return DH_P; }
    static uint8_t getG() { return DH_G; }

private:
    // Helper for modular exponentiation: base^exp mod mod
    std::vector<uint8_t> modPow(
        const std::vector<uint8_t>& base,
        const std::vector<uint8_t>& exponent,
        const std::vector<uint8_t>& modulus
    );

    // Big integer arithmetic helpers
    std::vector<uint8_t> bigIntMultiply(
        const std::vector<uint8_t>& a,
        const std::vector<uint8_t>& b
    );

    std::vector<uint8_t> bigIntMod(
        const std::vector<uint8_t>& a,
        const std::vector<uint8_t>& mod
    );

    // Compare big integers: returns -1 if a < b, 0 if equal, 1 if a > b
    int bigIntCompare(
        const std::vector<uint8_t>& a,
        const std::vector<uint8_t>& b
    );

    // Subtract big integers: a - b (assumes a >= b)
    std::vector<uint8_t> bigIntSubtract(
        const std::vector<uint8_t>& a,
        const std::vector<uint8_t>& b
    );
};

// ============================================================================
// MSE/PE Key Derivation
// ============================================================================

// Derive encryption keys from shared secret and SKEY (info_hash)
// For initiator:
//   - outgoing_key = SHA1("keyA" + S + SKEY)
//   - incoming_key = SHA1("keyB" + S + SKEY)
// For responder (swap A and B)
struct MSEKeys {
    std::vector<uint8_t> outgoing_key;
    std::vector<uint8_t> incoming_key;
};

MSEKeys deriveKeys(
    const std::vector<uint8_t>& shared_secret,
    const std::vector<uint8_t>& skey,  // info_hash
    bool is_initiator
);

// ============================================================================
// HMAC-SHA1
// ============================================================================

std::vector<uint8_t> hmacSha1(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& data
);

// ============================================================================
// XOR operation
// ============================================================================

std::vector<uint8_t> xorBytes(
    const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b
);

// ============================================================================
// Padding generation
// ============================================================================

// Generate random padding (0-max_len bytes)
std::vector<uint8_t> generatePadding(size_t max_len = 512);

// ============================================================================
// Utilities
// ============================================================================

// Convert integer to big-endian bytes
std::vector<uint8_t> uint32ToBigEndian(uint32_t value);
std::vector<uint8_t> uint16ToBigEndian(uint16_t value);

// Convert big-endian bytes to integer
uint32_t bigEndianToUint32(const uint8_t* data);
uint16_t bigEndianToUint16(const uint8_t* data);

} // namespace crypto
} // namespace torrent
