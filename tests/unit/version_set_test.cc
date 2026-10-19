#include "version/version_set.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "test_util.h"
#include "util/file.h"
#include "util/filename.h"

namespace talus {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq) {
  std::string k;
  AppendInternalKey(&k, user_key, seq, ValueType::kValue);
  return k;
}

FileMetaData File(uint64_t number, std::string_view smallest, std::string_view largest,
                  uint64_t size = 1000) {
  FileMetaData f;
  f.number = number;
  f.file_size = size;
  f.smallest = IKey(smallest, 100);
  f.largest = IKey(largest, 1);
  return f;
}

class VersionSetTest : public ::testing::Test {
 protected:
  VersionSet::Options options() const {
    VersionSet::Options o;
    o.l0_compaction_trigger = 2;
    o.l1_max_bytes = 10000;
    return o;
  }

  std::unique_ptr<VersionSet> OpenSet(bool create = false) {
    auto vs = std::make_unique<VersionSet>(dir_.path(), options());
    if (create) {
      EXPECT_TRUE(vs->CreateNew().ok());
    }
    EXPECT_TRUE(vs->Recover().ok());
    return vs;
  }

  Status Apply(VersionSet* vs, VersionEdit* edit) {
    std::unique_lock<std::mutex> lock(mu_);
    return vs->LogAndApply(edit, &lock);
  }

  test::ScopedTempDir dir_;
  std::mutex mu_;
};

TEST_F(VersionSetTest, RecoverReproducesAppliedEdits) {
  {
    auto vs = OpenSet(/*create=*/true);
    VersionEdit e1;
    e1.AddFile(0, File(10, "a", "m"));
    e1.AddFile(0, File(11, "c", "z"));
    e1.AddFile(2, File(12, "b", "d"));
    vs->MarkFileNumberUsed(12);
    vs->SetLastSequence(77);
    ASSERT_TRUE(Apply(vs.get(), &e1).ok());
    VersionEdit e2;
    e2.DeleteFile(0, 10);
    e2.AddFile(1, File(13, "a", "f"));
    e2.SetLogNumber(9);
    vs->MarkFileNumberUsed(13);
    ASSERT_TRUE(Apply(vs.get(), &e2).ok());
  }
  auto vs = OpenSet();
  const auto v = vs->current();
  ASSERT_EQ(v->files(0).size(), 1u);
  EXPECT_EQ(v->files(0)[0]->number, 11u);
  ASSERT_EQ(v->files(1).size(), 1u);
  EXPECT_EQ(v->files(1)[0]->number, 13u);
  ASSERT_EQ(v->files(2).size(), 1u);
  EXPECT_EQ(vs->last_sequence(), 77u);
  EXPECT_EQ(vs->log_number(), 9u);
  EXPECT_GT(vs->NewFileNumber(), 13u);
  std::set<uint64_t> live;
  vs->AddLiveFiles(&live);
  EXPECT_EQ(live, (std::set<uint64_t>{11, 12, 13}));
}

TEST_F(VersionSetTest, FilesStayLiveWhileAnOldVersionHoldsThem) {
  auto vs = OpenSet(true);
  VersionEdit add;
  add.AddFile(1, File(20, "a", "b"));
  ASSERT_TRUE(Apply(vs.get(), &add).ok());
  const std::shared_ptr<const Version> old = vs->current();
  VersionEdit remove;
  remove.DeleteFile(1, 20);
  ASSERT_TRUE(Apply(vs.get(), &remove).ok());
  std::set<uint64_t> live;
  vs->AddLiveFiles(&live);
  EXPECT_EQ(live.count(20), 1u);  // `old` still references it.
}

TEST_F(VersionSetTest, RejectsOverlappingFilesInSortedLevels) {
  auto vs = OpenSet(true);
  VersionEdit edit;
  edit.AddFile(1, File(30, "a", "m"));
  edit.AddFile(1, File(31, "k", "z"));  // Overlaps [a, m].
  EXPECT_TRUE(Apply(vs.get(), &edit).IsCorruption());
  VersionEdit split;
  split.AddFile(1, File(32, "a", "k"));
  split.AddFile(1, File(33, "k", "z"));  // Same user key "k" in two files.
  EXPECT_TRUE(Apply(vs.get(), &split).IsCorruption());
  EXPECT_TRUE(vs->current()->files(1).empty());  // Unchanged on failure.
}

