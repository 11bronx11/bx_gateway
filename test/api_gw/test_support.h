#pragma once

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <json/json.h>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace api_gw_test {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

inline int remainingMs(Clock::time_point end) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - Clock::now()).count();
    return ms > 0 ? static_cast<int>(ms) : 0;
}

inline void waitStep(Clock::time_point end, int capMs = 20) {
    const int ms = std::min(capMs, remainingMs(end));
    if(ms > 0) (void)::poll(nullptr, 0, ms);
}

inline std::string lower(std::string value) {
    for(char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline std::string trim(std::string value) {
    size_t first = 0;
    while(first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while(last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

inline bool containsToken(const std::string& value, const std::string& token) {
    std::istringstream in(value);
    std::string part;
    while(std::getline(in, part, ',')) {
        if(lower(trim(part)) == lower(token)) return true;
    }
    return false;
}

struct Child {
    pid_t pid{-1};
    int output{-1};
    std::string text;
};

inline bool drainOutput(Child& child, int timeoutMs) {
    if(child.output < 0) return true;
    pollfd pfd{child.output, POLLIN | POLLHUP, 0};
    const int ret = ::poll(&pfd, 1, timeoutMs);
    if(ret < 0) return errno == EINTR;
    if(ret == 0) return true;
    char buffer[4096];
    for(;;) {
        const ssize_t n = ::read(child.output, buffer, sizeof(buffer));
        if(n > 0) {
            child.text.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if(n == 0) {
            ::close(child.output);
            child.output = -1;
            return true;
        }
        if(errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

inline void drainNow(Child& child) {
    while(child.output >= 0 && drainOutput(child, 0)) {
        pollfd pfd{child.output, POLLIN | POLLHUP, 0};
        if(::poll(&pfd, 1, 0) <= 0) break;
    }
}

inline Child launch(const std::vector<std::string>& args, const fs::path& cwd) {
    if(args.empty()) return {};
    int pipefd[2] = {-1, -1};
    if(::pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0) return {};

    Child child;
    child.pid = ::fork();
    if(child.pid == 0) {
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        if(::chdir(cwd.c_str()) != 0) _exit(126);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for(const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    ::close(pipefd[1]);
    if(child.pid < 0) {
        ::close(pipefd[0]);
        return {};
    }
    child.output = pipefd[0];
    return child;
}

inline bool waitForExit(Child& child, int timeoutMs, int* exitCode = nullptr) {
    if(child.pid <= 0) return true;
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    int status = 0;
    while(remainingMs(end) > 0) {
        const pid_t got = ::waitpid(child.pid, &status, WNOHANG);
        if(got == child.pid) {
            drainNow(child);
            child.pid = -1;
            if(exitCode) *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if(got < 0 && errno != EINTR) return false;
        (void)drainOutput(child, std::min(remainingMs(end), 50));
    }
    return false;
}

inline bool stop(Child& child, int timeoutMs = 5000) {
    if(child.pid <= 0) return true;
    if(::kill(child.pid, SIGTERM) != 0 && errno != ESRCH) return false;
    if(waitForExit(child, timeoutMs)) return true;
    if(child.pid > 0) {
        (void)::kill(child.pid, SIGKILL);
        int status = 0;
        while(::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
        child.pid = -1;
    }
    drainNow(child);
    return false;
}

inline bool alive(const Child& child) {
    return child.pid > 0 && (::kill(child.pid, 0) == 0 || errno == EPERM);
}

inline uint16_t freePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t len = sizeof(addr);
    if(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}

inline int connectPort(uint16_t port, int timeoutMs = 2000) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
       && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    pollfd pfd{fd, POLLOUT, 0};
    if(::poll(&pfd, 1, timeoutMs) <= 0) {
        ::close(fd);
        return -1;
    }
    int error = 0;
    socklen_t len = sizeof(error);
    if(::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

inline int connectPortFrom(uint16_t port, const std::string& localIp, int timeoutMs = 2000) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(fd < 0) return -1;
    sockaddr_in source{};
    source.sin_family = AF_INET;
    if(::inet_pton(AF_INET, localIp.c_str(), &source.sin_addr) != 1
       || ::bind(fd, reinterpret_cast<sockaddr*>(&source), sizeof(source)) != 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
       && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    pollfd pfd{fd, POLLOUT, 0};
    if(::poll(&pfd, 1, timeoutMs) <= 0) {
        ::close(fd);
        return -1;
    }
    int error = 0;
    socklen_t len = sizeof(error);
    if(::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

inline bool sendAll(int fd, const std::string& wire, Clock::time_point end) {
    size_t offset = 0;
    while(offset < wire.size() && remainingMs(end) > 0) {
        const ssize_t n = ::send(fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
        if(n > 0) {
            offset += static_cast<size_t>(n);
            continue;
        }
        if(n < 0 && errno == EINTR) continue;
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{fd, POLLOUT, 0};
            if(::poll(&pfd, 1, remainingMs(end)) > 0) continue;
        }
        return false;
    }
    return offset == wire.size();
}

struct HttpResponse {
    int status{0};
    std::map<std::string, std::string> headers;
    std::vector<std::pair<std::string, std::string>> headerList;
    std::string body;
    std::string raw;

    std::string header(const std::string& name) const {
        auto it = headers.find(lower(name));
        return it == headers.end() ? "" : it->second;
    }

    std::vector<std::string> headerValues(const std::string& name) const {
        const std::string key = lower(name);
        std::vector<std::string> values;
        for(const auto& [header, value] : headerList) {
            if(header == key) values.push_back(value);
        }
        return values;
    }
};

inline bool parseHead(const std::string& head, HttpResponse& out) {
    const size_t lineEnd = head.find("\r\n");
    if(lineEnd == std::string::npos) return false;
    std::istringstream statusLine(head.substr(0, lineEnd));
    std::string version;
    if(!(statusLine >> version >> out.status) || version.rfind("HTTP/", 0) != 0) return false;
    size_t pos = lineEnd + 2;
    while(pos < head.size()) {
        const size_t end = head.find("\r\n", pos);
        if(end == std::string::npos || end == pos) break;
        const std::string line = head.substr(pos, end - pos);
        const size_t colon = line.find(':');
        if(colon == std::string::npos) return false;
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        out.headers[key] = value;
        out.headerList.emplace_back(key, value);
        pos = end + 2;
    }
    return true;
}

enum class ChunkParse { NEED_MORE, BAD, DONE };

inline ChunkParse decodeChunked(const std::string& wire, size_t begin, std::string& body,
                                size_t& consumed) {
    body.clear();
    size_t pos = begin;
    for(;;) {
        const size_t lineEnd = wire.find("\r\n", pos);
        if(lineEnd == std::string::npos) return ChunkParse::NEED_MORE;
        const std::string line = wire.substr(pos, lineEnd - pos);
        const size_t semi = line.find(';');
        const std::string sizeText = trim(line.substr(0, semi));
        if(sizeText.empty()) return ChunkParse::BAD;
        size_t size = 0;
        for(char c : sizeText) {
            unsigned digit = 0;
            if(c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if(c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if(c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else return ChunkParse::BAD;
            if(size > (std::numeric_limits<size_t>::max() - digit) / 16) {
                return ChunkParse::BAD;
            }
            size = size * 16 + digit;
        }
        pos = lineEnd + 2;
        if(size == 0) {
            for(;;) {
                const size_t trailerEnd = wire.find("\r\n", pos);
                if(trailerEnd == std::string::npos) return ChunkParse::NEED_MORE;
                if(trailerEnd == pos) {
                    consumed = trailerEnd + 2;
                    return ChunkParse::DONE;
                }
                if(wire.substr(pos, trailerEnd - pos).find(':') == std::string::npos) {
                    return ChunkParse::BAD;
                }
                pos = trailerEnd + 2;
            }
        }
        if(size > wire.size() - pos) return ChunkParse::NEED_MORE;
        const size_t end = pos + size;
        if(end + 2 > wire.size()) return ChunkParse::NEED_MORE;
        if(wire.compare(end, 2, "\r\n") != 0) return ChunkParse::BAD;
        body.append(wire, pos, size);
        pos = end + 2;
    }
}

inline bool readResponse(int fd, std::string& pending, HttpResponse& out,
                         Clock::time_point deadline) {
    bool untilClose = false;
    for(;;) {
        const size_t headEnd = pending.find("\r\n\r\n");
        if(headEnd != std::string::npos) {
            const size_t bodyBegin = headEnd + 4;
            HttpResponse parsed;
            if(!parseHead(pending.substr(0, headEnd + 2), parsed)) return false;
            if(parsed.status >= 100 && parsed.status < 200 && parsed.status != 101) {
                pending.erase(0, bodyBegin);
                continue;
            }
            if(containsToken(parsed.header("Transfer-Encoding"), "chunked")) {
                std::string body;
                size_t consumed = 0;
                switch(decodeChunked(pending, bodyBegin, body, consumed)) {
                    case ChunkParse::DONE:
                        parsed.body = std::move(body);
                        parsed.raw.assign(pending.data(), consumed);
                        pending.erase(0, consumed);
                        out = std::move(parsed);
                        return true;
                    case ChunkParse::BAD:
                        return false;
                    case ChunkParse::NEED_MORE:
                        break;
                }
            }
            size_t contentLength = 0;
            bool hasLength = false;
            if(auto value = parsed.header("Content-Length"); !value.empty()) {
                char* tail = nullptr;
                errno = 0;
                const unsigned long long n = std::strtoull(value.c_str(), &tail, 10);
                if(errno || !tail || *tail) return false;
                contentLength = static_cast<size_t>(n);
                hasLength = true;
            }
            if(hasLength && pending.size() >= bodyBegin + contentLength) {
                parsed.body.assign(pending.data() + bodyBegin, contentLength);
                parsed.raw.assign(pending.data(), bodyBegin + contentLength);
                pending.erase(0, bodyBegin + contentLength);
                out = std::move(parsed);
                return true;
            }
            if(!hasLength && containsToken(parsed.header("Connection"), "close")) {
                untilClose = true;
            }
        }
        if(remainingMs(deadline) <= 0) return false;
        pollfd pfd{fd, POLLIN | POLLHUP, 0};
        const int ready = ::poll(&pfd, 1, remainingMs(deadline));
        if(ready <= 0) return false;
        char buffer[4096];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if(n > 0) {
            pending.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if(n == 0 && untilClose) {
            const size_t headEnd = pending.find("\r\n\r\n");
            HttpResponse parsed;
            if(headEnd == std::string::npos || !parseHead(pending.substr(0, headEnd + 2), parsed)) {
                return false;
            }
            parsed.body = pending.substr(headEnd + 4);
            parsed.raw = std::move(pending);
            pending.clear();
            out = std::move(parsed);
            return true;
        }
        if(n < 0 && errno == EINTR) continue;
        return false;
    }
}

class HttpClient {
public:
    ~HttpClient() { close(); }

    bool connect(uint16_t port, int timeoutMs = 2000) {
        close();
        m_fd = connectPort(port, timeoutMs);
        return m_fd >= 0;
    }

    bool connectFrom(uint16_t port, const std::string& localIp, int timeoutMs = 2000) {
        close();
        m_fd = connectPortFrom(port, localIp, timeoutMs);
        return m_fd >= 0;
    }

    void close() {
        if(m_fd >= 0) ::close(m_fd);
        m_fd = -1;
        m_pending.clear();
    }

    bool request(const std::string& method, const std::string& target, const std::string& body,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 bool keepAlive, HttpResponse& response, int timeoutMs = 3000) {
        if(m_fd < 0) return false;
        std::string wire = method + " " + target + " HTTP/1.1\r\nHost: client.test\r\n";
        bool hasLength = false;
        bool hasConnection = false;
        for(const auto& [key, value] : headers) {
            wire += key + ": " + value + "\r\n";
            if(lower(key) == "content-length") hasLength = true;
            if(lower(key) == "connection") hasConnection = true;
        }
        if(!hasLength) wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        if(!hasConnection) wire += std::string("Connection: ") + (keepAlive ? "keep-alive" : "close")
            + "\r\n";
        wire += "\r\n" + body;
        const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
        if(!sendAll(m_fd, wire, end) || !readResponse(m_fd, m_pending, response, end)) {
            close();
            return false;
        }
        if(!keepAlive || containsToken(response.header("Connection"), "close")) close();
        return true;
    }

private:
    int m_fd{-1};
    std::string m_pending;
};

inline std::optional<HttpResponse> requestOnce(
    uint16_t port, const std::string& method, const std::string& target,
    const std::string& body = {},
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    int timeoutMs = 3000) {
    HttpClient client;
    if(!client.connect(port, timeoutMs)) return std::nullopt;
    HttpResponse response;
    if(!client.request(method, target, body, headers, false, response, timeoutMs)) {
        return std::nullopt;
    }
    return response;
}

inline std::optional<HttpResponse> requestOnceFrom(
    uint16_t port, const std::string& localIp, const std::string& method,
    const std::string& target, const std::string& body = {},
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    int timeoutMs = 3000) {
    HttpClient client;
    if(!client.connectFrom(port, localIp, timeoutMs)) return std::nullopt;
    HttpResponse response;
    if(!client.request(method, target, body, headers, false, response, timeoutMs)) {
        return std::nullopt;
    }
    return response;
}

inline std::string requestRaw(uint16_t port, const std::string& wire, int timeoutMs = 3000) {
    const int fd = connectPort(port, timeoutMs);
    if(fd < 0) return {};
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    if(!sendAll(fd, wire, end)) {
        ::close(fd);
        return {};
    }
    std::string response;
    while(remainingMs(end) > 0) {
        pollfd pfd{fd, POLLIN | POLLHUP, 0};
        if(::poll(&pfd, 1, remainingMs(end)) <= 0) break;
        char buffer[4096];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if(n > 0) {
            response.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if(n == 0) break;
        if(errno != EINTR) break;
    }
    ::close(fd);
    return response;
}

inline bool isSocket(const fs::path& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

inline std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline bool parseJson(const HttpResponse& response, Json::Value& value) {
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in(response.body);
    return Json::parseFromStream(builder, in, &value, &errors);
}

inline std::optional<uint64_t> jsonUnsigned(const Json::Value& value, const char* key) {
    if(!value.isMember(key) || !value[key].isNumeric()) return std::nullopt;
    return value[key].asUInt64();
}

inline std::optional<double> promValue(const std::string& body, const std::string& metric) {
    std::istringstream in(body);
    std::string line;
    while(std::getline(in, line)) {
        if(line.rfind(metric + " ", 0) != 0) continue;
        char* end = nullptr;
        const double value = std::strtod(line.c_str() + metric.size() + 1, &end);
        if(end && *end == '\0') return value;
    }
    return std::nullopt;
}

inline double promSum(const std::string& body, const std::string& metricPrefix) {
    std::istringstream in(body);
    std::string line;
    double sum = 0;
    while(std::getline(in, line)) {
        if(line.rfind(metricPrefix, 0) != 0) continue;
        const size_t space = line.rfind(' ');
        if(space == std::string::npos) continue;
        char* end = nullptr;
        const double value = std::strtod(line.c_str() + space + 1, &end);
        if(end && *end == '\0') sum += value;
    }
    return sum;
}

inline bool waitUntil(const std::function<bool()>& ready, int timeoutMs) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        if(ready()) return true;
        waitStep(end);
    } while(remainingMs(end) > 0);
    return ready();
}

struct MockRequest {
    std::string method;
    std::string target;
    std::string body;
    std::map<std::string, std::string> headers;
    std::vector<std::pair<std::string, std::string>> headerList;

    std::vector<std::string> headerValues(const std::string& name) const {
        const std::string key = lower(name);
        std::vector<std::string> values;
        for(const auto& [header, value] : headerList) {
            if(header == key) values.push_back(value);
        }
        return values;
    }

    std::string header(const std::string& name) const {
        auto it = headers.find(lower(name));
        return it == headers.end() ? "" : it->second;
    }
};

enum class MockMode {
    HEALTHY,
    REJECT,
    TIMEOUT_HEAD,
    DELAY_BODY,
    PARTIAL_RESPONSE,
    BAD_CHUNK,
    CHUNKED,
    INFORMATIONAL,
    UNTIL_CLOSE,
};

class RawMockUpstream {
public:
    RawMockUpstream() { start(); }

    ~RawMockUpstream() { stop(); }

    RawMockUpstream(const RawMockUpstream&) = delete;
    RawMockUpstream& operator=(const RawMockUpstream&) = delete;

    uint16_t port() const { return m_port; }

    void setHealthy(bool value) {
        setMode(value ? MockMode::HEALTHY : MockMode::REJECT);
    }

    void setMode(MockMode mode) { m_mode.store(mode, std::memory_order_release); }
    void setDelayMs(int delayMs) { m_delayMs.store(std::max(delayMs, 0), std::memory_order_release); }

    std::vector<MockRequest> records() const {
        std::lock_guard<std::mutex> lock(m_recordsMutex);
        return m_records;
    }

    size_t requestCount() const {
        std::lock_guard<std::mutex> lock(m_recordsMutex);
        return m_records.size();
    }

    bool waitForRequests(size_t count, int timeoutMs) const {
        return waitUntil([this, count] { return requestCount() >= count; }, timeoutMs);
    }

private:
    void start() {
        m_listen = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if(m_listen < 0) return;
        int one = 1;
        (void)::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if(::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
           || ::listen(m_listen, 64) != 0) {
            ::close(m_listen);
            m_listen = -1;
            return;
        }
        socklen_t len = sizeof(addr);
        if(::getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(m_listen);
            m_listen = -1;
            return;
        }
        m_port = ntohs(addr.sin_port);
        m_acceptThread = std::thread([this] { acceptLoop(); });
    }

    static bool readExact(int fd, std::string& pending, size_t bytes, Clock::time_point end) {
        while(pending.size() < bytes && remainingMs(end) > 0) {
            pollfd pfd{fd, POLLIN | POLLHUP, 0};
            if(::poll(&pfd, 1, remainingMs(end)) <= 0) return false;
            char buffer[4096];
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if(n > 0) {
                pending.append(buffer, static_cast<size_t>(n));
                continue;
            }
            if(n < 0 && errno == EINTR) continue;
            return false;
        }
        return pending.size() >= bytes;
    }

    static bool readRequest(int fd, std::string& pending, MockRequest& out,
                            Clock::time_point deadline) {
        size_t headEnd = pending.find("\r\n\r\n");
        while(headEnd == std::string::npos) {
            if(!readExact(fd, pending, pending.size() + 1, deadline)) return false;
            headEnd = pending.find("\r\n\r\n");
        }
        const std::string head = pending.substr(0, headEnd);
        const size_t firstEnd = head.find("\r\n");
        if(firstEnd == std::string::npos) return false;
        std::istringstream line(head.substr(0, firstEnd));
        std::string version;
        if(!(line >> out.method >> out.target >> version) || version.rfind("HTTP/", 0) != 0) {
            return false;
        }
        size_t pos = firstEnd + 2;
        size_t contentLength = 0;
        bool chunked = false;
        while(pos < head.size()) {
            size_t end = head.find("\r\n", pos);
            if(end == std::string::npos) end = head.size();
            const std::string item = head.substr(pos, end - pos);
            const size_t colon = item.find(':');
            if(colon == std::string::npos) return false;
            const std::string key = lower(trim(item.substr(0, colon)));
            const std::string value = trim(item.substr(colon + 1));
            out.headers[key] = value;
            out.headerList.emplace_back(key, value);
            if(key == "content-length") {
                char* tail = nullptr;
                errno = 0;
                const unsigned long long n = std::strtoull(value.c_str(), &tail, 10);
                if(errno || !tail || *tail) return false;
                contentLength = static_cast<size_t>(n);
            }
            if(key == "transfer-encoding" && containsToken(value, "chunked")) chunked = true;
            pos = end == head.size() ? end : end + 2;
        }
        const size_t bodyBegin = headEnd + 4;
        if(chunked) {
            for(;;) {
                size_t consumed = 0;
                switch(decodeChunked(pending, bodyBegin, out.body, consumed)) {
                    case ChunkParse::DONE:
                        pending.erase(0, consumed);
                        return true;
                    case ChunkParse::BAD:
                        return false;
                    case ChunkParse::NEED_MORE:
                        if(!readExact(fd, pending, pending.size() + 1, deadline)) return false;
                        break;
                }
            }
        }
        const size_t complete = bodyBegin + contentLength;
        if(!readExact(fd, pending, complete, deadline)) return false;
        out.body.assign(pending.data() + bodyBegin, contentLength);
        pending.erase(0, complete);
        return true;
    }

    bool delay() const {
        const int delayMs = m_delayMs.load(std::memory_order_acquire);
        if(delayMs <= 0) return !m_stopping.load(std::memory_order_acquire);
        const auto end = Clock::now() + std::chrono::milliseconds(delayMs);
        while(!m_stopping.load(std::memory_order_acquire) && remainingMs(end) > 0) {
            waitStep(end, 20);
        }
        return !m_stopping.load(std::memory_order_acquire);
    }

    bool writeResponse(int fd, const std::string& response) const {
        return sendAll(fd, response, Clock::now() + std::chrono::milliseconds(3000));
    }

    static std::string hexSize(size_t size) {
        std::ostringstream out;
        out << std::hex << size;
        return out.str();
    }

    void acceptLoop() {
        while(!m_stopping.load(std::memory_order_acquire)) {
            pollfd pfd{m_listen, POLLIN, 0};
            const int ready = ::poll(&pfd, 1, 100);
            if(ready <= 0) continue;
            const int fd = ::accept4(m_listen, nullptr, nullptr, SOCK_CLOEXEC);
            if(fd < 0) {
                if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                m_clients.push_back(fd);
            }
            std::lock_guard<std::mutex> lock(m_workersMutex);
            m_workers.emplace_back([this, fd] { serve(fd); });
        }
    }

    void serve(int fd) {
        std::string pending;
        while(!m_stopping.load(std::memory_order_acquire)) {
            MockRequest request;
            if(!readRequest(fd, pending, request,
                            Clock::now() + std::chrono::milliseconds(3000))) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(m_recordsMutex);
                m_records.push_back(request);
            }
            const MockMode mode = m_mode.load(std::memory_order_acquire);
            if(mode == MockMode::REJECT) break;
            if(mode == MockMode::TIMEOUT_HEAD) {
                (void)delay();
                break;
            }

            const std::string body = "mock:" + request.target + ":" + request.body;
            if(mode == MockMode::PARTIAL_RESPONSE) {
                const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: "
                    + std::to_string(body.size() + 4) + "\r\nConnection: close\r\n\r\n";
                (void)writeResponse(fd, head + body.substr(0, body.size() / 2));
                break;
            }
            if(mode == MockMode::BAD_CHUNK) {
                (void)writeResponse(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                                    "Connection: close\r\n\r\nQ\r\nnot-a-chunk\r\n");
                break;
            }
            if(mode == MockMode::DELAY_BODY) {
                const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: "
                    + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                if(!writeResponse(fd, head) || !delay() || !writeResponse(fd, body)) break;
                break;
            }
            if(mode == MockMode::CHUNKED) {
                const size_t split = std::max<size_t>(1, body.size() / 2);
                const std::string response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                    "Connection: close\r\n\r\n" + hexSize(split) + "\r\n"
                    + body.substr(0, split) + "\r\n" + hexSize(body.size() - split)
                    + "\r\n" + body.substr(split) + "\r\n0\r\n\r\n";
                if(!writeResponse(fd, response)) break;
                break;
            }
            if(mode == MockMode::INFORMATIONAL) {
                if(!writeResponse(fd, "HTTP/1.1 103 Early Hints\r\nLink: </app.js>; rel=preload\r\n\r\n")) {
                    break;
                }
            }
            if(mode == MockMode::UNTIL_CLOSE) {
                if(!writeResponse(fd, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + body)) break;
                break;
            }
            const std::string response = "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: keep-alive, X-Up-Hop\r\n"
                "Proxy-Connection: upstream-private\r\n"
                "X-Up-Hop: upstream-private\r\n"
                "X-Forwarded-For: upstream-forged\r\n"
                "Set-Cookie: one=1\r\n"
                "Set-Cookie: two=2\r\n"
                "\r\n" + body;
            if(!writeResponse(fd, response)) break;
            auto connection = request.headers.find("connection");
            if(connection != request.headers.end() && containsToken(connection->second, "close")) break;
        }
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            auto it = std::find(m_clients.begin(), m_clients.end(), fd);
            if(it != m_clients.end()) m_clients.erase(it);
        }
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    void stop() {
        if(m_stopping.exchange(true, std::memory_order_acq_rel)) return;
        if(m_listen >= 0) {
            ::shutdown(m_listen, SHUT_RDWR);
            ::close(m_listen);
            m_listen = -1;
        }
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            for(int fd : m_clients) (void)::shutdown(fd, SHUT_RDWR);
        }
        if(m_acceptThread.joinable()) m_acceptThread.join();
        std::lock_guard<std::mutex> lock(m_workersMutex);
        for(auto& worker : m_workers) {
            if(worker.joinable()) worker.join();
        }
        m_workers.clear();
    }

    int m_listen{-1};
    uint16_t m_port{0};
    std::atomic<bool> m_stopping{false};
    std::atomic<MockMode> m_mode{MockMode::HEALTHY};
    std::atomic<int> m_delayMs{0};
    std::thread m_acceptThread;
    mutable std::mutex m_recordsMutex;
    std::vector<MockRequest> m_records;
    std::mutex m_clientsMutex;
    std::vector<int> m_clients;
    std::mutex m_workersMutex;
    std::vector<std::thread> m_workers;
};

inline bool writeText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::trunc);
    out << text;
    return out.good();
}

inline bool atomicWrite(const fs::path& path, const std::string& text) {
    const fs::path temp = path.string() + ".tmp." + std::to_string(::getpid());
    if(!writeText(temp, text)) return false;
    std::error_code error;
    fs::rename(temp, path, error);
    if(error) {
        fs::remove(temp);
        return false;
    }
    return true;
}

inline std::string frameworkConfig() {
    return
        "cpu_pool:\n"
        "  threads: 1\n"
        "  max_queue: 32\n"
        "  name: api-gw-test\n"
        "fiber:\n"
        "  stack_size: 131072\n"
        "  stack_pool_size: 2\n"
        "  guard_page: 1\n"
        "tcp:\n"
        "  connect:\n"
        "    timeout: 3000\n"
        "tcp_server:\n"
        "  recv_timeout: 10000\n"
        "logs:\n"
        "  - name: root\n"
        "    level: warn\n"
        "    appenders:\n"
        "      - type: BxStdoutLogAppender\n"
        "        async: false\n";
}

struct GatewaySpec {
    size_t ioWorkers{2};
    std::vector<std::string> trustedProxies{"127.0.0.1"};
    uint16_t upstreamPort{0};
    uint64_t totalTimeoutMs{3000};
    uint64_t connectTimeoutMs{1000};
    uint64_t readTimeoutMs{3000};
    bool maintenance{false};
    bool ipFilterEnabled{false};
    std::string ipFilterMode{"denylist"};
    std::vector<std::string> ipCidrs;
    bool rateLimit{false};
    double rateCapacity{0};
    double rateRefillPerSec{0};
    std::string extra;
};

class Apps {
public:
    Apps(std::string name, bool wafEnabled)
        : m_name(std::move(name))
        , m_wafEnabled(wafEnabled)
        , m_root(fs::temp_directory_path() / ("bronx_api_gw_" + m_name + "_"
                                              + std::to_string(::getpid())))
        , m_execDir(m_root / "bin")
        , m_appDir(m_root / "api_gw/bin")
        , m_gatewayConfig(m_appDir / "gateway.yml")
        , m_hubConfig(m_appDir / "hub.yml")
        , m_gatewayFramework(m_root / "gw-bronx.yml")
        , m_hubFramework(m_root / "hub-bronx.yml")
        , m_submit(m_root / "submit.sock")
        , m_subscribe(m_root / "subscribe.sock")
        , m_adminSock(m_root / "admin.sock")
        , m_db(m_root / "hub.db") {
        fs::create_directories(m_appDir);
        m_businessPort = freePort();
        m_gatewayAdminPort = freePort();
        m_hubHttpPort = freePort();
        while(m_gatewayAdminPort == m_businessPort) m_gatewayAdminPort = freePort();
        while(m_hubHttpPort == m_businessPort || m_hubHttpPort == m_gatewayAdminPort) {
            m_hubHttpPort = freePort();
        }
    }

    ~Apps() {
        stopGateway();
        stopHub();
        std::error_code error;
        fs::remove_all(m_root, error);
    }

    bool prepare(const std::string& routeName = "api", const GatewaySpec& spec = {}) {
        return m_businessPort && m_gatewayAdminPort && m_hubHttpPort && m_mock.port()
            && materializeApp("gw") && materializeApp("hub")
            && writeText(m_gatewayFramework, frameworkConfig())
            && writeText(m_hubFramework, frameworkConfig())
            && writeGateway(routeName, false, spec)
            && writeHub();
    }

    bool writeGateway(const std::string& routeName, bool atomic, const GatewaySpec& spec = {}) {
        std::string text =
            "server:\n"
            "  address: 127.0.0.1:" + std::to_string(m_businessPort) + "\n"
            "  admin_address: 127.0.0.1:" + std::to_string(m_gatewayAdminPort) + "\n"
            "  io_workers: " + std::to_string(spec.ioWorkers) + "\n"
            "  maintenance: " + (spec.maintenance ? "true" : "false") + "\n";
        if(!spec.trustedProxies.empty()) {
            text += "trusted_proxies:\n";
            for(const auto& cidr : spec.trustedProxies) text += "  - " + cidr + "\n";
        }
        const uint16_t upstreamPort = spec.upstreamPort ? spec.upstreamPort : m_mock.port();
        text +=
            "upstreams:\n"
            "  - name: mock\n"
            "    lb: round_robin\n"
            "    timeout:\n"
            "      total_ms: " + std::to_string(spec.totalTimeoutMs) + "\n"
            "      connect_ms: " + std::to_string(spec.connectTimeoutMs) + "\n"
            "      read_ms: " + std::to_string(spec.readTimeoutMs) + "\n"
            "    endpoints:\n"
            "      - host: 127.0.0.1\n"
            "        port: " + std::to_string(upstreamPort) + "\n"
            "routes:\n"
            "  - name: " + routeName + "\n"
            "    path: /api\n"
            "    upstream: mock\n"
            "    strip_prefix: true\n";
        if(spec.rateLimit) {
            text += "    rate_limit:\n"
                "      enabled: true\n"
                "      capacity: " + std::to_string(spec.rateCapacity) + "\n"
                "      refill_per_sec: " + std::to_string(spec.rateRefillPerSec) + "\n"
                "      key: ip\n";
        }
        text += std::string("ip_filter:\n")
            + "  enabled: " + (spec.ipFilterEnabled ? "true" : "false") + "\n"
            + "  mode: " + spec.ipFilterMode + "\n";
        if(spec.ipCidrs.empty()) text += "  cidrs: []\n";
        else {
            text += "  cidrs:\n";
            for(const auto& cidr : spec.ipCidrs) text += "    - " + cidr + "\n";
        }
        text +=
            "ip_policy:\n"
            "  submit_sock: " + m_submit.string() + "\n"
            "  subscribe_sock: " + m_subscribe.string() + "\n"
            "  instance_id: " + m_name + "-" + std::to_string(::getpid()) + "\n"
            "  waf:\n"
            "    enabled: " + (m_wafEnabled ? "true" : "false") + "\n"
            "    ban_ms: 2000\n"
            "  rate_report:\n"
            "    enabled: false\n";
        text += spec.extra;
        return atomic ? atomicWrite(m_gatewayConfig, text) : writeText(m_gatewayConfig, text);
    }

    bool writeRawGateway(const std::string& text, bool atomic = true) {
        return atomic ? atomicWrite(m_gatewayConfig, text) : writeText(m_gatewayConfig, text);
    }

    bool setGatewayLogLevel(const std::string& level) {
        std::string text = frameworkConfig();
        const std::string from = "    level: warn\n";
        const size_t pos = text.find(from);
        if(pos == std::string::npos) return false;
        text.replace(pos, from.size(), "    level: " + level + "\n");
        return writeText(m_gatewayFramework, text);
    }

    bool writeHub() {
        return writeText(m_hubConfig,
            "server:\n"
            "  submit_sock: " + m_submit.string() + "\n"
            "  subscribe_sock: " + m_subscribe.string() + "\n"
            "  admin_sock: " + m_adminSock.string() + "\n"
            "  http_address: 127.0.0.1:" + std::to_string(m_hubHttpPort) + "\n"
            "  db_path: " + m_db.string() + "\n"
            "  iom_workers: 2\n");
    }

    bool startHub() {
        stopHub();
        (void)::unlink(m_submit.c_str());
        (void)::unlink(m_subscribe.c_str());
        (void)::unlink(m_adminSock.c_str());
        m_hub = launch({appExecutable("hub").string(), "-c", m_hubFramework.string()}, m_root);
        return m_hub.pid > 0;
    }

    bool startGateway() {
        stopGateway();
        m_gw = launch({appExecutable("gw").string(), "-c", m_gatewayFramework.string()}, m_root);
        return m_gw.pid > 0;
    }

    bool stopHub(int timeoutMs = 5000) { return stop(m_hub, timeoutMs); }
    bool stopGateway(int timeoutMs = 5000) { return stop(m_gw, timeoutMs); }

    bool waitHubReady(int timeoutMs = 7000) {
        return waitUntil([this] {
            auto response = requestOnce(m_hubHttpPort, "GET", "/healthz");
            return response && response->status == 200 && isSocket(m_submit)
                && isSocket(m_subscribe) && isSocket(m_adminSock) && alive(m_hub);
        }, timeoutMs);
    }

    bool waitGatewayReady(int timeoutMs = 7000) {
        return waitUntil([this] {
            auto response = requestOnce(m_businessPort, "GET", "/healthz");
            return response && response->status == 200 && alive(m_gw);
        }, timeoutMs);
    }

    std::optional<Json::Value> stats() const {
        auto response = requestOnce(m_gatewayAdminPort, "GET", "/stats");
        if(!response || response->status != 200) return std::nullopt;
        Json::Value value;
        if(!parseJson(*response, value)) return std::nullopt;
        return value;
    }

    std::optional<std::string> hubMetrics() const {
        auto response = requestOnce(m_hubHttpPort, "GET", "/metrics");
        if(!response || response->status != 200) return std::nullopt;
        return response->body;
    }

    std::optional<HttpResponse> gatewayRequest(
        const std::string& method, const std::string& target, const std::string& body,
        const std::string& xff,
        const std::vector<std::pair<std::string, std::string>>& extra = {},
        int timeoutMs = 3000) const {
        auto headers = extra;
        if(!xff.empty()) headers.emplace_back("X-Forwarded-For", xff);
        return requestOnce(m_businessPort, method, target, body, headers, timeoutMs);
    }

    std::string diagnostics() {
        drainNow(m_gw);
        drainNow(m_hub);
        std::ostringstream out;
        out << "runtime=" << m_root << "\n"
            << "gateway.yml:\n" << readFile(m_gatewayConfig)
            << "hub.yml:\n" << readFile(m_hubConfig)
            << "gw output:\n" << m_gw.text
            << "hub output:\n" << m_hub.text
            << "mock records:\n";
        const auto records = m_mock.records();
        const size_t limit = std::min<size_t>(records.size(), 20);
        for(size_t i = 0; i < limit; ++i) {
            const auto& record = records[i];
            out << record.method << " " << record.target << " body=" << record.body;
            for(const auto& [key, value] : record.headers) out << " " << key << "=" << value;
            out << "\n";
        }
        if(records.size() > limit) out << "... " << (records.size() - limit) << " more records\n";
        return out.str();
    }

    static fs::path appBinary(const char* name) {
        return fs::path(API_GW_APP_BIN_DIR) / name;
    }

    RawMockUpstream& mock() { return m_mock; }
    const RawMockUpstream& mock() const { return m_mock; }
    Child& gateway() { return m_gw; }
    Child& hub() { return m_hub; }
    const fs::path& root() const { return m_root; }
    const fs::path& submitSock() const { return m_submit; }
    const fs::path& subscribeSock() const { return m_subscribe; }
    const fs::path& adminSock() const { return m_adminSock; }
    const fs::path& gatewayConfigPath() const { return m_gatewayConfig; }
    uint16_t businessPort() const { return m_businessPort; }
    uint16_t gatewayAdminPort() const { return m_gatewayAdminPort; }
    uint16_t hubHttpPort() const { return m_hubHttpPort; }

private:
    bool materializeApp(const char* name) {
        const fs::path source = appBinary(name);
        const fs::path target = m_execDir / name;
        std::error_code error;
        fs::create_directories(m_execDir, error);
        if(error || !fs::is_regular_file(source)) return false;
        fs::create_hard_link(source, target, error);
        if(!error) return true;
        error.clear();
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, error);
        return !error;
    }

    fs::path appExecutable(const char* name) const {
        return m_execDir / name;
    }

    std::string m_name;
    bool m_wafEnabled{false};
    fs::path m_root;
    fs::path m_execDir;
    fs::path m_appDir;
    fs::path m_gatewayConfig;
    fs::path m_hubConfig;
    fs::path m_gatewayFramework;
    fs::path m_hubFramework;
    fs::path m_submit;
    fs::path m_subscribe;
    fs::path m_adminSock;
    fs::path m_db;
    uint16_t m_businessPort{0};
    uint16_t m_gatewayAdminPort{0};
    uint16_t m_hubHttpPort{0};
    RawMockUpstream m_mock;
    Child m_gw;
    Child m_hub;
};

struct ToolResult {
    int exitCode{-1};
    std::string output;
};

inline ToolResult runTool(const std::vector<std::string>& args, const fs::path& cwd,
                          int timeoutMs = 5000) {
    Child child = launch(args, cwd);
    ToolResult result;
    if(child.pid <= 0) return result;
    int exitCode = -1;
    (void)waitForExit(child, timeoutMs, &exitCode);
    drainNow(child);
    result.exitCode = exitCode;
    result.output = std::move(child.text);
    if(child.pid > 0) (void)stop(child);
    return result;
}

inline size_t fdCount(pid_t pid) {
    std::error_code error;
    const fs::path path = fs::path("/proc") / std::to_string(pid) / "fd";
    size_t count = 0;
    for(fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
        ++count;
    }
    return error ? 0 : count;
}

inline size_t threadCount(pid_t pid) {
    std::error_code error;
    const fs::path path = fs::path("/proc") / std::to_string(pid) / "task";
    size_t count = 0;
    for(fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
        ++count;
    }
    return error ? 0 : count;
}

} // namespace api_gw_test
