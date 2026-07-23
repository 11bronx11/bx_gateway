#include "buf.h"
#include "mempool.h"
#include "net_socket.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace bronx {
namespace gateway {

namespace {

static size_t ceil_pow2(size_t v) {
    if(v <= 1) return 1;
    const size_t maxPow2 = (size_t)1 << (std::numeric_limits<size_t>::digits - 1);
    if(v > maxPow2) return std::numeric_limits<size_t>::max();
    --v;
    for(size_t shift = 1; shift < std::numeric_limits<size_t>::digits; shift <<= 1) {
        v |= v >> shift;
    }
    return v + 1;
}

} // namespace

InBuf::InBuf(size_t initCap) {
    if(initCap == 0) initCap = 1;
    m_data = static_cast<char*>(MemAlloc::alloc(initCap));
    m_cap  = initCap;
}

InBuf::~InBuf() {
    if(m_data) MemAlloc::free(m_data, m_cap);
}

void InBuf::ensure_writable(size_t need) {
    if(m_cap - m_wpos >= need) return;
    // 先左移回收已消费空间
    size_t r = readable();
    if(m_rpos > 0) {
        if(r > 0) memmove(m_data, m_data + m_rpos, r);
        m_rpos = 0;
        m_wpos = r;
    }
    if(m_cap - m_wpos >= need) return;
    // 扩容
    if(need > std::numeric_limits<size_t>::max() - m_wpos) {
        throw std::overflow_error("InBuf capacity overflow");
    }
    size_t newCap = m_cap * 2;
    if(newCap < m_cap) {
        throw std::overflow_error("InBuf capacity overflow");
    }
    while(newCap - m_wpos < need) {
        if(newCap > std::numeric_limits<size_t>::max() / 2) {
            throw std::overflow_error("InBuf capacity overflow");
        }
        newCap *= 2;
    }
    char* newData = static_cast<char*>(MemAlloc::alloc(newCap));
    if(m_wpos > 0) memcpy(newData, m_data, m_wpos);
    MemAlloc::free(m_data, m_cap);
    m_data = newData;
    m_cap  = newCap;
}

void InBuf::append(const char* data, size_t len) {
    if(len == 0) return;
    ensure_writable(len);
    memcpy(m_data + m_wpos, data, len);
    m_wpos += len;
}

int InBuf::fill(bronx::BxSocket* sock) {
    const size_t chunk = 4096;
    ensure_writable(chunk);
    int n = sock->recv(m_data + m_wpos, chunk);
    if(n > 0) m_wpos += n;
    return n;
}

void InBuf::consume(size_t n) {
    if(n >= readable()) consumeAll();
    else m_rpos += n;
}

bool InBuf::trim(size_t min_capacity, size_t reserve_writable) {
    size_t r = readable();
    if(reserve_writable > std::numeric_limits<size_t>::max() - r) {
        return false;
    }

    size_t needed = r + reserve_writable;
    size_t target = min_capacity > needed ? min_capacity : needed;
    if(target == 0) target = 1;
    target = ceil_pow2(target);
    if(target < needed || target < min_capacity) {
        return false;
    }
    if(target >= m_cap) {
        return false;
    }

    const size_t kMinSavings = 64 * 1024;
    bool enoughRatio = (target <= m_cap / 4);
    bool enoughSavings = (m_cap - target >= kMinSavings);
    if(!enoughRatio && !enoughSavings) {
        return false;
    }

    char* newData = nullptr;
    try {
        newData = static_cast<char*>(MemAlloc::alloc(target));
    } catch(...) {
        return false;
    }
    if(!newData) {
        return false;
    }
    if(r > 0) {
        memcpy(newData, m_data + m_rpos, r);
    }
    MemAlloc::free(m_data, m_cap);
    m_data = newData;
    m_cap = target;
    m_rpos = 0;
    m_wpos = r;
    return true;
}

size_t InBuf::findHeaderEnd() const {
    const char* p = peek();
    size_t len = readable();
    if(len < 4) return 0;
    for(size_t i = 0; i + 3 < len; ++i) {
        if(p[i]=='\r' && p[i+1]=='\n' && p[i+2]=='\r' && p[i+3]=='\n')
            return i + 4;
    }
    return 0;
}

} // namespace gateway
} // namespace bronx