TEST_F(VersionSetTest, ScoresAndCompactionPicking) {
  auto vs = OpenSet(true);
  VersionEdit edit;
  edit.AddFile(0, File(40, "a", "c"));
  edit.AddFile(0, File(41, "b", "e"));  // Overlaps file 40.
  edit.AddFile(0, File(42, "x", "y"));  // Disjoint.
  edit.AddFile(1, File(43, "a", "b"));
  edit.AddFile(1, File(44, "d", "f"));
  edit.AddFile(1, File(45, "p", "q"));
  ASSERT_TRUE(Apply(vs.get(), &edit).ok());
  EXPECT_DOUBLE_EQ(vs->current()->compaction_scores()[0], 1.5);  // 3 files / trigger 2.

  std::vector<bool> busy(7, false);
  auto c = vs->PickCompaction(busy);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->level(), 0);
  // Starts at file 40, pulls in overlapping 41, then L1 files within [a, e].
  ASSERT_EQ(c->inputs(0).size(), 2u);
  std::set<uint64_t> l1;
  for (const auto& f : c->inputs(1)) {
    l1.insert(f->number);
  }
  EXPECT_EQ(l1, (std::set<uint64_t>{43, 44}));

  busy[1] = true;  // L1 reserved by another compaction.
  EXPECT_EQ(vs->PickCompaction(busy), nullptr);
}

TEST_F(VersionSetTest, L0OverlapExpandsTransitively) {
  auto vs = OpenSet(true);
  VersionEdit edit;
  edit.AddFile(0, File(50, "a", "c"));
  edit.AddFile(0, File(51, "c", "f"));
  edit.AddFile(0, File(52, "f", "h"));
  edit.AddFile(0, File(53, "x", "z"));
  ASSERT_TRUE(Apply(vs.get(), &edit).ok());
  std::vector<FilePtr> inputs;
  const std::string_view begin = "a";
  const std::string_view end = "b";
  vs->current()->GetOverlappingInputs(0, &begin, &end, &inputs);
  EXPECT_EQ(inputs.size(), 3u);  // a-c pulls in c-f, which pulls in f-h.
}

TEST_F(VersionSetTest, CorruptManifestIsCorruptionButTornTailIsNot) {
  {
    auto vs = OpenSet(true);
    VersionEdit first;
    first.AddFile(1, File(60, "a", "b"));
    ASSERT_TRUE(Apply(vs.get(), &first).ok());
    VersionEdit second;  // Will be torn: as if the process died mid-write.
    second.AddFile(1, File(61, "c", "d"));
    ASSERT_TRUE(Apply(vs.get(), &second).ok());
  }
  // Find the live MANIFEST through CURRENT.
  std::ifstream current(CurrentFileName(dir_.path()));
  std::string name;
  std::getline(current, name);
  const std::string manifest = dir_.path() + "/" + name;
  const auto size = std::filesystem::file_size(manifest);

  std::filesystem::resize_file(manifest, size - 3);  // Torn final record.
  {
    VersionSet vs(dir_.path(), options());
    ASSERT_TRUE(vs.Recover().ok());
    ASSERT_EQ(vs.current()->files(1).size(), 1u);       // The first edit survives;
    EXPECT_EQ(vs.current()->files(1)[0]->number, 60u);  // the torn one is ignored.
  }
  {
    std::fstream f(manifest, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(12);
    f.put('\x7f');  // Damages an early record's header or payload: its CRC fails.
  }
  VersionSet vs(dir_.path(), options());
  EXPECT_TRUE(vs.Recover().IsCorruption());
}

TEST(FileNameTest, ParseAndFormat) {
  uint64_t number = 0;
  FileType type{};
  EXPECT_EQ(TableFileName("db", 7), "db/000007.sst");
  EXPECT_EQ(LogFileName("db", 1234567), "db/1234567.log");
  EXPECT_EQ(DescriptorFileName("db", 3), "db/MANIFEST-000003");
  ASSERT_TRUE(ParseFileName("000007.sst", &number, &type));
  EXPECT_EQ(number, 7u);
  EXPECT_EQ(type, FileType::kTableFile);
  ASSERT_TRUE(ParseFileName("MANIFEST-000012", &number, &type));
  EXPECT_EQ(type, FileType::kDescriptorFile);
  EXPECT_EQ(number, 12u);
  ASSERT_TRUE(ParseFileName("CURRENT", &number, &type));
  ASSERT_TRUE(ParseFileName("LOCK", &number, &type));
  ASSERT_TRUE(ParseFileName("9.dbtmp", &number, &type));
  for (const char* bad : {"", "foo", "000007.txt", "MANIFEST-", "MANIFEST-12x", ".sst",
                          "99999999999999999999999.log", "12.log.bak"}) {
    EXPECT_FALSE(ParseFileName(bad, &number, &type)) << bad;
  }
}

TEST(FileNameTest, SetCurrentFileIsAtomicRename) {
  test::ScopedTempDir dir;
  ASSERT_TRUE(SetCurrentFile(dir.path(), 5).ok());
  std::ifstream in(CurrentFileName(dir.path()));
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "MANIFEST-000005\n");
  EXPECT_FALSE(FileExists(TempFileName(dir.path(), 5)));  // Renamed, not copied.
}

}  // namespace
}  // namespace talus
