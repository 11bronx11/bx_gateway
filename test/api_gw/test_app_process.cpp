#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Logs {
    fs::path root;
    fs::path system;
    fs::path gw;
    fs::path hub;
};

struct Child {
    pid_t pid{-1};
    int output{-1};
    std::string text;
};

static void fail(const std::string& message) {
    std::cerr << "test_api_gw_process: " << message << "\n";
}

static int remainingMs(Clock::time_point end) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
    return remaining <= 0 ? 0 : static_cast<int>(remaining);
}

static bool drainOutput(Child& child, int timeoutMs) {
    if(child.output < 0) return false;
    pollfd fd{child.output, POLLIN | POLLHUP, 0};
    if(::poll(&fd, 1, timeoutMs) < 0) return errno == EINTR;
    if(fd.revents == 0) return true;
    char buffer[4096];
    while(true) {
        ssize_t n = ::read(child.output, buffer, sizeof(buffer));
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

static bool waitForText(Child& child, const std::string& needle, int timeoutMs = 5000) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while(child.text.find(needle) == std::string::npos && remainingMs(end) > 0) {
        if(!drainOutput(child, remainingMs(end))) return false;
    }
    return child.text.find(needle) != std::string::npos;
}

static bool waitForTextAfter(Child& child, const std::string& needle, size_t offset,
                             int timeoutMs = 5000) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while(child.text.find(needle, offset) == std::string::npos && remainingMs(end) > 0) {
        if(!drainOutput(child, remainingMs(end))) return false;
    }
    return child.text.find(needle, offset) != std::string::npos;
}

static bool waitForExit(Child& child, int timeoutMs = 8000) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    int status = 0;
    while(remainingMs(end) > 0) {
        pid_t got = ::waitpid(child.pid, &status, WNOHANG);
        if(got == child.pid) {
            drainOutput(child, 0);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if(got < 0 && errno != EINTR) return false;
        if(!drainOutput(child, remainingMs(end))) return false;
    }
    return false;
}

static bool stop(Child& child) {
    if(child.pid <= 0) return false;
    if(::kill(child.pid, SIGTERM) != 0) return false;
    if(waitForExit(child)) {
        child.pid = -1;
        return true;
    }
    ::kill(child.pid, SIGKILL);
    int status = 0;
    (void)::waitpid(child.pid, &status, 0);
    child.pid = -1;
    return false;
}

