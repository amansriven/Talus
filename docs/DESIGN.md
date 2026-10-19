# Talus design

This document describes Talus's on-disk formats, concurrency model and
invariants precisely enough to write a compatible reader from it alone. It
grows with each build phase (see [PHASES.md](PHASES.md)); sections for
components not yet built are absent rather than stubbed.

## 1. Decisions where the spec was ambiguous

| Topic | Decision | Rationale |
|---|---|---|
| Byte order | All fixed-width integers are little-endian. Big-endian hosts are rejected at compile time (`static_assert` in `src/util/coding.h`). | Both target platforms (x86-64, ARM64) are little-endian; encoding with `memcpy` is then a single load/store. |
| Stored CRCs | Every CRC-32C written to disk is **masked** (§3.2). | Same as LevelDB: computing a CRC over data that itself contains CRCs is otherwise prone to degenerate results. |
| Varint strictness | Decoders reject encodings longer than the maximum, and final bytes that would overflow the destination type. | LevelDB silently truncates such values. Rejecting them turns corrupt input into `Corruption` rather than a wrong value. |
| Arena alignment | 8 bytes, not `alignof(max_align_t)`. | The spec requires alignment "for atomics"; 8 suffices for `std::atomic<T*>` and `uint64_t`, while 16 (x86-64's `max_align_t`) wastes space in every skiplist node. |
| Cache-line size | 128 bytes on Apple Silicon, 64 elsewhere (`kCacheLineSize`, `src/util/port.h`). | Apple's M-series cores use 128-byte lines. Padding to 64 there would still leave adjacent slots sharing a line. |
| WAL record `type` byte | Only `1` (full record) is defined; `0` is reserved and any other value is corruption. Records are never fragmented. | The spec defines a type byte without values. Its 4-byte length removes LevelDB's need to fragment records across 32 KiB blocks. Reserving `0` means zero-filled space never parses as a valid record. |
| Torn WAL tail with `paranoid_checks` | Still a clean end of log, not `Corruption`. Only a complete record that fails its checksum, has an unknown type or holds a malformed batch is corruption. | This matches LevelDB. A torn tail is the normal result of a crash, so reporting it would make every crash unrecoverable in paranoid mode. |
| WAL length field pointing past end of file | Treated as a torn tail (end of log). | It can't be told apart from a crash mid-write. Checking it against the file size first also stops a corrupt length from causing a huge allocation. |
| Sync and non-sync writes in one group | Allowed. The group fsyncs if any member asked for sync. | The spec says so explicitly. LevelDB instead keeps sync writes out of a non-sync leader's group. |
| SSTable filter granularity | One filter per table, over all of its user keys, not LevelDB's one filter per 2 KiB of data. | The spec describes a single filter block that lets a read skip the whole table. This is RocksDB's "full filter". A lookup does one probe per table, before any index search. |
| Index block restart interval | 1 (every index entry is a restart point). | Index lookups binary-search the restart points, so this makes the search cover every entry. Separator keys are short, so the lost prefix compression costs little. |
| Compressed block kept only if it saves ≥ 12.5% | Otherwise the block is stored uncompressed (type 0). | Same rule as LevelDB: a barely-compressible block costs decompression time on every read for little space. |
| mmap reads and the block cache | In mmap mode, uncompressed data blocks bypass the block cache and are used in place in the mapping. | A cached block pointing into a mapping would dangle once its table was closed, and the OS page cache already holds the pages. A decompressed LZ4 block owns its memory, so it is cached as usual. |
| Index and filter blocks | Held by the open `Table` for its lifetime (so they live in the table cache), not in the block cache. | Spec §7 lists them under the table cache: "file handle, parsed index, filter". |
| Table-cache sharding | 16 shards when `max_open_files ≥ 256`, otherwise 1. Each shard gets `max_open_files / shards`, rounded down. | Rounding down keeps the total within the bound, and a single shard keeps small bounds exact. |
| Opening a database that does not exist | Fails unless `create_if_missing` is set; `error_if_exists` is also honored. A `LOCK` file (`flock`) rejects a second concurrent open with `Busy`. | These match LevelDB; the spec does not define Open's behavior. |
| MVCC drop rule | LevelDB's: an entry is dropped if a newer entry of the same key is at or below the *smallest* live snapshot, and a tombstone is dropped if it is at or below that snapshot and no deeper level may hold the key. | This keeps every version any live snapshot can see, as the spec requires, and sometimes a little more. A per-snapshot-stripe rule would reclaim more space under many snapshots, but it is more complex. |
| Compaction output boundaries | An output file ends only between two user keys, never between versions of the same key. | Then each user key in L1+ lives in exactly one file. This prevents LevelDB's old bug where newer versions of a key were compacted below older ones (later fixed with "boundary inputs"). `VersionSet` rejects any version that breaks the rule. |
| Corrupt record in an older WAL during recovery | Replay of that WAL stops at the record, and replay continues with later WALs. | This matches LevelDB. "Everything after it" is read as the rest of that file. |
| Empty `CompactRange` bounds | An empty `begin` or `end` means unbounded. | `std::string_view` has no null value. |
| Which L0 file is newest | L0 is ordered by file number, which grows over time. | Flushes always write to L0 and compactions never do, so a higher number is always newer data. |
| Value lifetime for a memtable hit in `Get(PinnableValue*)` | The value is pinned through a reference on the SuperVersion that holds the memtable. | This is what lets the memtable path avoid a copy. The cost is one atomic increment and one decrement on a shared counter per pinned memtable read. `Get(std::string*)` copies instead and avoids it. |
| `DBStats::user_bytes_written` | The bytes of each batch's serialized operations (tags, length varints, keys and values), excluding the 12-byte batch header. | The spec does not define "bytes written by users". This is what the WAL actually stores per operation, so the WAL's write amplification is close to 1. |
| Space amplification estimate | Live bytes ≈ the size of the deepest non-empty level, as in RocksDB's estimate. | Data that has been fully compacted ends up in the last level with one version per key. Upper levels hold newer versions and tombstones not yet merged. |
| A MANIFEST whose last edit was lost | Open returns `Corruption` if the WAL named by the MANIFEST's log number does not exist. | Damage to the final record's length field reads exactly like a crash-torn tail, so recovery would silently drop a *durable* edit. The writes that edit made obsolete would then be gone from both the MANIFEST and the deleted WAL: wrong data, with no error. Losing a compaction edit is caught anyway, as missing input tables. A lost flush edit is caught by this check, because that WAL always exists while it is named: it is created before any edit names it and deleted only after a newer edit names another. Found while designing the corruption tests. |
| `CURRENT` names a missing MANIFEST | `Corruption`, not `IOError`. | A dangling pointer in our own metadata is damage, not an I/O failure. |
| WAL write failure | Sticky. Every later write returns the same error. | A failed append can leave a partial record in the log, and recovery would drop every record after it. LevelDB has the same "background error" rule. |

## 2. Error handling

Fallible operations return `talus::Status` (`include/Talus/status.h`) with
code `OK`, `NotFound`, `Corruption`, `IOError`, `InvalidArgument` or `Busy`.
The class is `[[nodiscard]]`. A status without a message holds a null pointer
and never allocates, so `OK` and message-less `NotFound` are free to return on
hot paths.

## 3. Primitive encodings

### 3.1 Integers

| Encoding | Layout |
|---|---|
| `fixed32` / `fixed64` | 4 / 8 bytes, little-endian. |
| `varint32` / `varint64` | Unsigned LEB128: 7 value bits per byte, least significant group first; bit 7 set on every byte except the last. At most 5 bytes (varint32) or 10 bytes (varint64). In a 5-byte varint32 the last byte must be ≤ `0x0f`; in a 10-byte varint64 it must be ≤ `0x01`. |
| length-prefixed bytes | `varint32 length` followed by `length` raw bytes. |

### 3.2 CRC-32C

- Polynomial: Castagnoli, `0x1EDC6F41` (reflected form `0x82F63B78`). Initial value and final XOR are both `0xFFFFFFFF`, as in iSCSI (RFC 3720). The CRC of `"123456789"` is `0xE3069283`.
- **Masking.** A CRC `c` is stored as `mask(c) = ((c >> 15) | (c << 17)) + 0xA282EAD8` (32-bit arithmetic). A reader computes the CRC of the data and compares it with `unmask(stored)`, where `unmask(m) = (r >> 17) | (r << 15)` with `r = m - 0xA282EAD8`.
- **Implementations.** `crc32c::Extend` resolves a function pointer once, on first use:
  1. x86-64: `_mm_crc32_u64`, compiled with `__attribute__((target("sse4.2")))` and used only if `__builtin_cpu_supports("sse4.2")`. The rest of the binary does not require SSE4.2.
  2. ARM64: `__crc32cd`. On Linux the file is compiled with `-march=armv8-a+crc` and the instructions run only if `AT_HWCAP` has `HWCAP_CRC32`. Every Apple Silicon CPU has them.
  3. Otherwise: slicing-by-8 software CRC, using eight 256-entry tables generated at compile time (`constexpr`).

  The hardware loops first consume bytes singly up to an 8-byte boundary, so their 8-byte loads never straddle a cache line.

### 3.3 64-bit hash

`Hash64(data, n, seed)` (`src/util/hash.h`) implements the published XXH64
algorithm exactly, so it can be tested against reference vectors. It is used
for bloom filter probes and block-cache sharding, and is not persisted except
inside filter blocks (described with the SSTable format).

## 4. Concurrency primitives

### 4.1 Concurrent arena (`src/util/arena.{h,cc}`)

The arena is a thread-safe bump allocator for memtable nodes. Memory is never
freed individually; the arena frees every block in its destructor.

**Layout.** Each block is one allocation from `std::aligned_alloc`:
- a header padded to one cache line, holding `next`, `capacity`, `total_bytes` and `std::atomic<size_t> used`;
- then `capacity` data bytes.

The default block is 4 KiB in total. With `use_huge_pages` it is 2 MiB, 2 MiB-aligned, and advised `MADV_HUGEPAGE` on Linux.

Keeping `used` (the word every allocating thread writes) on its own line means that writes to newly allocated memory don't invalidate it.

**Fast path.** `Allocate(n)`:
1. Round `n` up to 8.
2. `b = current_.load(acquire)`.
3. `off = b->used.fetch_add(n, relaxed)`.
4. If `off + n <= b->capacity`, return `b->data + off`.

**Slow path.** Taken when the block is exhausted:
1. Lock `mu_`.
2. If another thread already installed a newer block, try it.
3. Otherwise allocate a new block, claim the first `n` bytes with `used.store(n)`, and publish it with `current_.store(release)`.

Requests larger than a quarter of a block's capacity get a dedicated block under the mutex, which is never published as `current_`.

**Invariants and justification.**
- *No overlapping ranges.* Every allocation from a block comes from one `fetch_add` on that block's `used`. All read-modify-writes on one atomic object are totally ordered (its modification order), so each caller receives a distinct `off` whatever memory order it uses. `relaxed` is therefore sufficient: the counter partitions addresses and publishes no data. Once `used` exceeds `capacity`, every later caller fails the bounds check and takes the slow path, abandoning the tail of that block. `used` cannot overflow in practice: it exceeds `capacity` by at most the sum of the in-flight request sizes.
- *No torn block headers.* A new block's header is fully written, including the claim of the installer's range, before the `release` store to `current_`. Readers load `current_` with `acquire`, so they see `capacity`, and `used` at least at the installer's claim. A range claimed before publication is therefore never handed out twice.
- *Publication of user data* is not the arena's job. Callers (e.g. the skiplist) publish what they write into arena memory with their own release/CAS operations.
- *Memory reclamation.* There is none during the arena's lifetime. Blocks are freed only in the destructor, which by contract runs after all users are done. A returned pointer therefore stays valid for the life of the arena, with no hazard pointers or epochs needed.
- `memory_usage_` is a relaxed statistic: it is used only to decide when a memtable is full, and an approximate value is acceptable.

Block memory comes from `std::aligned_alloc`, not `operator new`. The spec's zero-allocation test explicitly excludes arena growth, and using the malloc family keeps that growth invisible to the counting `operator new`.

## 5. Internal keys and MVCC (`src/db/dbformat.{h,cc}`)

```
internal_key = user_key || fixed64(tag)        tag = sequence << 8 | type
type:  0 = Deletion, 1 = Value                 sequence: 56 bits, 0 .. 2^56 - 1
```

**Ordering.** Internal keys sort by user key (bytewise, `memcmp` order with
shorter-prefix-first) ascending, then by `tag` descending. The newest version of
a user key therefore comes first, and at equal sequence numbers `Value` sorts
before `Deletion`. A batch never writes two operations with the same sequence,
so in practice that tie never arises.

**Lookups.** A read at sequence `s` seeks to `user_key || fixed64(s << 8 | 1)`.
Type 1 is the highest type, so this key sorts immediately before every entry
for that user key with sequence ≤ `s`. The first entry at or after it is the
newest visible version, provided its user key matches. `LookupKey` builds this
key in a 200-byte inline buffer and falls back to the heap only for longer
user keys, so typical lookups do not allocate.

## 6. Memtable (`src/memtable/`)

### 6.1 Skiplist node layout

Each node is one contiguous arena allocation, 8-byte aligned:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `key_size` (uint32, internal key length) |
| 4 | 4 | `value_size` (uint32) |
| 8 | `key_size` | internal key |
| 8 + `key_size` | `value_size` | value |
| 8 + round_up(`key_size` + `value_size`, 8) | 8 × height | `std::atomic<Node*> next[height]` |

- **Height.** Heights are in [1, 12]. Each extra level is kept with probability 1/4: two random bits per level, taken from the high 32 bits of one draw from a thread-local xorshift64* generator. Each thread seeds its generator once, from a global counter passed through SplitMix64.
- **Head node.** The head is a node with an empty key and value and the full 12 levels.
- **Writing entries.** Writers encode the internal key and value directly into node memory through a fill callback, so an insert makes no allocation other than the arena's.
- **Layout trade-off.** Because the spec puts the links after the value, a search touches a node's header and key, and also its `next[level]`, which is on a different cache line when key and value exceed about 50 bytes. Prefetching (§6.3) is meant to hide this second miss. Experiment 7 measures the effect.

### 6.2 Concurrency

There are two insert modes:
- **`Insert`, single writer.** Callers serialize inserts. The new node's links are set with relaxed stores, then each predecessor is updated with a release store, from level 0 upward.
- **`InsertConcurrently`, lock-free with many writers.** At each level from 0 upward:
  1. Set `new.next[L] = next` (relaxed).
  2. CAS `prev.next[L]` from `next` to `new`: release on success, relaxed on failure.
  3. On failure, recompute the splice for level `L`, starting from `prev`, and retry.

Both modes first raise `max_height_` (relaxed store or CAS loop). Readers never lock.

The invariants that make this safe, and the reason for each memory order:

1. **Sublist property.** Every level's list is sorted strictly ascending, and every node in level L+1 is in level L. Bottom-up linking ensures this: a node's level-L CAS succeeds before its level-(L+1) link is attempted. A reader or writer that reaches a node at level L+1 can therefore descend from it to level L.
2. **Safe publication.** A node is reachable at level L only through a release store or CAS of a level-L link. Before that point, its header, key, value and `next[L]` are all written. Every link load that is followed by a dereference uses acquire, so a thread that reaches a node sees it fully initialized. This holds transitively when one writer links its node in front of another writer's node, because that writer read the other node through an acquire load.
3. **CAS retry is sound.** Nodes are never removed, so a failed CAS at level L leaves `prev.key < key` true. Restarting the splice from `prev` is therefore valid. A CAS fails only when another thread's CAS on the same link succeeded, so some thread always makes progress (lock-freedom).
4. **`max_height_` is relaxed.** A reader that sees a raised height before the head's new links are set reads null at those levels and descends. A reader that sees a stale height starts lower, which is still correct by (1).
5. **Prefetch reads are relaxed.** The prefetch hint reads `n.next[L]` relaxed and passes it to `__builtin_prefetch`, which never dereferences it.
6. **Memory reclamation.** None is needed while the skiplist exists. Nodes live in the memtable's arena (§4.1), which is freed only when the memtable is destroyed, after its last reader is gone (reference counting arrives with the SuperVersion in phase 6).
7. **Unique keys.** Keys must be unique. The memtable guarantees this because every entry carries a distinct sequence number.

**Verification.** `SkipListConcurrentTest` has readers scan while writers insert. Each writer publishes its progress with a release store; each reader acquires those progress values before scanning and checks three things:
- the scan is strictly ascending;
- each writer's keys appear without gaps;
- every key published before the scan is present.

As a check of the check, changing the two publishing operations from `release` to `relaxed` makes TSan report data races in these tests.

### 6.3 Prefetching

`SkipList` takes a template parameter, `kPrefetch`, which is on by default. While a search compares the key of candidate node `n`, it issues `__builtin_prefetch(n.next[L], /*read*/ 0, /*low locality*/ 1)`, so the node after `n` is already loading if the search advances. The template parameter removes the hint at compile time, giving the off case of experiment 7 with no runtime branch.

### 6.4 MemTable

`MemTable` stores internal keys with their values (empty for deletions) in a
`SkipList<InternalKeyComparator>` backed by its own `Arena`.
- `Add` uses `Insert` or `InsertConcurrently`, depending on `concurrent_writes`, which comes from the `allow_concurrent_memtable_write` option.
- `Get` seeks to the lookup key and checks the user key. It returns the value as a `std::string_view` into arena memory, which is the basis of the zero-copy `PinnableValue` path, or `NotFound` for a deletion.
- `ApproximateMemoryUsage` reports the arena's reserved bytes. It decides when to switch memtables (phase 6).

## 7. Write path

### 7.1 WriteBatch (`include/Talus/write_batch.h`, `src/db/write_batch*.{h,cc}`)

```
batch := sequence (fixed64) | count (fixed32) | op[count]
op    := 0x01 | varint32 key_len | key | varint32 value_len | value      (Put)
       | 0x00 | varint32 key_len | key                                  (Delete)
```

- **Sequence numbers.** Operation *i* (0-based) of a batch gets sequence number `sequence + i`.
- **Decoding.** Decoding uses a template visitor (`WriteBatchInternal::Iterate`), so the memtable insert loop has no virtual calls. A batch is malformed, and decodes as `Corruption`, if it is shorter than 12 bytes, holds a truncated operation or an unknown tag byte, or has an operation count that differs from `count`.
- **Reuse.** `Clear()` keeps the batch's capacity, so a reused batch doesn't allocate.

### 7.2 WAL file (`NNNNNN.log`, `src/wal/`)

A log file is a sequence of records with no padding or block structure:

```
record := masked_crc32c (fixed32) | length (fixed32) | type (1B = 0x01) | payload[length]
```

- **CRC.** `masked_crc32c` is `mask(crc32c(type || payload))` (§3.2). The CRC does not cover the length. A corrupted length either fails the CRC (wrong span) or points past the end of the file (§1).
- **Payload.** In a WAL, the payload is one serialized `WriteBatch`. For a commit group it is the group's batches merged into one (§7.4).
- **Reading a record.** A reader handles each record as follows:
  1. Fewer than 9 header bytes before the end of file, or a `length` that reaches past it, means a torn tail: a clean end of log.
  2. Otherwise it reads the payload and verifies the CRC, then the type.
  3. A mismatch is `Corruption`.
- **Replay (`ReplayLogFile`).** Recovery applies records in order. Before applying a batch it validates the whole batch, so a malformed batch is never half-applied. The first corrupt record ends replay: that record and everything after it count as never written. With `paranoid_checks`, replay returns `Corruption` instead. A torn tail is never corruption (§1).

### 7.3 Files and durability (`src/util/file.{h,cc}`)

- **Descriptors.** Every descriptor is owned by `FileDescriptor` (RAII). `WritableFile` keeps one 64 KiB buffer, allocated once at creation. Appends larger than the buffer bypass it.
- **After each record.** `log::Writer::AddRecord` encodes the header on the stack and flushes after every record. An acknowledged non-sync write therefore survives a process crash, though not a machine crash.
- **`Sync()`.** This is a flush plus `fdatasync` on Linux, or `fcntl(F_FULLFSYNC)` on macOS, falling back to `fsync` if the file system rejects `F_FULLFSYNC`. Plain `fsync` on macOS does not flush the drive's write cache.
- **`SyncDirectory()`.** This fsyncs a directory, which makes file creation and renames durable. The DB uses it when it creates WAL and MANIFEST files and renames `CURRENT` (phase 6).

### 7.4 Group commit (`src/db/write_thread.{h,cc}`, `src/db/group_commit.{h,cc}`)

`GroupCommitter::Write` runs this protocol:

1. **Join.** The writer pushes a stack-allocated `Writer` onto a lock-free stack (`newest_writer_`, CAS with `acq_rel`). If the stack was empty, it becomes the leader. Otherwise it waits.
2. **Form the group.** The leader walks from the stack head back to itself, filling in `link_newer`, which gives the queue in FIFO order. It then takes writers in order until their batches total 1 MiB. Its own batch always counts, even when larger.
3. **Assign sequences.** The leader gives each member a consecutive range, starting at `last_sequence + 1`, and writes it into the member's batch header.
4. **Write the WAL.** The leader makes one `AddRecord` call. For a group of one, the record is the leader's batch. Otherwise it is a merged batch: the leader's header (first sequence), the summed count, and every member's operations in order. Because the ranges are consecutive, replaying the merged batch assigns exactly the same sequence numbers. If any member asked for sync, the leader then does one `Sync()`. The merge buffer is a reused `std::string`.
5. **Insert into the memtable.** If the memtable allows concurrent writes and the group has more than one member, the leader wakes every follower with `kParallelMemTableWriter`. Each member, the leader included, inserts its own batch and then decrements `group.running` (`acq_rel`). Otherwise the leader inserts every batch and sets each member's status.
6. **Publish.** The last writer to finish (the leader, or whoever brought `running` to 0) publishes `group.last_sequence` with a release store to `visible_sequence_`. It then exits the group: it hands leadership to the next queued writer, if any, and wakes every member with its own status.

**Ordering guarantees.**
- *Groups are strictly serialized.* The next leader is chosen only in step 6, after publication, so a visible sequence `S` implies that every batch at or below `S` is fully inserted.
- *Reads are consistent.* A reader that acquires `visible_sequence_` and reads at that sequence sees whole batches only. The last decrement acquires every member's release of its inserts, and the release store of `visible_sequence_` passes them on to the reader.
- *Leader-only state needs no lock.* This state is the sequence counter, the log writer, the merge buffer and the sticky error. Leadership passes either by `SetState` (release) to the next leader's `AwaitState` (acquire), or by the exit CAS that empties the stack (release) to the next pusher's CAS (acquire).

The full set of invariants (list structure, lifetime, waiting) is in the comment block at the top of `src/db/write_thread.h`.

**Adaptive waiting.** A waiter first polls its `state` up to `spin_iterations` times with `CpuRelax()` (PAUSE on x86-64, YIELD on ARM64), then blocks in `std::atomic::wait`, which is a futex on Linux and `__ulock` on macOS.

A simple `store(); notify_one()` would be unsafe here. A waiter that sees the new state while still spinning can return and destroy its stack `Writer` before the setter's `notify_one()` runs. The protocol therefore has an explicit handshake:
1. A waiter about to block CASes its state to `kLockedWaiting`. A failed CAS means the setter already changed the state, and the waiter returns.
2. A setter that finds the waiter still spinning (state not `kLockedWaiting`) changes the state with one CAS, which is its only access to the `Writer`.
3. A setter that finds `kLockedWaiting` stores `new | kNotifyPending`, calls `notify_one()`, then stores `new`. That final store is its last access to the `Writer`.
4. A woken waiter never returns while `kNotifyPending` is set. That window is a single syscall, so it yields while it waits.

So `notify_one()` never runs on a destroyed object. The tests run every group-commit scenario with `spin_iterations = 0` as well, which forces every wait through this blocking path.

**Spin threshold.** The default is 256 polls, roughly 1–10 µs depending on the CPU's PAUSE/YIELD latency. That is about the time of one group's `write(2)` plus its memtable inserts. Waits that short stay in user space, while waits for an fsync (milliseconds) go to the kernel. This is a starting value, not yet a measurement. Experiment 1 in `PERFORMANCE.md` sweeps it against sync-write throughput and p99, and this paragraph will be updated with the result.

**Early observation, not a benchmark.** On an Apple M3 with 8 threads writing one-op, 100-byte batches, groups averaged 4 writers, with a maximum of 8. 400 sync writes needed 93–99 fsyncs. Parallel memtable inserts were slower than leader-only inserts for these tiny batches (about 200k vs 320k writes/s), so waking followers seems to cost more than one small insert. Experiment 2 will measure this properly.

## 8. SSTable format (`NNNNNN.sst`, `src/table/`)

### 8.1 File layout

```
[data block 0] ... [data block N-1] [filter block] [index block] [footer]
```

- **Block trailer.** Every block, the filter and index included, is followed by a 5-byte trailer:
  ```
  compression_type (1B: 0 = none, 1 = LZ4) | masked crc32c (fixed32)
  ```
  The CRC covers the stored block bytes plus the type byte. `ReadBlock` verifies it on **every** read from disk, then decompresses the block if needed. A bad checksum, an unknown type or a handle beyond the end of the file is `Corruption`.
- **Block handle.** `BlockHandle = varint64 offset | varint64 size`, where `size` excludes the trailer.
- **Footer.** The footer is the last 48 bytes of the file:

  | Offset | Size | Field |
  |---|---|---|
  | 0 | ≤ 20 | filter handle (size 0 if the table has no filter) |
  | after it | ≤ 20 | index handle |
  | up to 40 | – | zero padding |
  | 40 | 8 | magic, fixed64 `0x54535373756C6154`: the bytes `"TalusSST"` |

  Data blocks come first, then the filter, then the index. So `filter.offset + filter.size + 5 == index.offset`, and `index.offset + index.size + 5 == file_size - 48`; a test checks both.
- **LZ4 blocks.** With `-DTalus_WITH_LZ4=ON`, a compressed block's bytes are `varint32 uncompressed_size | LZ4 block-format data`. A reader rejects a claimed size above `255 × compressed + 16` before allocating, which is LZ4's maximum expansion. A build without LZ4 reports such a block as `Corruption`, and refuses to write LZ4 tables (`InvalidArgument`).

### 8.2 Blocks (`block_builder.{h,cc}`, `block.{h,cc}`)

Data and index blocks share one format:

```
entry   := varint32 shared | varint32 non_shared | varint32 value_len | key[shared:] | value
block   := entry* | fixed32 restart_offset[num_restarts] | fixed32 num_restarts
```

- **Prefix compression.** `shared` is the length of the prefix this key shares with the previous key. Every `block_restart_interval` entries (16 by default), an entry is a **restart point**: it stores its full key (`shared = 0`) and its offset goes into the restart array. The first entry is always a restart point. An empty block is `fixed32 0 | fixed32 1`.
- **`Seek(target)`.** The reader binary-searches the restart points for the last one whose key is less than `target`, then scans forward. Each restart key is read in place, with no copy.
- **Rebuilding keys.** The iterator rebuilds the current key in a `KeyBuffer`, which stores keys of up to 128 bytes in place, so iterating a cached block doesn't allocate.
- **Malformed blocks.** The reader never reads outside the block. Any of the following makes the iterator invalid with `Corruption`:
  - `num_restarts` is 0 or doesn't fit in the block;
  - a varint or length overruns the entry area;
  - a restart offset is out of range;
  - a restart entry has `shared ≠ 0`;
  - a key is shorter than the comparator's minimum (8 bytes for internal keys).
- **Fuzz check.** A test applies 2,000 random multi-byte corruptions under ASan.

### 8.3 Index block

The index has one entry per data block. Its value is the block's `BlockHandle`. Its key is a separator: an internal key `k` with `last_key(block i) ≤ k < first_key(block i+1)`. The last block's key is a successor: `k ≥ last_key`.

- **Shortening.** `InternalKeyComparator::FindShortestSeparator` shortens the user key bytewise (it increments the first differing byte, if that stays below the limit) and appends the largest tag, `(2^56 − 1) << 8 | 1`. A test checks the bounds over 20,000 random key pairs.
- **Search.** The index block has restart interval 1, so a lookup's binary search covers every entry.

### 8.4 Filter block (`filter_policy.{h,cc}`)

The filter block holds one filter over `Hash64(user_key)` for every distinct user key in the table:

```
filter := bits... | num_probes (1B) | filter_type (1B: 1 = standard, 2 = blocked)
```

- **Dispatch.** Readers dispatch on `filter_type` with a switch. Both policies satisfy the `FilterPolicy` concept: static `Build`, `MayContain` and `Validate`, with no virtual calls.
- **Validation.** `Table::Open` validates the trailer and the size. A malformed filter could give false negatives, which would mean wrong `NotFound` results, so such a table is rejected with `Corruption`.
- **Alignment.** `Table::Open` copies the filter into a 64-byte-aligned buffer.

**Cache-line-blocked bloom** (the default):
- *Layout.* `bits` is `num_blocks × 64` bytes, with `num_blocks = max(1, ceil(n × bits_per_key / 512))`.
- *Block choice.* For hash `h`, the block is `((h >> 32) × num_blocks) >> 32` (multiply-shift, no division).
- *Probes.* With `h2 = (uint32) h`, probe `i` tests bit `h2 >> 23` (0–511) of that block, then sets `h2 *= 0x9E3779B9`. All probes for a key fall in one 64-byte block, so with the aligned buffer a lookup touches exactly one cache line.
- *Probe count.* `k` is chosen to minimize the theoretical false-positive rate: the average over a Poisson(λ = 512 / bits_per_key) number of keys in a block of `(1 − (1 − 1/512)^(k·j))^k`. Blocks that hold more keys than average fill up first, so this optimum is lower than the standard `k`.

**Standard bloom** (for comparison):
- *Layout.* `bits` is `m / 8` bytes, with `m = max(64, n × bits_per_key)` rounded up to a multiple of 64.
- *Probes.* `k = round(bits_per_key × ln 2)`. Probe `i` tests bit `((h + i·δ) × m) >> 64` (128-bit multiply), with `δ = rotl(h, 21) | 1` (Kirsch–Mitzenmacher double hashing). A lookup touches up to `k` cache lines.

**Measured at 10 bits/key** (100k keys, 1M absent-key queries; the spec allows ±20% of theory):

| Policy | k | Measured FPR | Theoretical FPR |
|---|---|---|---|
| Standard | 7 | 0.823% | 0.819% |
| Blocked | 6 (from the model) | 0.953% | 0.957% |

### 8.5 Reading tables (`table.{h,cc}`)

- **Open.** `Table::Open` reads and checks the footer (magic, handles), loads the index block, and loads and validates the filter. Both stay in memory for the table's lifetime.
- **Data blocks.** These are read on demand through `RandomAccessFile`, which uses either `pread` or a read-only `mmap` of the whole file. With `mmap`, block data is used in place with no copy. Experiment 6 compares the two modes.
- **`InternalGet(lookup_key, fn)`.** This probes the filter first and returns with no I/O if the filter excludes the user key. Otherwise it seeks the index, reads one data block, seeks within it, and calls `fn(key, value)` on the first entry ≥ `lookup_key` while that block is loaded.
- **Iterator.** `NewIterator()` is a two-level iterator: an index-block iterator selects data blocks, and a block iterator walks each one.
- **Errors.** The table iterator and the merging iterator both **stop at the first error** (a failed read or a corrupt block) and report it through `status()`. They never skip ahead, because skipping would silently drop entries.
- **Corruption check.** A test flips a byte at about 350 positions across a table, densely in the footer, in all 18 read-mode × filter × block-size configurations. Every flip either fails `Open` with `Corruption` or is harmless (for example, footer padding). An iterator returns only a correct prefix of the entries, ending at the true end or at a `Corruption` status.

### 8.6 Merging iterator (`merging_iterator.h`)

`MergingIterator<Cmp>` merges `k` child `InternalIterator`s through a binary min-heap of child indexes, at O(log k) per `Next()`.
- **Ties.** Equal keys come out in child order (lower index first). Internal keys are unique, so a tie only arises from a duplicated input.
- **Errors.** If any child reports an error, the merge becomes invalid and returns that error.
- **Interfaces.** Concrete iterators such as `BlockIter` and the skiplist iterator are used directly where their type is known, and satisfy the `ForwardIterator` concept. The virtual `InternalIterator` interface is used only where children differ in type: memtable, L0 files and levels.

## 9. Caches (`src/cache/`, `src/util/pin.h`, `src/util/buffer_pool.{h,cc}`)

### 9.1 Sharded LRU cache (`lru_cache.h`)

`ShardedLRUCache<Key, Value, Hasher>` is the one template behind both the block cache and the table cache.

- **Shards.** A key's 64-bit hash picks a shard by multiply-shift on its high bits, and a bucket within the shard by its low bits. Each shard is `alignas(kCacheLineSize)`, so two threads in different shards never share a cache line. Each shard has:
  - its own `std::mutex`;
  - a capacity of `total / num_shards`;
  - an intrusive chained hash table that doubles once the shard holds as many entries as it has buckets;
  - two intrusive lists, as in LevelDB: `lru` holds entries referenced only by the cache, oldest first; `in_use` holds entries pinned by at least one client.
- **Reference counting.** A cached entry holds one reference for the cache. Each `Lookup` or `Insert` returns a handle holding another.
  - Pinning moves an entry from `lru` to `in_use`; releasing the last client reference moves it back.
  - Evicting, erasing or replacing an entry unlinks it at once and drops the cache's reference. The value is destroyed only when the count reaches 0, so a pinned value stays valid after eviction.
- **Eviction.** Only `lru` entries are evicted, so pinned entries can push usage above capacity. `EvictIfNeeded` runs after every insert and whenever an entry becomes unpinned, so usage returns under capacity as soon as pins are released.
- **Charging.** Block-cache entries cost their block size in bytes. Table-cache entries cost 1 each, with capacity `max_open_files`.
- **No allocation on hits.** `Lookup` does a hash, an uncontended lock, a chain walk and a reference increment.

### 9.2 Pins (`pin.h`)

A `Pin` is a move-only object holding up to two `(function, arg, arg)` cleanups inline, which run in reverse order on destruction or `Reset()`. It is how a reader keeps a value alive without copying it, and it never allocates.

A table-cache lookup hands its callback a pin holding:
1. **The data block:** a block-cache handle, or a heap block owned by the pin when `fill_cache = false`, or nothing for an in-place mmap block.
2. **The table:** an extra table-cache reference, which keeps the file descriptor or mapping alive.

So `value` stays valid for as long as the pin does, even after the block is evicted and the table is closed. The phase 7 `PinnableValue` is a `std::string_view` plus such a pin.

**Measured:** after warm-up, 100,000 block-cache-hit lookups with the value pinned and then released made **0 heap allocations**, in both pread and mmap modes. A probe replacing global `operator new` counted them. The phase 8 allocation test makes this a permanent check.

### 9.3 Block reads through the cache (`table.cc`, `Table::GetDataBlock`)

For a data block at `offset` in table `file_number`:
1. **mmap mode.** Read the block (no copy). If it is uncompressed, use it in place; it is valid for the table's lifetime. Otherwise treat it as owned memory and continue with step 4.
2. **Look up `(file_number, offset)` in the block cache.** On a hit, pin the entry and return it.
3. **On a miss,** read the block, verify its CRC and decompress it if needed. Its buffer comes from the huge-page pool if one is configured.
4. **Store or own it.** If `fill_cache` is set, insert it with charge = block size and pin it. Otherwise the pin owns the block outright and frees it on release.

File numbers are never reused, so a deleted table's blocks simply age out of the cache. `TableReadStats` counts block-cache hits and misses, filter checks, and "definitely absent" answers. The DB keeps one per-thread copy of these counters (§10 of the spec).

### 9.4 Table cache (`table_cache.{h,cc}`)

The table cache maps a file number to an open `Table`: its file descriptor or mapping, its parsed index block and its aligned filter.
- **Misses.** On a miss it opens `NNNNNN.sst` and checks that the file's size equals the size recorded in the manifest, returning `Corruption` if not. Then it parses the table and inserts it.
- **Failures.** A failed open is not cached, so a later read retries it.
- **Concurrent misses.** Two threads that miss the same file at once both open it. The second insert replaces the first, which closes once its last pin is released.
- **Iterators.** `NewIterator` wraps the table iterator together with a table pin. `Evict(file_number)` is called once a file is deleted; readers that still pin the table keep it open.
- **The bound holds.** A test opens 12 tables through a cache with `max_open_files = 3` and checks, by counting `/dev/fd` entries, that the process never has more than 3 extra descriptors open.

### 9.5 Huge-page buffer pool (`buffer_pool.{h,cc}`)

With `use_huge_pages`, cached blocks are allocated from a `HugePageBufferPool`:
- **Chunks.** The pool takes 2 MiB chunks from `aligned_alloc(2 MiB)` and advises each with `MADV_HUGEPAGE`, which is a no-op on macOS. Chunks return to the system only when the pool is destroyed.
- **Size classes.** Requests are rounded up to multiples of 512 bytes, up to 64 KiB. A typical 4 KiB block, which ends up slightly over 4 KiB because a block is flushed only after reaching the target, wastes under 512 bytes. A power-of-two class would waste almost 4 KiB.
- **Free lists.** Each class has an intrusive free list stored in the freed buffers themselves, so steady-state churn reuses memory and needs no new chunks. A test checks this.
- **Large requests.** Requests above 64 KiB go to the regular heap.
- **Locking.** One mutex guards the pool. It is used only on block-cache misses (the I/O path), never on hits.

Arenas get huge pages separately (§4.1). Experiment 6 compares huge pages on and off.

## 10. Database directory, versions and the MANIFEST (`src/version/`, `src/util/filename.*`)

### 10.1 Files

| Name | Contents |
|---|---|
| `NNNNNN.log` | WAL (§7.2). One per memtable. |
| `NNNNNN.sst` | SSTable (§8). |
| `MANIFEST-NNNNNN` | A log of `VersionEdit` records in the WAL record format. |
| `CURRENT` | The text `MANIFEST-NNNNNN\n`, naming the live MANIFEST. |
| `LOCK` | Held with `flock(LOCK_EX)` while the database is open. |
| `NNNNNN.dbtmp` | A staging file for `CURRENT`. |

All numbered files share one counter (`next_file_number`), so numbers are never reused.

**Atomic `CURRENT` update (`SetCurrentFile`).**
1. Write the new contents to `NNNNNN.dbtmp`.
2. fsync it, then close it.
3. `rename` it over `CURRENT`.
4. fsync the directory.

A crash leaves either the old `CURRENT` or the new one, never a partial file.

### 10.2 VersionEdit encoding

A record is a sequence of `varint32 tag` fields, each followed by its payload:

| Tag | Field | Payload |
|---|---|---|
| 1 | log number | varint64: WALs below this number are no longer needed |
| 2 | next file number | varint64 |
| 3 | last sequence | varint64, ≤ 2^56 − 1 |
| 4 | compact pointer | varint32 level, length-prefixed internal key |
| 5 | deleted file | varint32 level, varint64 file number |
| 6 | new file | varint32 level, varint64 number, varint64 file size, length-prefixed smallest internal key, length-prefixed largest internal key |

Decoding is strict. An unknown tag, a truncated field, a level ≥ 64, an internal key shorter than 8 bytes, or a sequence above 2^56 − 1 is `Corruption`.

### 10.3 Versions

A `Version` is an immutable set of files per level, shared through `shared_ptr`.
- **L0** is sorted by file number; its files may overlap.
- **L1 and below** are sorted by key and strictly non-overlapping *by user key*: `user(largest[i]) < user(smallest[i+1])`. `VersionSet::Apply` rejects any edit that would break this, reporting `Corruption` during recovery.

**Live-file registry.** `FileMetaData` objects are shared among Versions. Each one adds its file number to a registry (`LiveFiles`) when constructed and removes it when destroyed. A table file is therefore referenced by a live Version exactly when its number is in the registry. Combined with the set of outputs still being written, that gives the exact set of tables that must not be deleted.

**Compaction scores** are computed when a Version is created:
- for L0: `files / l0_compaction_trigger`;
- for Ln (n ≥ 1): `bytes / (l1_max_bytes × multiplier^(n−1))`.

### 10.4 MANIFEST protocol (`VersionSet::LogAndApply`)

Requires the DB mutex; concurrent callers are serialized.
1. Fill in the edit's `next_file_number` and `last_sequence`, and its `log_number` if it has none.
2. Build the new Version in memory and check the level invariants.
3. If this is the first edit since `Open`, allocate a new MANIFEST number. Encode a full snapshot of the current state as an edit (every file and compact pointer) to write first.
4. Release the mutex, then:
   1. append the snapshot (new MANIFEST only) and the edit;
   2. fsync;
   3. for a new MANIFEST, update `CURRENT` atomically (§10.1).
5. Retake the mutex. On success, install the new Version. On failure, keep the old Version and drop the MANIFEST writer, so the next edit starts a fresh MANIFEST instead of appending after a possibly partial record.

**Recovery (`VersionSet::Recover`).**
- Read `CURRENT`, then replay every MANIFEST record in order.
- A torn final record is ignored: that edit was never acknowledged.
- Any other bad record is `Corruption`.
- A MANIFEST without a log number, next file number or last sequence is `Corruption`.

## 11. Engine (`src/db/db_impl.{h,cc}`)

### 11.1 Open and recovery

1. Validate options.
2. Create the directory if allowed, and take the `LOCK`.
3. If `CURRENT` is missing, create `MANIFEST-000001` and `CURRENT`.
4. Recover the VersionSet (§10.4). Check that every table it references exists; a missing table is `Corruption`.
5. Replay every WAL numbered at or above the MANIFEST's log number, oldest first, into a memtable (§7.2). Flush to an L0 table whenever the memtable exceeds `memtable_size`, and once more at the end.
6. fsync the directory if tables were written.
7. Create a new WAL, and fsync the directory.
8. In one `LogAndApply`, add the new L0 files, set the log number to the new WAL (making every replayed WAL obsolete) and record the last sequence. The last sequence is the larger of the MANIFEST's value and the largest sequence replayed.
9. Install a SuperVersion, delete obsolete files (including stray `.dbtmp` files), and start the background threads.

**Crash at any step.** A crash during recovery redoes the same work on the next open:
- the replayed WALs still exist until step 8 is durable;
- tables written before the crash are not in the MANIFEST, so they count as obsolete and are deleted.

### 11.2 Memtable switching and write stalls (`MakeRoomForWrite`)

The group-commit leader runs this before forming each group (§7.4), so no write is in progress while it runs. With the DB mutex held, it loops:
1. **Background error.** Return it: the error is sticky.
2. **Slowdown.** If L0 has ≥ `l0_slowdown_trigger` files, sleep 1 ms, once per group.
3. **Room left.** If the memtable is below `memtable_size` (and no flush was requested), return.
4. **Previous flush pending.** If an immutable memtable is still being flushed, wait for it. Only one may be pending.
5. **Stop.** If L0 has ≥ `l0_stop_trigger` files, wait for compaction.
6. **Switch.**
   1. Create the new WAL and fsync the directory.
   2. Close the old WAL.
   3. Make the memtable immutable and create a new one.
   4. Point the GroupCommitter at the new WAL and memtable.
   5. Install a new SuperVersion and wake the flush thread.

Every wait in steps 2, 4 and 5 counts as a stall (`stall_count`, `stall_micros`).

`Flush()` sets a flag and writes an empty batch. The leader that handles it switches memtables if the current one is not empty. `Flush()` then waits until every memtable switched so far has been flushed.

### 11.3 SuperVersion and the lock-free read path (`src/db/super_version.*`)

**Install.** On every memtable switch, flush and compaction, the DB builds a new `SuperVersion` (`{mem, imm, version}`) under its mutex, gives it the next generation number, and publishes the generation with `store(release)`. It then drops its reference to the old SuperVersion. SuperVersions are intrusively reference-counted:
- `Ref`: `fetch_add(relaxed)`, which is safe because the caller already holds a reference or the mutex;
- `Unref`: `fetch_sub(acq_rel)`, where the thread that drops the last reference deletes it.

**Per-thread cache.** `PerThreadState` gives each (thread, DB) pair a cache-line-aligned `ThreadSlot` holding a cached SuperVersion reference, its generation, and the thread's statistics counters. `Get`:
1. Loads the **visible sequence first** (acquire). See the ordering argument below.
2. Loads the global generation (acquire). If it equals the slot's cached generation, uses the cached SuperVersion. This **fast path** takes no mutex and no reference count: the slot's own reference keeps it alive, and only this thread can drop that reference.
3. Otherwise takes the DB mutex once to reference the current SuperVersion. It drops the old one after unlocking, since that may free a memtable. Each acquisition is counted as fast or slow.

**Why the sequence must be loaded first.** A write that lands in a newer memtable is published (release store of its sequence) only after that memtable's SuperVersion was installed (release store of its generation).
- With the sequence loaded first: if it covers the write, the acquire load synchronizes with the publish. The generation install happened before that publish, so the generation check sees the new generation and takes the slow path.
- In the opposite order, a reader could see the old generation, then a sequence covering a write that only the new memtable holds, and miss a committed write.

**Lifetime.**
- *Thread exit.* A `thread_local` destructor releases the thread's cached references and folds its counters into per-DB totals for exited threads. A global registry, behind one mutex taken only off the fast path, tracks every thread's slots.
- *DB destruction.* The destructor releases the DB's slot in every live thread. DB indexes are reused only after this cleanup, so a stale slot can never match a new DB.

**Statistics.**
- *Writes.* Each counter is written by its owning thread alone, as a relaxed load followed by a relaxed store. That avoids a locked read-modify-write instruction and needs no shared atomic.
- *Reads.* `GetInternalStats` sums all slots.
- *Check.* A test confirms that 1,000 steady-state lookups on one thread take exactly 1 slow-path acquisition and 999 fast ones.

**Read order.** Active memtable, immutable memtable, then the Version:
- L0 files containing the key, newest first;
- then one binary-searched file per deeper level, skipping tables whose bloom filter rules the key out.

Readers never wait for flush or compaction.

### 11.4 Background work

- **Threads.**
  - A dedicated **flush thread** waits for an immutable memtable. Flush never queues behind compaction, which gives it priority.
  - `max_background_compactions` **compaction threads** each pick the highest-scoring compaction whose input and output levels are not reserved by another running compaction.
  - All threads are pinned to `background_thread_cpus` on Linux.
- **Flush.**
  1. Build an L0 table from the immutable memtable: write it, fsync it, close it, then fsync the directory.
  2. `LogAndApply` adds the file and sets the log number to the current WAL, which makes the immutable memtable's WALs obsolete.
  3. Install a SuperVersion and delete obsolete files.
- **Compaction picking.**
  1. Take the level with the highest score ≥ 1.
  2. Its first input is the first file past that level's round-robin compaction pointer, wrapping around. For L0, add every L0 file overlapping it, transitively.
  3. Add every file in level + 1 overlapping the inputs' user-key range.
  4. Advance the pointer to the inputs' largest key, and record it in the edit.
- **Compaction execution.**
  - *Input.* A k-way heap merge (§8.6) over all input tables, read with `fill_cache = false`.
  - *Drop rules.* LevelDB's (§1), measured against the smallest live snapshot. With no snapshots, that is the current visible sequence.
  - *Output splitting.* A new user key may start a new output when the current one has reached `target_file_size`, or when it overlaps more than 10 × `target_file_size` bytes of level + 2. Outputs never split between versions of a key.
  - *Rate limiting.* Every byte written is charged to the rate limiter as data blocks reach the file.
  - *Install.* After syncing the outputs and the directory, one `LogAndApply` deletes the inputs and adds the outputs.
  - *Cleanup.* The compaction object, which holds its input Version, is destroyed before obsolete files are deleted. Otherwise its own inputs would survive until the next background job.
- **Rate limiter (`src/util/rate_limiter.*`).** A token bucket: tokens accrue at the configured rate, up to 1 ms worth. A request takes tokens and may go into debt, and a caller in debt sleeps until the debt is repaid. The small bucket keeps output paced at block granularity rather than in bursts. A test checks that no 25 ms window exceeds its share of the rate plus a few blocks. Time spent waiting is counted.
- **`CompactRange(begin, end)`.** Flushes, then compacts each level from 0 up to `max(1, deepest level holding data in the range)` into the next one, until no file at that level overlaps the range. The minimum of 1 matches LevelDB: data held only in L0 is still compacted, which drops the stale versions a flush keeps. It runs on the calling thread, taking the same level reservations as background compactions.
- **Errors.** A failed flush or compaction sets a sticky background error. After that, writes fail and background work and file deletion stop. Shutdown abandons an in-progress compaction without recording an error.

### 11.5 Obsolete files

`DeleteObsoleteFiles` runs after each successful flush or compaction, and at open. It computes:
- **live tables:** the live-file registry plus outputs still being written;
- the MANIFEST's log number;
- the current MANIFEST's number.

It then deletes:
- tables not in the live set (evicting them from the table cache);
- WALs below the log number;
- MANIFESTs older than the current one;
- `.dbtmp` files, at open only, since a running `LogAndApply` may be using one.

**Why deletion is safe.** A file leaves the registry only when the last Version holding it is destroyed. A Version that drops a file is installed only after the edit removing the file is durable. So a file is never deleted before the MANIFEST no longer needs it. The deletion itself runs without the mutex: new files always get numbers above every existing file, so nothing on the delete list can become live meanwhile.

**Stale readers.** A reader thread caching an old SuperVersion keeps that SuperVersion's files on disk until it refreshes. The files are deleted by a later cleanup.

## 12. Public API (`include/Talus/`, `src/db/db.cc`, `src/db/db_iter.*`)

`DB` is a thin facade over `DBImpl`, so no internal header appears on the public include path. The public headers are `db.h`, `options.h`, `write_batch.h`, `iterator.h`, `pinnable_value.h` and `status.h`.

### 12.1 Reads

- **`Get(ReadOptions, key, std::string*)`** copies the value.
- **`Get(ReadOptions, key, PinnableValue*)`** first releases whatever the `PinnableValue` held. It then pins the value without copying:
  - *Memtable hit:* a reference on the SuperVersion keeps the memtable alive.
  - *SSTable hit:* the pin holds a block-cache handle, or the owned block if `fill_cache = false` (nothing for an mmap block), plus a table-cache reference. The value stays valid after its block is evicted, its file is compacted away, and its table is closed. Tests check this under ASan.
- **`ReadOptions::snapshot`** fixes the read sequence. Otherwise reads use the visible sequence, loaded before the SuperVersion as described in §11.3.

### 12.2 Iterators

`NewIterator` loads the read sequence first, then takes a reference on the current SuperVersion under the mutex; the iterator releases it on destruction. It merges (§8.6):
- the memtable;
- the immutable memtable;
- every L0 table;
- one `LevelIterator` per non-empty deeper level, which walks that level's sorted file list and opens each table only when the scan reaches it.

`DBIter` turns the merged internal entries into the user's view:
1. It skips entries newer than the read sequence.
2. For each user key, the first remaining entry is the newest visible version. If it is a deletion, every version of that key is hidden. Otherwise it is yielded.
3. `Next()` skips the rest of the current key's versions.
4. `Seek(t)` seeks the internal key `(t, read sequence, type 1)`.

A corrupt key or a child error makes the iterator invalid, and the error is reported through `status()`. Because the iterator holds its SuperVersion, it is a stable snapshot: writes, flushes and compactions after its creation are invisible to it, and do not invalidate it.

### 12.3 Snapshots

`GetSnapshot()` appends a `Snapshot` holding the current visible sequence to a `std::list`, under the mutex. The visible sequence only grows, so the list stays ordered and its front is the oldest snapshot: the "smallest snapshot" that compaction's drop rules use. `ReleaseSnapshot` removes it.

### 12.4 Zero allocation (spec §9)

| Path | Why it does not allocate |
|---|---|
| `Put` / `Delete` | Each thread reuses one `WriteBatch` whose `Clear()` keeps its capacity. The group-commit `Writer` lives on the stack. The merge buffer and the WAL write buffer are reused. Memtable nodes come from the arena, which is excluded from the count. |
| `Write(batch)` | The same, with the caller's batch. |
| `Get` into a `PinnableValue`, memtable or block-cache hit | `LookupKey` and `BlockIter` keys use inline buffers. The per-thread slot is found without locking. Cache lookups make no allocation, and `Pin` stores its cleanups inline. |

**Measured** with a probe that replaces global `operator new`. After warm-up, each path made **0 allocations over 100,000 operations**: `Put`, `Write` with a reused 2-op batch, memtable-hit `Get`, and block-cache-hit `Get` with 99,965 hits and 0 misses. The SuperVersion stayed on the fast path for the whole run. Phase 8 turns this into a permanent test binary.

### 12.5 Statistics

`DBStats` (spec §10) sums each thread's slot (§11.3) with the background counters. Every hot-path counter is per thread:
- **Reads:** Gets, tables consulted, bloom checks and "definitely absent" answers, block-cache hits and misses, SuperVersion fast and slow paths.
- **Writes:** user bytes, WAL bytes, write groups, and a group-size histogram.

The group-commit leader counts groups and WAL bytes on its own thread through `GroupCommitter::Hooks::on_logged`.

Derived ratios:
- **write amplification** = (WAL + flush + compaction bytes) / user bytes;
- **read amplification** = tables consulted / Gets;
- **space amplification** = SST bytes / the deepest non-empty level's bytes.

## 13. Verification (`tests/`, `fuzz/`)

Every binary below runs under CTest in every preset. CI runs debug, ASan+UBSan and TSan on Ubuntu, release with LZ4 on macOS, and a libFuzzer job.

| Spec §11 item | Binary | What it checks |
|---|---|---|
| Unit tests | `talus_unit_tests` | Every component: arena, skiplist (single-writer and concurrent), coding, CRC32C (all paths), both bloom filters (false-positive rate within 20% of theory at 10 bits/key), blocks, tables, merging iterator, LRU and table caches, VersionEdit, WAL, rate limiter, group commit, the DB API. |
| Model-based test | `talus_model_test` | 1,000,000 seeded operations (150,000 under sanitizers) against a `std::map` model, with snapshots modeled as frozen copies. Operations: Put, Delete, atomic batches, Get, Scan, snapshot take/release/read, Flush, CompactRange (bounded and unbounded), Reopen. On a mismatch it writes `talus_model_failure_<seed>.txt` (the seed plus every executed op), and `--replay FILE` re-executes exactly those ops. Two CTest cases check that path itself: a run with an injected mismatch must fail and write the log, and the replay must fail at the same op. |
| Concurrent model test | `talus_concurrent_model_test` | 4 writers on disjoint key ranges, each checking reads of its own keys against a private model, while flushes and compactions run. Snapshot readers check that atomically written key pairs never appear torn, that snapshot reads repeat exactly after more writes, flushes and compactions, and that pair versions never go backwards across snapshots. |
| Crash harness | `talus_crash_test` | Each iteration forks a child that writes `sync=true` batches and reports each acknowledged batch number through a pipe. After 0–200 ms the parent sends SIGKILL, reopens and verifies: every acknowledged batch present, no batch partial, batches present form a prefix, and the `last_batch` counter matches. A 32 KiB memtable means kills land mid-flush, mid-compaction and mid-MANIFEST-write. 100 iterations by default; `--iterations 10000` for the long run. |
| Corruption tests | `talus_corruption_test` | For each damage position, a pristine database is copied, one file is damaged, and the copy is reopened. **WAL:** byte flips and truncations open cleanly, and recovered batches are a complete, correct prefix of those written; in paranoid mode a flip is `Corruption`. **SST:** flips and truncations make every read return the correct value or `Corruption`, never a different value or a false `NotFound`, and scans return a correct prefix. **MANIFEST and CURRENT:** Open fails with `Corruption`, or succeeds with every key correct. |
| Fuzz targets | `fuzz/fuzz_*` | Block reader, SSTable reader (pread and mmap), WAL replay (normal and paranoid; only `Corruption` is an acceptable failure), and the VersionEdit decoder (anything accepted must round-trip). With `-DTalus_BUILD_FUZZERS=ON` (the `fuzz` preset, clang) they are libFuzzer binaries with ASan and UBSan, and the library is instrumented too. Otherwise they link a standalone driver and run over a generated seed corpus as regression tests. |
| TSan stress test | `talus_stress_test` | For `TALUS_STRESS_SECONDS` (default 5), all at once: 3 writers (puts, deletes, batches, occasional sync), 3 point readers (copying and pinned, with and without `fill_cache`), 2 iterator threads (half on snapshots), snapshot churn, manual flushes and range compactions, and `GetStats` while counters change. 2 compaction threads and a 256 KiB block cache keep compactions concurrent and evictions constant. Values embed their keys, so every read is checked. |
| Allocation test | `talus_alloc_test` | Replaces global `operator new`/`delete` (every overload) with per-thread counting versions, and asserts 0 allocations over 100K operations each for `Put`, `Write` with a reused batch, and pinned `Get` on a memtable hit and on a block-cache hit (the latter also asserting 100K cache hits, 0 misses). It also asserts that 4 reader threads doing 100K Gets each never take the SuperVersion slow path once warmed up. |

**Results, Apple M3.** In debug, every suite passes (265 CTest cases). The 1M-op model test takes about 90 s and 100 crash iterations about 13 s. 60 s of libFuzzer per target ran 15.3M (block), 2.6M (VersionEdit), 227K (WAL) and 168K (SSTable) executions, with no crash, leak or timeout.
