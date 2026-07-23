#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace bronx {
namespace gateway {

enum class UpMark : uint8_t { OK, FAIL, SKIP, COUNT };

enum class UpWhy : uint8_t {
    NONE, CONNECT, SEND, TIMEOUT, BAD_RESP, BAD_BODY,
    STATUS, BUSY, OPEN, DOWN, DEADLINE, COUNT
};

enum class AcqWhy : uint8_t {
    NONE, BUSY, OPEN, DOWN, CONNECT, TIMEOUT, DEADLINE
};

struct UpRet {
    UpMark mark = UpMark::SKIP;
    UpWhy why = UpWhy::NONE;
    int status = 0;
    uint64_t costMs = 0;
    bool slow = false;
};

inline uint64_t upMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline uint64_t leftMs(uint64_t dueMs) {
    if(dueMs == 0) return std::numeric_limits<uint64_t>::max();
    uint64_t now = upMs();
    return dueMs > now ? dueMs - now : 0;
}

inline const char* upMarkName(UpMark v) {
    switch(v) {
        case UpMark::OK: return "ok";
        case UpMark::FAIL: return "fail";
        case UpMark::SKIP: return "skip";
        case UpMark::COUNT: break;
    }
    return "skip";
}

inline const char* upWhyName(UpWhy v) {
    switch(v) {
        case UpWhy::NONE: return "none";
        case UpWhy::CONNECT: return "connect";
        case UpWhy::SEND: return "send";
        case UpWhy::TIMEOUT: return "timeout";
        case UpWhy::BAD_RESP: return "bad_resp";
        case UpWhy::BAD_BODY: return "bad_body";
        case UpWhy::STATUS: return "status";
        case UpWhy::BUSY: return "busy";
        case UpWhy::OPEN: return "open";
        case UpWhy::DOWN: return "down";
        case UpWhy::DEADLINE: return "deadline";
        case UpWhy::COUNT: break;
    }
    return "none";
}

inline UpWhy acqWhy(AcqWhy v) {
    switch(v) {
        case AcqWhy::BUSY: return UpWhy::BUSY;
        case AcqWhy::OPEN: return UpWhy::OPEN;
        case AcqWhy::DOWN: return UpWhy::DOWN;
        case AcqWhy::CONNECT: return UpWhy::CONNECT;
        case AcqWhy::TIMEOUT: return UpWhy::TIMEOUT;
        case AcqWhy::DEADLINE: return UpWhy::DEADLINE;
        case AcqWhy::NONE: break;
    }
    return UpWhy::NONE;
}

struct UpStat {
    static constexpr size_t kMarks = (size_t)UpMark::COUNT;
    static constexpr size_t kWhys = (size_t)UpWhy::COUNT;
    static constexpr std::array<uint64_t, 12> kBounds{
        1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000,
        std::numeric_limits<uint64_t>::max()
    };

    UpStat() {
        for(auto& n : req) n.store(0);
        for(auto& n : rejected) n.store(0);
        for(auto& n : lat) n.store(0);
    }

    void add(const UpRet& ret) {
        size_t m = (size_t)ret.mark;
        size_t w = (size_t)ret.why;
        if(m >= kMarks || w >= kWhys) return;
        ++req[m * kWhys + w];
        if(ret.costMs == 0 && ret.mark == UpMark::SKIP) return;
        for(size_t i = 0; i < kBounds.size(); ++i) {
            if(ret.costMs <= kBounds[i]) {
                ++lat[i];
                break;
            }
        }
        latSum += ret.costMs;
        ++latCount;
    }

    void reject(UpWhy why) {
        size_t w = (size_t)why;
        if(w < kWhys) ++rejected[w];
    }

    uint64_t requests(UpMark mark, UpWhy why) const {
        size_t m = (size_t)mark;
        size_t w = (size_t)why;
        if(m >= kMarks || w >= kWhys) return 0;
        return req[m * kWhys + w].load(std::memory_order_relaxed);
    }

    uint64_t rejects(UpWhy why) const {
        size_t w = (size_t)why;
        return w < kWhys ? rejected[w].load(std::memory_order_relaxed) : 0;
    }

    uint64_t latency(size_t i) const {
        return i < lat.size() ? lat[i].load(std::memory_order_relaxed) : 0;
    }

    uint64_t latencySum() const { return latSum.load(std::memory_order_relaxed); }
    uint64_t latencyCount() const { return latCount.load(std::memory_order_relaxed); }

private:
    std::array<std::atomic<uint64_t>, kMarks * kWhys> req;
    std::array<std::atomic<uint64_t>, kWhys> rejected;
    std::array<std::atomic<uint64_t>, kBounds.size()> lat;
    std::atomic<uint64_t> latSum{0};
    std::atomic<uint64_t> latCount{0};
};

} // namespace gateway
} // namespace bronx
