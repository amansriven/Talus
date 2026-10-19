#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>

#include "db/dbformat.h"
#include "util/arena.h"
#include "util/random.h"

namespace talus {

// An ordered set of (key, value) entries in arena memory, supporting
// lock-free readers and either one writer at a time (Insert) or many
// concurrent lock-free writers (InsertConcurrently).
//
// Node layout: one contiguous arena allocation per node:
//
//   uint32 key_size | uint32 value_size | key | value | pad to 8 |
//   std::atomic<Node*> next[height]
//
// Invariants (these make lock-free reads and inserts safe):
//  1. Each level-L list is sorted strictly ascending by key. Every node in
//     the level-(L+1) list is also in the level-L list, because inserts link
//     a node bottom-up: level L is linked before level L+1.
//  2. A node becomes reachable at level L only through a release store/CAS of
//     a level-L link. Its key, value and next[L] are written before that
//     release, and readers load links with acquire, so a reader that reaches
//     a node sees its contents fully initialized. Transitively, a node seen
//     by any path has all of its writes visible.
//  3. Nodes are never unlinked or freed while the SkipList exists: memory is
//     reclaimed only when the owning Arena is destroyed, after all readers
//     and writers are done. Readers may therefore hold Node pointers
//     indefinitely without hazard pointers or epochs.
//  4. `max_height_` is advisory and only grows. If a reader sees a raised
//     height before the head's new links are set, it reads null at those
//     levels and descends; if it sees a stale height, it starts lower, which
//     is still correct by (1). Hence relaxed ordering suffices.
//  5. Concurrent insert at level L computes a splice (prev, next) with
//     prev.key < key < next.key, sets new.next[L] = next, and CASes
//     prev.next[L] from `next` to `new`. A failed CAS means another node was
//     linked after prev; because nodes are never removed, prev.key < key
//     still holds, so the splice is recomputed starting from prev. A CAS only
//     fails when another thread's CAS on the same link succeeded, so the
//     algorithm is lock-free.
//
// REQUIRES: keys are unique. No two entries with equal keys may ever be
// inserted. The memtable guarantees this, because every internal key carries
// a distinct sequence number.
template <KeyComparator Comparator, bool kPrefetch = true>
class SkipList {
 private:
  struct Node;

 public:
  static constexpr int kMaxHeight = 12;
  static constexpr int kBranching = 4;

  // `arena` must outlive the SkipList; all nodes are allocated from it.
  SkipList(Comparator cmp, Arena* arena)
      : compare_(cmp), arena_(arena), head_(NewNode(0, 0, kMaxHeight)) {}

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Insert an entry whose key and value are written in place by
  // `fill(char* key, char* value)`, so callers can encode directly into node
  // memory without a temporary buffer.
  //
  // REQUIRES: external synchronization; no other Insert or InsertConcurrently
  // is running. Concurrent readers are fine.
  template <typename Fill>
  void Insert(size_t key_size, size_t value_size, Fill&& fill) {
    InsertImpl</*kConcurrent=*/false>(key_size, value_size, fill);
  }

  // Like Insert, but may run concurrently with other InsertConcurrently calls.
  template <typename Fill>
  void InsertConcurrently(size_t key_size, size_t value_size, Fill&& fill) {
    InsertImpl</*kConcurrent=*/true>(key_size, value_size, fill);
  }

  // A height in [1, kMaxHeight], where each extra level is kept with
  // probability 1/kBranching.
  static int RandomHeight() {
    static_assert(kBranching == 4, "the loop below consumes 2 random bits per level");
    // xorshift64*'s high bits are its strongest; 32 of them cover all 11
    // possible promotions.
    uint64_t r = ThreadLocalRandom() >> 32;
    int height = 1;
    while (height < kMaxHeight && (r & 3) == 0) {
      ++height;
      r >>= 2;
    }
    return height;
  }

