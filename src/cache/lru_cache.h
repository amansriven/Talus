#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "util/pin.h"
#include "util/port.h"

namespace talus {

template <typename H, typename Key>
concept CacheKeyHasher = requires(const Key& key) {
  { H::Hash(key) } -> std::convertible_to<uint64_t>;
};

// A sharded LRU cache of reference-counted entries, used for both the block
// cache (charge = block bytes) and the table cache (charge = 1 per table).
//
// The key's hash picks a shard (high bits) and a bucket within it (low
// bits). Each shard has its own mutex and capacity of total / num_shards, and
// is cache-line aligned so threads working in different shards never share a
// line.
//
// Entries are reference-counted: the cache holds one reference while the
// entry is cached, and each Lookup()/Insert() returns a handle holding
// another. An evicted or erased entry is unlinked at once but destroyed only
// when its last handle is released, so a pinned value stays valid. Like
// LevelDB, a shard keeps two lists: `lru` holds entries only the cache
// references (evictable, oldest first), and `in_use` holds pinned entries.
// Only `lru` entries are evicted, so pinned entries can push usage above
// capacity until they are released.
template <typename Key, typename Value, CacheKeyHasher<Key> Hasher>
class ShardedLRUCache {
 public:
  struct Handle {
    Key key{};
    std::unique_ptr<Value> value;
    size_t charge = 0;
    uint64_t hash = 0;
    uint32_t refs = 0;      // The cache's reference (if in_cache) plus clients'.
    bool in_cache = false;  // Reachable through the hash table.
    Handle* next_hash = nullptr;
    Handle* prev = nullptr;  // Links in the shard's lru or in_use list.
    Handle* next = nullptr;
  };

  ShardedLRUCache(size_t capacity, size_t num_shards)
      : num_shards_(std::max<size_t>(num_shards, 1)),
        shards_(std::make_unique<Shard[]>(num_shards_)) {
    for (size_t i = 0; i < num_shards_; ++i) {
      shards_[i].capacity = capacity / num_shards_;
    }
  }

  // REQUIRES: every handle has been released.
  ~ShardedLRUCache() {
    for (size_t i = 0; i < num_shards_; ++i) {
      Shard& s = shards_[i];
      assert(s.in_use.next == &s.in_use && "cache destroyed with pinned entries");
      for (Handle* e = s.lru.next; e != &s.lru;) {
        Handle* next = e->next;
        const std::unique_ptr<Handle> doomed(e);  // Frees the entry and its value.
        e = next;
      }
    }
  }

  ShardedLRUCache(const ShardedLRUCache&) = delete;
  ShardedLRUCache& operator=(const ShardedLRUCache&) = delete;

  // Inserts (replacing any entry with the same key) and returns a pinned
  // handle. Evicts least-recently-used unpinned entries to stay within the
  // shard's capacity.
  Handle* Insert(const Key& key, std::unique_ptr<Value> value, size_t charge) {
    auto owned = std::make_unique<Handle>();
    Handle* e = owned.release();  // Owned by the cache's lists from here on.
    e->key = key;
    e->value = std::move(value);
    e->charge = charge;
    e->hash = Hasher::Hash(key);
    e->refs = 2;  // The cache's reference plus the returned handle.
    e->in_cache = true;

    Shard& s = ShardFor(e->hash);
    std::lock_guard<std::mutex> lock(s.mu);
    Append(&s.in_use, e);
    s.usage += charge;
    Handle** slot = FindSlot(s, key, e->hash);
    Handle* old = *slot;
    e->next_hash = old != nullptr ? old->next_hash : nullptr;
    *slot = e;
    if (old != nullptr) {
      FinishErase(s, old);
    } else if (++s.elems > s.buckets.size()) {
      Rehash(s);
    }
    EvictIfNeeded(s);
    return e;
  }

  // Returns a pinned handle for `key`, or nullptr. Does not allocate.
  Handle* Lookup(const Key& key) {
    const uint64_t hash = Hasher::Hash(key);
    Shard& s = ShardFor(hash);
    std::lock_guard<std::mutex> lock(s.mu);
    Handle* e = *FindSlot(s, key, hash);
    if (e != nullptr) {
      Ref(s, e);
    }
    return e;
  }

  // Adds a reference to an already pinned handle.
  void Ref(Handle* e) {
    Shard& s = ShardFor(e->hash);
    std::lock_guard<std::mutex> lock(s.mu);
    Ref(s, e);
  }

  void Release(Handle* e) {
    Shard& s = ShardFor(e->hash);
    std::lock_guard<std::mutex> lock(s.mu);
    Unref(s, e);
  }

