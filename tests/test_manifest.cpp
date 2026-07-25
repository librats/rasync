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

// ── patches ──────────────────────────────────────────────────────────────────

namespace {
/// The property every patch must have: applying it to `from` reproduces `to`.
void expect_patch_bridges(const Manifest& from, const Manifest& to) {
    ManifestPatch p = diff_manifests(from, to);
    Manifest rebuilt = from;
    rebuilt.apply(p);
    ASSERT_EQ(rebuilt.size(), to.size());
    for (const auto& [path, fm] : to.entries()) {
        const FileMeta* got = rebuilt.find(path);
        ASSERT_NE(got, nullptr) << path;
        EXPECT_EQ(*got, fm) << path;
    }
}
} // namespace

TEST(ManifestPatch, BridgesAddsModifiesAndRemoves) {
    Manifest from;
    from.set("keep", meta(1, 1, "keep"));
    from.set("edit", meta(2, 2, "before"));
    from.set("gone", meta(3, 3, "gone"));
    from.set("zzz/last", meta(4, 4, "last"));  // exercises the tail of the merge walk

    Manifest to;
    to.set("keep", meta(1, 1, "keep"));
    to.set("edit", meta(9, 9, "after"));
    to.set("added", meta(5, 5, "added"));
    to.set("zzz/last", meta(4, 4, "last"));

    ManifestPatch p = diff_manifests(from, to);
    EXPECT_EQ(p.set.size(), 2u);       // edit + added, not the untouched two
    EXPECT_EQ(p.removed.size(), 1u);   // gone
    EXPECT_EQ(p.removed.front(), "gone");
    expect_patch_bridges(from, to);
}

TEST(ManifestPatch, EmptyWhenNothingChanged) {
    Manifest m;
    m.set("a", meta(1, 1, "a"));
    m.set("b", meta(2, 2, "b"));
    EXPECT_TRUE(diff_manifests(m, m).empty());
}

TEST(ManifestPatch, CarriesMetadataOnlyChanges) {
    // A touched-but-unchanged file leaves the fingerprint alone (mtime is not in
    // it), yet the peer still has to learn the new mtime — conflict resolution
    // compares it. So the patch must not be gated on the fingerprint.
    Manifest from, to;
    from.set("f", meta(10, 100, "same"));
    to.set("f", meta(10, 200, "same"));
    EXPECT_EQ(from.fingerprint(), to.fingerprint());

    ManifestPatch p = diff_manifests(from, to);
    ASSERT_EQ(p.set.size(), 1u);
    EXPECT_EQ(p.set.front().second.mtime, 200);
    expect_patch_bridges(from, to);
}

TEST(ManifestPatch, HandlesEmptyEndpoints) {
    Manifest empty, full;
    full.set("a", meta(1, 1, "a"));
    full.set("b/c", meta(2, 2, "c"));

    EXPECT_EQ(diff_manifests(empty, full).set.size(), 2u);      // first advertisement
    EXPECT_EQ(diff_manifests(full, empty).removed.size(), 2u);  // everything dropped
    expect_patch_bridges(empty, full);
    expect_patch_bridges(full, empty);
}

TEST(ManifestPatch, EncodeDecodeRoundTrip) {
    Manifest from, to;
    from.set("dropped", meta(1, 1, "x"));
    to.set("kept/deep.bin", meta(1234, -5, "deep"));
    to.set("unicode ✓.txt", meta(7, 7, "u"));

    ManifestPatch p = diff_manifests(from, to);
    auto back = ManifestPatch::decode(p.encode());
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->set.size(), p.set.size());
    ASSERT_EQ(back->removed, p.removed);
    for (size_t i = 0; i < p.set.size(); ++i) {
        EXPECT_EQ(back->set[i].first, p.set[i].first);
        EXPECT_EQ(back->set[i].second, p.set[i].second);
    }
}

TEST(ManifestPatch, DecodeRejectsMalformed) {
    Bytes huge_set{0xff, 0xff, 0xff, 0xff};                  // count with no entries
    EXPECT_FALSE(ManifestPatch::decode(huge_set).has_value());

    Bytes huge_del{0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
    EXPECT_FALSE(ManifestPatch::decode(huge_del).has_value());

    // An empty path would name the sync root itself — never acceptable.
    BinaryWriter w;
    w.u32(0);
    w.u32(1);
    w.str16("");
    EXPECT_FALSE(ManifestPatch::decode(w.take()).has_value());
}
