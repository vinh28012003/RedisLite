#include <gtest/gtest.h>
#include "rdb.hpp"
#include "store.hpp"
#include <string>
#include <thread>
#include <chrono>

// ═══════════════════════════════════════════════════════════════════
// Category 1: Roundtrip tests — serialize → load → verify same data
// ═══════════════════════════════════════════════════════════════════

TEST(RDB, RoundtripEmptyStore) {
    Store src;
    std::string blob = rdb::serialize(src);

    Store dst;
    rdb::load(blob, dst);
    EXPECT_EQ(dst.size(), 0);
}

TEST(RDB, RoundtripSingleKey) {
    Store src;
    src.set("hello", "world");
    std::string blob = rdb::serialize(src);

    Store dst;
    rdb::load(blob, dst);
    EXPECT_EQ(dst.size(), 1);
    EXPECT_EQ(dst.get("hello").value(), "world");
}

TEST(RDB, RoundtripMultipleKeys) {
    Store src;
    src.set("k1", "v1");
    src.set("k2", "v2");
    src.set("k3", "v3");
    std::string blob = rdb::serialize(src);

    Store dst;
    rdb::load(blob, dst);
    EXPECT_EQ(dst.size(), 3);
    EXPECT_EQ(dst.get("k1").value(), "v1");
    EXPECT_EQ(dst.get("k2").value(), "v2");
    EXPECT_EQ(dst.get("k3").value(), "v3");
}

TEST(RDB, RoundtripKeyWithExpiry) {
    Store src;
    src.set("temp", "data", 10000);  // 10s TTL — generous to avoid clock drift
    std::string blob = rdb::serialize(src);

    Store dst;
    rdb::load(blob, dst);
    EXPECT_EQ(dst.size(), 1);
    // Key should exist — 10s TTL hasn't elapsed during serialize→load
    EXPECT_EQ(dst.get("temp").value(), "data");
}

TEST(RDB, SerializeSkipsExpiredKeys) {
    Store src;
    src.set("alive", "yes");
    src.set("dead", "no", 1);  // 1ms TTL — expires almost immediately

    // Wait for expiry to pass
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::string blob = rdb::serialize(src);

    Store dst;
    rdb::load(blob, dst);
    EXPECT_EQ(dst.size(), 1);
    EXPECT_EQ(dst.get("alive").value(), "yes");
    EXPECT_FALSE(dst.get("dead").has_value());  // expired key not serialized
}

TEST(RDB, LoadSkipsAlreadyExpiredKeys) {
    // Serialize a key with very short TTL
    Store src;
    src.set("ephemeral", "gone", 10);  // 10ms TTL
    std::string blob = rdb::serialize(src);

    // Wait for the absolute timestamp in the RDB to be in the past
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Store dst;
    rdb::load(blob, dst);
    // load() sees abs_ms < now → remaining <= 0 → skips insertion
    EXPECT_EQ(dst.size(), 0);
}

TEST(RDB, LoadClearsPreExistingKeys) {
    Store src;
    src.set("new", "data");
    std::string blob = rdb::serialize(src);

    // dst has stale key that should be wiped
    Store dst;
    dst.set("stale", "old");
    EXPECT_EQ(dst.size(), 1);

    rdb::load(blob, dst);
    EXPECT_EQ(dst.size(), 1);
    EXPECT_EQ(dst.get("new").value(), "data");
    EXPECT_FALSE(dst.get("stale").has_value());  // cleared by load()
}

// ═══════════════════════════════════════════════════════════════════
// Category 2: Format validation — verify RDB binary structure
// ═══════════════════════════════════════════════════════════════════

TEST(RDB, SerializeMagicBytes) {
    Store src;
    std::string blob = rdb::serialize(src);
    // First 9 bytes must be "REDIS0011" (RDB version 11)
    ASSERT_GE(blob.size(), 9);
    EXPECT_EQ(blob.substr(0, 9), "REDIS0011");
}

TEST(RDB, SerializeContainsEOFMarker) {
    Store src;
    std::string blob = rdb::serialize(src);
    // EOF marker (0xFF) should be at position [size - 9]
    // (last 8 bytes are CRC64 checksum)
    ASSERT_GT(blob.size(), 9);
    uint8_t eof_byte = static_cast<uint8_t>(blob[blob.size() - 9]);
    EXPECT_EQ(eof_byte, 0xFF);
}

TEST(RDB, SerializeCRC64Present) {
    Store src;
    std::string blob = rdb::serialize(src);
    // Last 8 bytes = CRC64 checksum. Must not be all zeros.
    ASSERT_GE(blob.size(), 17);  // at minimum: 9 magic + ... + 1 EOF + 8 CRC
    std::string crc_bytes = blob.substr(blob.size() - 8);
    EXPECT_NE(crc_bytes, std::string(8, '\0'));
}

TEST(RDB, RoundtripLargeKey64Plus) {
    Store src;
    // Key > 63 bytes triggers 2-byte length encoding (01xxxxxx yyyyyyyy)
    std::string long_key(100, 'k');
    std::string long_val(100, 'v');
    src.set(long_key, long_val);
    std::string blob = rdb::serialize(src);

    Store dst;
    rdb::load(blob, dst);
    EXPECT_EQ(dst.size(), 1);
    EXPECT_EQ(dst.get(long_key).value(), long_val);
}

TEST(RDB, RoundtripLargeKey16384Plus) {
    Store src;
    // Key > 16383 bytes triggers 5-byte length encoding (10000000 + 4-byte LE)
    std::string huge_key(20000, 'K');
    std::string huge_val(20000, 'V');
    src.set(huge_key, huge_val);
    std::string blob = rdb::serialize(src);

    Store dst;
    rdb::load(blob, dst);
    EXPECT_EQ(dst.size(), 1);
    EXPECT_EQ(dst.get(huge_key).value(), huge_val);
}

// ═══════════════════════════════════════════════════════════════════
// Category 3: Error paths — load rejects malformed input
// ═══════════════════════════════════════════════════════════════════

TEST(RDB, LoadTooShortThrows) {
    Store dst;
    EXPECT_THROW(rdb::load("SHORT", dst), std::runtime_error);
}

TEST(RDB, LoadInvalidMagicThrows) {
    Store dst;
    // 9 bytes but wrong magic
    EXPECT_THROW(rdb::load("NOTREDIS!", dst), std::runtime_error);
}

TEST(RDB, LoadUnsupportedOpcodeThrows) {
    Store dst;
    // Valid magic + unsupported opcode byte (0x0F = list type, not supported)
    std::string bad = "REDIS0011";
    bad += static_cast<char>(0x0F);  // unsupported type
    EXPECT_THROW(rdb::load(bad, dst), std::runtime_error);
}
