#include "torrent/network/dht/dht_manager.h"
#include "torrent/metainfo/bencode.h"
#include <gtest/gtest.h>
#include <future>
#include <set>
#include <filesystem>
#include <fstream>

using namespace torrent;

TEST(Dht, PortOwnershipAndRestart) {
    dht::DHTManager first(0);
    first.start();
    const auto port = first.port();
    EXPECT_NE(port, 0);
    dht::DHTManager second(port);
    EXPECT_THROW(second.start(), std::runtime_error);
    first.stop();
    EXPECT_NO_THROW(second.start());
    second.stop();
    EXPECT_NO_THROW(first.start());
    first.stop();
}

TEST(Dht, InvalidLookupsAreRejectedAndAnEmptyStoppedNodeDoesNotWait) {
    dht::DHTManager node(0);
    EXPECT_THROW(node.getPeers({1, 2, 3}), std::invalid_argument);
    EXPECT_THROW(node.getPeers(std::vector<uint8_t>(20), -1), std::invalid_argument);
    EXPECT_TRUE(node.getPeers(std::vector<uint8_t>(20)).empty());
    EXPECT_THROW(node.announcePeer(std::vector<uint8_t>(21), 1), std::invalid_argument);
}

TEST(Dht, KrpcRejectsMissingIdsAndWrongHashLengths) {
    const auto packet = [](const BencodeValue::Dictionary& value) {
        const auto bytes = BencodeParser::encode(value);
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    };
    BencodeValue::Dictionary args{{"id", std::string(20, 'a')}, {"info_hash", std::string(19, 'b')}};
    BencodeValue::Dictionary query{{"t", std::string("ab")}, {"y", std::string("q")}, {"q", std::string("get_peers")}, {"a", args}};
    EXPECT_THROW(dht::KRPCMessageFactory::parse(packet(query)), std::runtime_error);
    query["a"] = BencodeValue::Dictionary{{"info_hash", std::string(20, 'b')}};
    EXPECT_THROW(dht::KRPCMessageFactory::parse(packet(query)), std::runtime_error);
    const BencodeValue::Dictionary response{{"t", std::string("ab")}, {"y", std::string("r")},
        {"r", BencodeValue::Dictionary{{"id", std::string("short")}}}};
    EXPECT_THROW(dht::KRPCMessageFactory::parse(packet(response)), std::runtime_error);
}

TEST(Dht, EmptyFindNodeResponseContainsAnEmptyNodesString) {
    const auto response = dht::KRPCMessageFactory::createFindNodeResponse(dht::NodeID{}, {}, "ab");
    const auto value = response.toBencode().getDictionary().at("r").getDictionary();
    ASSERT_TRUE(value.count("nodes"));
    EXPECT_TRUE(value.at("nodes").getString().empty());
}

TEST(Dht, TransactionIdsRemainUniqueAcrossThreads) {
    std::vector<std::future<std::set<std::string>>> jobs;
    for (int thread = 0; thread < 4; ++thread) jobs.push_back(std::async(std::launch::async, [] {
        std::set<std::string> ids;
        for (int i = 0; i < 1000; ++i) ids.insert(dht::KRPCMessageFactory::generateTransactionId());
        return ids;
    }));
    std::set<std::string> ids;
    for (auto& job : jobs) { const auto batch = job.get(); ids.insert(batch.begin(), batch.end()); }
    EXPECT_EQ(ids.size(), 4000);
}

TEST(Dht, BootstrapConfigurationRoundTripsAndRejectsPortOverflow) {
    const auto file = std::filesystem::current_path() / "dht-config-test.json";
    Config config; config.dht_port = 0;
    config.dht_bootstrap_nodes = {{"127.0.0.1", 12345}, {"router.example", 65535}};
    ASSERT_TRUE(config.saveToFile(file.u8string()));
    Config restored;
    ASSERT_TRUE(restored.loadFromFile(file.u8string()));
    ASSERT_EQ(restored.dht_bootstrap_nodes.size(), 2);
    EXPECT_EQ(restored.dht_port, 0);
    EXPECT_EQ(restored.dht_bootstrap_nodes[0].host, "127.0.0.1");
    EXPECT_EQ(restored.dht_bootstrap_nodes[1].port, 65535);
    for (const auto port : {"-1", "65536", "1.5"}) {
        std::ofstream output(file);
        output << "{\"dht_bootstrap_nodes\":[{\"host\":\"127.0.0.1\",\"port\":" << port << "}]}";
        output.close(); EXPECT_FALSE(restored.loadFromFile(file.u8string()));
    }
    std::filesystem::remove(file);
}
