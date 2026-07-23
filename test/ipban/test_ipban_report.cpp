// Reporter 测试: 有界队列 + 网络发送 + backoff 打断 + drain-on-stop
#include "test_util.h"
#include "report.h"
#include "daemon.h"
#include "metrics.h"
#include "reactor.h"
#include "util.h"
#include <unistd.h>
#include <string>

using namespace bronx::ipban;

static Risk mkRisk(const char* id, const char* ip = "1.2.3.4") {
    Risk r; r.id = id; parseCidr(ip, r.ip); r.src = Src::RATE;
    r.atMs = bronx::GetCurrentMs(); return r;
}

// 纯内存: 队列上限 + 冷却 + stop 后计数
static void testBounded() {
    ReportOpts o; o.maxQueue = 3;
    Reporter rep(o);
    auto start = bronx::gateway::GatewayMetrics::instance().snapshot();
    TEST_CHECK(rep.tryReport(mkRisk("a")));
    TEST_CHECK_MSG(!rep.tryReport(mkRisk("a")), "same risk should cool down");
    TEST_CHECK(rep.tryReport(mkRisk("b")));
    TEST_CHECK(rep.tryReport(mkRisk("c")));
    TEST_CHECK_EQ(rep.queueDepth(), 3u);
    TEST_CHECK_MSG(!rep.tryReport(mkRisk("d")), "队列满应丢弃");
    TEST_CHECK_EQ(rep.queueDepth(), 3u);
    auto before = bronx::gateway::GatewayMetrics::instance().snapshot();
    rep.stop();
    auto after = bronx::gateway::GatewayMetrics::instance().snapshot();
    TEST_CHECK_EQ(rep.queueDepth(), 0u);
    TEST_CHECK(after.riskStopDropped >= before.riskStopDropped + 3);
    TEST_CHECK_MSG(!rep.tryReport(mkRisk("e")), "停机后应拒绝新举报");
    after = bronx::gateway::GatewayMetrics::instance().snapshot();
    size_t rate = static_cast<size_t>(Src::RATE) * 6;
    TEST_CHECK_EQ(after.riskByResult[rate] - start.riskByResult[rate], 3u);
    TEST_CHECK_EQ(after.riskByResult[rate + 1] - start.riskByResult[rate + 1], 1u);
    TEST_CHECK_EQ(after.riskByResult[rate + 2] - start.riskByResult[rate + 2], 1u);
    TEST_CHECK_EQ(after.riskByResult[rate + 3] - start.riskByResult[rate + 3], 4u);
    TEST_CHECK_EQ(after.riskQueue, 0u);
}

// 网络发送: Reporter 连上 Daemon，举报到达 engine
static void testNetSend() {
    std::string base = "/tmp/bronx_rep_send_" + std::to_string(::getpid());
    DaemonOpts dopts;
    dopts.submitPath   = base + "_sub.sock";
    dopts.subscribePath = base + "_push.sock";
    dopts.expiryTickMs = 200;

    bronx::BxIoManager iom(1, "rnet");
    auto daemon = std::make_shared<Daemon>(&iom, dopts);
    iom.post([&]() { TEST_CHECK(daemon->start()); });
    usleep(100 * 1000);

    ReportOpts ropts;
    ropts.submitPath = dopts.submitPath;
    auto rep = std::make_shared<Reporter>(ropts);
    iom.post([rep]() { rep->start(bronx::BxIoManager::Current()); });
    usleep(100 * 1000);

    auto before = bronx::gateway::GatewayMetrics::instance().snapshot();
    TEST_CHECK(rep->tryReport(mkRisk("net-1", "10.0.0.1")));
    TEST_CHECK(rep->tryReport(mkRisk("net-2", "10.0.0.2")));

    // 等 daemon engine 收到
    uint64_t due = bronx::GetCurrentMs() + 3000;
    while(daemon->version() < 2 && bronx::GetCurrentMs() < due) usleep(50 * 1000);
    TEST_CHECK_MSG(daemon->version() >= 2, "举报应到达 engine");
    auto after = bronx::gateway::GatewayMetrics::instance().snapshot();
    size_t sent = static_cast<size_t>(Src::RATE) * 6 + 4;
    TEST_CHECK_EQ(after.riskByResult[sent] - before.riskByResult[sent], 2u);

    rep->stop();
    TEST_CHECK_EQ(rep->queueDepth(), 0u);
    daemon->stop();
    iom.stop();
    ::unlink(dopts.submitPath.c_str());
    ::unlink(dopts.subscribePath.c_str());
}

