#include <gtest/gtest.h>

#include "core/manifest.h"
#include "test_util.h"

using namespace rasync;

namespace {
FileMeta meta(uint64_t size, int64_t mtime, const std::string& content_seed) {
    FileMeta m;
    m.size = size;
    m.mtime = mtime;
    m.hash = sha256(content_seed);
    return m;
}
} // namespace

TEST(Manifest, BasicAccess) {
    Manifest m;
    EXPECT_TRUE(m.empty());
    m.set("a/b.txt", meta(10, 100, "x"));
    m.set("c.txt", meta(20, 200, "y"));
    EXPECT_EQ(m.size(), 2u);
    EXPECT_TRUE(m.contains("a/b.txt"));
    ASSERT_NE(m.find("c.txt"), nullptr);
    EXPECT_EQ(m.find("c.txt")->size, 20u);
    EXPECT_EQ(m.total_bytes(), 30u);
    m.remove("c.txt");
    EXPECT_FALSE(m.contains("c.txt"));
}

TEST(Manifest, EncodeDecodeRoundTrip) {
    Manifest m;
    m.set("dir/one.bin", meta(1234, 555, "one"));
    m.set("two.txt", meta(0, 0, "two"));
    m.set("nested/deep/three", meta(99, -1, "three"));

    Bytes enc = m.encode();
    auto back = Manifest::decode(enc);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->size(), m.size());
    for (const auto& [path, fm] : m.entries()) {
        const FileMeta* got = back->find(path);
        ASSERT_NE(got, nullptr) << path;
        EXPECT_EQ(got->size, fm.size);
        EXPECT_EQ(got->mtime, fm.mtime);
        EXPECT_EQ(got->hash, fm.hash);
    }
}

TEST(Manifest, FingerprintStableAndSensitive) {
    Manifest a;
    a.set("x", meta(1, 1, "aaa"));
    a.set("y", meta(2, 2, "bbb"));

    Manifest b;  // same content, inserted in the other order
    b.set("y", meta(2, 2, "bbb"));
    b.set("x", meta(1, 1, "aaa"));

    EXPECT_EQ(a.fingerprint(), b.fingerprint());
    EXPECT_EQ(a.generation(), b.generation());

    b.set("x", meta(1, 999, "aaa"));  // only mtime changed — content identical
    // mtime is folded into the fingerprint via size/mode/hash only; mtime alone
    // must NOT change the fingerprint (touched-but-unchanged files don't resync).
    EXPECT_EQ(a.fingerprint(), b.fingerprint());

    b.set("x", meta(1, 1, "CHANGED"));  // content changed
    EXPECT_NE(a.fingerprint(), b.fingerprint());
}

TEST(Manifest, SaveLoad) {
    test::TempDir dir;
    Manifest m;
    m.set("a", meta(5, 5, "a"));
    m.set("b/c", meta(6, 6, "c"));
    std::string path = dir.sub("baseline.man");
    ASSERT_TRUE(m.save(path));

    auto loaded = Manifest::load(path);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->fingerprint(), m.fingerprint());
}

TEST(Manifest, DecodeGarbageFails) {
    Bytes junk{0xff, 0xff, 0xff, 0xff, 0x00};  // huge count, no entries
    EXPECT_FALSE(Manifest::decode(junk).has_value());
}
