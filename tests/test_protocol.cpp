#include <gtest/gtest.h>

#include "net/protocol.h"
#include "core/manifest.h"
#include "core/delta.h"

using namespace rasync;

TEST(Protocol, MessageStartsWithOpcode) {
    auto w = proto::message(proto::Op::FileStart);
    ASSERT_GE(w.size(), 1u);
    EXPECT_EQ(w.buffer()[0], static_cast<uint8_t>(proto::Op::FileStart));
}

TEST(Protocol, OpNamesCovered) {
    for (uint8_t i = 1; i <= 12; ++i)
        EXPECT_STRNE(proto::op_name(static_cast<proto::Op>(i)), "?");
}

TEST(Protocol, HelloRoundTrip) {
    auto w = proto::message(proto::Op::Hello);
    w.u8(proto::kVersion);
    w.u8(0);           // mode
    w.u8(1);           // conflict
    w.u64(42);         // base gen
    w.str16("data");

    BinaryReader r(w.buffer());
    EXPECT_EQ(static_cast<proto::Op>(r.u8()), proto::Op::Hello);
    EXPECT_EQ(r.u8(), proto::kVersion);
    EXPECT_EQ(r.u8(), 0);
    EXPECT_EQ(r.u8(), 1);
    EXPECT_EQ(r.u64(), 42u);
    EXPECT_EQ(r.str16(), "data");
    EXPECT_TRUE(r.ok());
}

TEST(Protocol, ManifestMessageRoundTrip) {
    Manifest m;
    FileMeta fm; fm.size = 3; fm.mtime = 9; fm.hash = sha256("abc");
    m.set("a/b.txt", fm);

    auto w = proto::message(proto::Op::Manifest);
    m.encode(w);

    BinaryReader r(w.buffer());
    EXPECT_EQ(static_cast<proto::Op>(r.u8()), proto::Op::Manifest);
    auto back = Manifest::decode(r);
    ASSERT_TRUE(back.has_value());
    ASSERT_NE(back->find("a/b.txt"), nullptr);
    EXPECT_EQ(back->find("a/b.txt")->hash, fm.hash);
}

TEST(Protocol, RequestWithSignatureRoundTrip) {
    std::vector<uint8_t> data(1000, 7);
    Signature sig = signature_of(data.data(), data.size(), 256);

    auto w = proto::message(proto::Op::Request);
    w.str16("file.bin");
    w.u8(1);
    sig.encode(w);

    BinaryReader r(w.buffer());
    EXPECT_EQ(static_cast<proto::Op>(r.u8()), proto::Op::Request);
    EXPECT_EQ(r.str16(), "file.bin");
    EXPECT_EQ(r.u8(), 1);
    auto back = Signature::decode(r);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->blocks.size(), sig.blocks.size());
}
