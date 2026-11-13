#include "network_diagnostics.h"
#include <gtest/gtest.h>

using namespace torrent;
using namespace torrent::app;

namespace {
DownloadSnapshot active() {
    DownloadSnapshot state;
    state.state = DownloadState::Downloading;
    state.network.listener = {ListenerState::Listening, 24567, true, {}};
    return state;
}
bool contains(const std::string& text, const char* value) { return text.find(value) != std::string::npos; }
}

TEST(NetworkDiagnostics, LocalListeningAndIncomingEvidenceDoNotClaimInternetReachability) {
    auto state = active();
    state.network.incoming_handshakes = 3;
    state.network.loopback_handshakes = 2;
    const auto diagnosis = diagnoseNetwork(state);
    EXPECT_TRUE(contains(diagnosis.port, "24567"));
    EXPECT_TRUE(contains(diagnosis.port, "IPv4 + IPv6"));
    EXPECT_TRUE(contains(diagnosis.port, u8"локально слушается"));
    EXPECT_TRUE(contains(diagnosis.port, u8"Доступность из интернета: не проверена"));
    EXPECT_TRUE(contains(diagnosis.incoming, u8"с этого ПК: 2"));
    EXPECT_TRUE(contains(diagnosis.incoming, u8"с других адресов: 1"));
    EXPECT_TRUE(contains(networkReport(state), u8"из локальной сети"));
}

TEST(NetworkDiagnostics, DistinguishesStartupPortFailureAndClosedSession) {
    auto state = active();
    state.state = DownloadState::Ready;
    state.network.listener.state = ListenerState::NotStarted;
    EXPECT_TRUE(contains(diagnoseNetwork(state).port, u8"при запуске загрузки"));
    state.state = DownloadState::Failed;
    state.network.listener.state = ListenerState::Error;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"TCP-порт"));
    EXPECT_TRUE(contains(diagnoseNetwork(state).advice, u8"начальный порт"));
    state.network.listener.state = ListenerState::Listening;
    EXPECT_TRUE(contains(diagnoseNetwork(state).port, u8"закрыт")); // Last snapshot after a later session failure.
    state.network.listener.state = ListenerState::Stopped;
    state.state = DownloadState::Stopped;
    EXPECT_TRUE(contains(diagnoseNetwork(state).port, u8"закрыт"));
    EXPECT_TRUE(contains(diagnoseNetwork(state).advice, u8"последнего сеанса"));
}

TEST(NetworkDiagnostics, DistinguishesPeerPermissionPendingRequestsAndSeeding) {
    auto state = active();
    state.network.peers.push_back({"127.0.0.1", 12345, PeerState::Connected});
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"не разрешают"));
    state.network.peers[0].choked = false;
    state.network.peers[0].pending_requests = 1;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"запрошенных блоков"));
    state.network.peers[0].pending_requests = 0;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"ожидание данных"));
    state.state = DownloadState::Seeding;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"Раздача"));
    EXPECT_TRUE(contains(diagnoseNetwork(state).advice, u8"запрашивают"));
}

TEST(NetworkDiagnostics, ExplainsMissingDiscoveryEmptyTrackerAndConnectionFailures) {
    auto state = active();
    state.network.pex_enabled = true;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"Нет источника"));
    EXPECT_TRUE(contains(diagnoseNetwork(state).advice, "PEX"));
    state.network.dht_enabled = true;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"узлов DHT"));
    state.network.dht_nodes = 1;
    state.network.tracker.state = TrackerState::Working;
    EXPECT_TRUE(contains(diagnoseNetwork(state).advice, u8"без адресов"));
    state.network.tracker.peers_returned = 3;
    state.network.failed_connections = 3;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"подключиться к найденным пирам"));
    state.network.tracker.state = TrackerState::Announcing;
    state.network.tracker.attempt = 2;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"резервному"));
}

TEST(NetworkDiagnostics, WebSeedTransfersAreVisibleWithoutPeerConnections) {
    auto state = active();
    state.network.web_seeds = 1;
    state.download_speed = 123;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"Скачивание"));
    state.download_speed = 0;
    EXPECT_TRUE(contains(diagnoseNetwork(state).summary, u8"HTTP-источников"));
}

TEST(NetworkDiagnostics, RouterAcknowledgementIsSeparateFromPublicReachability) {
    auto state = active();
    state.network.port_mapping.enabled = true;
    state.network.port_mapping.state = MappingState::Active;
    state.network.port_mapping.protocol = MappingProtocol::NatPmp;
    state.network.port_mapping.ports.push_back({PortTransport::Tcp, 24567, 45000, 3600, true, {}});
    const auto diagnosis = diagnoseNetwork(state);
    EXPECT_TRUE(contains(diagnosis.mapping, "NAT-PMP"));
    EXPECT_TRUE(contains(diagnosis.mapping, "45000"));
    EXPECT_TRUE(contains(diagnosis.port, u8"Доступность из интернета: не проверена"));
}

TEST(NetworkDiagnostics, ReportIncludesEffectiveCapsLimitsAndFailures) {
    auto state = active();
    state.name = u8"Тестовая раздача";
    state.network.peers.push_back({"127.0.0.1", 12345, PeerState::Connected});
    state.network.max_peers = 1;
    state.network.download_limit = 1536;
    state.network.global_download_limit = 65536;
    state.network.rejected_incoming = 2;
    state.network.failed_incoming_handshakes = 1;
    state.network.tracker.url = "https://user:secret@tracker.invalid/private-token/announce?passkey=query-token";
    state.network.tracker.error = "fixture tracker error";
    state.network.last_peer_error = "fixture peer error";
    const auto diagnosis = diagnoseNetwork(state);
    EXPECT_TRUE(contains(diagnosis.advice, u8"Достигнут лимит пиров: 1"));
    const auto report = networkReport(state);
    for (const auto* value : {u8"Тестовая раздача", u8"1.5 КиБ/с", u8"64 КиБ/с", "fixture tracker error", "fixture peer error",
         u8"Отклонено по лимиту пиров: 2", u8"Ошибок входящего приветствия: 1"}) EXPECT_TRUE(contains(report, value)) << value;
    EXPECT_TRUE(contains(report, "https://tracker.invalid"));
    for (const auto* secret : {"user", "secret", "private-token", "query-token"}) EXPECT_FALSE(contains(report, secret));
}
