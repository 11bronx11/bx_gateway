#pragma once
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include "noncopyable.h"
namespace bronx {
template <typename T>
class BxMpscRing : public Noncopyable {
public:
    explicit BxMpscRing(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
        , cells_(new Cell[capacity]) {
        // mask_ = capacity - 1 这套取模只对 2 的幂成立,非法值直接抛。
        // 用 C++20 的 has_single_bit 判 2 的幂,省得手写位运算。
        if (!std::has_single_bit(capacity)) {
            throw std::invalid_argument(
                "BxMpscRing capacity must be a power of two and > 0");
        }
        for (size_t i = 0; i < capacity; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }
    ~BxMpscRing() {
        T tmp;
        while (try_pop(tmp)) {
        }
    }
    bool try_push(T&& v) {
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[pos & mask_];
            size_t seq = cell.seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1,
                                                 std::memory_order_relaxed)) {
                    cell.data = std::move(v);
                    cell.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }
    bool try_pop(T& out) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        Cell& cell = cells_[pos & mask_];
        size_t seq = cell.seq.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
        if (diff == 0) {
            out = std::move(cell.data);
            cell.seq.store(pos + capacity_, std::memory_order_release);
            tail_.store(pos + 1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    size_t capacity() const { return capacity_; }
    size_t size_approx() const {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        return h >= t ? (h - t) : 0;
    }
    bool empty_approx() const {
        return head_.load(std::memory_order_relaxed)
             == tail_.load(std::memory_order_relaxed);
    }
private:
    struct alignas(64) Cell {
        std::atomic<size_t> seq{0};
        T data{};
    };
    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<Cell[]> cells_;
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};


}
