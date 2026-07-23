// G3 端到端:中间件链 + 路由 + 上游流式转发
// ==========================================
// 起一个"上游" GatewayServer(echo) + 一个"网关" GatewayServer(router+proxy 链),
// 客户端打网关 → 网关转发到上游 → 响应流式回传。验证:转发、路由命中/未命中(404)、
// 中间件短路、上游不可达(502)、大 body 流式中继。

#include "test_util.h"
#include "gateway.h"
#include "conn.h"
#include "ctx.h"
#include "mw.h"
#include "middlewares/proxy.h"
#include "middlewares/stubs.h"
#include "middlewares/builtin.h"
#include "router.h"
#include "ups_group.h"
#include "lb.h"
#include "reactor.h"
#include "endpoint.h"
#include "io_hook.h"
#include "ip.h"
#include "metrics.h"
#include "str.h"
#include "jwt.h"
#include <jwt-cpp/jwt.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <sstream>

using namespace bronx::gateway;
static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

static const uint16_t GW_PORT = 18700;
static const uint16_t UP_PORT = 18701;

// 上游 echo handler:回 200,body = "UP:<path>:<reqbody>"
static void upstream_handler(GatewayConnection& c) {
    auto req = c.request();
    std::string rb;
    if(req->getMethod() != HttpMethod::HEAD) {
        auto reader = c.requestBody();
        if(reader) { int n; while((n = reader->readChunk(rb)) > 0) {} }
    }
    auto rsp = std::make_shared<GwResponse>();
    rsp->setStatus(200);
    rsp->setHeader("Proxy-Connection", "should-strip");
    rsp->setHeader("Trailer", "X-Reply-Sum");
    if(req->getMethod() == HttpMethod::HEAD) {
        rsp->setHeader("Content-Length", "123");
        c.sendResponseHead(rsp);
        return;
    }
    if(req->getPath() == "/empty") {
        c.sendResponse(rsp, "");
        return;
    }
    std::string body = "UP:" + req->getPath() + ":" + rb
        + ":marker=" + req->getHeader("X-Rewrite-Marker")
        + ":remove=" + req->getHeader("X-Remove-Me", "<missing>")
        + ":hop=" + req->getHeader("X-Hop-Private", "<missing>")
        + ":proxy=" + req->getHeader("Proxy-Connection", "<missing>")
        + ":trailer=" + req->getHeader("Trailer", "<missing>")
        + ":keep=" + req->getHeader("Keep-Alive", "<missing>")
        + ":rulehop=" + req->getHeader("X-Rule-Hop", "<missing>")
        + ":expect=" + req->getHeader("Expect", "<missing>")
        + ":xff=" + req->getHeader("X-Forwarded-For", "<missing>")
        + ":real=" + req->getHeader("X-Real-IP", "<missing>")
        + ":proto=" + req->getHeader("X-Forwarded-Proto", "<missing>")
        + ":auth=" + req->getHeader("Authorization", "<missing>")
        + ":uid=" + req->getHeader("X-User-Id", "<missing>")
        + ":scopes=" + req->getHeader("X-User-Scopes", "<missing>")
        + ":roles=" + req->getHeader("X-User-Roles", "<missing>");
    c.sendResponse(rsp, body);
}

// 网关 handler:对每请求建 context + 跑中间件链
static GatewayServer::RequestHandler makeChainHandler(MwChain::ptr chain) {
    return [chain](GatewayConnection& c) {
        ReqCtx ctx(&c);
        chain->run(ctx);
    };
}

static std::string auth_jwt() {
    return jwt::create()
        .set_payload_claim("sub", jwt::claim(std::string("proxy-user")))
        .set_payload_claim("scope", jwt::claim(std::string("read write")))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::hs256{"proxy-secret"});
}

static int connect_fd(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if(connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

static void wake_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (sockaddr*)&addr, sizeof(addr));
    close(fd);
}

