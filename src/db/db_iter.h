#pragma once

#include <memory>
#include <vector>

#include "Talus/iterator.h"
#include "cache/table_cache.h"
#include "db/dbformat.h"
#include "memtable/memtable.h"
#include "table/iterator.h"
#include "version/version_set.h"

namespace talus {

struct SuperVersion;

// Adapts a memtable's skiplist iterator to InternalIterator.
std::unique_ptr<InternalIterator> NewMemTableIterator(const MemTable* mem);

// Iterates the files of one sorted level (L1+) as a single stream, opening
// each table only when the iteration reaches it. `files` must stay alive
// (the iterator's SuperVersion keeps its Version alive).
std::unique_ptr<InternalIterator> NewLevelIterator(const std::vector<FilePtr>* files,
                                                   TableCache* table_cache,
                                                   const TableReadOptions& options);

// The user-facing iterator. It takes the merged stream of internal entries
// and, at read sequence `sequence`, yields each user key's newest visible
// version, hiding deleted keys. It holds a reference on `super_version`
// (released on destruction) so the memtables and files it reads stay alive.
std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<InternalIterator> internal,
                                        SequenceNumber sequence, SuperVersion* super_version);

}  // namespace talus
