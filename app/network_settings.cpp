#include "network_settings.h"
#include <charconv>
#include <stdexcept>

namespace torrent::app {
namespace {
int number(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) throw std::invalid_argument("Enter a whole number");
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Enter a whole number");
    return value;
}
}

NetworkSettings NetworkSettings::fromConfig(const Config& config) {
    return {config.listen_port, config.max_peers, config.enable_dht, config.enable_pex};
}

NetworkSettings NetworkSettings::parse(std::string_view port, std::string_view peers, bool dht, bool pex) {
    NetworkSettings result{number(port), number(peers), dht, pex};
    result.validate();
    return result;
}

void NetworkSettings::validate(size_t downloads) const {
    // The desktop assigns separate TCP/DHT port pairs to concurrent jobs.
    if (first_port < 1024 || first_port > 65534)
        throw std::invalid_argument("The first port must be between 1024 and 65534");
    if (max_peers < 1 || max_peers > 1000)
        throw std::invalid_argument("The peer limit must be between 1 and 1000");
    if (downloads > static_cast<size_t>((65536 - first_port) / 2))
        throw std::invalid_argument("Not enough port pairs for the download list; choose a lower first port");
}

void NetworkSettings::applyTo(Config& config) const {
    validate();
    config.listen_port = static_cast<uint16_t>(first_port);
    config.dht_port = static_cast<uint16_t>(first_port + 1);
    config.max_peers = max_peers;
    config.enable_dht = enable_dht;
    config.enable_pex = enable_pex;
}

} // namespace torrent::app
