#include "Talus/write_batch.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "db/write_batch_internal.h"
#include "memtable/memtable.h"

namespace talus {
namespace {

// Renders a batch's operations as "Put(k, v)" / "Delete(k)" strings.
struct Recorder {
  std::vector<std::string> ops;
  void Put(std::string_view k, std::string_view v) {
    ops.push_back("Put(" + std::string(k) + ", " + std::string(v) + ")");
  }
  void Delete(std::string_view k) { ops.push_back("Delete(" + std::string(k) + ")"); }
};

std::vector<std::string> Ops(const WriteBatch& b) {
  Recorder r;
  EXPECT_TRUE(WriteBatchInternal::Iterate(WriteBatchInternal::Contents(b), r).ok());
  return r.ops;
}

TEST(WriteBatchTest, Empty) {
  WriteBatch b;
  EXPECT_EQ(b.Count(), 0u);
  EXPECT_EQ(WriteBatchInternal::Contents(b), std::string(12, '\0'));
  EXPECT_TRUE(Ops(b).empty());
}

TEST(WriteBatchTest, ExactByteFormat) {
  WriteBatch b;
  b.Put("k", "v");
  b.Delete("x");
  WriteBatchInternal::SetSequence(&b, 0x0102030405060708ULL);
  const std::string expected =
      std::string("\x08\x07\x06\x05\x04\x03\x02\x01", 8)  // sequence, little-endian
      + std::string("\x02\x00\x00\x00", 4)                // count
      + std::string("\x01\x01k\x01v", 5)                  // Put: tag, len, key, len, value
      + std::string("\x00\x01x", 3);                      // Delete: tag, len, key
  EXPECT_EQ(WriteBatchInternal::Contents(b), expected);
  EXPECT_EQ(WriteBatchInternal::Sequence(b), 0x0102030405060708ULL);
}

TEST(WriteBatchTest, IterateInOrder) {
  WriteBatch b;
  b.Put("foo", "bar");
  b.Delete("box");
  b.Put("baz", "");
  EXPECT_EQ(b.Count(), 3u);
  EXPECT_EQ(Ops(b), (std::vector<std::string>{"Put(foo, bar)", "Delete(box)", "Put(baz, )"}));
}

TEST(WriteBatchTest, AppendAndClear) {
  WriteBatch a;
  WriteBatch b;
  a.Put("a", "1");
  b.Delete("b");
  b.Put("c", "3");
  a.Append(b);
  EXPECT_EQ(a.Count(), 3u);
  EXPECT_EQ(Ops(a), (std::vector<std::string>{"Put(a, 1)", "Delete(b)", "Put(c, 3)"}));
  EXPECT_EQ(b.Count(), 2u);  // Source unchanged.

  a.Clear();
  EXPECT_EQ(a.Count(), 0u);
  EXPECT_EQ(a.ApproximateSize(), WriteBatchInternal::kHeaderSize);
}

TEST(WriteBatchTest, InsertIntoUsesConsecutiveSequences) {
  WriteBatch b;
  b.Put("a", "1");
  b.Delete("b");
  b.Put("c", "3");
  WriteBatchInternal::SetSequence(&b, 100);
  MemTable mem;
  ASSERT_TRUE(WriteBatchInternal::InsertInto(WriteBatchInternal::Contents(b), &mem).ok());

  const std::string expected[] = {"a@100:1", "b@101:del", "c@102:3"};
  MemTable::Iterator iter = mem.NewIterator();
  iter.SeekToFirst();
  for (const std::string& e : expected) {
    ASSERT_TRUE(iter.Valid());
    ParsedInternalKey k;
    ASSERT_TRUE(ParseInternalKey(iter.key(), &k));
    const std::string v = k.type == ValueType::kDeletion ? "del" : std::string(iter.value());
    EXPECT_EQ(std::string(k.user_key) + "@" + std::to_string(k.sequence) + ":" + v, e);
    iter.Next();
  }
  EXPECT_FALSE(iter.Valid());
}

TEST(WriteBatchTest, MalformedContentsAreCorruption) {
  WriteBatch b;
  b.Put("key", "value");
  b.Delete("gone");
  const std::string good(WriteBatchInternal::Contents(b));

  // Every strict prefix fails: too small, a truncated op, or a missing op.
  for (size_t len = 0; len < good.size(); ++len) {
    EXPECT_TRUE(WriteBatchInternal::Validate(good.substr(0, len)).IsCorruption()) << len;
  }
  EXPECT_TRUE(WriteBatchInternal::Validate(good).ok());

  std::string wrong_count = good;
  wrong_count[8] = 3;
  EXPECT_TRUE(WriteBatchInternal::Validate(wrong_count).IsCorruption());

  std::string bad_tag = good;
  bad_tag[12] = 0x7f;
  EXPECT_TRUE(WriteBatchInternal::Validate(bad_tag).IsCorruption());

  WriteBatch target;
  EXPECT_TRUE(WriteBatchInternal::SetContents(&target, "short").IsCorruption());
  EXPECT_TRUE(WriteBatchInternal::SetContents(&target, good).ok());
  EXPECT_EQ(target.Count(), 2u);
}

}  // namespace
}  // namespace talus
