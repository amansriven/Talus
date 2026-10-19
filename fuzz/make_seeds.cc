// Writes a seed corpus for each fuzz target: valid inputs built with the
// real encoders, plus truncated and bit-flipped variants. Starting from
// valid structure lets the fuzzer get past checksums and magic numbers into
// the parsing code.
//
// Usage: talus_fuzz_seeds OUTPUT_DIR
//   creates OUTPUT_DIR/{block_reader,sstable_reader,wal_reader,version_edit}/

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Talus/write_batch.h"
#include "db/write_batch_internal.h"
#include "table/block_builder.h"
#include "table/table_builder.h"
#include "util/file.h"
#include "version/version_edit.h"
#include "wal/log_writer.h"

namespace talus {
namespace {

namespace fs = std::filesystem;

std::string IKey(const std::string& user_key, SequenceNumber seq, ValueType type) {
  std::string k;
  AppendInternalKey(&k, user_key, seq, type);
  return k;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Writes `seed` and some damaged variants of it.
void WriteSeeds(const fs::path& dir, const std::string& name, const std::string& seed) {
  fs::create_directories(dir);
  auto write = [&](const std::string& suffix, const std::string& bytes) {
    std::ofstream(dir / (name + suffix), std::ios::binary) << bytes;
  };
  write("", seed);
  if (!seed.empty()) {
    write("_half", seed.substr(0, seed.size() / 2));
    std::string flipped = seed;
    flipped[flipped.size() / 3] = static_cast<char>(flipped[flipped.size() / 3] ^ 0x20);
    write("_flipped", flipped);
  }
}

int Run(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s OUTPUT_DIR\n", argv[0]);
    return 2;
  }
  const fs::path out = argv[1];
  const fs::path scratch = out / "scratch";
  fs::create_directories(scratch);

  // Blocks: a few restart intervals and key shapes.
  for (int interval : {1, 4, 16}) {
    BlockBuilder builder(interval);
    for (int i = 0; i < 40; ++i) {
      builder.Add(IKey("key" + std::to_string(1000 + i), 100 - static_cast<SequenceNumber>(i),
                       ValueType::kValue),
                  "value" + std::to_string(i));
    }
    WriteSeeds(out / "block_reader", "restart" + std::to_string(interval),
               std::string(builder.Finish()));
  }

  // SSTables: with and without filters.
  for (FilterType filter :
       {FilterType::kNone, FilterType::kBlockedBloom, FilterType::kStandardBloom}) {
    const fs::path path = scratch / "seed.sst";
    {
      std::unique_ptr<WritableFile> file;
      (void)WritableFile::Create(path.string(), &file);
      TableOptions options;
      options.block_size = 256;
      options.filter_type = filter;
      TableBuilder builder(options, file.get());
      for (int i = 0; i < 100; ++i) {
        builder.Add(IKey("k" + std::to_string(10000 + i), 7, ValueType::kValue),
                    std::string(20 + i % 30, 'v'));
      }
      (void)builder.Finish();
      (void)file->Close();
    }
    WriteSeeds(out / "sstable_reader", "filter" + std::to_string(static_cast<int>(filter)),
               ReadFile(path));
  }

  // WAL: several records holding batches.
  {
    const fs::path path = scratch / "seed.log";
    {
      std::unique_ptr<WritableFile> file;
      (void)WritableFile::Create(path.string(), &file);
      log::Writer writer(std::move(file));
      for (int r = 0; r < 5; ++r) {
        WriteBatch batch;
        batch.Put("key" + std::to_string(r), "value" + std::to_string(r));
        batch.Delete("gone" + std::to_string(r));
        WriteBatchInternal::SetSequence(&batch, 1 + static_cast<SequenceNumber>(r) * 2);
        (void)writer.AddRecord(WriteBatchInternal::Contents(batch));
      }
    }
    WriteSeeds(out / "wal_reader", "batches", ReadFile(path));
  }

  // VersionEdits: one using every field.
  {
    VersionEdit edit;
    edit.SetLogNumber(5);
    edit.SetNextFileNumber(20);
    edit.SetLastSequence(1000);
    edit.SetCompactPointer(1, IKey("m", 5, ValueType::kValue));
    edit.DeleteFile(2, 9);
    FileMetaData f;
    f.number = 11;
    f.file_size = 4096;
    f.smallest = IKey("a", 9, ValueType::kValue);
    f.largest = IKey("z", 1, ValueType::kDeletion);
    edit.AddFile(0, f);
    std::string encoded;
    edit.EncodeTo(&encoded);
    WriteSeeds(out / "version_edit", "all_fields", encoded);
  }

  fs::remove_all(scratch);
  std::printf("wrote seed corpora to %s\n", out.string().c_str());
  return 0;
}

}  // namespace
}  // namespace talus

int main(int argc, char** argv) { return talus::Run(argc, argv); }
