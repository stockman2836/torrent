#include "socket_support.h"
#include <fstream>
#include <limits>
#include <sstream>
#ifdef _WIN32
#include <iphlpapi.h>
#endif

namespace torrent::mapping {

void checkCancelled(const Cancelled& cancelled) {
    if (cancelled && cancelled()) throw std::runtime_error("Router request cancelled");
}

std::string defaultGateway() {
    uint32_t gateway = 0;
    unsigned long best = std::numeric_limits<unsigned long>::max();
#ifdef _WIN32
    ULONG size = 0;
    if (GetIpForwardTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        throw std::runtime_error("Cannot read IPv4 routing table");
    std::vector<uint8_t> buffer(size);
    auto table = reinterpret_cast<PMIB_IPFORWARDTABLE>(buffer.data());
    if (GetIpForwardTable(table, &size, FALSE) != NO_ERROR) throw std::runtime_error("Cannot read IPv4 routing table");
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto& route = table->table[index];
        if (route.dwForwardDest == 0 && route.dwForwardMask == 0 && route.dwForwardNextHop &&
            route.dwForwardType == MIB_IPROUTE_TYPE_INDIRECT && route.dwForwardMetric1 < best) {
            gateway = route.dwForwardNextHop; best = route.dwForwardMetric1;
        }
    }
#elif defined(__linux__)
    std::ifstream routes("/proc/net/route");
    std::string line; std::getline(routes, line);
    while (std::getline(routes, line)) {
        std::istringstream row(line);
        std::string interface, destination, next, flags, references, uses;
        unsigned long metric;
        if (!(row >> interface >> destination >> next >> flags >> references >> uses >> metric)) continue;
        if (destination == "00000000" && (std::stoul(flags, nullptr, 16) & 3) == 3 && metric < best) {
            gateway = static_cast<uint32_t>(std::stoul(next, nullptr, 16)); best = metric;
        }
    }
#endif
    if (!gateway) throw std::runtime_error("No supported IPv4 default gateway found");
    in_addr address{}; address.s_addr = gateway; return ipv4(address);
}

std::string localAddress(const std::string& gateway) {
    Socket socket;
    const auto remote = endpoint(gateway, 5351);
    if (::connect(socket.value, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) != 0)
        throw std::runtime_error("Cannot select the router interface");
    sockaddr_in local{}; socklen_t size = sizeof(local);
    if (getsockname(socket.value, reinterpret_cast<sockaddr*>(&local), &size) != 0)
        throw std::runtime_error("Cannot determine the local router interface");
    return ipv4(local.sin_addr); // UDP connect chooses a route without sending a packet.
}

GatewayClient::GatewayClient(PortMappingOptions options) : options_(std::move(options)) {}

void GatewayClient::discover(const Cancelled& cancelled) {
    checkCancelled(cancelled);
    // Initialize Winsock before using address conversion on Windows.
    { Socket socket; }
    gateway_ = options_.gateway.empty() ? defaultGateway() : options_.gateway;
    local_address_ = localAddress(gateway_);
    protocol_ = MappingProtocol::None;
    std::string nat_error;
    try {
        const auto response = udp({0, 0}, 0, cancelled);
        in_addr external{}; std::memcpy(&external, response.data() + 8, 4);
        if (!external.s_addr) throw std::runtime_error("NAT-PMP returned an empty external address");
        external_address_ = ipv4(external);
        protocol_ = MappingProtocol::NatPmp;
        return;
    } catch (const std::exception& error) { nat_error = error.what(); }
    checkCancelled(cancelled);
    try {
        discoverUpnp(cancelled);
        protocol_ = MappingProtocol::Upnp;
    } catch (const std::exception& error) {
        throw std::runtime_error("NAT-PMP: " + nat_error + "; UPnP: " + error.what());
    }
}

} // namespace torrent::mapping
