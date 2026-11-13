#include "socket_support.h"
#include <algorithm>
#include <mutex>

namespace torrent::mapping {
namespace {
uint16_t word(const uint8_t* bytes) { return (uint16_t(bytes[0]) << 8) | bytes[1]; }
uint32_t integer(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
}
}

std::vector<uint8_t> GatewayClient::udp(const std::vector<uint8_t>& request, uint8_t opcode, const Cancelled& cancelled) {
    static std::timed_mutex requests; // RFC 6886: serialize requests from this process.
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.request_timeout_ms);
    std::unique_lock<std::timed_mutex> lock(requests, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(50))) {
        checkCancelled(cancelled);
        if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("NAT-PMP queued request timed out");
    }
    checkCancelled(cancelled);
    Socket socket;
    const auto remote = endpoint(gateway_, options_.natpmp_port);
    if (::connect(socket.value, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) != 0)
        throw std::runtime_error("Cannot contact NAT-PMP gateway");
    auto next = std::chrono::steady_clock::time_point{};
    int delay = 250;
    while (std::chrono::steady_clock::now() < end) {
        checkCancelled(cancelled);
        if (std::chrono::steady_clock::now() >= next) {
            if (send(socket.value, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()), 0) < 0)
                throw std::runtime_error("NAT-PMP send failed");
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
            delay = std::min(delay * 2, 2000);
        }
        if (!readable(socket)) continue;
        std::array<uint8_t, 2048> bytes{};
        const int size = recv(socket.value, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
        if (size < 0) throw std::runtime_error("NAT-PMP receive failed");
        if (size < 8 || bytes[0] != 0 || bytes[1] != opcode + 128) continue;
        const auto result = word(bytes.data() + 2);
        if (result) throw std::runtime_error("NAT-PMP result code " + std::to_string(result));
        if (size < (opcode == 0 ? 12 : 16)) continue;
        if (opcode && word(bytes.data() + 8) != word(request.data() + 4)) continue;
        return {bytes.begin(), bytes.begin() + size};
    }
    throw std::runtime_error("NAT-PMP timed out");
}

Lease GatewayClient::natpmp(PortTransport transport, uint16_t internal, uint16_t external,
                           uint32_t seconds, const Cancelled& cancelled) {
    if (!internal) throw std::invalid_argument("Refusing a wildcard NAT-PMP mapping");
    const uint8_t opcode = transport == PortTransport::Tcp ? 2 : 1;
    const auto response = udp({0, opcode, 0, 0, uint8_t(internal >> 8), uint8_t(internal),
        uint8_t(external >> 8), uint8_t(external), uint8_t(seconds >> 24), uint8_t(seconds >> 16),
        uint8_t(seconds >> 8), uint8_t(seconds)}, opcode, cancelled);
    Lease lease{word(response.data() + 10), integer(response.data() + 12)};
    if (seconds && (!lease.external_port || !lease.seconds)) throw std::runtime_error("NAT-PMP returned an invalid finite lease");
    if (!seconds && (lease.seconds || lease.external_port)) throw std::runtime_error("NAT-PMP did not acknowledge deletion");
    return lease;
}

} // namespace torrent::mapping