// backoff 打断: socket 不存在时 Reporter 进 backoff，stop() 应快速退出
static void testBackoffStop() {
    ReportOpts ropts;
    ropts.submitPath   = "/tmp/bronx_rep_noexist_" + std::to_string(::getpid()) + ".sock";
    ropts.minBackoffMs = 500;
    ropts.maxBackoffMs = 5000;
    ropts.stopTimeoutMs = 1000;

    bronx::BxIoManager iom(1, "rboff");
    auto rep = std::make_shared<Reporter>(ropts);
    iom.post([rep]() { rep->start(bronx::BxIoManager::Current()); });
    usleep(50 * 1000);

    // 给一个举报让它开始尝试连接
    auto before = bronx::gateway::GatewayMetrics::instance().snapshot();
    rep->tryReport(mkRisk("boff-1"));
    usleep(100 * 1000);  // 等进 backoff

    uint64_t t0 = bronx::GetCurrentMs();
    rep->stop();
    uint64_t elapsed = bronx::GetCurrentMs() - t0;
    // 应在 stopTimeoutMs + 几百ms 内返回，绝不卡够 maxBackoffMs
    TEST_CHECK_MSG(elapsed < ropts.stopTimeoutMs + 500,
                   "backoff 中 stop() 应快速打断");
    auto after = bronx::gateway::GatewayMetrics::instance().snapshot();
    TEST_CHECK(after.riskRetries > before.riskRetries);

    iom.stop();
}

// drain-on-stop: stop 时有队列，应在期限内排空发出去
static void testDrain() {
    std::string base = "/tmp/bronx_rep_drain_" + std::to_string(::getpid());
    DaemonOpts dopts;
    dopts.submitPath    = base + "_sub.sock";
    dopts.subscribePath = base + "_push.sock";
    dopts.expiryTickMs  = 200;

    bronx::BxIoManager iom(1, "rdrain");
    auto daemon = std::make_shared<Daemon>(&iom, dopts);
    iom.post([&]() { TEST_CHECK(daemon->start()); });
    usleep(100 * 1000);

    ReportOpts ropts;
    ropts.submitPath   = dopts.submitPath;
    ropts.stopTimeoutMs = 2000;
    auto rep = std::make_shared<Reporter>(ropts);
    iom.post([rep]() { rep->start(bronx::BxIoManager::Current()); });
    usleep(150 * 1000);  // 等连上

    // 队列 5 条
    for(int i = 0; i < 5; ++i) {
        char id[16]; snprintf(id, sizeof(id), "drain-%d", i);
        char ip[32]; snprintf(ip, sizeof(ip), "10.1.0.%d", i + 1);
        rep->tryReport(mkRisk(id, ip));
    }

    rep->stop();  // 应在 stopTimeoutMs 内排完
    TEST_CHECK_EQ(rep->queueDepth(), 0u);

    uint64_t due = bronx::GetCurrentMs() + 2000;
    while(daemon->version() < 5 && bronx::GetCurrentMs() < due) usleep(50 * 1000);
    TEST_CHECK_MSG(daemon->version() >= 5, "drain 后所有举报应到达 engine");

    daemon->stop();
    iom.stop();
    ::unlink(dopts.submitPath.c_str());
    ::unlink(dopts.subscribePath.c_str());
}

int main() {
    testBounded();
    testNetSend();
    testBackoffStop();
    testDrain();
    return TEST_SUMMARY();
}
