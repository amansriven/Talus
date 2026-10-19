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
