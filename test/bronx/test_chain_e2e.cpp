// 全链路集成测试
// ===============
// 验证常规路径:BxConfig 加载 YAML → Log → BxIoManager 起线程 → BxTcpServer bind/accept
// → onConnection recv/send(走 hook 协程 IO)→ 真实 client 往返 → 干净 stop。
// 这是"整条链路常规可用"的运行证据(非阅读)。

#include "test_util.h"
#include "config.h"
#include "log.h"
#include "reactor.h"
#include "tcp_listener.h"
#include "endpoint.h"
#include <yaml-cpp/yaml.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <atomic>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

// echo 服务器:读到什么回什么,直到对端关闭
class EchoServer : public bronx::BxTcpServer {
public:
    using bronx::BxTcpServer::BxTcpServer;
protected:
    void onConnection(bronx::BxSocket::ptr client) override {
        char buf[256];
        while(true){
            int rt = client->recv(buf, sizeof(buf));
            if(rt <= 0) break;
            int sent = 0;
            while(sent < rt){
                int w = client->send(buf + sent, rt - sent);
                if(w <= 0) goto done;
                sent += w;
            }
        }
        done:
        client->close();
    }
};

// 阻塞客户端:连接 → 发 → 收
static bool client_roundtrip(uint16_t port, const std::string& msg, std::string& reply) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if(connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0){ close(fd); return false; }
    if(send(fd, msg.data(), msg.size(), 0) != (ssize_t)msg.size()){ close(fd); return false; }
    char buf[256];
    int n = recv(fd, buf, sizeof(buf), 0);
    if(n > 0) reply.assign(buf, n);
    close(fd);
    return n > 0;
}

// 1. BxConfig 加载 YAML(常规小写 key)+ 类型转换正确
static void test_config_load() {
    auto cv = bronx::BxConfig::Lookup<int>("test.intval", 0, "test int");
    auto cs = bronx::BxConfig::Lookup<std::string>("test.strval", "", "test str");
    YAML::Node root = YAML::Load("test:\n  intval: 42\n  strval: hello\n");
    bronx::BxConfig::LoadFromYaml(root);
    TEST_CHECK_EQ(cv->getValue(), 42);
    TEST_CHECK_MSG(cs->getValue() == "hello", "strval got: " << cs->getValue());
}

// 2. BxConfig 变更回调被触发
static void test_config_callback() {
    auto cv = bronx::BxConfig::Lookup<int>("test.cbval", 1, "test cb");
    std::atomic<int> fired{0}, newv{0};
    cv->addListener([&](const int& ov, const int& nv){ fired++; newv = nv; });
    YAML::Node root = YAML::Load("test:\n  cbval: 99\n");
    bronx::BxConfig::LoadFromYaml(root);
    TEST_CHECK_EQ(fired.load(), 1);
    TEST_CHECK_EQ(newv.load(), 99);
}

// 2b. BxConfig 大小写 key:含大写的 key 不应导致整个文件被丢弃(回归 ListAllMember bug)
static void test_config_case_insensitive() {
    auto cv = bronx::BxConfig::Lookup<int>("test.mixedcase", 0, "test case");
    auto cv2 = bronx::BxConfig::Lookup<int>("test.sibling", 0, "test sibling");
    // YAML 用大写 key,且同级有另一项 —— 修复前整个 test 树会被 throw 丢弃
    YAML::Node root = YAML::Load("Test:\n  MixedCase: 7\n  Sibling: 8\n");
    bronx::BxConfig::LoadFromYaml(root);
    TEST_CHECK_MSG(cv->getValue() == 7, "mixedcase got " << cv->getValue() << " (bug: file dropped?)");
    TEST_CHECK_MSG(cv2->getValue() == 8, "sibling got " << cv2->getValue());
}

// 3. 全链路:BxIoManager + BxTcpServer + 真实 client 往返
static void test_full_chain() {
    const uint16_t PORT = 18500;
    bronx::BxIoManager iom(2);
    EchoServer::ptr server(new EchoServer(&iom, &iom));
    auto addr = bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT));
    TEST_CHECK_MSG(addr != nullptr, "resolve addr failed");
    TEST_CHECK_MSG(server->bind(addr), "bind failed");
    server->start();
    usleep(150 * 1000);

    // 3 次往返,验证 accept→onConnection→recv/send hook 链
    for(int i = 0; i < 3; ++i){
        std::string msg = "ping-" + std::to_string(i);
        std::string reply;
        bool ok = client_roundtrip(PORT, msg, reply);
        TEST_CHECK_MSG(ok, "roundtrip " << i << " failed");
        TEST_CHECK_MSG(reply == msg, "echo mismatch: sent '" << msg << "' got '" << reply << "'");
    }

    // 统计正确
    auto st = server->getStats();
    TEST_CHECK_MSG(st.acceptedTotal >= 3, "acceptedTotal=" << st.acceptedTotal);

    // 干净 stop
    server->stop();
    usleep(200 * 1000);
    TEST_CHECK_EQ((int)server->getActiveConnectionCount(), 0);
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_chain_e2e start ===";
    test_config_load();
    test_config_callback();
    test_config_case_insensitive();
    test_full_chain();
    return TEST_SUMMARY();
}
