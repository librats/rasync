#include <gtest/gtest.h>

#include "core/serialize.h"

using namespace rasync;

TEST(Serialize, RoundTripsScalars) {
    BinaryWriter w;
    w.u8(0x12);
    w.u16(0x3456);
    w.u32(0x789abcde);
    w.u64(0x0102030405060708ull);
    w.i64(-42);
    w.str16("hello");
    w.str32("world!");

    BinaryReader r(w.buffer());
    EXPECT_EQ(r.u8(), 0x12);
    EXPECT_EQ(r.u16(), 0x3456);
    EXPECT_EQ(r.u32(), 0x789abcdeu);
    EXPECT_EQ(r.u64(), 0x0102030405060708ull);
    EXPECT_EQ(r.i64(), -42);
    EXPECT_EQ(r.str16(), "hello");
    EXPECT_EQ(r.str32(), "world!");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.remaining(), 0u);
}

TEST(Serialize, BigEndianOnWire) {
    BinaryWriter w;
    w.u32(0x01020304);
    const Bytes& b = w.buffer();
    ASSERT_EQ(b.size(), 4u);
    EXPECT_EQ(b[0], 0x01);
    EXPECT_EQ(b[3], 0x04);
}

TEST(Serialize, FixedArrayRoundTrip) {
    std::array<uint8_t, 4> a{1, 2, 3, 4};
    BinaryWriter w;
    w.bytes(a);
    BinaryReader r(w.buffer());
    EXPECT_EQ(r.bytes<4>(), a);
}

TEST(Serialize, TruncatedReadFailsSafely) {
    BinaryWriter w;
    w.u16(0xffff);  // only 2 bytes present
    BinaryReader r(w.buffer());
    (void)r.u64();               // asks for 8 — must not over-read
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.u32(), 0u);      // subsequent reads yield zero, still !ok
    EXPECT_FALSE(r.ok());
}

TEST(Serialize, StringLengthOverrunFails) {
    BinaryWriter w;
    w.u16(100);                  // claims 100 bytes of string...
    w.raw("short", 5);           // ...but only 5 follow
    BinaryReader r(w.buffer());
    EXPECT_EQ(r.str16(), "");
    EXPECT_FALSE(r.ok());
}

TEST(Serialize, Blob32RoundTrip) {
    Bytes payload{9, 8, 7, 6, 5};
    BinaryWriter w;
    w.blob32(payload.data(), payload.size());
    BinaryReader r(w.buffer());
    EXPECT_EQ(r.blob32(), payload);
    EXPECT_TRUE(r.ok());
}
