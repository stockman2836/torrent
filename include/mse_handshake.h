#pragma once

#include "crypto.h"
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>

namespace torrent {

// Forward declaration
class PeerConnection;

// ============================================================================
// MSE/PE Handshake Protocol (BEP 8)
// ============================================================================

class MSEHandshake {
public:
    // Encryption mode preference
    enum class Mode {
        PREFER_PLAINTEXT,      // Try plaintext first, fall back to RC4
        PREFER_ENCRYPTED,      // Try RC4 first, fall back to plaintext
        REQUIRE_ENCRYPTED      // Only RC4, reject plaintext peers
    };

    // Encryption method flags (can be ORed together in crypto_provide)
    enum CryptoMethod : uint32_t {
        PLAINTEXT = 0x01,
        RC4 = 0x02
    };

    // Handshake result
    struct Result {
        bool success = false;
        CryptoMethod selected_method = PLAINTEXT;
        std::vector<uint8_t> initial_payload;  // Any IA data from packet 3
        std::string error_message;
    };

    MSEHandshake(Mode mode, const std::vector<uint8_t>& info_hash);
    ~MSEHandshake();

    // Perform handshake as initiator (client)
    Result performHandshakeInitiator(PeerConnection& conn);

    // Perform handshake as responder (server)
    Result performHandshakeResponder(PeerConnection& conn);

    // Get encryption mode
    Mode getMode() const { return mode_; }

    // Check if method is acceptable based on mode
    bool isMethodAcceptable(CryptoMethod method) const;

private:
    // ========================================================================
    // Handshake steps (Initiator)
    // ========================================================================

    // Step 1: Send Ya + PadA
    bool sendPacket1(PeerConnection& conn);

    // Step 2: Receive Yb + PadB
    bool receivePacket2(PeerConnection& conn);

    // Step 3: Send encrypted verification and negotiation
    bool sendPacket3(PeerConnection& conn, const std::vector<uint8_t>& initial_payload);

    // Step 4: Receive encrypted response
    bool receivePacket4(PeerConnection& conn);

    // ========================================================================
    // Handshake steps (Responder)
    // ========================================================================

    // Step 1: Receive Ya + PadA
    bool receivePacket1Responder(PeerConnection& conn);

    // Step 2: Send Yb + PadB
    bool sendPacket2Responder(PeerConnection& conn);

    // Step 3: Receive encrypted verification
    bool receivePacket3Responder(PeerConnection& conn);

    // Step 4: Send encrypted response
    bool sendPacket4Responder(PeerConnection& conn);

    // ========================================================================
    // Helper functions
    // ========================================================================

    // Compute HASH("req1", S)
    std::vector<uint8_t> computeReq1() const;

    // Compute HASH("req2", SKEY)
    std::vector<uint8_t> computeReq2() const;

    // Compute HASH("req3", S)
    std::vector<uint8_t> computeReq3() const;

    // Search for synchronization pattern in stream
    // Returns position if found, -1 otherwise
    int findSyncPattern(const std::vector<uint8_t>& data,
                        const std::vector<uint8_t>& pattern,
                        size_t max_search = 65536);

    // Determine crypto_provide flags based on mode
    uint32_t getCryptoProvide() const;

    // Select best method from peer's crypto_provide
    CryptoMethod selectCryptoMethod(uint32_t peer_crypto_provide) const;

    // Read exactly N bytes from connection (blocking)
    bool readExactly(PeerConnection& conn, uint8_t* buffer, size_t length);

    // Read with timeout and potential padding search
    bool readWithPadding(PeerConnection& conn, std::vector<uint8_t>& buffer,
                        size_t min_length, size_t max_length);

    // ========================================================================
    // Member variables
    // ========================================================================

    Mode mode_;
    std::vector<uint8_t> info_hash_;  // SKEY

    // DH key exchange
    crypto::DiffieHellman dh_;
    std::vector<uint8_t> private_key_;
    std::vector<uint8_t> public_key_;
    std::vector<uint8_t> shared_secret_;

    // Encryption keys
    crypto::MSEKeys keys_;

    // RC4 ciphers (initialized after key derivation)
    std::unique_ptr<crypto::RC4> outgoing_cipher_;
    std::unique_ptr<crypto::RC4> incoming_cipher_;

    // Selected encryption method
    CryptoMethod selected_method_ = PLAINTEXT;

    // Verification constant (8 zero bytes)
    static const std::vector<uint8_t> VC;
};

// ============================================================================
// Encrypted Stream Wrapper
// ============================================================================

class EncryptedStream {
public:
    EncryptedStream();
    ~EncryptedStream();

    // Initialize with encryption method and ciphers
    void init(MSEHandshake::CryptoMethod method,
             std::unique_ptr<crypto::RC4> outgoing_cipher,
             std::unique_ptr<crypto::RC4> incoming_cipher);

    // Encrypt data before sending
    void encrypt(uint8_t* data, size_t length);
    void encrypt(std::vector<uint8_t>& data);

    // Decrypt data after receiving
    void decrypt(uint8_t* data, size_t length);
    void decrypt(std::vector<uint8_t>& data);

    // Check if encryption is active
    bool isEncrypted() const { return method_ == MSEHandshake::CryptoMethod::RC4; }

    // Get current method
    MSEHandshake::CryptoMethod getMethod() const { return method_; }

private:
    MSEHandshake::CryptoMethod method_;
    std::unique_ptr<crypto::RC4> outgoing_cipher_;
    std::unique_ptr<crypto::RC4> incoming_cipher_;
};

} // namespace torrent
