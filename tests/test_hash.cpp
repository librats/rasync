#include <gtest/gtest.h>

#include "core/hash.h"
#include "test_util.h"

using namespace rasync;

TEST(Hash, KnownVectorAbc) {
    // SHA-256("abc") — the canonical FIPS 180-4 test vector.
    EXPECT_EQ(to_hex(sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Hash, EmptyVector) {
    EXPECT_EQ(to_hex(empty_hash()),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Hash, HexRoundTrip) {
    Hash h = sha256("the quick brown fox");
    auto back = hash_from_hex(to_hex(h));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, h);
}

TEST(Hash, HexRejectsMalformed) {
    EXPECT_FALSE(hash_from_hex("tooshort").has_value());
    EXPECT_FALSE(hash_from_hex(std::string(64, 'z')).has_value());
}

TEST(Hash, FileMatchesBuffer) {
    test::TempDir dir;
    std::string content = "some file contents to hash\n";
    std::string path = dir.sub("f.txt");
    test::write_file(path, content);

    auto fh = sha256_file(path);
    ASSERT_TRUE(fh.has_value());
    EXPECT_EQ(*fh, sha256(content));
}

TEST(Hash, FileMissingReturnsNullopt) {
    EXPECT_FALSE(sha256_file("/no/such/file/here.xyz").has_value());
}

TEST(Hash, LargeFileStreams) {
    test::TempDir dir;
    auto data = test::random_bytes(1024 * 1024 + 123, 7);  // > streaming window
    std::string path = dir.sub("big.bin");
    test::write_file(path, data);

    auto fh = sha256_file(path);
    ASSERT_TRUE(fh.has_value());
    EXPECT_EQ(*fh, sha256(data.data(), data.size()));
}
