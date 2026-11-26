// A fixed-size single-producer, single-consumer queue.
//
// The bus task and the network task talk through two of these, so the bus
// task never waits on a lock while it has a Clear to Send to answer.
#pragma once

#include <atomic>
#include <cstddef>

namespace bwa {

template <typename T, size_t N>
class SpscRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

 public:
  // Producer side. False when full (the item is not stored).
  bool push(const T& item) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail == N) {
      ++dropped_;
      return false;
    }
    items_[head & (N - 1)] = item;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer side. False when empty.
  bool pop(T& out) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    if (head == tail) return false;
    out = items_[tail & (N - 1)];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer side: throws away everything queued.
  void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

  size_t size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  bool empty() const { return size() == 0; }
  // Producer-side count of items that did not fit.
  unsigned dropped() const { return dropped_; }

 private:
  T items_[N];
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
  unsigned dropped_ = 0;
};

}  // namespace bwa
