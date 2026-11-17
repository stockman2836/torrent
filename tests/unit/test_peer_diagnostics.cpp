#include "peer_diagnostics.h"
#include "network_diagnostics.h"
#include "download_task.h"
#include <gtest/gtest.h>

using namespace torrent;
using namespace torrent::app;

TEST(PeerDiagnostics, DistinguishesObservedFailuresFromRemoteClosureAndLocalStop) {
    EXPECT_NE(peerEndText(PeerEndReason::RemoteClosed).find(u8"Пир закрыл"), std::string::npos);
    EXPECT_NE(peerEndText(PeerEndReason::Stopped).find(u8"приложением"), std::string::npos);
    EXPECT_NE(peerEndText(PeerEndReason::WrongTorrent).find(u8"другой торрент"), std::string::npos);
    EXPECT_NE(peerEndText(PeerEndReason::Unknown).find(u8"не определена"), std::string::npos);
    PeerStatus peer; peer.end_reason = PeerEndReason::ConnectFailed; peer.error_code = 10061;
    EXPECT_NE(peerResultText(peer).find("10061"), std::string::npos);
    EXPECT_EQ(peerResultText(peer).find(u8"роутер"), std::string::npos);
}

TEST(PeerDiagnostics, FormatsIpv6AndDoesNotGuessClientIdentity) {
    PeerStatus peer; peer.ip = "::1"; peer.port = 6881;
    EXPECT_EQ(peerAddress(peer), "[::1]:6881");
    peer.ip = "127.0.0.1"; EXPECT_EQ(peerAddress(peer), "127.0.0.1:6881");
    DownloadSnapshot snapshot;
    snapshot.network.peers.push_back(peer);
    const auto report = networkReport(snapshot);
    EXPECT_NE(report.find(u8"клиент (сообщён пиром): не указан"), std::string::npos);
}

TEST(PeerDiagnostics, ReportRetainsFinishedTransferTotalsAndReason) {
    DownloadSnapshot snapshot;
    PeerStatus peer; peer.id = 7; peer.ip = "127.0.0.1"; peer.port = 12345;
    peer.state = PeerState::Disconnected; peer.client = "Fixture/1.2";
    peer.downloaded_bytes = 16384; peer.uploaded_bytes = 4096; peer.duration_ms = 2500;
    peer.end_reason = PeerEndReason::Stopped; peer.finished_at_ms = 1700000000000;
    snapshot.network.peer_history.push_back(peer);
    const auto report = networkReport(snapshot);
    for (const auto* value : {"Fixture/1.2", "16384", "4096", "2500"}) EXPECT_NE(report.find(value), std::string::npos);
    EXPECT_NE(report.find(peerEndText(PeerEndReason::Stopped)), std::string::npos);
}