static void set_blocking_fd(int fd) {
    int flags = fcntl_f(fd, F_GETFL, 0);
    if(flags >= 0) {
        fcntl_f(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
}

// 收完整响应(读到对端关闭)
static std::string recv_all(int fd) {
    std::string out; char buf[2048]; int n;
    while((n = recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, n);
    return out;
}

static bool send_all_fd(int fd, const char* data, size_t len) {
    size_t off = 0;
    while(off < len) {
        ssize_t n = send(fd, data + off, len - off, 0);
        if(n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static bool recv_exact_fd(int fd, char* data, size_t len) {
    size_t off = 0;
    while(off < len) {
        ssize_t n = recv(fd, data + off, len - off, 0);
        if(n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static bool recv_exact_buffered(int fd, std::string& pending, char* data, size_t len) {
    size_t off = 0;
    if(!pending.empty()) {
        size_t take = std::min(len, pending.size());
        memcpy(data, pending.data(), take);
        pending.erase(0, take);
        off += take;
    }
    return off == len || recv_exact_fd(fd, data + off, len - off);
}

static std::string base64(const unsigned char* data, size_t len) {
    std::string out;
    out.resize(4 * ((len + 2) / 3));
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), data, (int)len);
    out.resize((size_t)n);
    return out;
}

static std::string ws_accept_key(const std::string& key) {
    static const char* guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string in = key + guid;
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(in.data()), in.size(), digest);
    return base64(digest, sizeof(digest));
}

static std::string get_header_value(const std::string& head, const std::string& name) {
    std::string needle = "\r\n" + name + ":";
    auto p = head.find(needle);
    if(p == std::string::npos) {
        if(head.compare(0, name.size(), name) == 0 && head.size() > name.size()
                && head[name.size()] == ':') {
            p = 0;
        } else {
            return "";
        }
    } else {
        p += 2;
    }
    p += name.size() + 1;
    while(p < head.size() && (head[p] == ' ' || head[p] == '\t')) ++p;
    auto e = head.find("\r\n", p);
    return head.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static std::string ws_client_frame(uint8_t opcode, const std::string& payload) {
    std::string out;
    out.push_back((char)(0x80 | (opcode & 0x0f)));
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    if(payload.size() < 126) {
        out.push_back((char)(0x80 | payload.size()));
    } else {
        out.push_back((char)(0x80 | 126));
        out.push_back((char)((payload.size() >> 8) & 0xff));
        out.push_back((char)(payload.size() & 0xff));
    }
    out.append(reinterpret_cast<const char*>(mask), sizeof(mask));
    for(size_t i = 0; i < payload.size(); ++i) {
        out.push_back((char)(payload[i] ^ mask[i % 4]));
    }
    return out;
}

static std::string ws_client_text_frame(const std::string& payload) {
    return ws_client_frame(0x1, payload);
}

static std::string ws_server_frame(uint8_t opcode, const std::string& payload) {
    std::string out;
    out.push_back((char)(0x80 | (opcode & 0x0f)));
    if(payload.size() < 126) {
        out.push_back((char)payload.size());
    } else {
        out.push_back((char)126);
        out.push_back((char)((payload.size() >> 8) & 0xff));
        out.push_back((char)(payload.size() & 0xff));
    }
    out += payload;
    return out;
}

static std::string ws_server_text_frame(const std::string& payload) {
    return ws_server_frame(0x1, payload);
}

static bool read_ws_frame_buffered(int fd, std::string& pending,
                                   std::string& payload, uint8_t* opcode = nullptr) {
    unsigned char h[2];
    if(!recv_exact_buffered(fd, pending, reinterpret_cast<char*>(h), 2)) return false;
    bool masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7f;
    if(len == 126) {
        unsigned char ext[2];
        if(!recv_exact_buffered(fd, pending, reinterpret_cast<char*>(ext), 2)) return false;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if(len == 127) {
        return false;
    }
    unsigned char mask[4] = {0, 0, 0, 0};
    if(masked && !recv_exact_buffered(fd, pending, reinterpret_cast<char*>(mask), 4)) {
        return false;
    }
    payload.assign((size_t)len, '\0');
    if(len > 0 && !recv_exact_buffered(fd, pending, &payload[0], (size_t)len)) {
        return false;
    }
    if(masked) {
        for(size_t i = 0; i < payload.size(); ++i) {
            payload[i] = (char)(payload[i] ^ mask[i % 4]);
        }
    }
    if(opcode) *opcode = h[0] & 0x0f;
    return true;
}

static bool read_ws_frame(int fd, std::string& payload, uint8_t* opcode = nullptr) {
    std::string pending;
    return read_ws_frame_buffered(fd, pending, payload, opcode);
}

struct RawTruncatedChunkedUpstream {
    int fd = -1;
    uint16_t port = 0;
    std::thread worker;

    RawTruncatedChunkedUpstream() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "raw upstream socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "raw upstream bind");
        TEST_CHECK_MSG(listen(fd, 4) == 0, "raw upstream listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0, "raw upstream getsockname");
        port = ntohs(addr.sin_port);
        worker = std::thread([this]() {
            bronx::set_hook_enable(false);
            int c = accept(fd, nullptr, nullptr);
            if(c < 0) return;
            set_blocking_fd(c);
            timeval tv{2, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[512];
            std::string req;
            int n;
            while(req.find("\r\n\r\n") == std::string::npos
                  && (n = recv(c, buf, sizeof(buf), 0)) > 0) {
                req.append(buf, n);
            }
            std::string resp =
                "HTTP/1.1 200 OK\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: close\r\n\r\n"
                "5\r\nhello\r\n";
            send(c, resp.data(), resp.size(), 0);
            close(c);
        });
    }

    ~RawTruncatedChunkedUpstream() {
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

struct RawUp {
    int fd = -1;
    uint16_t port = 0;
    std::string out;
    uint64_t waitMs = 0;
    std::thread worker;

    RawUp(std::string v, uint64_t wait = 0)
        : out(std::move(v))
        , waitMs(wait) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "raw up socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "raw up bind");
        TEST_CHECK_MSG(listen(fd, 4) == 0, "raw up listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0, "raw up port");
        port = ntohs(addr.sin_port);
        worker = std::thread([this]() {
            bronx::set_hook_enable(false);
            int c = accept(fd, nullptr, nullptr);
            if(c < 0) return;
            set_blocking_fd(c);
            timeval tv{2, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[512];
            std::string req;
            int n;
            while(req.find("\r\n\r\n") == std::string::npos
                  && (n = recv(c, buf, sizeof(buf), 0)) > 0) {
                req.append(buf, n);
            }
            if(waitMs) usleep(waitMs * 1000);
            if(!out.empty()) send_all_fd(c, out.data(), out.size());
            close(c);
        });
    }

    ~RawUp() {
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

struct RawRetryUp {
    int fd = -1;
    uint16_t port = 0;
    std::atomic<int> posts{0};
    std::atomic<bool> done{false};
    std::thread worker;

    RawRetryUp() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "retry up socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "retry up bind");
        TEST_CHECK_MSG(listen(fd, 4) == 0, "retry up listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0, "retry up port");
        port = ntohs(addr.sin_port);
        worker = std::thread([this]() {
            bronx::set_hook_enable(false);
            auto readHead = [](int c) {
                std::string out;
                char buf[512];
                int n;
                while(out.find("\r\n\r\n") == std::string::npos
                      && (n = recv(c, buf, sizeof(buf), 0)) > 0) {
                    out.append(buf, n);
                }
                return out;
            };

            int c = accept(fd, nullptr, nullptr);
            if(c < 0) { done = true; return; }
            set_blocking_fd(c);
            timeval tv{2, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            readHead(c);
            std::string ok =
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                "Connection: keep-alive\r\n\r\nok";
            send_all_fd(c, ok.data(), ok.size());
            std::string second = readHead(c);
            if(second.find("POST ") == 0) ++posts;
            close(c);

            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            timeval wait{0, 500000};
            if(select(fd + 1, &set, nullptr, nullptr, &wait) > 0) {
                int c2 = accept(fd, nullptr, nullptr);
                if(c2 >= 0) {
                    set_blocking_fd(c2);
                    setsockopt(c2, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    std::string retry = readHead(c2);
                    if(retry.find("POST ") == 0) ++posts;
                    send_all_fd(c2, ok.data(), ok.size());
                    close(c2);
                }
            }
            done = true;
        });
    }

    ~RawRetryUp() {
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

struct RawBody {
    int fd = -1;
    uint16_t port = 0;
    std::thread worker;

    RawBody() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "raw body socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "raw body bind");
        TEST_CHECK_MSG(listen(fd, 4) == 0, "raw body listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0, "raw body port");
        port = ntohs(addr.sin_port);
        worker = std::thread([this]() {
            bronx::set_hook_enable(false);
            int c = accept(fd, nullptr, nullptr);
            if(c < 0) return;
            set_blocking_fd(c);
            timeval tv{2, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            char buf[512];
            std::string req;
            int n;
            while(req.find("\r\n\r\n") == std::string::npos
                  && (n = recv(c, buf, sizeof(buf), 0)) > 0) {
                req.append(buf, n);
            }
            constexpr size_t total = 1024 * 1024;
            std::string head =
                "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total)
                + "\r\nConnection: close\r\n\r\n";
            std::string chunk(16 * 1024, 'x');
            if(send_all_fd(c, head.data(), head.size())) {
                send_all_fd(c, chunk.data(), chunk.size());
                usleep(100 * 1000);
                for(size_t sent = chunk.size(); sent < total; sent += chunk.size()) {
                    if(!send_all_fd(c, chunk.data(), chunk.size())) break;
                }
            }
            close(c);
        });
    }

    ~RawBody() {
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

struct RawLateBody {
    int fd = -1;
    uint16_t port = 0;
    std::thread worker;

    explicit RawLateBody(uint64_t waitMs) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "late body socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "late body bind");
        TEST_CHECK_MSG(listen(fd, 4) == 0, "late body listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0, "late body port");
        port = ntohs(addr.sin_port);
        worker = std::thread([this, waitMs]() {
            bronx::set_hook_enable(false);
            int c = accept(fd, nullptr, nullptr);
            if(c < 0) return;
            set_blocking_fd(c);
            char buf[512];
            std::string req;
            int n = 0;
            while(req.find("\r\n\r\n") == std::string::npos
                    && (n = recv(c, buf, sizeof(buf), 0)) > 0) req.append(buf, n);
            std::string head =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Content-Length: 2\r\nConnection: close\r\n\r\n";
            send_all_fd(c, head.data(), head.size());
            usleep(waitMs * 1000);
            send_all_fd(c, "ok", 2);
            close(c);
        });
    }

    ~RawLateBody() {
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

struct RawInformationalUpstream {
    int fd = -1;
    uint16_t port = 0;
    std::thread worker;

    RawInformationalUpstream() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "informational upstream socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0,
                       "informational upstream bind");
        TEST_CHECK_MSG(listen(fd, 4) == 0, "informational upstream listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0,
                       "informational upstream getsockname");
        port = ntohs(addr.sin_port);
        worker = std::thread([this]() {
            bronx::set_hook_enable(false);
            int c = accept(fd, nullptr, nullptr);
            if(c < 0) return;
            set_blocking_fd(c);
            timeval tv{2, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            char buf[512];
            std::string req;
            int n;
            while(req.find("\r\n\r\n") == std::string::npos
                  && (n = recv(c, buf, sizeof(buf), 0)) > 0) {
                req.append(buf, n);
            }
            std::string body = "final-response";
            std::string resp =
                "HTTP/1.1 100 Continue\r\n\r\n"
                "HTTP/1.1 200 OK\r\n"
                "Set-Cookie: a=1\r\n"
                "Set-Cookie: b=2\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            send_all_fd(c, resp.data(), resp.size());
            close(c);
        });
    }

    ~RawInformationalUpstream() {
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

struct RawHealthUpstream {
    int fd = -1;
    uint16_t port = 0;
    bool healthy = true;
    int hits = 0;
    std::thread worker;

    explicit RawHealthUpstream(bool ok) : healthy(ok) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "health upstream socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "health upstream bind");
        TEST_CHECK_MSG(listen(fd, 8) == 0, "health upstream listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0, "health upstream getsockname");
        port = ntohs(addr.sin_port);
        worker = std::thread([this]() {
            bronx::set_hook_enable(false);
            for(int i = 0; i < 4; ++i) {
                int c = accept(fd, nullptr, nullptr);
                if(c < 0) return;
                set_blocking_fd(c);
                timeval tv{2, 0};
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                char buf[512];
                std::string req;
                int n;
                while(req.find("\r\n\r\n") == std::string::npos
                      && (n = recv(c, buf, sizeof(buf), 0)) > 0) {
                    req.append(buf, n);
                }
                bool isHealth = req.find("GET /healthz ") != std::string::npos;
                if(!isHealth) {
                    ++hits;
                }
                if(isHealth && !healthy) {
                    std::string resp =
                        "HTTP/1.1 500 Internal Server Error\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
                    send(c, resp.data(), resp.size(), 0);
                } else {
                    std::string body = healthy ? "healthy" : "unhealthy-hit";
                    std::string resp =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: " + std::to_string(body.size()) + "\r\n"
                        "Connection: close\r\n\r\n" + body;
                    send(c, resp.data(), resp.size(), 0);
                }
                close(c);
            }
        });
    }

    ~RawHealthUpstream() {
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

struct RawWsEchoUpstream {
    int fd = -1;
    uint16_t port = 0;
    std::atomic<bool> stopping{false};
    std::atomic<int> sessions{0};
    std::atomic<int> frames{0};
    std::mutex mtx;
    std::string lastPath;
    std::string lastMarker;
    std::string lastHopPrivate;
    std::string lastTrailer;
    std::string lastProxy;
    std::string lastRuleHop;
    std::string lastAuth;
    std::string lastUser;
    std::vector<std::thread> workers;
    std::thread acceptor;

    RawWsEchoUpstream() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "ws upstream socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "ws upstream bind");
        TEST_CHECK_MSG(listen(fd, 128) == 0, "ws upstream listen");
        int flags = fcntl_f(fd, F_GETFL, 0);
        TEST_CHECK_MSG(flags >= 0 && fcntl_f(fd, F_SETFL, flags | O_NONBLOCK) == 0,
                       "ws upstream nonblock listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0, "ws upstream getsockname");
        port = ntohs(addr.sin_port);
        acceptor = std::thread([this]() {
            bronx::set_hook_enable(false);
            while(true) {
                int c = accept(fd, nullptr, nullptr);
                if(c < 0) {
                    if(stopping.load()) return;
                    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        usleep(1000);
                        continue;
                    }
                    return;
                }
                set_blocking_fd(c);
                if(stopping.load()) {
                    close(c);
                    return;
                }
                workers.emplace_back([this, c]() { handle(c); });
            }
        });
    }

    ~RawWsEchoUpstream() {
        stopping.store(true);
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(acceptor.joinable()) acceptor.join();
        for(auto& t : workers) {
            if(t.joinable()) t.join();
        }
    }

    void handle(int c) {
        bronx::set_hook_enable(false);
        timeval tv{5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        char buf[1024];
        std::string head;
        int n;
        while(head.find("\r\n\r\n") == std::string::npos
              && (n = recv(c, buf, sizeof(buf), 0)) > 0) {
            head.append(buf, n);
        }
        if(head.find("\r\n\r\n") == std::string::npos) {
            close(c);
            return;
        }
        std::string pending;
        auto headEnd = head.find("\r\n\r\n");
        if(headEnd != std::string::npos) {
            headEnd += 4;
            pending = head.substr(headEnd);
            head.resize(headEnd);
        }
        auto lineEnd = head.find("\r\n");
        std::string line = head.substr(0, lineEnd);
        auto sp1 = line.find(' ');
        auto sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
            ? line.substr(sp1 + 1, sp2 - sp1 - 1) : "";
        std::string key = get_header_value(head, "Sec-WebSocket-Key");
        if(key.empty()) {
            close(c);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            lastPath = path;
            lastMarker = get_header_value(head, "X-WS-Marker");
            lastHopPrivate = get_header_value(head, "X-Hop-Private");
            lastTrailer = get_header_value(head, "Trailer");
            lastProxy = get_header_value(head, "Proxy-Connection");
            lastRuleHop = get_header_value(head, "X-Rule-Hop");
            lastAuth = get_header_value(head, "Authorization");
            lastUser = get_header_value(head, "X-User-Id");
        }
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Accept: " + ws_accept_key(key) + "\r\n\r\n";
        if(!send_all_fd(c, resp.data(), resp.size())) {
            close(c);
            return;
        }
        ++sessions;
        while(true) {
            std::string payload;
            uint8_t opcode = 0;
            if(!read_ws_frame_buffered(c, pending, payload, &opcode)) break;
            if(opcode == 0x8) break;
            ++frames;
            auto out = ws_server_frame(opcode == 0x2 ? 0x2 : 0x1, payload);
            if(!send_all_fd(c, out.data(), out.size())) break;
        }
        close(c);
    }
};

struct RawWsHandshakeUpstream {
    int fd = -1;
    uint16_t port = 0;
    bool includeConnectionUpgrade = true;
    bool validAccept = true;
    size_t bytes = 0;
    std::thread worker;

    RawWsHandshakeUpstream(bool includeConnection, bool validAcceptHeader,
                           size_t sendBytes = 0)
        : includeConnectionUpgrade(includeConnection)
        , validAccept(validAcceptHeader)
        , bytes(sendBytes) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_CHECK_MSG(fd >= 0, "ws handshake upstream socket");
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0,
                       "ws handshake upstream bind");
        TEST_CHECK_MSG(listen(fd, 4) == 0, "ws handshake upstream listen");
        socklen_t len = sizeof(addr);
        TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0,
                       "ws handshake upstream getsockname");
        port = ntohs(addr.sin_port);
        worker = std::thread([this]() {
            bronx::set_hook_enable(false);
            int c = accept(fd, nullptr, nullptr);
            if(c < 0) return;
            set_blocking_fd(c);
            timeval tv{2, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            char buf[1024];
            std::string head;
            int n;
            while(head.find("\r\n\r\n") == std::string::npos
                  && (n = recv(c, buf, sizeof(buf), 0)) > 0) {
                head.append(buf, n);
            }
            std::string key = get_header_value(head, "Sec-WebSocket-Key");
            std::string accept = validAccept ? ws_accept_key(key) : "invalid-accept";
            std::string resp =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n";
            if(includeConnectionUpgrade) {
                resp += "Connection: keep-alive, Upgrade\r\n";
            }
            resp += "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
            send_all_fd(c, resp.data(), resp.size());
            std::string chunk(16 * 1024, 'w');
            for(size_t sent = 0; sent < bytes; sent += chunk.size()) {
                if(!send_all_fd(c, chunk.data(), chunk.size())) break;
            }
            close(c);
        });
    }

    ~RawWsHandshakeUpstream() {
        wake_listener(port);
        if(fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

static bool ws_roundtrip(uint16_t port, const std::string& path,
                         const std::string& payload, std::string& got,
                         std::string* detail = nullptr,
                         bool coalesceFirstFrame = false,
                         std::string* rspHead = nullptr,
                         const std::string& extraHeaders = "") {
    int fd = connect_fd(port);
    if(fd < 0) {
        if(detail) *detail = "connect failed";
        return false;
    }
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: x\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade, X-Hop-Private\r\n"
        "X-Hop-Private: should-strip\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "Trailer: X-WS-Sum\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n" + extraHeaders + "\r\n";
    auto frame = ws_client_text_frame(payload);
    if(coalesceFirstFrame) {
        req += frame;
    }
    if(!send_all_fd(fd, req.data(), req.size())) {
        if(detail) *detail = "send handshake failed";
        close(fd);
        return false;
    }
    std::string head;
    char buf[256];
    int n;
    while(head.find("\r\n\r\n") == std::string::npos
          && (n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        head.append(buf, n);
    }
    std::string pending;
    auto headEnd = head.find("\r\n\r\n");
    if(headEnd != std::string::npos) {
        headEnd += 4;
        pending = head.substr(headEnd);
        head.resize(headEnd);
    }
    if(head.find("101") == std::string::npos
            || get_header_value(head, "Sec-WebSocket-Accept") != ws_accept_key(key)) {
        if(detail) *detail = "bad handshake response: " + head;
        close(fd);
        return false;
    }
    if(rspHead) *rspHead = head;
    if(!coalesceFirstFrame && !send_all_fd(fd, frame.data(), frame.size())) {
        if(detail) *detail = "send frame failed";
        close(fd);
        return false;
    }
    bool ok = read_ws_frame_buffered(fd, pending, got);
    if(!ok && detail) {
        *detail = "read echo frame failed after head: " + head;
    }
    close(fd);
    return ok;
}

static bool ws_wait_close(uint16_t port, const std::string& path,
                          std::string* detail = nullptr) {
    int fd = connect_fd(port);
    if(fd < 0) {
        if(detail) *detail = "connect failed";
        return false;
    }
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: x\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if(!send_all_fd(fd, req.data(), req.size())) {
        close(fd);
        return false;
    }
    std::string head;
    char buf[512];
    int n;
    while(head.find("\r\n\r\n") == std::string::npos
          && (n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        head.append(buf, n);
    }
    if(head.find(" 101 ") == std::string::npos) {
        if(detail) *detail = head;
        close(fd);
        return false;
    }
    n = recv(fd, buf, 1, 0);
    if(n != 0 && detail) {
        *detail = "tunnel did not close n=" + std::to_string(n)
                + " errno=" + std::to_string(errno);
    }
    close(fd);
    return n == 0;
}

static bool ws_drop(uint16_t port, const std::string& path) {
    int fd = connect_fd(port);
    if(fd < 0) return false;
    int small = 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if(!send_all_fd(fd, req.data(), req.size())) {
        close(fd);
        return false;
    }
    std::string head;
    char buf[512];
    int n;
    while(head.find("\r\n\r\n") == std::string::npos
          && (n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        head.append(buf, n);
    }
    bool ok = head.find(" 101 ") != std::string::npos;
    usleep(100 * 1000);
    linger rst{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
    close(fd);
    return ok;
}

static GatewayServer::ptr start_ws_gw(bronx::BxIoManager& iom,
                                      uint16_t upPort, uint16_t gwPort,
                                      uint64_t idleMs) {
    auto reg = std::make_shared<UpstreamRegistry>();
    auto group = std::make_shared<UpstreamGroup>("ws", MakeLoadBalancer("round_robin"));
    group->addEndpoint(std::make_shared<Endpoint>(
        "127.0.0.1", upPort, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
    reg->add(group);
    auto router = std::make_shared<Router>();
    RouteRule rule;
    rule.name = "ws";
    rule.pathPattern = "/ws";
    rule.upstream = "ws";
    router->addRoute(rule);
    auto chain = std::make_shared<MwChain>();
    chain->use(MakeRequestIdMiddleware());
    chain->use(MakeSecurityHeadersMiddleware());
    chain->use(MakeRouterMiddleware(router, reg));
    chain->use(MakeWebSocketTunnelStub(1000, idleMs));
    auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    server->setRequestHandler(makeChainHandler(chain));
    server->bind(bronx::BxAddress::LookupAny(
        "127.0.0.1:" + std::to_string(gwPort)));
    server->start();
    usleep(100 * 1000);
    return server;
}

static bool ws_expect_status(uint16_t port, const std::string& path,
                             int status, std::string* detail = nullptr) {
    int fd = connect_fd(port);
    if(fd < 0) {
        if(detail) *detail = "connect failed";
        return false;
    }
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: x\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if(!send_all_fd(fd, req.data(), req.size())) {
        if(detail) *detail = "send handshake failed";
        close(fd);
        return false;
    }
    std::string head;
    char buf[256];
    int n;
    while(head.find("\r\n\r\n") == std::string::npos
          && (n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        head.append(buf, n);
    }
    close(fd);
    std::string needle = " " + std::to_string(status) + " ";
    if(head.find(needle) == std::string::npos) {
        if(detail) *detail = "unexpected handshake response: " + head;
        return false;
    }
    return true;
}

static bool ws_multi_roundtrip(uint16_t port, const std::string& path,
                               const std::vector<std::string>& payloads,
                               std::string* detail = nullptr) {
    int fd = connect_fd(port);
    if(fd < 0) {
        if(detail) *detail = "connect failed";
        return false;
    }
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: x\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade, X-Hop-Private\r\n"
        "X-Hop-Private: should-strip\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if(!send_all_fd(fd, req.data(), req.size())) {
        if(detail) *detail = "send handshake failed";
        close(fd);
        return false;
    }
    std::string head;
    char buf[256];
    int n;
    while(head.find("\r\n\r\n") == std::string::npos
          && (n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        head.append(buf, n);
    }
    std::string pending;
    auto headEnd = head.find("\r\n\r\n");
    if(headEnd != std::string::npos) {
        headEnd += 4;
        pending = head.substr(headEnd);
        head.resize(headEnd);
    }
    if(head.find("101") == std::string::npos
            || get_header_value(head, "Sec-WebSocket-Accept") != ws_accept_key(key)) {
        if(detail) *detail = "bad handshake response: " + head;
        close(fd);
        return false;
    }
    for(const auto& payload : payloads) {
        auto frame = ws_client_text_frame(payload);
        if(!send_all_fd(fd, frame.data(), frame.size())) {
            if(detail) *detail = "send frame failed";
            close(fd);
            return false;
        }
        std::string got;
        if(!read_ws_frame_buffered(fd, pending, got) || got != payload) {
            if(detail) *detail = "echo mismatch";
            close(fd);
            return false;
        }
    }
    close(fd);
    return true;
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    BRONX_LOG_INFO(g_logger) << "=== test_gw_proxy start ===";
    bronx::BxIoManager iom(3);

    // 1. 上游 server
    auto upstream = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    upstream->setRequestHandler(upstream_handler);
    upstream->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(UP_PORT)));
    upstream->start();

    // 2. 网关 server:router(/api → 上游, strip 前缀) + proxy 终结
    auto router = std::make_shared<Router>();
    auto upReg0 = std::make_shared<UpstreamRegistry>();
    auto up0Ep = std::make_shared<Endpoint>(
        "127.0.0.1", UP_PORT, 1, CircuitBreakerConfig{}, ConnPoolConfig{});
    auto up0Group = std::make_shared<UpstreamGroup>("up0", MakeLoadBalancer("round_robin"));
    up0Group->addEndpoint(up0Ep);
    upReg0->add(up0Group);
    RouteRule rule; rule.name="up0"; rule.pathPattern="/api"; rule.upstream="up0"; rule.stripPrefix=true;
    router->addRoute(rule);
    RouteRule rewriteRule;
    rewriteRule.name = "rewrite";
    rewriteRule.pathPattern = "/rewrite";
    rewriteRule.upstream = "up0";
    rewriteRule.stripPrefix = true;
    rewriteRule.rewritePrefix = "/v1";
    rewriteRule.reqHeaderSet["X-Rewrite-Marker"] = "set-by-route";
    rewriteRule.reqHeaderSet["Connection"] = "X-Rule-Hop";
    rewriteRule.reqHeaderSet["X-Rule-Hop"] = "drop-me";
    rewriteRule.reqHeaderSet["Keep-Alive"] = "timeout=5";
    rewriteRule.reqHeaderSet["Trailer"] = "X-Route-Sum";
    rewriteRule.reqHeaderRemove.push_back("X-Remove-Me");
    router->addRoute(rewriteRule);
    RouteRule rootRewriteRule;
    rootRewriteRule.name = "rewrite-root";
    rootRewriteRule.pathPattern = "/rewrite-root";
    rootRewriteRule.upstream = "up0";
    rootRewriteRule.stripPrefix = true;
    rootRewriteRule.rewritePrefix = "/";
    router->addRoute(rootRewriteRule);
    RouteRule guardRule;
    guardRule.name = "guard";
    guardRule.pathPattern = "/guard";
    guardRule.upstream = "up0";
    router->addRoute(guardRule);
    RouteRule authRule;
    authRule.name = "auth";
    authRule.pathPattern = "/auth";
    authRule.upstream = "up0";
    authRule.stripPrefix = true;
    authRule.authPolicy = "jwt";
    router->addRoute(authRule);
    auto chain = std::make_shared<MwChain>();
    chain->use(std::make_shared<FuncMiddleware>(
        [](ReqCtx& ctx, const NextFn& next) {
            if(ctx.request() && ctx.request()->getPath() == "/guard") {
                ctx.add_resp_hdr("Connection", "X-Reply-Hop");
                ctx.add_resp_hdr("X-Reply-Hop", "should-strip");
                ctx.add_resp_hdr("X-Forwarded-For", "should-strip");
                ctx.add_resp_hdr("Content-Length", "999");
                ctx.add_resp_hdr("Transfer-Encoding", "chunked");
            }
            next();
        }, "guard"));
    chain->use(MakeRouterMiddleware(router, upReg0));
    JwtAuthConfig jwtCfg; jwtCfg.enabled = true; jwtCfg.secret = "proxy-secret";
    RouteAuthConfig authCfg; authCfg.fwdUser = true;
    chain->use(MakeRouteAuthMiddleware(std::make_shared<JwtAuthenticator>(jwtCfg), authCfg));
    chain->use(MakeProxyMiddleware(2000, 5000));

    auto gw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    gw->setRequestHandler(makeChainHandler(chain));
    gw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(GW_PORT)));
    gw->start();

    usleep(200 * 1000);

    // -- 测试 1:命中路由 → 转发到上游,strip /api 前缀 --
    {
        int fd = connect_fd(GW_PORT);
        TEST_CHECK_MSG(fd >= 0, "connect gw");
        std::string wire = "GET /api/users HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("200") != std::string::npos, "status 200: " << resp.substr(0,30));
        // 上游看到的 path 应是剥掉 /api 后的 /users
        TEST_CHECK_MSG(resp.find("UP:/users:") != std::string::npos,
                       "upstream path stripped: " << resp);
        close(fd);
    }

    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "GET /auth/check HTTP/1.1\r\nHost: x\r\n"
                           "Authorization: Bearer " + auth_jwt() + "\r\n"
                           "X-User-Id: fake\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find(":auth=<missing>") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":uid=proxy-user") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":scopes=read,write") != std::string::npos, resp);
        close(fd);
    }

    // -- 测试 2:POST body 经网关流式转发到上游 --
    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                           "Connection: close\r\n\r\nhello";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("UP:/echo:hello") != std::string::npos,
                       "body forwarded: " << resp);
        close(fd);
    }

    // -- 测试 2a:路径 rewrite + 请求头 set/remove + Connection 扩展逐跳头剥离 --
    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "GET /rewrite/users?x=1 HTTP/1.1\r\nHost: x\r\n"
                           "Connection: close, X-Hop-Private\r\n"
                           "X-Hop-Private: should-strip\r\n"
                           "Proxy-Connection: keep-alive\r\n"
                           "Trailer: X-Req-Sum\r\n"
                           "X-Forwarded-For: 198.51.100.8\r\n"
                           "X-Real-IP: 198.51.100.9\r\n"
                           "X-Forwarded-Proto: https\r\n"
                           "X-Remove-Me: should-remove\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("UP:/v1/users:") != std::string::npos,
                       "path rewrite should strip /rewrite and prepend /v1: " << resp);
        TEST_CHECK_MSG(resp.find(":marker=set-by-route") != std::string::npos,
                       "request header set should reach upstream: " << resp);
        TEST_CHECK_MSG(resp.find(":remove=<missing>") != std::string::npos,
                       "request header remove should hide client header: " << resp);
        TEST_CHECK_MSG(resp.find(":hop=<missing>") != std::string::npos,
                       "Connection-listed hop-by-hop header must be stripped: " << resp);
        TEST_CHECK_MSG(resp.find(":proxy=<missing>") != std::string::npos,
                       "Proxy-Connection must not reach upstream: " << resp);
        TEST_CHECK_MSG(resp.find(":trailer=<missing>") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":keep=<missing>") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":rulehop=<missing>") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("Proxy-Connection:") == std::string::npos,
                       "Proxy-Connection must not reach client: " << resp);
        TEST_CHECK_MSG(resp.find("Trailer:") == std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":xff=127.0.0.1") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":real=127.0.0.1") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":proto=http") != std::string::npos, resp);
        close(fd);
    }

    {
        int fd = connect_fd(GW_PORT);
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::string head =
            "POST /api/expect HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
            "Expect: 100-continue\r\nConnection: close\r\n\r\n";
        send_all_fd(fd, head.data(), head.size());
        char buf[512];
        int n = recv(fd, buf, sizeof(buf), 0);
        std::string interim = n > 0 ? std::string(buf, n) : "";
        TEST_CHECK_MSG(interim.find(" 100 Continue") != std::string::npos, interim);
        std::string body = "hello";
        send_all_fd(fd, body.data(), body.size());
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("UP:/expect:hello") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":expect=<missing>") != std::string::npos, resp);
        close(fd);
    }

    {
        int fd = connect_fd(GW_PORT);
        std::string wire =
            "POST /api/expect HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
            "Expect: fancy\r\nConnection: keep-alive\r\n\r\n";
        send_all_fd(fd, wire.data(), wire.size());
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find(" 417 ") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("Connection: close") != std::string::npos, resp);
        close(fd);
    }

    {
        std::vector<bronx::ipban::Ip> trusted(1);
        TEST_CHECK(bronx::ipban::parseCidr("127.0.0.1", trusted[0]));
        auto trustedChain = std::make_shared<MwChain>();
        trustedChain->use(MakeClientAddrMiddleware(trusted));
        trustedChain->use(MakeRouterMiddleware(router, upReg0));
        trustedChain->use(MakeProxyMiddleware(2000, 5000, trusted));
        auto trustedGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        trustedGw->setRequestHandler(makeChainHandler(trustedChain));
        uint16_t port = GW_PORT + 24;
        trustedGw->bind(bronx::BxAddress::LookupAny(
            "127.0.0.1:" + std::to_string(port)));
        trustedGw->start();
        usleep(100 * 1000);

        int fd = connect_fd(port);
        std::string wire =
            "GET /api/ip HTTP/1.1\r\nHost: x\r\n"
            "X-Forwarded-For: 198.51.100.8, 127.0.0.1\r\n"
            "X-Real-IP: 198.51.100.9\r\nX-Forwarded-Proto: https\r\n"
            "Connection: close\r\n\r\n";
        send_all_fd(fd, wire.data(), wire.size());
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find(":xff=198.51.100.8") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":real=198.51.100.8") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find(":proto=https") != std::string::npos, resp);
        close(fd);
        trustedGw->stop();
        usleep(100 * 1000);
    }

    TEST_CHECK_EQ(host_port("::1", 8080), std::string("[::1]:8080"));
    TEST_CHECK_EQ(host_port("[::1]", 8080), std::string("[::1]:8080"));

    // -- 测试 2a-2:rewrite_prefix 为 / 时不能生成双斜杠 --
    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "GET /rewrite-root/users HTTP/1.1\r\nHost: x\r\n"
                           "Connection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("UP:/users:") != std::string::npos,
                       "root rewrite should not produce //users: " << resp);
        TEST_CHECK_MSG(resp.find("UP://users:") == std::string::npos,
                       "root rewrite must avoid double slash: " << resp);
        close(fd);
    }

    // -- 测试 2b:chunked 请求体经网关转发到上游 --
    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "POST /api/chunk HTTP/1.1\r\nHost: x\r\n"
                           "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                           "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("UP:/chunk:Wikipedia") != std::string::npos,
                       "chunked body forwarded: " << resp);
        close(fd);
    }

    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "POST /api/chunk HTTP/1.1\r\nHost: x\r\n"
                           "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                           "0x5\r\nhello\r\n0\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find(" 400 ") != std::string::npos,
                       "bad chunk size should be 400: " << resp);
        close(fd);
    }

    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "GET /guard HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        auto cut = resp.find("\r\n\r\n");
        TEST_CHECK_MSG(cut != std::string::npos, resp);
        std::string body = cut == std::string::npos ? "" : resp.substr(cut + 4);
        TEST_CHECK_MSG(resp.find("X-Reply-Hop:") == std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("X-Forwarded-For:") == std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("Transfer-Encoding:") == std::string::npos, resp);
        TEST_CHECK_EQ(get_header_value(resp, "Content-Length"), std::to_string(body.size()));
        close(fd);
    }

    // -- 测试 2c:proxy 路径下 chunked body 超限应返回客户端错误,不是 502 --
    {
        up0Ep->cb->recordFailure();
        auto cb0 = up0Ep->cb->snapshot();
        uint64_t skip0 = up0Ep->stat.requests(UpMark::SKIP, UpWhy::BAD_BODY);
        GatewayOptions limited;
        limited.maxBodySize = 4;
        auto limitedGw = std::make_shared<GatewayServer>(limited, &iom, &iom);
        limitedGw->setRequestHandler(makeChainHandler(chain));
        uint16_t limitedPort = GW_PORT + 10;
        limitedGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(limitedPort)));
        limitedGw->start();
        usleep(150 * 1000);

        int fd = connect_fd(limitedPort);
        std::string wire = "POST /api/chunk HTTP/1.1\r\nHost: x\r\n"
                           "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                           "5\r\nhello\r\n0\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("413") != std::string::npos,
                       "proxy oversized chunked body should be 413: " << resp);
        auto cb1 = up0Ep->cb->snapshot();
        TEST_CHECK_EQ(cb1.requests, cb0.requests);
        TEST_CHECK_EQ(cb1.failures, cb0.failures);
        TEST_CHECK_EQ(up0Ep->stat.requests(UpMark::SKIP, UpWhy::BAD_BODY), skip0 + 1);
        up0Ep->cb->recordSuccess();
        close(fd);
        limitedGw->stop();
        usleep(150 * 1000);
    }

    // -- 测试 2d:上游 200 空 body 必须保留明确 Content-Length: 0 --
    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "GET /api/empty HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("200") != std::string::npos, "empty body status 200: " << resp);
        TEST_CHECK_MSG(resp.find("Content-Length: 0") != std::string::npos,
                       "empty body should keep Content-Length: 0: " << resp);
        close(fd);
    }
    // -- 测试 2d-2:HEAD 代理必须保留上游 Content-Length,但不向客户端发送 body --
    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "HEAD /api/head HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("200") != std::string::npos, "HEAD status 200: " << resp);
        TEST_CHECK_MSG(resp.find("Content-Length: 123") != std::string::npos,
                       "HEAD should preserve upstream Content-Length: " << resp);
        auto p = resp.find("\r\n\r\n");
        TEST_CHECK_MSG(p != std::string::npos && resp.substr(p + 4).empty(),
                       "HEAD should not forward a body: " << resp);
        close(fd);
    }

    // -- 测试 2e:上游 chunked 响应中途断开时,网关不能补 0 终止块伪装成完整响应 --
    {
        RawTruncatedChunkedUpstream rawUpstream;
        auto rawReg = std::make_shared<UpstreamRegistry>();
        auto rawGroup = std::make_shared<UpstreamGroup>("raw", MakeLoadBalancer("round_robin"));
        auto rawEp = std::make_shared<Endpoint>(
            "127.0.0.1", rawUpstream.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{});
        rawGroup->addEndpoint(rawEp);
        rawReg->add(rawGroup);

        auto rawRouter = std::make_shared<Router>();
        RouteRule rawRule;
        rawRule.name = "raw";
        rawRule.pathPattern = "/raw";
        rawRule.upstream = "raw";
        rawRouter->addRoute(rawRule);

        auto rawChain = std::make_shared<MwChain>();
        rawChain->use(MakeRouterMiddleware(rawRouter, rawReg));
        rawChain->use(MakeProxyMiddleware(1000, 2000));

        uint16_t rawGwPort = GW_PORT + 11;
        auto rawGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        rawGw->setRequestHandler(makeChainHandler(rawChain));
        rawGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(rawGwPort)));
        rawGw->start();
        usleep(150 * 1000);

        auto m0 = GatewayMetrics::instance().snapshot();
        int fd = connect_fd(rawGwPort);
        std::string wire = "GET /raw HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("200") != std::string::npos, "raw upstream status 200: " << resp);
        TEST_CHECK_MSG(resp.find("5\r\nhello\r\n") != std::string::npos,
                       "raw upstream partial chunk forwarded: " << resp);
        TEST_CHECK_MSG(resp.find("0\r\n\r\n") == std::string::npos,
                       "truncated upstream response must not be completed by gateway: " << resp);
        TEST_CHECK_MSG(resp.find("HTTP/1.1", 1) == std::string::npos,
                       "committed response must not get a second response: " << resp);
        auto m1 = GatewayMetrics::instance().snapshot();
        TEST_CHECK_EQ(m1.writeFail, m0.writeFail + 1);
        TEST_CHECK_EQ(rawEp->stat.requests(UpMark::FAIL, UpWhy::BAD_BODY), 1);
        TEST_CHECK_EQ(rawEp->stat.requests(UpMark::OK, UpWhy::NONE), 0);
        close(fd);
        rawGw->stop();
        usleep(150 * 1000);
    }

    {
        auto badResp = [&](const std::string& out, uint64_t waitMs,
                           uint64_t recvMs, uint16_t gwPort) {
            RawUp raw(out, waitMs);
            auto reg = std::make_shared<UpstreamRegistry>();
            auto group = std::make_shared<UpstreamGroup>("bad", MakeLoadBalancer("round_robin"));
            group->addEndpoint(std::make_shared<Endpoint>(
                "127.0.0.1", raw.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
            reg->add(group);
            auto router = std::make_shared<Router>();
            RouteRule rule;
            rule.name = "bad";
            rule.pathPattern = "/bad";
            rule.upstream = "bad";
            router->addRoute(rule);
            auto chain = std::make_shared<MwChain>();
            chain->use(MakeRouterMiddleware(router, reg));
            chain->use(MakeProxyMiddleware(1000, recvMs));
            auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
            server->setRequestHandler(makeChainHandler(chain));
            server->bind(bronx::BxAddress::LookupAny(
                "127.0.0.1:" + std::to_string(gwPort)));
            server->start();
            usleep(100 * 1000);
            int fd = connect_fd(gwPort);
            std::string req = "GET /bad HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            send_all_fd(fd, req.data(), req.size());
            std::string resp = recv_all(fd);
            close(fd);
            server->stop();
            usleep(100 * 1000);
            return resp;
        };

        std::string bad = badResp("bad response\r\n\r\n", 0, 1000, GW_PORT + 17);
        TEST_CHECK_MSG(bad.find(" 502 ") != std::string::npos, bad);
        TEST_CHECK_MSG(bad.find("HTTP/1.1", 1) == std::string::npos, bad);

        std::string late = badResp("", 300, 100, GW_PORT + 18);
        TEST_CHECK_MSG(late.find(" 504 ") != std::string::npos, late);
        TEST_CHECK_MSG(late.find("HTTP/1.1", 1) == std::string::npos, late);

        std::string switched = badResp(
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: test\r\n"
            "Connection: Upgrade\r\n\r\n", 0, 1000, GW_PORT + 21);
        TEST_CHECK_MSG(switched.find(" 502 ") != std::string::npos, switched);

        std::string hints;
        for(int i = 0; i < 9; ++i) hints += "HTTP/1.1 103 Early Hints\r\n\r\n";
        hints += "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        std::string many = badResp(hints, 0, 1000, GW_PORT + 25);
        TEST_CHECK_MSG(many.find(" 502 ") != std::string::npos, many);

        std::string huge = "HTTP/1.1 200 OK\r\nX-Big: " + std::string(64 * 1024, 'x')
                         + "\r\nContent-Length: 0\r\n\r\n";
        std::string tooBig = badResp(huge, 0, 1000, GW_PORT + 26);
        TEST_CHECK_MSG(tooBig.find(" 502 ") != std::string::npos, tooBig);

        std::string hop = badResp(
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
            "Connection: close, X-Reply-Hop\r\nX-Reply-Hop: should-strip\r\n"
            "Proxy-Connection: should-strip\r\nX-Forwarded-For: should-strip\r\n\r\nok",
            0, 1000, GW_PORT + 28);
        TEST_CHECK_MSG(hop.find(" 200 ") != std::string::npos, hop);
        TEST_CHECK_MSG(hop.find("X-Reply-Hop:") == std::string::npos, hop);
        TEST_CHECK_MSG(hop.find("Proxy-Connection:") == std::string::npos, hop);
        TEST_CHECK_MSG(hop.find("X-Forwarded-For:") == std::string::npos, hop);
        TEST_CHECK_MSG(hop.find("\r\n\r\nok") != std::string::npos, hop);
    }

    {
        auto governed = [&](const std::string& wire, uint64_t waitMs,
                            CircuitBreakerConfig cb, uint64_t totalMs,
                            uint16_t port, uint64_t* cost) {
            RawUp raw(wire, waitMs);
            auto reg = std::make_shared<UpstreamRegistry>();
            auto group = std::make_shared<UpstreamGroup>(
                "governed", MakeLoadBalancer("round_robin"));
            if(totalMs) group->setTimeouts(totalMs, 1000, 1000);
            auto ep = std::make_shared<Endpoint>(
                "127.0.0.1", raw.port, 1, cb, ConnPoolConfig{});
            group->addEndpoint(ep);
            reg->add(group);
            auto router = std::make_shared<Router>();
            RouteRule rule;
            rule.name = "governed";
            rule.pathPattern = "/governed";
            rule.upstream = "governed";
            router->addRoute(rule);
            auto chain = std::make_shared<MwChain>();
            chain->use(MakeRouterMiddleware(router, reg));
            chain->use(MakeProxyMiddleware(1000, 1000));
            auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
            server->setRequestHandler(makeChainHandler(chain));
            server->bind(bronx::BxAddress::LookupAny(
                "127.0.0.1:" + std::to_string(port)));
            server->start();
            usleep(100 * 1000);
            auto start = std::chrono::steady_clock::now();
            int fd = connect_fd(port);
            std::string req = "GET /governed HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            send_all_fd(fd, req.data(), req.size());
            std::string resp = recv_all(fd);
            close(fd);
            if(cost) {
                *cost = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
            }
            server->stop();
            usleep(100 * 1000);
            std::istringstream line(resp);
            std::string http;
            int status = 0;
            line >> http >> status;
            return std::make_pair(status, ep);
        };

        CircuitBreakerConfig cb;
        cb.failureThreshold = 1;
        cb.failureRate = 0;
        cb.slowRate = 0;
        auto bad = governed(
            "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            0, cb, 0, GW_PORT + 29, nullptr);
        TEST_CHECK_EQ(bad.first, 503);
        TEST_CHECK(bad.second->cb->state() == CircuitBreaker::State::OPEN);
        TEST_CHECK_EQ(bad.second->stat.requests(UpMark::FAIL, UpWhy::STATUS), 1);

        auto ok4 = governed(
            "HTTP/1.1 404 Missing\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            0, cb, 0, GW_PORT + 30, nullptr);
        TEST_CHECK_EQ(ok4.first, 404);
        TEST_CHECK(ok4.second->cb->state() == CircuitBreaker::State::CLOSED);
        TEST_CHECK_EQ(ok4.second->stat.requests(UpMark::OK, UpWhy::NONE), 1);

        uint64_t cost = 0;
        auto late = governed("", 300, cb, 50, GW_PORT + 31, &cost);
        TEST_CHECK_EQ(late.first, 504);
        TEST_CHECK_MSG(cost < 250, "total deadline cost=" << cost);
        TEST_CHECK_EQ(late.second->stat.requests(UpMark::FAIL, UpWhy::TIMEOUT), 1);
        TEST_CHECK_EQ(late.second->activeConns.load(), 0);
    }

    {
        RawLateBody raw(150);
        auto reg = std::make_shared<UpstreamRegistry>();
        auto group = std::make_shared<UpstreamGroup>(
            "sse", MakeLoadBalancer("round_robin"));
        group->setTimeouts(50, 1000, 500);
        auto ep = std::make_shared<Endpoint>(
            "127.0.0.1", raw.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{});
        group->addEndpoint(ep);
        reg->add(group);
        auto router = std::make_shared<Router>();
        RouteRule rule;
        rule.name = "sse";
        rule.pathPattern = "/sse";
        rule.upstream = "sse";
        router->addRoute(rule);
        auto chain = std::make_shared<MwChain>();
        chain->use(MakeRouterMiddleware(router, reg));
        chain->use(MakeProxyMiddleware(1000, 500));
        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler(makeChainHandler(chain));
        uint16_t port = GW_PORT + 32;
        server->bind(bronx::BxAddress::LookupAny(
            "127.0.0.1:" + std::to_string(port)));
        server->start();
        usleep(100 * 1000);
        int fd = connect_fd(port);
        std::string req = "GET /sse HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send_all_fd(fd, req.data(), req.size());
        std::string resp = recv_all(fd);
        close(fd);
        TEST_CHECK_MSG(resp.find(" 200 ") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("\r\n\r\nok") != std::string::npos, resp);
        TEST_CHECK_EQ(ep->stat.requests(UpMark::OK, UpWhy::NONE), 1);
        TEST_CHECK_EQ(ep->activeConns.load(), 0);
        server->stop();
        usleep(100 * 1000);
    }

    {
        RawRetryUp raw;
        auto reg = std::make_shared<UpstreamRegistry>();
        auto group = std::make_shared<UpstreamGroup>("retry", MakeLoadBalancer("round_robin"));
        ConnPoolConfig pool;
        pool.maxIdle = 2;
        pool.idleTimeoutMs = 5000;
        group->addEndpoint(std::make_shared<Endpoint>(
            "127.0.0.1", raw.port, 1, CircuitBreakerConfig{}, pool));
        reg->add(group);
        auto router = std::make_shared<Router>();
        RouteRule rule;
        rule.name = "retry";
        rule.pathPattern = "/retry";
        rule.upstream = "retry";
        router->addRoute(rule);
        auto chain = std::make_shared<MwChain>();
        chain->use(MakeRouterMiddleware(router, reg));
        chain->use(MakeProxyMiddleware(1000, 2000));
        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler(makeChainHandler(chain));
        uint16_t port = GW_PORT + 27;
        server->bind(bronx::BxAddress::LookupAny(
            "127.0.0.1:" + std::to_string(port)));
        server->start();
        usleep(100 * 1000);

        int first = connect_fd(port);
        std::string get = "GET /retry HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send_all_fd(first, get.data(), get.size());
        std::string firstResp = recv_all(first);
        TEST_CHECK_MSG(firstResp.find(" 200 ") != std::string::npos, firstResp);
        close(first);

        int second = connect_fd(port);
        std::string post =
            "POST /retry HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send_all_fd(second, post.data(), post.size());
        std::string secondResp = recv_all(second);
        TEST_CHECK_MSG(secondResp.find(" 502 ") != std::string::npos, secondResp);
        close(second);
        for(int i = 0; i < 200 && !raw.done.load(); ++i) usleep(5 * 1000);
        TEST_CHECK_EQ(raw.posts.load(), 1);
        server->stop();
        usleep(100 * 1000);
    }

    {
        RawBody raw;
        auto reg = std::make_shared<UpstreamRegistry>();
        auto group = std::make_shared<UpstreamGroup>("body", MakeLoadBalancer("round_robin"));
        group->addEndpoint(std::make_shared<Endpoint>(
            "127.0.0.1", raw.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
        reg->add(group);
        auto router = std::make_shared<Router>();
        RouteRule rule;
        rule.name = "body";
        rule.pathPattern = "/body";
        rule.upstream = "body";
        router->addRoute(rule);
        auto chain = std::make_shared<MwChain>();
        chain->use(MakeRouterMiddleware(router, reg));
        chain->use(MakeProxyMiddleware(1000, 2000));
        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler(makeChainHandler(chain));
        uint16_t gwPort = GW_PORT + 22;
        server->bind(bronx::BxAddress::LookupAny(
            "127.0.0.1:" + std::to_string(gwPort)));
        server->start();
        usleep(100 * 1000);

        auto m0 = GatewayMetrics::instance().snapshot();
        int fd = connect_fd(gwPort);
        std::string req = "GET /body HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send_all_fd(fd, req.data(), req.size());
        char buf[4096];
        TEST_CHECK_MSG(recv(fd, buf, sizeof(buf), 0) > 0, "client got response bytes");
        linger rst{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
        close(fd);
        for(int i = 0; i < 400; ++i) {
            if(GatewayMetrics::instance().snapshot().writeFail > m0.writeFail) break;
            usleep(5 * 1000);
        }
        auto m1 = GatewayMetrics::instance().snapshot();
        TEST_CHECK_EQ(m1.writeFail, m0.writeFail + 1);
        server->stop();
        usleep(100 * 1000);
    }

    // -- 测试 2e-2:上游 1xx informational 响应不能被当成最终响应,重复 Set-Cookie 要保留 --
    {
        RawInformationalUpstream infoUpstream;
        auto infoReg = std::make_shared<UpstreamRegistry>();
        auto infoGroup = std::make_shared<UpstreamGroup>("info", MakeLoadBalancer("round_robin"));
        infoGroup->addEndpoint(std::make_shared<Endpoint>(
            "127.0.0.1", infoUpstream.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
        infoReg->add(infoGroup);

        auto infoRouter = std::make_shared<Router>();
        RouteRule infoRule;
        infoRule.name = "info";
        infoRule.pathPattern = "/info";
        infoRule.upstream = "info";
        infoRouter->addRoute(infoRule);

        auto infoChain = std::make_shared<MwChain>();
        infoChain->use(MakeRouterMiddleware(infoRouter, infoReg));
        infoChain->use(MakeProxyMiddleware(1000, 2000));

        uint16_t infoGwPort = GW_PORT + 16;
        auto infoGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        infoGw->setRequestHandler(makeChainHandler(infoChain));
        infoGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(infoGwPort)));
        infoGw->start();
        usleep(150 * 1000);

        int fd = connect_fd(infoGwPort);
        std::string wire = "GET /info HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("HTTP/1.1 200 OK") != std::string::npos,
                       "gateway should forward final 200, not 1xx: " << resp);
        TEST_CHECK_MSG(resp.find("HTTP/1.1 100 Continue") == std::string::npos,
                       "informational response should not be sent as final response: " << resp);
        TEST_CHECK_MSG(resp.find("Set-Cookie: a=1") != std::string::npos,
                       "first Set-Cookie preserved: " << resp);
        TEST_CHECK_MSG(resp.find("Set-Cookie: b=2") != std::string::npos,
                       "second Set-Cookie preserved: " << resp);
        TEST_CHECK_MSG(resp.find("final-response") != std::string::npos,
                       "final body forwarded: " << resp);
        close(fd);
        infoGw->stop();
        usleep(150 * 1000);
    }

    // -- 测试 2f:主动健康检查标记 unhealthy 后,代理选择必须跳过该 endpoint --
    {
        RawHealthUpstream bad(false);
        RawHealthUpstream good(true);
        auto hcReg = std::make_shared<UpstreamRegistry>();
        auto hcGroup = std::make_shared<UpstreamGroup>("hc", MakeLoadBalancer("round_robin"));
        hcGroup->addEndpoint(std::make_shared<Endpoint>(
            "127.0.0.1", bad.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
        hcGroup->addEndpoint(std::make_shared<Endpoint>(
            "127.0.0.1", good.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
        HealthCheckConfig hc;
        hc.enabled = true;
        hc.path = "/healthz";
        hc.timeoutMs = 1000;
        hc.unhealthyThreshold = 1;
        hc.healthyThreshold = 1;
        hcGroup->setHealthCheck(hc);
        hcGroup->runHealthCheckOnce();
        auto eps = hcGroup->endpoints();
        TEST_CHECK_MSG(!eps[0]->healthy.load(), "bad endpoint should be unhealthy");
        TEST_CHECK_MSG(eps[1]->healthy.load(), "good endpoint should stay healthy");
        hcReg->add(hcGroup);

        auto hcRouter = std::make_shared<Router>();
        RouteRule hcRule;
        hcRule.name = "hc";
        hcRule.pathPattern = "/hc";
        hcRule.upstream = "hc";
        hcRouter->addRoute(hcRule);

        auto hcChain = std::make_shared<MwChain>();
        hcChain->use(MakeRouterMiddleware(hcRouter, hcReg));
        hcChain->use(MakeProxyMiddleware(1000, 2000));

        uint16_t hcGwPort = GW_PORT + 12;
        auto hcGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        hcGw->setRequestHandler(makeChainHandler(hcChain));
        hcGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(hcGwPort)));
        hcGw->start();
        usleep(150 * 1000);

        int fd = connect_fd(hcGwPort);
        std::string wire = "GET /hc HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("200") != std::string::npos, "health-filtered proxy 200: " << resp);
        TEST_CHECK_MSG(resp.find("healthy") != std::string::npos, "healthy endpoint response: " << resp);
        TEST_CHECK_EQ(bad.hits, 0);
        TEST_CHECK_MSG(good.hits >= 1, "healthy endpoint should handle proxy traffic");
        close(fd);
        hcGw->stop();
        usleep(150 * 1000);
    }

    // -- 测试 2g:WebSocket Upgrade 走路由改写后建立透明隧道,101 后帧双向透传 --
    {
        bronx::BxIoManager wsIom(8, "ws_test");
        RawWsEchoUpstream wsUpstream;
        auto wsReg = std::make_shared<UpstreamRegistry>();
        auto wsGroup = std::make_shared<UpstreamGroup>("ws", MakeLoadBalancer("least_conn"));
        wsGroup->addEndpoint(std::make_shared<Endpoint>(
            "127.0.0.1", wsUpstream.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
        wsReg->add(wsGroup);

        auto wsRouter = std::make_shared<Router>();
        RouteRule wsRule;
        wsRule.name = "ws";
        wsRule.pathPattern = "/ws";
        wsRule.upstream = "ws";
        wsRule.stripPrefix = true;
        wsRule.rewritePrefix = "/socket";
        wsRule.reqHeaderSet["X-WS-Marker"] = "via-gateway";
        wsRule.reqHeaderSet["Connection"] = "X-Rule-Hop";
        wsRule.reqHeaderSet["X-Rule-Hop"] = "drop-me";
        wsRule.reqHeaderSet["Trailer"] = "X-Route-Sum";
        wsRouter->addRoute(wsRule);
        RouteRule authWsRule = wsRule;
        authWsRule.name = "auth-ws";
        authWsRule.pathPattern = "/authws";
        authWsRule.authPolicy = "jwt";
        wsRouter->addRoute(authWsRule);

        auto wsChain = std::make_shared<MwChain>();
        wsChain->use(MakeRequestIdMiddleware());
        wsChain->use(MakeStructuredAccessLogMiddleware({}));
        wsChain->use(MakeSecurityHeadersMiddleware());
        wsChain->use(MakeRouterMiddleware(wsRouter, wsReg));
        JwtAuthConfig wsJwtCfg; wsJwtCfg.enabled = true; wsJwtCfg.secret = "proxy-secret";
        RouteAuthConfig wsAuthCfg; wsAuthCfg.fwdUser = true;
        wsChain->use(MakeRouteAuthMiddleware(
            std::make_shared<JwtAuthenticator>(wsJwtCfg), wsAuthCfg));
        wsChain->use(MakeWebSocketTunnelStub());
        wsChain->use(MakeProxyMiddleware(1000, 2000));

        uint16_t wsGwPort = GW_PORT + 13;
        auto wsGw = std::make_shared<GatewayServer>(GatewayOptions(), &wsIom, &wsIom);
        wsGw->setRequestHandler(makeChainHandler(wsChain));
        wsGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(wsGwPort)));
        wsGw->start();
        usleep(150 * 1000);

        auto ws0 = GatewayMetrics::instance().snapshot();
        std::string got;
        std::string wsDetail;
        std::string wsHead;
        TEST_CHECK_MSG(ws_roundtrip(wsGwPort, "/ws/chat?room=1", "hello-ws", got,
                                    &wsDetail, false, &wsHead),
                       "websocket roundtrip should complete: " << wsDetail
                       << " sessions=" << wsUpstream.sessions.load()
                       << " frames=" << wsUpstream.frames.load());
        TEST_CHECK_EQ(got, std::string("hello-ws"));
        TEST_CHECK_MSG(wsHead.find("X-Request-Id:") != std::string::npos, wsHead);
        TEST_CHECK_MSG(wsHead.find("Content-Security-Policy:") != std::string::npos, wsHead);
        got.clear();
        wsDetail.clear();
        TEST_CHECK_MSG(ws_roundtrip(wsGwPort, "/ws/chat?room=2", "coalesced-ws",
                                    got, &wsDetail, true),
                       "websocket coalesced handshake+frame should complete: " << wsDetail
                       << " sessions=" << wsUpstream.sessions.load()
                       << " frames=" << wsUpstream.frames.load());
        TEST_CHECK_EQ(got, std::string("coalesced-ws"));
        got.clear();
        wsDetail.clear();
        std::string authHeaders = "Authorization: Bearer " + auth_jwt()
            + "\r\nX-User-Id: fake\r\n";
        TEST_CHECK_MSG(ws_roundtrip(wsGwPort, "/authws/chat", "auth-ws", got,
                                    &wsDetail, false, nullptr, authHeaders), wsDetail);
        TEST_CHECK_EQ(got, std::string("auth-ws"));
        {
            std::lock_guard<std::mutex> lk(wsUpstream.mtx);
            TEST_CHECK_EQ(wsUpstream.lastPath, std::string("/socket/chat"));
            TEST_CHECK_EQ(wsUpstream.lastMarker, std::string("via-gateway"));
            TEST_CHECK_EQ(wsUpstream.lastHopPrivate, std::string(""));
            TEST_CHECK_EQ(wsUpstream.lastTrailer, std::string(""));
            TEST_CHECK_EQ(wsUpstream.lastProxy, std::string(""));
            TEST_CHECK_EQ(wsUpstream.lastRuleHop, std::string(""));
            TEST_CHECK_EQ(wsUpstream.lastAuth, std::string(""));
            TEST_CHECK_EQ(wsUpstream.lastUser, std::string("proxy-user"));
        }

        constexpr int CONNS = 8;
        constexpr int FRAMES = 10;

        std::atomic<int> ok{0};
        std::mutex errMtx;
        std::string errDetail;
        std::vector<std::thread> clients;
        for(int i = 0; i < CONNS; ++i) {
            clients.emplace_back([&, i]() {
                std::vector<std::string> payloads;
                payloads.reserve(FRAMES);
                for(int j = 0; j < FRAMES; ++j) {
                    payloads.push_back("ws-" + std::to_string(i) + "-" + std::to_string(j));
                }
                std::string detail;
                if(ws_multi_roundtrip(wsGwPort, "/ws/stress", payloads, &detail)) {
                    ok.fetch_add(FRAMES);
                } else {
                    std::lock_guard<std::mutex> lk(errMtx);
                    if(errDetail.empty()) {
                        std::ostringstream os;
                        os << "client=" << i << " detail=" << detail;
                        errDetail = os.str();
                    }
                }
            });
        }
        for(auto& t : clients) {
            if(t.joinable()) t.join();
        }
        TEST_CHECK_MSG(ok.load() == CONNS * FRAMES,
                       "websocket pressure frames ok=" << ok.load()
                       << " expected=" << (CONNS * FRAMES)
                       << " detail=" << errDetail);
        TEST_CHECK_MSG(wsUpstream.sessions.load() >= CONNS + 2,
                       "all websocket sessions reached upstream");
        TEST_CHECK_MSG(wsUpstream.frames.load() >= CONNS * FRAMES + 2,
                       "all websocket frames reached upstream");
        for(int i = 0; i < 200; ++i) {
            if(GatewayMetrics::instance().snapshot().wsActive == ws0.wsActive) break;
            usleep(5 * 1000);
        }
        auto ws1 = GatewayMetrics::instance().snapshot();
        TEST_CHECK_MSG(ws1.wsOpen - ws0.wsOpen >= CONNS + 2, "ws open count");
        TEST_CHECK_EQ(ws1.wsClose - ws0.wsClose, ws1.wsOpen - ws0.wsOpen);
        TEST_CHECK_EQ(ws1.wsActive, ws0.wsActive);

        wsGw->stop();
        usleep(150 * 1000);

        {
            auto idleGw = start_ws_gw(wsIom, wsUpstream.port, GW_PORT + 19, 100);
            std::string detail;
            TEST_CHECK_MSG(ws_wait_close(GW_PORT + 19, "/ws/idle", &detail), detail);
            idleGw->stop();
            usleep(100 * 1000);
        }

        {
            RawWsHandshakeUpstream closeUp(true, true);
            auto closeGw = start_ws_gw(wsIom, closeUp.port, GW_PORT + 20, 1000);
            std::string detail;
            TEST_CHECK_MSG(ws_wait_close(GW_PORT + 20, "/ws/close", &detail), detail);
            closeGw->stop();
            usleep(100 * 1000);
        }

        {
            RawWsHandshakeUpstream flood(true, true, 16 * 1024 * 1024);
            auto floodGw = start_ws_gw(wsIom, flood.port, GW_PORT + 23, 2000);
            auto m0 = GatewayMetrics::instance().snapshot();
            TEST_CHECK(ws_drop(GW_PORT + 23, "/ws/drop"));
            for(int i = 0; i < 500; ++i) {
                if(GatewayMetrics::instance().snapshot().writeFail > m0.writeFail) break;
                usleep(5 * 1000);
            }
            auto m1 = GatewayMetrics::instance().snapshot();
            TEST_CHECK_EQ(m1.writeFail, m0.writeFail + 1);
            floodGw->stop();
            usleep(100 * 1000);
        }

        {
            RawWsHandshakeUpstream badNoConnection(false, true);
            auto badReg = std::make_shared<UpstreamRegistry>();
            auto badGroup = std::make_shared<UpstreamGroup>("ws_bad", MakeLoadBalancer("round_robin"));
            badGroup->addEndpoint(std::make_shared<Endpoint>(
                "127.0.0.1", badNoConnection.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
            badReg->add(badGroup);

            auto badRouter = std::make_shared<Router>();
            RouteRule badRule;
            badRule.name = "ws_bad";
            badRule.pathPattern = "/bad";
            badRule.upstream = "ws_bad";
            badRouter->addRoute(badRule);

            auto badChain = std::make_shared<MwChain>();
            badChain->use(MakeRouterMiddleware(badRouter, badReg));
            badChain->use(MakeWebSocketTunnelStub());
            badChain->use(MakeProxyMiddleware(1000, 2000));

            uint16_t badGwPort = GW_PORT + 14;
            auto badGw = std::make_shared<GatewayServer>(GatewayOptions(), &wsIom, &wsIom);
            badGw->setRequestHandler(makeChainHandler(badChain));
            badGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(badGwPort)));
            badGw->start();
            usleep(150 * 1000);

            std::string badDetail;
            TEST_CHECK_MSG(ws_expect_status(badGwPort, "/bad", 502, &badDetail),
                           "websocket upstream 101 without Connection: Upgrade should be rejected: "
                           << badDetail);
            badGw->stop();
            usleep(150 * 1000);
        }

        {
            RawWsHandshakeUpstream badAccept(true, false);
            auto badReg = std::make_shared<UpstreamRegistry>();
            auto badGroup = std::make_shared<UpstreamGroup>("ws_bad_accept", MakeLoadBalancer("round_robin"));
            badGroup->addEndpoint(std::make_shared<Endpoint>(
                "127.0.0.1", badAccept.port, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
            badReg->add(badGroup);

            auto badRouter = std::make_shared<Router>();
            RouteRule badRule;
            badRule.name = "ws_bad_accept";
            badRule.pathPattern = "/bad-accept";
            badRule.upstream = "ws_bad_accept";
            badRouter->addRoute(badRule);

            auto badChain = std::make_shared<MwChain>();
            badChain->use(MakeRouterMiddleware(badRouter, badReg));
            badChain->use(MakeWebSocketTunnelStub());
            badChain->use(MakeProxyMiddleware(1000, 2000));

            uint16_t badGwPort = GW_PORT + 15;
            auto badGw = std::make_shared<GatewayServer>(GatewayOptions(), &wsIom, &wsIom);
            badGw->setRequestHandler(makeChainHandler(badChain));
            badGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(badGwPort)));
            badGw->start();
            usleep(150 * 1000);

            std::string badDetail;
            TEST_CHECK_MSG(ws_expect_status(badGwPort, "/bad-accept", 502, &badDetail),
                           "websocket upstream 101 with bad Sec-WebSocket-Accept should be rejected: "
                           << badDetail);
            badGw->stop();
            usleep(150 * 1000);
        }

        {
            auto missReg = std::make_shared<UpstreamRegistry>();
            auto missGroup = std::make_shared<UpstreamGroup>(
                "ws_miss", MakeLoadBalancer("round_robin"));
            auto missEp = std::make_shared<Endpoint>(
                "127.0.0.1", 19998, 1, CircuitBreakerConfig{}, ConnPoolConfig{});
            missGroup->addEndpoint(missEp);
            missReg->add(missGroup);

            auto missRouter = std::make_shared<Router>();
            RouteRule missRule;
            missRule.name = "ws_miss";
            missRule.pathPattern = "/miss";
            missRule.upstream = "ws_miss";
            missRouter->addRoute(missRule);

            auto missChain = std::make_shared<MwChain>();
            missChain->use(MakeRouterMiddleware(missRouter, missReg));
            missChain->use(MakeWebSocketTunnelStub());

            uint16_t missGwPort = GW_PORT + 16;
            auto missGw = std::make_shared<GatewayServer>(GatewayOptions(), &wsIom, &wsIom);
            missGw->setRequestHandler(makeChainHandler(missChain));
            missGw->bind(bronx::BxAddress::LookupAny(
                "127.0.0.1:" + std::to_string(missGwPort)));
            missGw->start();
            usleep(150 * 1000);

            auto m0 = GatewayMetrics::instance().snapshot();
            std::string missDetail;
            TEST_CHECK_MSG(ws_expect_status(missGwPort, "/miss", 502, &missDetail), missDetail);
            auto m1 = GatewayMetrics::instance().snapshot();
            TEST_CHECK_EQ(m1.upstreamAcquireFail, m0.upstreamAcquireFail + 1);
            TEST_CHECK_EQ(m1.upstreamFail, m0.upstreamFail + 1);
            TEST_CHECK_EQ(missEp->stat.requests(UpMark::FAIL, UpWhy::CONNECT), 1);
            TEST_CHECK_EQ(missEp->activeConns.load(), 0);

            missGw->stop();
            usleep(150 * 1000);
        }
    }

    // -- 测试 3:未命中路由 → 404 短路 --
    {
        int fd = connect_fd(GW_PORT);
        std::string wire = "GET /nomatch HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("404") != std::string::npos, "should 404: " << resp.substr(0,30));
        close(fd);
    }

    // -- 测试 4:上游不可达 → 502 --
    {
        auto router2 = std::make_shared<Router>();
        auto upReg1 = std::make_shared<UpstreamRegistry>();
    { auto ep=std::make_shared<Endpoint>("127.0.0.1",19999,1,CircuitBreakerConfig{},ConnPoolConfig{});auto grp=std::make_shared<UpstreamGroup>("up1",MakeLoadBalancer("round_robin"));grp->addEndpoint(ep);upReg1->add(grp); }
    RouteRule bad; bad.name="up1"; bad.pathPattern="/dead"; bad.upstream="up1"; bad.stripPrefix=false;
    // 没人监听
        router2->addRoute(bad);
        auto chain2 = std::make_shared<MwChain>();
        chain2->use(MakeRouterMiddleware(router2, upReg1));
        chain2->use(MakeProxyMiddleware(1000, 2000));
        gw->setRequestHandler(makeChainHandler(chain2));

        int fd = connect_fd(GW_PORT);
        std::string wire = "GET /dead/x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(fd, wire.data(), wire.size(), 0);
        std::string resp = recv_all(fd);
        TEST_CHECK_MSG(resp.find("502") != std::string::npos, "should 502: " << resp.substr(0,30));
        close(fd);
    }

    gw->stop();
    upstream->stop();
    usleep(200 * 1000);
    return TEST_SUMMARY();
}
