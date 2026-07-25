#include <gtest/gtest.h>

#include "net/auth.h"
#include "version.h"

#include <string>

using namespace rasync;

// No key: the bare protocol name on both derivations — every rasync build shares
// it, which is exactly the "open" posture the flagless mode advertises.
TEST(AuthTest, NoKeyFallsBackToTheBareProtocolName) {
    EXPECT_EQ(protocol_id(""), std::string(kProtocolName));
    EXPECT_EQ(discovery_id(""), std::string(kProtocolName));
}

// The whole point: a key changes the handshake protocol id, and librats refuses
// a handshake whose prologue protocol differs (see librats HandshakeTest).
TEST(AuthTest, KeyChangesTheProtocolId) {
    EXPECT_NE(protocol_id("secret"), std::string(kProtocolName));
    EXPECT_NE(protocol_id("secret"), protocol_id("other"));
    EXPECT_EQ(protocol_id("secret"), protocol_id("secret"));  // deterministic across hosts
}

TEST(AuthTest, KeyChangesTheDiscoveryId) {
    EXPECT_NE(discovery_id("secret"), std::string(kProtocolName));
    EXPECT_NE(discovery_id("secret"), discovery_id("other"));
    EXPECT_EQ(discovery_id("secret"), discovery_id("secret"));
}

// The DHT publishes the discovery id's hash next to our address. If it were the
// key, or derivable into the handshake tag, crawling the DHT would hand out the
// credential — so the two derivations must not coincide, and neither may contain
// the key.
TEST(AuthTest, DerivationsAreDomainSeparatedAndHideTheKey) {
    const std::string key = "correct horse battery staple";
    EXPECT_NE(protocol_id(key), discovery_id(key));
    EXPECT_EQ(protocol_id(key).find(key), std::string::npos);
    EXPECT_EQ(discovery_id(key).find(key), std::string::npos);
}

// A NUL-separated label means no (label, key) pair can be re-split into another
// pair with the same bytes; without it, keys that differ only in where the label
// ends would collide.
TEST(AuthTest, LabelBoundaryIsNotAmbiguous) {
    EXPECT_NE(protocol_id("x"), protocol_id(std::string("\0x", 2)));
    EXPECT_NE(discovery_id("rasync-psk"), protocol_id(""));
}

// Long enough to be unguessable, short enough to stay readable in a banner.
TEST(AuthTest, IdsStayShort) {
    const std::string key = "a-fairly-long-passphrase-that-should-not-leak-into-the-id";
    EXPECT_EQ(protocol_id(key).size(), std::string(kProtocolName).size() + 1 + 32);
    EXPECT_EQ(discovery_id(key).size(), std::string("rasync:").size() + 32);
}