  // Removes `key` from the cache; pinned handles to it stay valid.
  void Erase(const Key& key) {
    const uint64_t hash = Hasher::Hash(key);
    Shard& s = ShardFor(hash);
    std::lock_guard<std::mutex> lock(s.mu);
    if (Handle* e = RemoveFromTable(s, key, hash)) {
      FinishErase(s, e);
    }
  }

  static Value* value(Handle* e) { return e->value.get(); }

  // Transfers the handle's reference to `pin`, which releases it.
  void PinInto(Handle* e, Pin* pin) {
    pin->Add(
        [](void* cache, void* handle) {
          static_cast<ShardedLRUCache*>(cache)->Release(static_cast<Handle*>(handle));
        },
        this, e);
  }

  // Total charge of cached entries (pinned or not), across all shards.
  size_t TotalCharge() const {
    size_t total = 0;
    for (size_t i = 0; i < num_shards_; ++i) {
      std::lock_guard<std::mutex> lock(shards_[i].mu);
      total += shards_[i].usage;
    }
    return total;
  }

  size_t num_shards() const { return num_shards_; }

 private:
  struct alignas(kCacheLineSize) Shard {
    Shard() {
      lru.next = lru.prev = &lru;
      in_use.next = in_use.prev = &in_use;
      buckets.resize(16);
    }
    mutable std::mutex mu;
    size_t capacity = 0;
    size_t usage = 0;  // Sum of charges of entries with in_cache = true.
    Handle lru;        // Dummy head; lru.next is the least recently used.
    Handle in_use;     // Dummy head.
    std::vector<Handle*> buckets;
    size_t elems = 0;
  };

  Shard& ShardFor(uint64_t hash) const {
    // Multiply-shift onto [0, num_shards): uses the hash's high bits, while
    // buckets use the low bits.
    __extension__ typedef unsigned __int128 u128;  // NOLINT(modernize-use-using)
    return shards_[static_cast<size_t>((static_cast<u128>(hash) * num_shards_) >> 64)];
  }

  static Handle** FindSlot(Shard& s, const Key& key, uint64_t hash) {
    Handle** slot = &s.buckets[hash & (s.buckets.size() - 1)];
    while (*slot != nullptr && ((*slot)->hash != hash || !((*slot)->key == key))) {
      slot = &(*slot)->next_hash;
    }
    return slot;
  }

  static Handle* RemoveFromTable(Shard& s, const Key& key, uint64_t hash) {
    Handle** slot = FindSlot(s, key, hash);
    Handle* e = *slot;
    if (e != nullptr) {
      *slot = e->next_hash;
      --s.elems;
    }
    return e;
  }

  static void Rehash(Shard& s) {
    std::vector<Handle*> buckets(s.buckets.size() * 2);
    for (Handle* head : s.buckets) {
      while (head != nullptr) {
        Handle* next = head->next_hash;
        Handle*& bucket = buckets[head->hash & (buckets.size() - 1)];
        head->next_hash = bucket;
        bucket = head;
        head = next;
      }
    }
    s.buckets.swap(buckets);
  }

  static void Unlink(Handle* e) {
    e->next->prev = e->prev;
    e->prev->next = e->next;
  }

  static void Append(Handle* list, Handle* e) {  // Newest at list->prev.
    e->next = list;
    e->prev = list->prev;
    e->prev->next = e;
    e->next->prev = e;
  }

  static void Ref(Shard& s, Handle* e) {
    if (e->refs == 1 && e->in_cache) {  // Only the cache held it: now pinned.
      Unlink(e);
      Append(&s.in_use, e);
    }
    ++e->refs;
  }

  static void Unref(Shard& s, Handle* e) {
    assert(e->refs > 0);
    --e->refs;
    if (e->refs == 0) {
      assert(!e->in_cache);
      const std::unique_ptr<Handle> doomed(e);  // Destroys the entry and its value.
    } else if (e->in_cache && e->refs == 1) {   // No longer pinned.
      Unlink(e);
      Append(&s.lru, e);
      EvictIfNeeded(s);
    }
  }

  // REQUIRES: e has been removed from the hash table.
  static void FinishErase(Shard& s, Handle* e) {
    assert(e->in_cache);
    e->in_cache = false;
    Unlink(e);
    s.usage -= e->charge;
    Unref(s, e);
  }

  static void EvictIfNeeded(Shard& s) {
    while (s.usage > s.capacity && s.lru.next != &s.lru) {
      Handle* oldest = s.lru.next;
      RemoveFromTable(s, oldest->key, oldest->hash);
      FinishErase(s, oldest);
    }
  }

  const size_t num_shards_;
  std::unique_ptr<Shard[]> shards_;
};

}  // namespace talus
