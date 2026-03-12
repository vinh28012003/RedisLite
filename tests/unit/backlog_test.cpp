#include <gtest/gtest.h>
#include "replication_backlog.hpp"

// --- Empty state ---

TEST(Backlog, InitiallyEmpty) {
    ReplicationBacklog backlog(64);
    EXPECT_TRUE(backlog.empty());
    EXPECT_FALSE(backlog.contains(0));
    EXPECT_EQ(backlog.start_offset(), 0);
    EXPECT_EQ(backlog.end_offset(), 0);
}

// --- Basic feed + read ---

TEST(Backlog, FeedAndReadBack) {
    ReplicationBacklog backlog(64);
    backlog.feed("hello", 0);
    EXPECT_FALSE(backlog.empty());
    EXPECT_EQ(backlog.start_offset(), 0);
    EXPECT_EQ(backlog.end_offset(), 5);
    EXPECT_EQ(backlog.read(0), "hello");
}

// --- Sequential feeds ---

TEST(Backlog, SequentialFeedsAccumulate) {
    ReplicationBacklog backlog(64);
    backlog.feed("aaa", 0);
    backlog.feed("bbb", 3);
    EXPECT_EQ(backlog.start_offset(), 0);
    EXPECT_EQ(backlog.end_offset(), 6);
    EXPECT_EQ(backlog.read(0), "aaabbb");
}

// --- Read from middle offset ---

TEST(Backlog, ReadFromMiddleOffset) {
    ReplicationBacklog backlog(16);
    backlog.feed("abcdef", 0);
    EXPECT_EQ(backlog.read(3), "def");
}

// --- contains() boundary conditions ---

TEST(Backlog, ContainsBoundaryConditions) {
    ReplicationBacklog backlog(64);
    backlog.feed("12345", 10);
    EXPECT_TRUE(backlog.contains(10));   // at start — valid
    EXPECT_FALSE(backlog.contains(9));   // before start — invalid
    EXPECT_TRUE(backlog.contains(15));   // at end — valid (caught up)
    EXPECT_FALSE(backlog.contains(16));  // past end — invalid
}

// --- Read at end_offset returns empty ---

TEST(Backlog, ReadAtEndOffsetReturnsEmpty) {
    ReplicationBacklog backlog(64);
    backlog.feed("data", 0);
    EXPECT_EQ(backlog.read(backlog.end_offset()), "");
}

// --- Wrap around ---

TEST(Backlog, WrapAroundOverwritesOldData) {
    ReplicationBacklog backlog(8);
    backlog.feed("aaaabbbb", 0);   // fills buffer exactly
    backlog.feed("cccc", 8);       // overwrites first 4 bytes
    EXPECT_EQ(backlog.start_offset(), 4);
    EXPECT_EQ(backlog.end_offset(), 12);
    EXPECT_FALSE(backlog.contains(3));  // old data gone
    EXPECT_TRUE(backlog.contains(4));
    EXPECT_EQ(backlog.read(4), "bbbbcccc");
}

// --- Feed exactly capacity ---

TEST(Backlog, FeedExactlyCapacity) {
    ReplicationBacklog backlog(8);
    backlog.feed("12345678", 0);
    EXPECT_EQ(backlog.start_offset(), 0);
    EXPECT_EQ(backlog.end_offset(), 8);
    EXPECT_EQ(backlog.read(0), "12345678");
}

// --- Multiple wraps ---

TEST(Backlog, MultipleWrapsKeepsOnlyRecent) {
    ReplicationBacklog backlog(8);
    backlog.feed("aaaaaaaa", 0);   // offsets 0-8
    backlog.feed("bbbbbbbb", 8);   // offsets 8-16
    backlog.feed("cccccccc", 16);  // offsets 16-24
    EXPECT_EQ(backlog.start_offset(), 16);
    EXPECT_EQ(backlog.end_offset(), 24);
    EXPECT_FALSE(backlog.contains(15));
    EXPECT_TRUE(backlog.contains(16));
    EXPECT_EQ(backlog.read(16), "cccccccc");
}

// --- Empty feed is no-op ---

TEST(Backlog, EmptyFeedIsNoop) {
    ReplicationBacklog backlog(64);
    backlog.feed("", 0);
    EXPECT_TRUE(backlog.empty());
}

// --- Non-zero starting offset ---

TEST(Backlog, NonZeroStartingOffset) {
    ReplicationBacklog backlog(64);
    backlog.feed("hello", 100);
    EXPECT_EQ(backlog.start_offset(), 100);
    EXPECT_EQ(backlog.end_offset(), 105);
    EXPECT_EQ(backlog.read(100), "hello");
    EXPECT_FALSE(backlog.contains(99));
}

// --- Read after wrap returns correct cross-boundary data ---

TEST(Backlog, ReadAcrossWrapBoundary) {
    ReplicationBacklog backlog(8);
    backlog.feed("aaaaaa", 0);     // write_pos=6, offsets 0-6
    backlog.feed("bbbbbb", 6);     // write_pos=4 (wraps), offsets 6-12
    // Buffer: "bbbb" + "aabb" → physical layout mixed, but read should be correct
    EXPECT_EQ(backlog.start_offset(), 4);
    EXPECT_EQ(backlog.end_offset(), 12);
    EXPECT_EQ(backlog.read(4), "aabbbbbb");
    EXPECT_EQ(backlog.read(6), "bbbbbb");
    EXPECT_EQ(backlog.read(10), "bb");
}

// --- Single byte operations ---

TEST(Backlog, SingleByteFeeds) {
    ReplicationBacklog backlog(4);
    backlog.feed("a", 0);
    backlog.feed("b", 1);
    backlog.feed("c", 2);
    backlog.feed("d", 3);
    EXPECT_EQ(backlog.read(0), "abcd");
    backlog.feed("e", 4);  // wraps, overwrites "a"
    EXPECT_EQ(backlog.start_offset(), 1);
    EXPECT_EQ(backlog.read(1), "bcde");
}

// --- Feed larger than capacity in one call ---

TEST(Backlog, FeedLargerThanCapacity) {
    ReplicationBacklog backlog(4);
    backlog.feed("abcdefgh", 0);  // 8 bytes into 4-byte buffer
    EXPECT_EQ(backlog.start_offset(), 4);
    EXPECT_EQ(backlog.end_offset(), 8);
    EXPECT_EQ(backlog.read(4), "efgh");
}

// --- set_start (post-promotion initialization) ---

TEST(Backlog, SetStartInitializesOffsetState) {
    ReplicationBacklog backlog(64);
    EXPECT_TRUE(backlog.empty());

    backlog.set_start(500);
    EXPECT_FALSE(backlog.empty());
    EXPECT_EQ(backlog.start_offset(), 500);
    EXPECT_EQ(backlog.end_offset(), 500);
    EXPECT_TRUE(backlog.contains(500));   // caught-up offset is valid
    EXPECT_FALSE(backlog.contains(499));  // before start
    EXPECT_EQ(backlog.read(500), "");     // no data yet
}

TEST(Backlog, SetStartThenFeedWorks) {
    ReplicationBacklog backlog(64);
    backlog.set_start(100);
    backlog.feed("hello", 100);
    EXPECT_EQ(backlog.start_offset(), 100);
    EXPECT_EQ(backlog.end_offset(), 105);
    EXPECT_EQ(backlog.read(100), "hello");
}
