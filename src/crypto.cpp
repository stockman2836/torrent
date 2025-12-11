#include "crypto.h"
#include "utils.h"
#include <algorithm>
#include <random>
#include <cstring>
#include <stdexcept>

namespace torrent {
namespace crypto {

// ============================================================================
// DH Prime (P) - 768-bit prime from Oakley Group 1 (RFC 2409)
// ============================================================================

const std::vector<uint8_t> DH_P = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xC9, 0x0F, 0xDA, 0xA2, 0x21, 0x68, 0xC2, 0x34,
    0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74,
    0x02, 0x0B, 0xBE, 0xA6, 0x3B, 0x13, 0x9B, 0x22,
    0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B,
    0x30, 0x2B, 0x0A, 0x6D, 0xF2, 0x5F, 0x14, 0x37,
    0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6,
    0xF4, 0x4C, 0x42, 0xE9, 0xA6, 0x3A, 0x36, 0x21,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x05, 0x63
};

const uint8_t DH_G = 2;

// ============================================================================
// RC4 Implementation
// ============================================================================

void RC4::init(const std::vector<uint8_t>& key) {
    init(key.data(), key.size());
}

void RC4::init(const uint8_t* key, size_t key_len) {
    if (key_len == 0 || key_len > 256) {
        throw std::invalid_argument("RC4 key length must be 1-256 bytes");
    }

    // Initialize S array
    for (size_t i = 0; i < 256; ++i) {
        S_[i] = static_cast<uint8_t>(i);
    }

    // Key scheduling algorithm (KSA)
    size_t j = 0;
    for (size_t i = 0; i < 256; ++i) {
        j = (j + S_[i] + key[i % key_len]) % 256;
        std::swap(S_[i], S_[j]);
    }

    i_ = 0;
    j_ = 0;
    initialized_ = true;
}

void RC4::crypt(uint8_t* data, size_t len) {
    if (!initialized_) {
        throw std::runtime_error("RC4 not initialized");
    }

    // Pseudo-random generation algorithm (PRGA)
    for (size_t k = 0; k < len; ++k) {
        i_ = (i_ + 1) % 256;
        j_ = (j_ + S_[i_]) % 256;
        std::swap(S_[i_], S_[j_]);

        uint8_t K = S_[(S_[i_] + S_[j_]) % 256];
        data[k] ^= K;
    }
}

void RC4::crypt(std::vector<uint8_t>& data) {
    crypt(data.data(), data.size());
}

void RC4::discard(size_t n) {
    if (!initialized_) {
        throw std::runtime_error("RC4 not initialized");
    }

    // Generate and discard n bytes
    for (size_t k = 0; k < n; ++k) {
        i_ = (i_ + 1) % 256;
        j_ = (j_ + S_[i_]) % 256;
        std::swap(S_[i_], S_[j_]);
    }
}

void RC4::reset() {
    std::memset(S_, 0, sizeof(S_));
    i_ = 0;
    j_ = 0;
    initialized_ = false;
}

// ============================================================================
// Diffie-Hellman Implementation
// ============================================================================

DiffieHellman::DiffieHellman() {
    // Constructor
}

std::vector<uint8_t> DiffieHellman::generatePrivateKey(size_t bits) {
    if (bits < 128 || bits > 160) {
        bits = 160; // Default to 160 bits
    }

    size_t bytes = (bits + 7) / 8;
    std::vector<uint8_t> key(bytes);

    // Generate random bytes
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint16_t> dist(0, 255);

    for (size_t i = 0; i < bytes; ++i) {
        key[i] = static_cast<uint8_t>(dist(gen));
    }

    // Ensure we don't exceed the bit length
    if (bits % 8 != 0) {
        uint8_t mask = (1 << (bits % 8)) - 1;
        key[0] &= mask;
    }

    return key;
}

std::vector<uint8_t> DiffieHellman::computePublicKey(const std::vector<uint8_t>& private_key) {
    // Public key = G^private_key mod P
    // Since G = 2, this is: 2^private_key mod P

    std::vector<uint8_t> base = {DH_G};
    return modPow(base, private_key, DH_P);
}

std::vector<uint8_t> DiffieHellman::computeSharedSecret(
    const std::vector<uint8_t>& their_public_key,
    const std::vector<uint8_t>& our_private_key)
{
    // Shared secret = their_public^our_private mod P
    return modPow(their_public_key, our_private_key, DH_P);
}

// Modular exponentiation: base^exp mod mod
std::vector<uint8_t> DiffieHellman::modPow(
    const std::vector<uint8_t>& base,
    const std::vector<uint8_t>& exponent,
    const std::vector<uint8_t>& modulus)
{
    // Using square-and-multiply algorithm
    std::vector<uint8_t> result = {1};  // Start with 1
    std::vector<uint8_t> base_mod = bigIntMod(base, modulus);

    // Process each bit of exponent from least significant to most significant
    for (size_t byte_idx = exponent.size(); byte_idx > 0; --byte_idx) {
        uint8_t byte = exponent[byte_idx - 1];

        for (int bit = 0; bit < 8; ++bit) {
            if (byte & (1 << bit)) {
                // result = (result * base_mod) mod modulus
                result = bigIntMultiply(result, base_mod);
                result = bigIntMod(result, modulus);
            }

            // base_mod = (base_mod * base_mod) mod modulus
            base_mod = bigIntMultiply(base_mod, base_mod);
            base_mod = bigIntMod(base_mod, modulus);
        }
    }

    return result;
}

// Big integer multiply (simple grade-school multiplication)
std::vector<uint8_t> DiffieHellman::bigIntMultiply(
    const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b)
{
    std::vector<uint32_t> result(a.size() + b.size(), 0);

    // Multiply each digit
    for (size_t i = 0; i < a.size(); ++i) {
        uint32_t carry = 0;
        for (size_t j = 0; j < b.size(); ++j) {
            uint32_t product = static_cast<uint32_t>(a[a.size() - 1 - i]) *
                              static_cast<uint32_t>(b[b.size() - 1 - j]) +
                              result[i + j] + carry;
            result[i + j] = product & 0xFF;
            carry = product >> 8;
        }
        if (carry > 0) {
            result[i + b.size()] += carry;
        }
    }

    // Convert back to bytes, removing leading zeros
    std::vector<uint8_t> output;
    bool leading_zero = true;
    for (auto it = result.rbegin(); it != result.rend(); ++it) {
        if (leading_zero && *it == 0) continue;
        leading_zero = false;
        output.push_back(static_cast<uint8_t>(*it));
    }

    if (output.empty()) {
        output.push_back(0);
    }

    return output;
}

// Big integer modulo
std::vector<uint8_t> DiffieHellman::bigIntMod(
    const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& mod)
{
    std::vector<uint8_t> result = a;

    // Simple repeated subtraction (inefficient but works for now)
    while (bigIntCompare(result, mod) >= 0) {
        result = bigIntSubtract(result, mod);
    }

    return result;
}

// Compare big integers
int DiffieHellman::bigIntCompare(
    const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b)
{
    // Remove leading zeros
    size_t a_start = 0;
    while (a_start < a.size() && a[a_start] == 0) ++a_start;

    size_t b_start = 0;
    while (b_start < b.size() && b[b_start] == 0) ++b_start;

    size_t a_len = a.size() - a_start;
    size_t b_len = b.size() - b_start;

    if (a_len != b_len) {
        return (a_len > b_len) ? 1 : -1;
    }

    for (size_t i = 0; i < a_len; ++i) {
        if (a[a_start + i] != b[b_start + i]) {
            return (a[a_start + i] > b[b_start + i]) ? 1 : -1;
        }
    }

    return 0;
}

// Subtract big integers (assumes a >= b)
std::vector<uint8_t> DiffieHellman::bigIntSubtract(
    const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b)
{
    std::vector<uint8_t> result = a;
    int borrow = 0;

    for (size_t i = 0; i < b.size() || borrow; ++i) {
        int pos = result.size() - 1 - i;
        int sub = (i < b.size() ? b[b.size() - 1 - i] : 0) + borrow;

        if (result[pos] >= sub) {
            result[pos] -= sub;
            borrow = 0;
        } else {
            result[pos] = 256 + result[pos] - sub;
            borrow = 1;
        }
    }

    // Remove leading zeros
    while (result.size() > 1 && result[0] == 0) {
        result.erase(result.begin());
    }

    return result;
}

// ============================================================================
// MSE Key Derivation
// ============================================================================

MSEKeys deriveKeys(
    const std::vector<uint8_t>& shared_secret,
    const std::vector<uint8_t>& skey,
    bool is_initiator)
{
    MSEKeys keys;

    // Prepare keyA and keyB
    std::vector<uint8_t> keyA_data;
    keyA_data.insert(keyA_data.end(), {'k', 'e', 'y', 'A'});
    keyA_data.insert(keyA_data.end(), shared_secret.begin(), shared_secret.end());
    keyA_data.insert(keyA_data.end(), skey.begin(), skey.end());

    std::vector<uint8_t> keyB_data;
    keyB_data.insert(keyB_data.end(), {'k', 'e', 'y', 'B'});
    keyB_data.insert(keyB_data.end(), shared_secret.begin(), shared_secret.end());
    keyB_data.insert(keyB_data.end(), skey.begin(), skey.end());

    // Compute SHA1 hashes
    auto keyA = utils::sha1(keyA_data);
    auto keyB = utils::sha1(keyB_data);

    if (is_initiator) {
        keys.outgoing_key = keyA;
        keys.incoming_key = keyB;
    } else {
        keys.outgoing_key = keyB;
        keys.incoming_key = keyA;
    }

    return keys;
}

// ============================================================================
// HMAC-SHA1
// ============================================================================

std::vector<uint8_t> hmacSha1(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& data)
{
    const size_t block_size = 64;  // SHA1 block size
    std::vector<uint8_t> key_padded = key;

    // If key is longer than block size, hash it first
    if (key_padded.size() > block_size) {
        key_padded = utils::sha1(key_padded);
    }

    // Pad key to block size
    key_padded.resize(block_size, 0);

    // Create inner and outer padded keys
    std::vector<uint8_t> i_key_pad(block_size);
    std::vector<uint8_t> o_key_pad(block_size);

    for (size_t i = 0; i < block_size; ++i) {
        i_key_pad[i] = key_padded[i] ^ 0x36;
        o_key_pad[i] = key_padded[i] ^ 0x5C;
    }

    // Compute inner hash: H(i_key_pad || data)
    std::vector<uint8_t> inner_data = i_key_pad;
    inner_data.insert(inner_data.end(), data.begin(), data.end());
    auto inner_hash = utils::sha1(inner_data);

    // Compute outer hash: H(o_key_pad || inner_hash)
    std::vector<uint8_t> outer_data = o_key_pad;
    outer_data.insert(outer_data.end(), inner_hash.begin(), inner_hash.end());
    return utils::sha1(outer_data);
}

// ============================================================================
// XOR operation
// ============================================================================

std::vector<uint8_t> xorBytes(
    const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b)
{
    size_t len = std::min(a.size(), b.size());
    std::vector<uint8_t> result(len);

    for (size_t i = 0; i < len; ++i) {
        result[i] = a[i] ^ b[i];
    }

    return result;
}

// ============================================================================
// Padding generation
// ============================================================================

std::vector<uint8_t> generatePadding(size_t max_len) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> len_dist(0, max_len);
    std::uniform_int_distribution<uint16_t> byte_dist(0, 255);

    size_t pad_len = len_dist(gen);
    std::vector<uint8_t> padding(pad_len);

    for (size_t i = 0; i < pad_len; ++i) {
        padding[i] = static_cast<uint8_t>(byte_dist(gen));
    }

    return padding;
}

// ============================================================================
// Utilities
// ============================================================================

std::vector<uint8_t> uint32ToBigEndian(uint32_t value) {
    std::vector<uint8_t> result(4);
    result[0] = (value >> 24) & 0xFF;
    result[1] = (value >> 16) & 0xFF;
    result[2] = (value >> 8) & 0xFF;
    result[3] = value & 0xFF;
    return result;
}

std::vector<uint8_t> uint16ToBigEndian(uint16_t value) {
    std::vector<uint8_t> result(2);
    result[0] = (value >> 8) & 0xFF;
    result[1] = value & 0xFF;
    return result;
}

uint32_t bigEndianToUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint16_t bigEndianToUint16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) |
           static_cast<uint16_t>(data[1]);
}

} // namespace crypto
} // namespace torrent
