#include "mse_handshake.h"
#include "peer_connection.h"
#include "utils.h"
#include "logger.h"
#include <cstring>
#include <algorithm>

namespace torrent {

// Verification constant (8 zero bytes)
const std::vector<uint8_t> MSEHandshake::VC = {0, 0, 0, 0, 0, 0, 0, 0};

// ============================================================================
// Constructor / Destructor
// ============================================================================

MSEHandshake::MSEHandshake(Mode mode, const std::vector<uint8_t>& info_hash)
    : mode_(mode)
    , info_hash_(info_hash)
{
    if (info_hash_.size() != 20) {
        throw std::invalid_argument("Info hash must be 20 bytes");
    }
}

MSEHandshake::~MSEHandshake() = default;

// ============================================================================
// Public API - Initiator
// ============================================================================

MSEHandshake::Result MSEHandshake::performHandshakeInitiator(PeerConnection& conn) {
    Result result;

    try {
        LOG_DEBUG("MSE: Starting handshake as initiator");

        // Step 1: Generate DH keys and send Ya + PadA
        private_key_ = dh_.generatePrivateKey(160);
        public_key_ = dh_.computePublicKey(private_key_);

        if (!sendPacket1(conn)) {
            result.error_message = "Failed to send packet 1 (Ya + PadA)";
            return result;
        }

        // Step 2: Receive Yb + PadB
        if (!receivePacket2(conn)) {
            result.error_message = "Failed to receive packet 2 (Yb + PadB)";
            return result;
        }

        // Compute shared secret
        shared_secret_ = dh_.computeSharedSecret(
            std::vector<uint8_t>(public_key_.begin() + 96, public_key_.end()),
            private_key_
        );

        // Derive encryption keys
        keys_ = crypto::deriveKeys(shared_secret_, info_hash_, true);

        // Initialize RC4 ciphers
        outgoing_cipher_ = std::make_unique<crypto::RC4>();
        incoming_cipher_ = std::make_unique<crypto::RC4>();
        outgoing_cipher_->init(keys_.outgoing_key);
        incoming_cipher_->init(keys_.incoming_key);
        outgoing_cipher_->discard(1024);
        incoming_cipher_->discard(1024);

        // Step 3: Send encrypted HASH("req1", S) + negotiation
        std::vector<uint8_t> initial_payload;  // Can include BT handshake
        if (!sendPacket3(conn, initial_payload)) {
            result.error_message = "Failed to send packet 3 (encrypted negotiation)";
            return result;
        }

        // Step 4: Receive encrypted response
        if (!receivePacket4(conn)) {
            result.error_message = "Failed to receive packet 4 (encrypted response)";
            return result;
        }

        // Success!
        result.success = true;
        result.selected_method = selected_method_;
        LOG_INFO("MSE: Handshake completed as initiator, method: {}",
                selected_method_ == RC4 ? "RC4" : "plaintext");

    } catch (const std::exception& e) {
        result.error_message = std::string("MSE handshake exception: ") + e.what();
        LOG_ERROR("MSE: {}", result.error_message);
    }

    return result;
}

// ============================================================================
// Public API - Responder
// ============================================================================

MSEHandshake::Result MSEHandshake::performHandshakeResponder(PeerConnection& conn) {
    Result result;

    try {
        LOG_DEBUG("MSE: Starting handshake as responder");

        // Generate our DH keys
        private_key_ = dh_.generatePrivateKey(160);
        public_key_ = dh_.computePublicKey(private_key_);

        // Step 1: Receive Ya + PadA
        if (!receivePacket1Responder(conn)) {
            result.error_message = "Failed to receive packet 1 (Ya + PadA)";
            return result;
        }

        // Step 2: Send Yb + PadB
        if (!sendPacket2Responder(conn)) {
            result.error_message = "Failed to send packet 2 (Yb + PadB)";
            return result;
        }

        // Derive encryption keys
        keys_ = crypto::deriveKeys(shared_secret_, info_hash_, false);

        // Initialize RC4 ciphers
        outgoing_cipher_ = std::make_unique<crypto::RC4>();
        incoming_cipher_ = std::make_unique<crypto::RC4>();
        outgoing_cipher_->init(keys_.outgoing_key);
        incoming_cipher_->init(keys_.incoming_key);
        outgoing_cipher_->discard(1024);
        incoming_cipher_->discard(1024);

        // Step 3: Receive encrypted verification
        if (!receivePacket3Responder(conn)) {
            result.error_message = "Failed to receive packet 3 (encrypted verification)";
            return result;
        }

        // Step 4: Send encrypted response
        if (!sendPacket4Responder(conn)) {
            result.error_message = "Failed to send packet 4 (encrypted response)";
            return result;
        }

        result.success = true;
        result.selected_method = selected_method_;
        LOG_INFO("MSE: Handshake completed as responder, method: {}",
                selected_method_ == RC4 ? "RC4" : "plaintext");

    } catch (const std::exception& e) {
        result.error_message = std::string("MSE handshake exception: ") + e.what();
        LOG_ERROR("MSE: {}", result.error_message);
    }

    return result;
}

// ============================================================================
// Initiator - Packet 1: Send Ya + PadA
// ============================================================================

bool MSEHandshake::sendPacket1(PeerConnection& conn) {
    LOG_DEBUG("MSE: Sending packet 1 (Ya + PadA)");

    // Public key is 96 bytes
    if (public_key_.size() != 96) {
        public_key_.resize(96, 0);
    }

    // Generate random padding (0-512 bytes)
    auto padding = crypto::generatePadding(512);

    // Send: Ya (96 bytes) + PadA
    std::vector<uint8_t> packet;
    packet.insert(packet.end(), public_key_.begin(), public_key_.end());
    packet.insert(packet.end(), padding.begin(), padding.end());

    LOG_DEBUG("MSE: Packet 1 size: {} bytes ({} Ya + {} PadA)",
             packet.size(), 96, padding.size());

    return conn.sendData(packet.data(), packet.size());
}

// ============================================================================
// Initiator - Packet 2: Receive Yb + PadB
// ============================================================================

bool MSEHandshake::receivePacket2(PeerConnection& conn) {
    LOG_DEBUG("MSE: Receiving packet 2 (Yb + PadB)");

    // Read 96 bytes for Yb
    std::vector<uint8_t> yb(96);
    if (!readExactly(conn, yb.data(), 96)) {
        LOG_ERROR("MSE: Failed to read Yb (96 bytes)");
        return false;
    }

    // Read potential padding (0-512 bytes)
    // We need to read until we can compute shared secret and verify next packet
    // For simplicity, try to read up to 512 more bytes
    std::vector<uint8_t> padding_buffer(512);
    // Try to read padding, but it's okay if there's less
    // This is a simplified approach; real implementation would be more sophisticated

    LOG_DEBUG("MSE: Received Yb (96 bytes)");

    // Compute shared secret
    shared_secret_ = dh_.computeSharedSecret(yb, private_key_);

    LOG_DEBUG("MSE: Computed shared secret ({} bytes)", shared_secret_.size());

    return true;
}

// ============================================================================
// Initiator - Packet 3: Send encrypted verification
// ============================================================================

bool MSEHandshake::sendPacket3(PeerConnection& conn,
                               const std::vector<uint8_t>& initial_payload) {
    LOG_DEBUG("MSE: Sending packet 3 (encrypted verification)");

    std::vector<uint8_t> packet;

    // HASH("req1", S)
    auto req1 = computeReq1();
    packet.insert(packet.end(), req1.begin(), req1.end());

    // HASH("req2", SKEY) xor HASH("req3", S)
    auto req2 = computeReq2();
    auto req3 = computeReq3();
    auto xored = crypto::xorBytes(req2, req3);
    packet.insert(packet.end(), xored.begin(), xored.end());

    // Encrypt everything from here
    size_t encrypt_start = packet.size();

    // VC (8 zero bytes)
    packet.insert(packet.end(), VC.begin(), VC.end());

    // crypto_provide (4 bytes, big-endian)
    uint32_t crypto_provide = getCryptoProvide();
    auto crypto_provide_bytes = crypto::uint32ToBigEndian(crypto_provide);
    packet.insert(packet.end(), crypto_provide_bytes.begin(), crypto_provide_bytes.end());

    // len(PadC) (2 bytes)
    auto padding = crypto::generatePadding(512);
    auto pad_len = crypto::uint16ToBigEndian(static_cast<uint16_t>(padding.size()));
    packet.insert(packet.end(), pad_len.begin(), pad_len.end());

    // PadC
    packet.insert(packet.end(), padding.begin(), padding.end());

    // len(IA) (2 bytes)
    auto ia_len = crypto::uint16ToBigEndian(static_cast<uint16_t>(initial_payload.size()));
    packet.insert(packet.end(), ia_len.begin(), ia_len.end());

    // IA (initial application data)
    if (!initial_payload.empty()) {
        packet.insert(packet.end(), initial_payload.begin(), initial_payload.end());
    }

    // Encrypt from VC onwards
    outgoing_cipher_->crypt(packet.data() + encrypt_start, packet.size() - encrypt_start);

    LOG_DEBUG("MSE: Packet 3 size: {} bytes ({} plaintext + {} encrypted)",
             packet.size(), encrypt_start, packet.size() - encrypt_start);

    return conn.sendData(packet.data(), packet.size());
}

// ============================================================================
// Initiator - Packet 4: Receive encrypted response
// ============================================================================

bool MSEHandshake::receivePacket4(PeerConnection& conn) {
    LOG_DEBUG("MSE: Receiving packet 4 (encrypted response)");

    // Read encrypted data: VC (8) + crypto_select (4) + len(PadD) (2) = 14 bytes minimum
    std::vector<uint8_t> buffer(65536);  // Read up to 64K
    size_t bytes_read = 0;

    // We need to search for VC (8 zero bytes) after decryption
    // Read in chunks and decrypt
    const size_t chunk_size = 1024;
    bool found_vc = false;
    size_t vc_position = 0;

    while (!found_vc && bytes_read < buffer.size()) {
        size_t to_read = std::min(chunk_size, buffer.size() - bytes_read);

        if (!readExactly(conn, buffer.data() + bytes_read, to_read)) {
            LOG_ERROR("MSE: Failed to read packet 4 data");
            return false;
        }

        // Decrypt what we just read
        incoming_cipher_->crypt(buffer.data() + bytes_read, to_read);
        bytes_read += to_read;

        // Search for VC
        for (size_t i = 0; i + 8 <= bytes_read; ++i) {
            if (std::memcmp(buffer.data() + i, VC.data(), 8) == 0) {
                found_vc = true;
                vc_position = i;
                LOG_DEBUG("MSE: Found VC at position {}", vc_position);
                break;
            }
        }
    }

    if (!found_vc) {
        LOG_ERROR("MSE: Could not find VC in packet 4");
        return false;
    }

    // VC starts at vc_position
    // After VC: crypto_select (4 bytes)
    if (vc_position + 8 + 4 > bytes_read) {
        LOG_ERROR("MSE: Insufficient data after VC");
        return false;
    }

    uint32_t crypto_select = crypto::bigEndianToUint32(buffer.data() + vc_position + 8);
    LOG_DEBUG("MSE: Peer crypto_select: 0x{:08x}", crypto_select);

    // Verify selected method
    if (crypto_select == RC4) {
        selected_method_ = RC4;
    } else if (crypto_select == PLAINTEXT) {
        selected_method_ = PLAINTEXT;
    } else {
        LOG_ERROR("MSE: Invalid crypto_select: 0x{:08x}", crypto_select);
        return false;
    }

    // Check if selected method is acceptable
    if (!isMethodAcceptable(selected_method_)) {
        LOG_ERROR("MSE: Peer selected unacceptable method: {}",
                 selected_method_ == RC4 ? "RC4" : "plaintext");
        return false;
    }

    LOG_DEBUG("MSE: Selected encryption method: {}",
             selected_method_ == RC4 ? "RC4" : "plaintext");

    return true;
}

// ============================================================================
// Responder - Packet 1: Receive Ya + PadA
// ============================================================================

bool MSEHandshake::receivePacket1Responder(PeerConnection& conn) {
    LOG_DEBUG("MSE: Receiving packet 1 as responder (Ya + PadA)");

    // Read 96 bytes for Ya
    std::vector<uint8_t> ya(96);
    if (!readExactly(conn, ya.data(), 96)) {
        LOG_ERROR("MSE: Failed to read Ya");
        return false;
    }

    LOG_DEBUG("MSE: Received Ya (96 bytes)");

    // Compute shared secret
    shared_secret_ = dh_.computeSharedSecret(ya, private_key_);

    LOG_DEBUG("MSE: Computed shared secret ({} bytes)", shared_secret_.size());

    // Note: PadA follows Ya, but we'll handle it when reading next packet
    return true;
}

// ============================================================================
// Responder - Packet 2: Send Yb + PadB
// ============================================================================

bool MSEHandshake::sendPacket2Responder(PeerConnection& conn) {
    LOG_DEBUG("MSE: Sending packet 2 as responder (Yb + PadB)");

    // Ensure public key is 96 bytes
    if (public_key_.size() != 96) {
        public_key_.resize(96, 0);
    }

    // Generate random padding
    auto padding = crypto::generatePadding(512);

    // Send: Yb (96 bytes) + PadB
    std::vector<uint8_t> packet;
    packet.insert(packet.end(), public_key_.begin(), public_key_.end());
    packet.insert(packet.end(), padding.begin(), padding.end());

    LOG_DEBUG("MSE: Packet 2 size: {} bytes ({} Yb + {} PadB)",
             packet.size(), 96, padding.size());

    return conn.sendData(packet.data(), packet.size());
}

// ============================================================================
// Responder - Packet 3: Receive encrypted verification
// ============================================================================

bool MSEHandshake::receivePacket3Responder(PeerConnection& conn) {
    LOG_DEBUG("MSE: Receiving packet 3 as responder (encrypted verification)");

    // Read: HASH("req1", S) (20) + HASH("req2", SKEY) xor HASH("req3", S) (20)
    //       + encrypted: VC (8) + crypto_provide (4) + len(PadC) (2) + PadC + len(IA) (2) + IA

    std::vector<uint8_t> buffer(65536);
    size_t bytes_read = 0;

    // Read initial 40 bytes (req1 + xored)
    if (!readExactly(conn, buffer.data(), 40)) {
        LOG_ERROR("MSE: Failed to read verification hashes");
        return false;
    }
    bytes_read = 40;

    // Verify HASH("req1", S)
    auto expected_req1 = computeReq1();
    if (std::memcmp(buffer.data(), expected_req1.data(), 20) != 0) {
        LOG_ERROR("MSE: req1 verification failed");
        return false;
    }

    // Extract SKEY from xored value
    // xored = HASH("req2", SKEY) xor HASH("req3", S)
    // We know HASH("req3", S), so: HASH("req2", SKEY) = xored xor HASH("req3", S)
    auto req3 = computeReq3();
    std::vector<uint8_t> xored(buffer.begin() + 20, buffer.begin() + 40);
    auto req2_received = crypto::xorBytes(xored, req3);
    auto req2_expected = computeReq2();

    if (req2_received != req2_expected) {
        LOG_ERROR("MSE: SKEY verification failed");
        return false;
    }

    LOG_DEBUG("MSE: Verification hashes OK");

    // Now read encrypted part
    // Read and decrypt until we find VC
    bool found_vc = false;
    size_t vc_position = 0;

    while (!found_vc && bytes_read < buffer.size()) {
        size_t to_read = std::min<size_t>(1024, buffer.size() - bytes_read);

        if (!readExactly(conn, buffer.data() + bytes_read, to_read)) {
            LOG_ERROR("MSE: Failed to read encrypted data");
            return false;
        }

        // Decrypt
        incoming_cipher_->crypt(buffer.data() + bytes_read, to_read);
        bytes_read += to_read;

        // Search for VC starting from position 40
        for (size_t i = 40; i + 8 <= bytes_read; ++i) {
            if (std::memcmp(buffer.data() + i, VC.data(), 8) == 0) {
                found_vc = true;
                vc_position = i;
                LOG_DEBUG("MSE: Found VC at position {}", vc_position);
                break;
            }
        }
    }

    if (!found_vc) {
        LOG_ERROR("MSE: Could not find VC");
        return false;
    }

    // Parse crypto_provide
    if (vc_position + 8 + 4 > bytes_read) {
        LOG_ERROR("MSE: Insufficient data after VC");
        return false;
    }

    uint32_t peer_crypto_provide = crypto::bigEndianToUint32(buffer.data() + vc_position + 8);
    LOG_DEBUG("MSE: Peer crypto_provide: 0x{:08x}", peer_crypto_provide);

    // Select crypto method
    selected_method_ = selectCryptoMethod(peer_crypto_provide);

    LOG_DEBUG("MSE: Selected method: {}",
             selected_method_ == RC4 ? "RC4" : "plaintext");

    return true;
}

// ============================================================================
// Responder - Packet 4: Send encrypted response
// ============================================================================

bool MSEHandshake::sendPacket4Responder(PeerConnection& conn) {
    LOG_DEBUG("MSE: Sending packet 4 as responder (encrypted response)");

    std::vector<uint8_t> packet;

    // VC (8 zero bytes)
    packet.insert(packet.end(), VC.begin(), VC.end());

    // crypto_select (4 bytes)
    auto crypto_select_bytes = crypto::uint32ToBigEndian(static_cast<uint32_t>(selected_method_));
    packet.insert(packet.end(), crypto_select_bytes.begin(), crypto_select_bytes.end());

    // len(PadD) (2 bytes)
    auto padding = crypto::generatePadding(512);
    auto pad_len = crypto::uint16ToBigEndian(static_cast<uint16_t>(padding.size()));
    packet.insert(packet.end(), pad_len.begin(), pad_len.end());

    // PadD
    packet.insert(packet.end(), padding.begin(), padding.end());

    // Encrypt entire packet
    outgoing_cipher_->crypt(packet.data(), packet.size());

    LOG_DEBUG("MSE: Packet 4 size: {} bytes (encrypted)", packet.size());

    return conn.sendData(packet.data(), packet.size());
}

// ============================================================================
// Helper Functions
// ============================================================================

std::vector<uint8_t> MSEHandshake::computeReq1() const {
    std::vector<uint8_t> data;
    data.insert(data.end(), {'r', 'e', 'q', '1'});
    data.insert(data.end(), shared_secret_.begin(), shared_secret_.end());
    return utils::sha1(data);
}

std::vector<uint8_t> MSEHandshake::computeReq2() const {
    std::vector<uint8_t> data;
    data.insert(data.end(), {'r', 'e', 'q', '2'});
    data.insert(data.end(), info_hash_.begin(), info_hash_.end());
    return utils::sha1(data);
}

std::vector<uint8_t> MSEHandshake::computeReq3() const {
    std::vector<uint8_t> data;
    data.insert(data.end(), {'r', 'e', 'q', '3'});
    data.insert(data.end(), shared_secret_.begin(), shared_secret_.end());
    return utils::sha1(data);
}

uint32_t MSEHandshake::getCryptoProvide() const {
    switch (mode_) {
        case Mode::PREFER_PLAINTEXT:
            return PLAINTEXT | RC4;
        case Mode::PREFER_ENCRYPTED:
            return RC4 | PLAINTEXT;
        case Mode::REQUIRE_ENCRYPTED:
            return RC4;
    }
    return PLAINTEXT | RC4;
}

MSEHandshake::CryptoMethod MSEHandshake::selectCryptoMethod(uint32_t peer_crypto_provide) const {
    // Select based on our mode and peer's capabilities
    switch (mode_) {
        case Mode::PREFER_PLAINTEXT:
            if (peer_crypto_provide & PLAINTEXT) return PLAINTEXT;
            if (peer_crypto_provide & RC4) return RC4;
            break;
        case Mode::PREFER_ENCRYPTED:
        case Mode::REQUIRE_ENCRYPTED:
            if (peer_crypto_provide & RC4) return RC4;
            if (mode_ == Mode::PREFER_ENCRYPTED && (peer_crypto_provide & PLAINTEXT)) {
                return PLAINTEXT;
            }
            break;
    }

    // Fallback
    return (peer_crypto_provide & RC4) ? RC4 : PLAINTEXT;
}

bool MSEHandshake::isMethodAcceptable(CryptoMethod method) const {
    if (mode_ == Mode::REQUIRE_ENCRYPTED && method == PLAINTEXT) {
        return false;
    }
    return true;
}

bool MSEHandshake::readExactly(PeerConnection& conn, uint8_t* buffer, size_t length) {
    return conn.receiveData(buffer, length);
}

// ============================================================================
// EncryptedStream Implementation
// ============================================================================

EncryptedStream::EncryptedStream()
    : method_(MSEHandshake::CryptoMethod::PLAINTEXT)
{
}

EncryptedStream::~EncryptedStream() = default;

void EncryptedStream::init(MSEHandshake::CryptoMethod method,
                           std::unique_ptr<crypto::RC4> outgoing_cipher,
                           std::unique_ptr<crypto::RC4> incoming_cipher)
{
    method_ = method;
    outgoing_cipher_ = std::move(outgoing_cipher);
    incoming_cipher_ = std::move(incoming_cipher);
}

void EncryptedStream::encrypt(uint8_t* data, size_t length) {
    if (method_ == MSEHandshake::CryptoMethod::RC4 && outgoing_cipher_) {
        outgoing_cipher_->crypt(data, length);
    }
}

void EncryptedStream::encrypt(std::vector<uint8_t>& data) {
    encrypt(data.data(), data.size());
}

void EncryptedStream::decrypt(uint8_t* data, size_t length) {
    if (method_ == MSEHandshake::CryptoMethod::RC4 && incoming_cipher_) {
        incoming_cipher_->crypt(data, length);
    }
}

void EncryptedStream::decrypt(std::vector<uint8_t>& data) {
    decrypt(data.data(), data.size());
}

} // namespace torrent
