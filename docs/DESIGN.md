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
