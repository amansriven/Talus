#pragma once

#include <string>

#include "Talus/status.h"
#include "db/dbformat.h"

namespace talus {

class MemTable;

// Replays the WAL file at `path` into `mem`, applying each record's
// WriteBatch in order.
//
// A torn final record (the normal result of a crash) ends replay cleanly.
// The first corrupt record, meaning a bad checksum, an unknown type or a
// malformed batch, also ends replay: that record and everything after it are
// treated as never written, and OK is returned. With `paranoid_checks`,
// Corruption is returned instead. A batch is validated before any of it is
// applied, so a batch is never half-applied.
//
// On return, *max_sequence is at least the last sequence number applied.
Status ReplayLogFile(const std::string& path, bool paranoid_checks, MemTable* mem,
                     SequenceNumber* max_sequence);

}  // namespace talus
