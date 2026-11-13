#pragma once

#include "torrent/core/port_mapping_options.h"
#include <memory>
#include <vector>
#include <chrono>

namespace torrent {

enum class MappingState { Disabled, Discovering, Mapping, Active, Partial, Error, Removing, Stopped };
enum class MappingProtocol { None, NatPmp, Upnp };
enum class PortTransport { Tcp, Udp };

struct MappedPort {
    PortTransport transport = PortTransport::Tcp;
    uint16_t internal_port = 0;
    uint16_t external_port = 0;
    uint32_t lease_seconds = 0;
    bool active = false;
    std::string error;
    std::chrono::steady_clock::time_point expires{};
};

struct PortMappingStatus {
    bool enabled = false;
    MappingState state = MappingState::Disabled;
    MappingProtocol protocol = MappingProtocol::None;
    std::string gateway;
    std::string external_address;
    std::string error;
    std::vector<MappedPort> ports;
};

// Owns one session's finite leases and its worker. Disabled construction sends nothing.
// The caller keeps its local TCP/UDP sockets reserved until stop() returns.
class PortMapper {
public:
    PortMapper(uint16_t tcp_port, uint16_t dht_port = 0, PortMappingOptions options = {});
    ~PortMapper();
    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;
    void setEnabled(bool enabled);
    void stop();
    PortMappingStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace torrent
