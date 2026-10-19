#include "table/table.h"

#include <cstring>
#include <memory>
#include <utility>

namespace talus {
namespace {

// A Pin cleanup that frees a block owned by the pin (one read with
// fill_cache = false, or with no block cache).
void DeleteBlock(void* block, void* /*unused*/) {
  const std::unique_ptr<Block> doomed(static_cast<Block*>(block));
}

}  // namespace

Table::Table(PrivateTag /*tag*/, const TableOpenOptions& options,
             std::unique_ptr<RandomAccessFile> file)
    : file_number_(options.file_number),
      block_cache_(options.block_cache),
      buffer_pool_(options.buffer_pool),
      file_(std::move(file)) {}

Status Table::Open(const TableOpenOptions& options, std::unique_ptr<RandomAccessFile> file,
                   std::unique_ptr<Table>* table) {
  const uint64_t size = file->size();
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file too short to be an sstable");
  }
  char footer_space[Footer::kEncodedLength];
  std::string_view footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                        footer_space);
  if (!s.ok()) {
    return s;
  }
  if (footer_input.size() != Footer::kEncodedLength) {
    return Status::Corruption("truncated sstable footer");
  }
  Footer footer;
  s = footer.DecodeFrom(footer_input);
  if (!s.ok()) {
    return s;
  }

  auto t = std::make_unique<Table>(PrivateTag{}, options, std::move(file));
  // The index and filter are read once and owned by the table, not cached.
  BlockContents index_contents;
  s = ReadBlock(*t->file_, footer.index, /*pool=*/nullptr, &index_contents);
  if (!s.ok()) {
    return s;
  }
  t->index_block_ = Block(std::move(index_contents));
  if (!t->index_block_.well_formed()) {
    return Status::Corruption("malformed sstable index block");
  }

  if (footer.filter.size > 0) {
    BlockContents filter_contents;
    s = ReadBlock(*t->file_, footer.filter, /*pool=*/nullptr, &filter_contents);
    if (!s.ok()) {
      return s;
    }
    s = ValidateFilter(filter_contents.data);
    if (!s.ok()) {
      return s;
    }
    // Over-allocate, then align within the buffer: avoids raw aligned new.
    constexpr size_t kAlign = BlockedBloomPolicy::kBlockBytes;
    const size_t n = filter_contents.data.size();
    t->filter_storage_ = std::make_unique_for_overwrite<char[]>(n + kAlign - 1);
    const auto base = reinterpret_cast<uintptr_t>(t->filter_storage_.get());
    char* aligned = t->filter_storage_.get() + ((kAlign - base % kAlign) % kAlign);
    std::memcpy(aligned, filter_contents.data.data(), n);
    t->filter_ = std::string_view(aligned, n);
  }
  *table = std::move(t);
  return Status::OK();
}

Status Table::GetDataBlock(std::string_view index_value, const TableReadOptions& options,
                           Block* storage, Pin* pin, const Block** block) const {
  BlockHandle handle;
  if (!handle.DecodeFrom(&index_value)) {
    return Status::Corruption("bad block handle in sstable index");
  }
  const bool use_cache = block_cache_ != nullptr && !file_->is_mmap();
  const BlockCacheKey key{file_number_, handle.offset};
  if (use_cache) {
    if (BlockCache::Handle* h = block_cache_->Lookup(key)) {
      if (options.stats != nullptr) {
        ++options.stats->block_cache_hits;
      }
      block_cache_->PinInto(h, pin);
      *block = BlockCache::value(h);
      return Status::OK();
    }
    if (options.stats != nullptr) {
      ++options.stats->block_cache_misses;
    }
  }

  BlockContents contents;
  Status s = ReadBlock(*file_, handle, buffer_pool_, &contents);
  if (!s.ok()) {
    return s;
  }
  if (contents.heap == nullptr) {
    // An uncompressed mmap block: used in place, valid while the table lives.
    *storage = Block(std::move(contents));
    if (!storage->well_formed()) {
      return Status::Corruption("malformed sstable data block");
    }
    *block = storage;
    return Status::OK();
  }
  auto owned = std::make_unique<Block>(std::move(contents));
  if (!owned->well_formed()) {
    return Status::Corruption("malformed sstable data block");
  }
  if (use_cache && options.fill_cache) {
    const size_t charge = owned->size();
    BlockCache::Handle* h = block_cache_->Insert(key, std::move(owned), charge);
    block_cache_->PinInto(h, pin);
    *block = BlockCache::value(h);
  } else {
    *block = owned.get();
    pin->Add(&DeleteBlock, owned.release(), nullptr);
  }
  return Status::OK();
}

// A two-level iterator: the index block picks a data block, then a data-block
// iterator walks it while a Pin keeps the block alive. On any error (a failed
// read, a corrupt block) it stops and reports it rather than skipping the
// bad block.
class TableIterator final : public InternalIterator {
 public:
  TableIterator(const Table* table, const TableReadOptions& options)
      : table_(table), options_(options), index_iter_(table->index_block_) {}

  bool Valid() const override { return data_valid_ && data_iter_.Valid(); }

  void SeekToFirst() override {
    index_iter_.SeekToFirst();
    LoadDataBlock();
    if (data_valid_) {
      data_iter_.SeekToFirst();
    }
    SkipExhaustedBlocks();
  }

  void Seek(std::string_view target) override {
    index_iter_.Seek(target);
    LoadDataBlock();
    if (data_valid_) {
      data_iter_.Seek(target);
    }
    SkipExhaustedBlocks();
  }

  void Next() override {
    data_iter_.Next();
    SkipExhaustedBlocks();
  }

  std::string_view key() const override { return data_iter_.key(); }
  std::string_view value() const override { return data_iter_.value(); }

  Status status() const override {
    if (!index_iter_.status().ok()) {
      return index_iter_.status();
    }
    if (!read_status_.ok()) {
      return read_status_;
    }
    return data_valid_ ? data_iter_.status() : Status::OK();
  }

 private:
  void LoadDataBlock() {
    data_valid_ = false;
    pin_.Reset();  // Release the previous block.
    if (!index_iter_.Valid()) {
      return;
    }
    const Block* block = nullptr;
    read_status_ = table_->GetDataBlock(index_iter_.value(), options_, &storage_, &pin_, &block);
    if (read_status_.ok()) {
      data_iter_.Reset(*block);
      data_valid_ = true;
    }
  }

  // Moves to the next data block while the current one is exhausted. Stops
  // on any error.
  void SkipExhaustedBlocks() {
    while (data_valid_ && !data_iter_.Valid()) {
      if (!data_iter_.status().ok()) {
        return;
      }
      index_iter_.Next();
      LoadDataBlock();
      if (data_valid_) {
        data_iter_.SeekToFirst();
      }
    }
  }

  const Table* const table_;
  const TableReadOptions options_;
  BlockIter<> index_iter_;
  Block storage_;  // Holds mmap blocks.
  Pin pin_;        // Keeps the current cached or owned block alive.
  BlockIter<> data_iter_;
  bool data_valid_ = false;
  Status read_status_;
};

std::unique_ptr<InternalIterator> Table::NewIterator(const TableReadOptions& options) const {
  return std::make_unique<TableIterator>(this, options);
}

}  // namespace talus