  // A forward iterator. It stays valid while the SkipList exists and sees
  // concurrently inserted entries if they are linked before it reaches them.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool Valid() const { return node_ != nullptr; }
    std::string_view key() const {
      assert(Valid());
      return node_->key();
    }
    std::string_view value() const {
      assert(Valid());
      return node_->value();
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    // Positions at the first entry with key >= target.
    void Seek(std::string_view target) { node_ = list_->FindGreaterOrEqual(target); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  struct Node {
    uint32_t key_size;
    uint32_t value_size;

    static size_t LinksOffset(size_t key_size, size_t value_size) {
      return (key_size + value_size + 7) & ~size_t{7};
    }

    char* data() { return reinterpret_cast<char*>(this) + sizeof(Node); }
    std::string_view key() { return {data(), key_size}; }
    std::string_view value() { return {data() + key_size, value_size}; }
    std::atomic<Node*>* links() {
      return reinterpret_cast<std::atomic<Node*>*>(data() + LinksOffset(key_size, value_size));
    }

    // Acquire: pairs with the release in SetNext/CasNext, so the returned
    // node's contents are visible (invariant 2).
    Node* Next(int level) { return links()[level].load(std::memory_order_acquire); }
    // Release: publishes this link's target and everything written to it.
    void SetNext(int level, Node* x) { links()[level].store(x, std::memory_order_release); }
    // Relaxed: only for the new node's own links before it is published, and
    // for prefetch hints that are never dereferenced.
    Node* NextRelaxed(int level) { return links()[level].load(std::memory_order_relaxed); }
    void SetNextRelaxed(int level, Node* x) { links()[level].store(x, std::memory_order_relaxed); }
    // Release on success publishes `x` (invariant 2). Relaxed on failure: the
    // observed value is discarded and the splice is reloaded with acquire.
    bool CasNext(int level, Node* expected, Node* x) {
      return links()[level].compare_exchange_strong(expected, x, std::memory_order_release,
                                                    std::memory_order_relaxed);
    }
  };
  static_assert(sizeof(Node) == 8 && alignof(Node) <= Arena::kAlignment);

  Node* NewNode(size_t key_size, size_t value_size, int height) {
    assert(key_size <= std::numeric_limits<uint32_t>::max());
    assert(value_size <= std::numeric_limits<uint32_t>::max());
    const size_t links_offset = Node::LinksOffset(key_size, value_size);
    char* mem = arena_->Allocate(sizeof(Node) + links_offset + sizeof(std::atomic<Node*>) * height);
    Node* x = new (mem) Node{static_cast<uint32_t>(key_size), static_cast<uint32_t>(value_size)};
    std::atomic<Node*>* links = x->links();
    for (int i = 0; i < height; ++i) {
      new (&links[i]) std::atomic<Node*>(nullptr);
    }
    return x;
  }

  // Starting from `before` (whose key is < key, or the head), finds the
  // level-`level` splice: *prev.key < key <= *next.key (next may be null).
  void FindSplice(std::string_view key, Node* before, int level, Node** prev, Node** next) const {
    Node* x = before;
    while (true) {
      Node* n = x->Next(level);
      if constexpr (kPrefetch) {
        if (n != nullptr) {
          // Start fetching the node after `n` while `n`'s key is compared.
          __builtin_prefetch(n->NextRelaxed(level), 0, 1);
        }
      }
      if (n != nullptr && compare_(n->key(), key) < 0) {
        x = n;
      } else {
        *prev = x;
        *next = n;
        return;
      }
    }
  }

  Node* FindGreaterOrEqual(std::string_view key) const {
    Node* prev = head_;
    Node* next = nullptr;
    // Relaxed: see invariant 4.
    for (int level = max_height_.load(std::memory_order_relaxed) - 1; level >= 0; --level) {
      FindSplice(key, prev, level, &prev, &next);
    }
    return next;
  }

  template <bool kConcurrent, typename Fill>
  void InsertImpl(size_t key_size, size_t value_size, Fill& fill) {
    const int height = RandomHeight();
    Node* x = NewNode(key_size, value_size, height);
    fill(x->data(), x->data() + key_size);
    const std::string_view key = x->key();

    // Raise max_height_ before linking (relaxed: invariant 4).
    int max_height = max_height_.load(std::memory_order_relaxed);
    if constexpr (kConcurrent) {
      while (height > max_height &&
             !max_height_.compare_exchange_weak(max_height, height, std::memory_order_relaxed)) {
      }
      max_height = std::max(max_height, height);
    } else if (height > max_height) {
      max_height_.store(height, std::memory_order_relaxed);
      max_height = height;
    }

    Node* prev[kMaxHeight];
    Node* next[kMaxHeight];
    Node* before = head_;
    for (int level = max_height - 1; level >= 0; --level) {
      FindSplice(key, before, level, &prev[level], &next[level]);
      assert(next[level] == nullptr || compare_(next[level]->key(), key) != 0);
      before = prev[level];
    }

    // Link bottom-up (invariant 1).
    for (int level = 0; level < height; ++level) {
      if constexpr (kConcurrent) {
        while (true) {
          x->SetNextRelaxed(level, next[level]);
          if (prev[level]->CasNext(level, next[level], x)) {
            break;
          }
          FindSplice(key, prev[level], level, &prev[level], &next[level]);
        }
      } else {
        x->SetNextRelaxed(level, next[level]);
        prev[level]->SetNext(level, x);
      }
    }
  }

  const Comparator compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_{1};
};

}  // namespace talus
