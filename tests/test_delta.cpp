#include <gtest/gtest.h>

#include "core/delta.h"
#include "test_util.h"

using namespace rasync;

namespace {

// Round-trip: signature(base) -> delta(target) -> apply == target, for the given
// block size. Returns the delta so the caller can assert on its efficiency.
Delta roundtrip(const std::vector<uint8_t>& base, const std::vector<uint8_t>& target,
                uint32_t bs = 256) {
    Signature sig = signature_of(base.data(), base.size(), bs);
    Delta d = compute_delta(sig, target.data(), target.size());
    std::vector<uint8_t> out = apply_delta(base.data(), base.size(), d);
    EXPECT_EQ(out, target);
    EXPECT_EQ(d.out_size, target.size());
    return d;
}

} // namespace

TEST(Delta, IdenticalFilesCopyEverything) {
    auto data = test::random_bytes(4096, 3);
    Delta d = roundtrip(data, data, 256);
    // Nothing changed → almost everything is COPY, negligible literals.
    EXPECT_LT(d.literal_bytes(), data.size() / 4);
    EXPECT_GT(d.copied_bytes(), data.size() / 2);
}

TEST(Delta, AppendOnlySendsTail) {
    auto base = test::random_bytes(8192, 5);
    auto target = base;
    auto tail = test::random_bytes(500, 99);
    target.insert(target.end(), tail.begin(), tail.end());

    Delta d = roundtrip(base, target, 256);
    // The unchanged prefix is copied; only the appended tail travels as literal.
    EXPECT_LT(d.literal_bytes(), 1500u);
}

TEST(Delta, MiddleEditIsLocalized) {
    auto base = test::random_bytes(16384, 11);
    auto target = base;
    for (size_t i = 4000; i < 4050; ++i) target[i] ^= 0xff;  // flip a small region

    Delta d = roundtrip(base, target, 256);
    // A 50-byte edit should not resend the whole 16 KiB file.
    EXPECT_LT(d.literal_bytes(), base.size() / 2);
}

TEST(Delta, PrependShiftsButStillMatches) {
    auto base = test::random_bytes(8192, 21);
    std::vector<uint8_t> target = test::random_bytes(300, 55);
    target.insert(target.end(), base.begin(), base.end());
    // Rolling checksum realigns after the inserted prefix, so most is still copied.
    Delta d = roundtrip(base, target, 256);
    EXPECT_LT(d.literal_bytes(), 1500u);
}

TEST(Delta, EmptyBaseAllLiteral) {
    std::vector<uint8_t> base;
    auto target = test::random_bytes(1000, 7);
    Delta d = roundtrip(base, target, 256);
    EXPECT_EQ(d.literal_bytes(), target.size());
    EXPECT_EQ(d.copied_bytes(), 0u);
}

TEST(Delta, EmptyTarget) {
    auto base = test::random_bytes(1000, 7);
    std::vector<uint8_t> target;
    Delta d = roundtrip(base, target, 256);
    EXPECT_EQ(d.ops.size(), 0u);
}

TEST(Delta, TargetSmallerThanBlock) {
    auto base = test::random_bytes(1000, 7);
    std::vector<uint8_t> target{1, 2, 3};
    roundtrip(base, target, 256);
}

TEST(Delta, WholeFileReplacement) {
    auto base = test::random_bytes(5000, 1);
    auto target = test::random_bytes(5000, 2);  // completely different
    Delta d = roundtrip(base, target, 256);
    EXPECT_GT(d.literal_bytes(), target.size() / 2);
}

TEST(Delta, SignatureEncodeDecode) {
    auto data = test::random_bytes(2000, 42);
    Signature sig = signature_of(data.data(), data.size(), 512);
    auto back = Signature::decode(sig.encode());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->block_size, sig.block_size);
    EXPECT_EQ(back->file_size, sig.file_size);
    ASSERT_EQ(back->blocks.size(), sig.blocks.size());
    for (size_t i = 0; i < sig.blocks.size(); ++i) {
        EXPECT_EQ(back->blocks[i].weak, sig.blocks[i].weak);
        EXPECT_EQ(back->blocks[i].strong, sig.blocks[i].strong);
    }
}

TEST(Delta, WeakChecksumRolls) {
    // The incremental roll must equal a fresh checksum of the shifted window.
    auto data = test::random_bytes(100, 77);
    const uint32_t L = 16;
    uint32_t direct = weak_checksum(data.data() + 1, L);

    // Reproduce the rolling update the scanner uses.
    uint32_t a = 0, b = 0;
    for (uint32_t i = 0; i < L; ++i) { a += data[i]; b += (L - i) * data[i]; }
    uint8_t out = data[0], in = data[L];
    a = (a - out + in) & 0xffff;
    b = (b - L * out + a) & 0xffff;
    uint32_t rolled = ((b & 0xffff) << 16) | (a & 0xffff);
    EXPECT_EQ(rolled, direct);
}
