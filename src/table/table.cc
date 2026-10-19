#include "table/table.h"

#include <cstring>
#include <memory>
#include <utility>

namespace talus {

Table::Table(PrivateTag /*tag*/, std::unique_ptr<RandomAccessFile> file) : file_(std::move(file)) {}

Status Table::Open(std::unique_ptr<RandomAccessFile> file, std::unique_ptr<Table>* table) {
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

  auto t = std::make_unique<Table>(PrivateTag{}, std::move(file));
  BlockContents index_contents;
  s = ReadBlock(*t->file_, footer.index, &index_contents);
  if (!s.ok()) {
    return s;
  }
  t->index_block_ = Block(std::move(index_contents));
  if (!t->index_block_.well_formed()) {
    return Status::Corruption("malformed sstable index block");
  }

  if (footer.filter.size > 0) {
    BlockContents filter_contents;
    s = ReadBlock(*t->file_, footer.filter, &filter_contents);
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

Status Table::ReadDataBlock(std::string_view index_value, Block* block) const {
  BlockHandle handle;
  if (!handle.DecodeFrom(&index_value)) {
    return Status::Corruption("bad block handle in sstable index");
  }
  BlockContents contents;
  Status s = ReadBlock(*file_, handle, &contents);
  if (!s.ok()) {
    return s;
  }
  *block = Block(std::move(contents));
  if (!block->well_formed()) {
    return Status::Corruption("malformed sstable data block");
  }
  return Status::OK();
}

// A two-level iterator: the index block picks a data block, then a data-block
// iterator walks it. On any error (a failed read, a corrupt block) it stops
// and reports it rather than skipping the bad block.
class TableIterator final : public InternalIterator {
 public:
  explicit TableIterator(const Table* table) : table_(table), index_iter_(table->index_block_) {}

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
    if (!index_iter_.Valid()) {
      return;
    }
    read_status_ = table_->ReadDataBlock(index_iter_.value(), &data_block_);
    if (read_status_.ok()) {
      data_iter_.Reset(data_block_);
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
  BlockIter<> index_iter_;
  Block data_block_;
  BlockIter<> data_iter_;
  bool data_valid_ = false;
  Status read_status_;
};

std::unique_ptr<InternalIterator> Table::NewIterator() const {
  return std::make_unique<TableIterator>(this);
}

}  // namespace talus
