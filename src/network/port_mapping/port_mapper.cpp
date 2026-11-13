#include "gateway_client.h"
#include "torrent/core/utils.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace torrent {
using Clock = std::chrono::steady_clock;

struct PortMapper::Impl {
    struct Port {
        MappedPort value;
        bool attempted = false;
        Clock::time_point renew{};
    };
    PortMappingOptions options;
    const std::string owner = "TorrentClient-" + utils::toHex(utils::sha1(utils::generatePeerId()));
    std::vector<Port> ports;
    std::unique_ptr<mapping::GatewayClient> client;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> enabled{false}, stopping{false};
    PortMappingStatus snapshot;
    std::thread worker;

    Impl(uint16_t tcp, uint16_t udp, PortMappingOptions value) : options(std::move(value)) {
        if (!tcp || options.lease_seconds < 2 || options.lease_seconds > 86400 || !options.natpmp_port || !options.discovery_port ||
            options.request_timeout_ms < 20 || options.request_timeout_ms > 10000 || options.discovery_timeout_ms < 20 ||
            options.discovery_timeout_ms > 10000 || options.retry_delay_ms < 100 || options.retry_delay_ms > 60000)
            throw std::invalid_argument("Invalid port mapping options");
        ports.push_back({{PortTransport::Tcp, tcp, 0, 0, false, {}}});
        if (udp) ports.push_back({{PortTransport::Udp, udp, 0, 0, false, {}}});
        for (const auto& port : ports) snapshot.ports.push_back(port.value);
    }

    void publish(MappingState state, std::string error = {}) {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.enabled = enabled;
        snapshot.state = state;
        snapshot.error = std::move(error);
        if (client) {
            snapshot.protocol = client->protocol();
            snapshot.gateway = client->gateway();
            snapshot.external_address = client->externalAddress();
        }
        snapshot.ports.clear();
        for (const auto& port : ports) snapshot.ports.push_back(port.value);
    }

    std::string cleanup() {
        if (!client) return {};
        publish(MappingState::Removing);
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        const auto expired = [deadline] { return Clock::now() >= deadline; };
        std::string error;
        for (auto& port : ports) {
            if (port.attempted) {
                try {
                    client->remove(port.value.transport, port.value.internal_port,
                        port.value.external_port ? port.value.external_port : port.value.internal_port, owner, expired);
                    port.value.error.clear();
                } catch (const std::exception& failure) {
                    port.value.error = std::string("Cleanup: ") + failure.what();
                    if (error.empty()) error = port.value.error;
                }
            }
            port.attempted = false;
            port.value.active = false;
            port.renew = {};
        }
        client.reset();
        return error;
    }

    void run() {
        const auto cancelled = [this] { return stopping.load() || !enabled.load(); };
        for (;;) {
            if (cancelled()) {
                const auto error = cleanup();
                publish(stopping ? MappingState::Stopped : MappingState::Disabled, error);
                if (stopping) return;
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return enabled.load() || stopping.load(); });
                continue;
            }
            if (!client) {
                client = std::make_unique<mapping::GatewayClient>(options);
                publish(MappingState::Discovering);
                try { client->discover(cancelled); }
                catch (const std::exception& failure) {
                    publish(MappingState::Error, failure.what());
                    client.reset();
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait_for(lock, std::chrono::milliseconds(options.retry_delay_ms), cancelled);
                    continue;
                }
            }
            auto next = Clock::now() + std::chrono::hours(24);
            for (auto& port : ports) {
                if (cancelled()) break;
                if (Clock::now() >= port.renew) {
                    publish(MappingState::Mapping);
                    try {
                        const auto requested = Clock::now();
                        const auto lease = client->map(port.value.transport, port.value.internal_port,
                            port.value.external_port ? port.value.external_port : port.value.internal_port, owner, port.attempted, cancelled);
                        const auto expires = requested + std::chrono::seconds(lease.seconds);
                        if (expires <= Clock::now()) throw std::runtime_error("Router lease expired before acknowledgement");
                        port.value.external_port = lease.external_port;
                        port.value.lease_seconds = lease.seconds;
                        port.value.expires = expires;
                        port.value.active = true;
                        port.value.error.clear();
                        port.renew = requested + std::chrono::milliseconds(int64_t(lease.seconds) * 500);
                    } catch (const std::exception& failure) {
                        port.value.active = false;
                        port.value.error = failure.what();
                        port.renew = Clock::now() + std::chrono::milliseconds(options.retry_delay_ms);
                    }
                }
                next = std::min(next, port.renew);
            }
            if (cancelled()) continue;
            const auto active = std::count_if(ports.begin(), ports.end(), [](const auto& port) { return port.value.active; });
            std::string error;
            for (const auto& port : ports) if (!port.value.error.empty()) { error = port.value.error; break; }
            publish(active == static_cast<std::ptrdiff_t>(ports.size()) ? MappingState::Active : active ? MappingState::Partial : MappingState::Error, error);
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait_until(lock, next, cancelled);
        }
    }
};

PortMapper::PortMapper(uint16_t tcp, uint16_t udp, PortMappingOptions options)
    : impl_(std::make_unique<Impl>(tcp, udp, std::move(options))) {}
PortMapper::~PortMapper() { stop(); }

void PortMapper::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping) return;
    impl_->enabled = enabled;
    impl_->snapshot.enabled = enabled;
    if (enabled && !impl_->worker.joinable()) impl_->worker = std::thread([this] { impl_->run(); });
    impl_->changed.notify_all();
}

void PortMapper::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->changed.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

PortMappingStatus PortMapper::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result = impl_->snapshot;
    bool expired = false;
    for (auto& port : result.ports) if (port.active && Clock::now() >= port.expires) {
        port.active = false;
        port.error = "Router lease expired before renewal was acknowledged";
        result.error = port.error; expired = true;
    }
    if (expired && result.state != MappingState::Removing) {
        const bool active = std::any_of(result.ports.begin(), result.ports.end(), [](const auto& port) { return port.active; });
        result.state = active ? MappingState::Partial : MappingState::Error;
    }
    return result;
}

} // namespace torrent
