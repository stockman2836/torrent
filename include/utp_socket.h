#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <memory>

// BEP 29: uTP (Micro Transport Protocol) Integration
// This is a wrapper around libutp (https://github.com/bittorrent/libutp)
//
// uTP is a UDP-based transport protocol with LEDBAT congestion control
// that provides better network behavior than TCP for BitTorrent traffic

namespace torrent {
namespace utp {

// Forward declarations for libutp types
struct UTPSocket;
struct utp_callback_arguments;

// ============================================================================
// uTP Socket Wrapper
// ============================================================================

class UtpSocket {
public:
    UtpSocket();
    ~UtpSocket();

    // Connection management
    bool connect(const std::string& host, uint16_t port);
    bool listen(uint16_t port);
    void close();

    // Data transmission
    ssize_t send(const uint8_t* data, size_t length);
    ssize_t recv(uint8_t* buffer, size_t buffer_size);

    // Check if connected
    bool isConnected() const;

    // Get statistics
    uint64_t getBytesSent() const { return bytes_sent_; }
    uint64_t getBytesReceived() const { return bytes_received_; }

    // Set callbacks
    using OnConnectCallback = std::function<void()>;
    using OnDataCallback = std::function<void(const uint8_t*, size_t)>;
    using OnErrorCallback = std::function<void(int error_code)>;

    void setOnConnect(OnConnectCallback callback) { on_connect_ = callback; }
    void setOnData(OnDataCallback callback) { on_data_ = callback; }
    void setOnError(OnErrorCallback callback) { on_error_ = callback; }

private:
    friend class UtpManager;

    // Pointer to libutp socket (opaque)
    UTPSocket* utp_socket_;

    // Statistics
    uint64_t bytes_sent_;
    uint64_t bytes_received_;

    // Callbacks
    OnConnectCallback on_connect_;
    OnDataCallback on_data_;
    OnErrorCallback on_error_;

    // Internal callbacks for libutp
    static uint64 onRead(utp_callback_arguments* args);
    static uint64 onWrite(utp_callback_arguments* args);
    static uint64 onState(utp_callback_arguments* args);
    static uint64 onError(utp_callback_arguments* args);
};

// ============================================================================
// uTP Manager (manages libutp context)
// ============================================================================

class UtpManager {
public:
    UtpManager(uint16_t port = 0);
    ~UtpManager();

    // Create new uTP socket
    std::shared_ptr<UtpSocket> createSocket();

    // Start listening for incoming connections
    bool startListening(uint16_t port);

    // Stop listening
    void stopListening();

    // Process network events (call periodically)
    void tick();

    // Check if uTP is available
    static bool isAvailable();

    // Get statistics
    uint64_t getTotalBytesSent() const { return total_bytes_sent_; }
    uint64_t getTotalBytesReceived() const { return total_bytes_received_; }

private:
    // libutp context (opaque pointer)
    void* utp_context_;

    // UDP socket for libutp
    int udp_socket_;
    uint16_t port_;

    // Statistics
    uint64_t total_bytes_sent_;
    uint64_t total_bytes_received_;

    // Internal
    void initializeLibutp();
    void shutdownLibutp();
};

} // namespace utp
} // namespace torrent

// ============================================================================
// Compile-time check for libutp availability
// ============================================================================

#ifdef HAVE_LIBUTP
#include <utp.h>

namespace torrent {
namespace utp {
    // libutp is available - full implementation
    constexpr bool UTP_AVAILABLE = true;
}
}

#else
// libutp not available - stub implementation

namespace torrent {
namespace utp {
    constexpr bool UTP_AVAILABLE = false;

    // Stub types for when libutp is not available
    using UTPSocket = void;
    struct utp_callback_arguments {};
    using uint64 = uint64_t;
}
}

// Warn at compile time
#warning "libutp not found - uTP support will be disabled. Install libutp to enable uTP."

#endif // HAVE_LIBUTP
