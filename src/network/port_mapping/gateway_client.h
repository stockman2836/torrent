#pragma once

#include "torrent/network/port_mapping/port_mapper.h"
#include <functional>
#include <utility>

namespace torrent::mapping {

using Cancelled = std::function<bool()>;

struct Lease {
    uint16_t external_port = 0;
    uint32_t seconds = 0;
};

class GatewayClient {
public:
    explicit GatewayClient(PortMappingOptions options);
    void discover(const Cancelled& cancelled);
    Lease map(PortTransport transport, uint16_t internal, uint16_t external,
              const std::string& owner, bool& attempted, const Cancelled& cancelled);
    void remove(PortTransport transport, uint16_t internal, uint16_t external,
                const std::string& owner, const Cancelled& cancelled);
    MappingProtocol protocol() const { return protocol_; }
    const std::string& gateway() const { return gateway_; }
    const std::string& externalAddress() const { return external_address_; }
private:
    Lease natpmp(PortTransport transport, uint16_t internal, uint16_t external, uint32_t seconds, const Cancelled& cancelled);
    std::vector<uint8_t> udp(const std::vector<uint8_t>& request, uint8_t opcode, const Cancelled& cancelled);
    std::string discoverUpnp(const Cancelled& cancelled);
    std::string http(const std::string& url, const std::string& action, const std::string& body, const Cancelled& cancelled);
    std::string soap(const std::string& action, const std::vector<std::pair<std::string, std::string>>& arguments, const Cancelled& cancelled);
    std::string sameGatewayUrl(const std::string& base, const std::string& relative = {}) const;
    PortMappingOptions options_;
    MappingProtocol protocol_ = MappingProtocol::None;
    std::string gateway_, local_address_, external_address_, control_url_, service_;
};

std::string defaultGateway();
std::string localAddress(const std::string& gateway);
void checkCancelled(const Cancelled& cancelled);

} // namespace torrent::mapping
