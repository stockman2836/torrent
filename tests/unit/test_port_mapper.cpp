#include "torrent/network/port_mapping/port_mapper.h"
#include <gtest/gtest.h>

using namespace torrent;

TEST(PortMapper, DisabledConstructionNeverResolvesItsGateway) {
    PortMappingOptions options; options.gateway = "not-a-router-address";
    PortMapper mapper(24567, 24568, options);
    const auto status = mapper.status();
    EXPECT_FALSE(status.enabled);
    EXPECT_EQ(status.state, MappingState::Disabled);
    EXPECT_TRUE(status.gateway.empty());
    ASSERT_EQ(status.ports.size(), 2u);
    EXPECT_FALSE(status.ports[0].active);
    EXPECT_EQ(status.ports[0].external_port, 0);
    mapper.stop(); mapper.stop();
    EXPECT_EQ(mapper.status().state, MappingState::Disabled);
}

TEST(PortMapper, RefusesWildcardPortsPermanentLeasesAndUnboundedTimeouts) {
    EXPECT_THROW(PortMapper(0), std::invalid_argument);
    PortMappingOptions options; options.lease_seconds = 0;
    EXPECT_THROW(PortMapper(24567, 0, options), std::invalid_argument);
    options = {}; options.request_timeout_ms = 0;
    EXPECT_THROW(PortMapper(24567, 0, options), std::invalid_argument);
    options = {}; options.discovery_port = 0;
    EXPECT_THROW(PortMapper(24567, 0, options), std::invalid_argument);
}