static Child launch(const fs::path& exe, const fs::path& cwd, const fs::path& bronxConfig = {}) {
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
        const std::string exeString = exe.string();
        if(bronxConfig.empty()) {
            ::execl(exeString.c_str(), exeString.c_str(), static_cast<char*>(nullptr));
        } else {
            const std::string configString = bronxConfig.string();
            ::execl(exeString.c_str(), exeString.c_str(), "-c", configString.c_str(),
                    static_cast<char*>(nullptr));
        }
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

static uint16_t freePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
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
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

static int connectPort(uint16_t port, int timeoutMs = 2000) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if(fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    pollfd pollfd{fd, POLLOUT, 0};
    if(::poll(&pollfd, 1, timeoutMs) <= 0) {
        ::close(fd);
        return -1;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if(::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static bool canConnect(uint16_t port) {
    int fd = connectPort(port);
    if(fd < 0) return false;
    ::close(fd);
    return true;
}

static bool canBind(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return false;
    int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

static bool sendAll(int fd, const std::string& request, int timeoutMs = 2000) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    size_t off = 0;
    while(off < request.size() && remainingMs(end) > 0) {
        ssize_t n = ::send(fd, request.data() + off, request.size() - off, MSG_NOSIGNAL);
        if(n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if(n < 0 && errno == EINTR) continue;
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pollfd{fd, POLLOUT, 0};
            if(::poll(&pollfd, 1, remainingMs(end)) > 0) continue;
        }
        return false;
    }
    return off == request.size();
}

static std::string http(uint16_t port, const std::string& method, const std::string& path) {
    int fd = connectPort(port);
    if(fd < 0) return {};
    const std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\nContent-Length: 0\r\n\r\n";
    if(!sendAll(fd, request)) {
        ::close(fd);
        return {};
    }
    const auto end = Clock::now() + std::chrono::milliseconds(2000);
    std::string response;
    char buffer[4096];
    while(remainingMs(end) > 0) {
        pollfd pollfd{fd, POLLIN | POLLHUP, 0};
        if(::poll(&pollfd, 1, remainingMs(end)) <= 0) break;
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
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

static bool isSocket(const fs::path& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

static bool fileContains(const fs::path& path, const std::string& needle) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find(needle) != std::string::npos;
}

static Logs writeBronxConfig(const fs::path& path, const fs::path& logDir,
                             const std::string& tag) {
    fs::create_directories(logDir);
    Logs logs{logDir / (tag + "-root.log"), logDir / (tag + "-system.log"),
              logDir / (tag + "-gw.log"), logDir / (tag + "-hub.log")};
    std::ofstream out(path, std::ios::trunc);
    out << "cpu_pool:\n"
        << "  threads: 1\n"
        << "  max_queue: 16\n"
        << "  name: process-test\n"
        << "fiber:\n"
        << "  stack_size: 131072\n"
        << "  stack_pool_size: 2\n"
        << "  guard_page: 1\n"
        << "tcp:\n"
        << "  connect:\n"
        << "    timeout: 5000\n"
        << "tcp_server:\n"
        << "  recv_timeout: 30000\n"
        << "logs:\n";
    for(const auto& logger : {std::pair{"root", logs.root}, std::pair{"system", logs.system},
                              std::pair{"gw", logs.gw}, std::pair{"hub", logs.hub}}) {
        out << "  - name: " << logger.first << "\n"
            << "    level: info\n"
            << "    appenders:\n"
            << "      - type: BxStdoutLogAppender\n"
            << "        async: false\n"
            << "      - type: BxFileLogAppender\n"
            << "        file: " << logger.second << "\n"
            << "        async: true\n";
    }
    if(!out.good()) return {};
    return logs;
}

static bool writeGatewayConfig(const fs::path& path, uint16_t business, uint16_t admin,
                               const std::string& route) {
    std::ofstream out(path, std::ios::trunc);
    out << "server:\n"
        << "  address: 127.0.0.1:" << business << "\n"
        << "  admin_address: 127.0.0.1:" << admin << "\n"
        << "  io_workers: 1\n"
        << "upstreams:\n"
        << "  - name: blackhole\n"
        << "    lb: round_robin\n"
        << "    endpoints:\n"
        << "      - host: 127.0.0.1\n"
        << "        port: 9\n"
        << "routes:\n"
        << "  - name: " << route << "\n"
        << "    path: /" << route << "\n"
        << "    upstream: blackhole\n";
    return out.good();
}

static bool writeHubConfig(const fs::path& path, uint16_t http,
                           const fs::path& submit, const fs::path& subscribe,
                           const fs::path& admin, const fs::path& db = {}) {
    std::ofstream out(path, std::ios::trunc);
    out << "server:\n"
        << "  submit_sock: " << submit << "\n"
        << "  subscribe_sock: " << subscribe << "\n"
        << "  admin_sock: " << admin << "\n"
        << "  http_address: 127.0.0.1:" << http << "\n"
        << "  db_path: '" << db.string() << "'\n"
        << "  iom_workers: 1\n";
    return out.good();
}

static fs::path appBinary(const char* name) {
    return fs::path(API_GW_APP_BIN_DIR) / name;
}

static fs::path anchoredBinary(const fs::path& source, const fs::path& runtime) {
    const fs::path target = runtime / "bin" / source.filename();
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    if(error) return {};
    fs::create_hard_link(source, target, error);
    if(!error) return target;
    error.clear();
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, error);
    return error ? fs::path{} : target;
}

static bool testGateway(const fs::path& gw, const fs::path& runtime) {
    const fs::path appDir = runtime / "api_gw/bin";
    const fs::path framework = appDir / "bronx.yml";
    const fs::path gateway = appDir / "gateway.yml";
    const fs::path logsDir = runtime / "logs";
    fs::create_directories(appDir);
    const uint16_t business = freePort();
    const uint16_t admin = freePort();
    const uint16_t changedBusiness = freePort();
    const uint16_t changedAdmin = freePort();
    if(!business || !admin || !changedBusiness || !changedAdmin
       || !writeGatewayConfig(gateway, business, admin, "before")) {
        return false;
    }
    const Logs first = writeBronxConfig(framework, logsDir, "gw-one");
    if(first.root.empty()) return false;

    const fs::path exe = anchoredBinary(gw, runtime);
    Child child = launch(exe, runtime, framework);
    bool ok = true;
    auto check = [&ok](bool value, const char* what) {
        if(!value) fail(std::string("gateway: ") + what);
        ok = ok && value;
    };
    check(child.pid > 0, "launch");
    check(waitForText(child, "up biz="), "startup marker");
    check(canConnect(business), "business listener");
    check(http(admin, "GET", "/healthz").find("200") != std::string::npos, "admin listener");
    check(http(admin, "GET", "/routes").find("\"name\":\"before\"") != std::string::npos,
          "initial route");

    const Logs second = writeBronxConfig(framework, logsDir, "gw-two");
    check(second.gw != first.gw, "second framework config");
    check(::kill(child.pid, SIGHUP) == 0, "framework SIGHUP");
    check(waitForText(child, "framework config reloaded"), "framework reload marker");

    check(writeGatewayConfig(gateway, changedBusiness, changedAdmin, "after"), "rewrite business YAML");
    const size_t beforeBusinessHup = child.text.size();
    check(::kill(child.pid, SIGHUP) == 0, "business SIGHUP");
    check(waitForTextAfter(child, "framework config reloaded", beforeBusinessHup),
          "business SIGHUP framework marker");
    check(canConnect(business), "business listener unchanged after SIGHUP");
    check(!canConnect(changedBusiness), "changed business listener absent after SIGHUP");
    check(http(admin, "GET", "/routes").find("\"name\":\"before\"") != std::string::npos,
          "old route after SIGHUP");
    check(http(admin, "GET", "/routes").find("\"name\":\"after\"") == std::string::npos,
          "new route absent after SIGHUP");

    check(http(admin, "POST", "/reload").find("200") != std::string::npos, "admin POST /reload");
    check(http(admin, "GET", "/routes").find("\"name\":\"after\"") != std::string::npos,
          "new route after admin reload");

    const bool stopped = stop(child);
    check(stopped, "TERM exit");
    check(canBind(business) && canBind(admin), "ports rebind after TERM");
    check(fileContains(first.system, "server bind success"), "system log landing");
    check(fileContains(first.gw, "up biz="), "gw async log flush");
    check(fileContains(second.root, "BxApplication stopping"), "root log landing");
    check(fileContains(second.gw, "framework config reloaded"), "reloaded gw log landing");
    if(!ok) {
        fail("gateway process boundary\n" + child.text);
    }
    return ok;
}

static bool testHub(const fs::path& hub, const fs::path& runtime) {
    const fs::path appDir = runtime / "api_gw/bin";
    const fs::path framework = appDir / "bronx.yml";
    const fs::path hubConfig = appDir / "hub.yml";
    const fs::path logsDir = runtime / "logs";
    fs::create_directories(appDir);
    const uint16_t httpPort = freePort();
    const uint16_t changedHttp = freePort();
    const fs::path submit = runtime / "submit.sock";
    const fs::path subscribe = runtime / "subscribe.sock";
    const fs::path admin = runtime / "admin.sock";
    const fs::path changedSubmit = runtime / "changed-submit.sock";
    const fs::path changedSubscribe = runtime / "changed-subscribe.sock";
    const fs::path changedAdmin = runtime / "changed-admin.sock";
    if(!httpPort || !changedHttp || !writeHubConfig(hubConfig, httpPort, submit, subscribe, admin)) {
        return false;
    }
    const Logs first = writeBronxConfig(framework, logsDir, "hub-one");
    if(first.root.empty()) return false;

    const fs::path exe = anchoredBinary(hub, runtime);
    Child child = launch(exe, runtime, framework);
    bool ok = child.pid > 0 && waitForText(child, "hub up http=")
        && http(httpPort, "GET", "/healthz").find("200") != std::string::npos
        && isSocket(submit) && isSocket(subscribe) && isSocket(admin);

    const Logs second = writeBronxConfig(framework, logsDir, "hub-two");
    ok = ok && writeHubConfig(hubConfig, changedHttp, changedSubmit, changedSubscribe, changedAdmin)
        && ::kill(child.pid, SIGHUP) == 0
        && waitForText(child, "framework config reloaded")
        && http(httpPort, "GET", "/healthz").find("200") != std::string::npos
        && !canConnect(changedHttp) && isSocket(submit) && isSocket(subscribe) && isSocket(admin)
        && !isSocket(changedSubmit) && !isSocket(changedSubscribe) && !isSocket(changedAdmin);

    const bool stopped = stop(child);
    ok = ok && stopped && canBind(httpPort)
        && !fs::exists(submit) && !fs::exists(subscribe) && !fs::exists(admin)
        && fileContains(first.system, "server bind success")
        && fileContains(first.hub, "hub up http=")
        && fileContains(second.root, "BxApplication stopping")
        && fileContains(second.hub, "framework config reloaded");
    if(!ok) {
        fail("hub process boundary\n" + child.text);
    }
    return ok;
}

static bool testHubDefaultFramework(const fs::path& hub, const fs::path& runtime) {
    const fs::path appDir = runtime / "api_gw/bin";
    const fs::path hubConfig = appDir / "hub.yml";
    const fs::path logsDir = runtime / "logs";
    fs::create_directories(appDir);
    const uint16_t httpPort = freePort();
    const fs::path submit = runtime / "submit.sock";
    const fs::path subscribe = runtime / "subscribe.sock";
    const fs::path admin = runtime / "admin.sock";
    const fs::path db = runtime / "db/hub.db";
    fs::create_directories(db.parent_path());
    if(!httpPort || !writeHubConfig(hubConfig, httpPort, submit, subscribe, admin, db)) {
        return false;
    }
    const Logs logs = writeBronxConfig(appDir / "hub_bronx.yml", logsDir, "hub-default");
    if(logs.root.empty()) return false;

    const fs::path exe = anchoredBinary(hub, runtime);
    Child child = launch(exe, runtime);
    bool ok = child.pid > 0 && waitForText(child, "hub up http=")
        && http(httpPort, "GET", "/healthz").find("200") != std::string::npos
        && isSocket(submit) && isSocket(subscribe) && isSocket(admin)
        && fs::is_regular_file(db);

    const bool stopped = stop(child);
    ok = ok && stopped && canBind(httpPort)
        && !fs::exists(submit) && !fs::exists(subscribe) && !fs::exists(admin)
        && fileContains(logs.system, "server bind success")
        && fileContains(logs.hub, "hub up http=")
        && fileContains(logs.root, "BxApplication stopping");
    if(!ok) {
        fail("hub default framework boundary\n" + child.text);
    }
    return ok;
}

int main() {
    const fs::path gw = appBinary("gw");
    const fs::path hub = appBinary("hub");
    if(!fs::is_regular_file(gw) || !fs::is_regular_file(hub)) {
        fail("gw/hub binaries are not in the configured app bin directory");
        return 1;
    }
    const fs::path dir = fs::temp_directory_path()
        / ("bronx_api_gw_process_" + std::to_string(getpid()));
    fs::create_directories(dir);
    const bool gatewayOk = testGateway(gw, dir / "gateway");
    const bool hubDefaultOk = testHubDefaultFramework(hub, dir / "hub-default");
    const bool hubOk = testHub(hub, dir / "hub");
    const bool ok = gatewayOk && hubDefaultOk && hubOk;
    fs::remove_all(dir);
    return ok ? 0 : 1;
}
