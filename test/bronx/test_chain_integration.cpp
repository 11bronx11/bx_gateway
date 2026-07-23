// 集成测试：静态初始化 -> Env -> BxApplication -> BxConfig(YAML) -> BxAsyncLogger 链路
//
// 这个测试不依赖真实的 conf/ 目录，所有 YAML 都在 /tmp/<pid>_bronx_chain_test/ 内
// 动态生成。每一阶段验证后立即检查不变式（若失败立刻 abort，错误码非零）。
//
// 覆盖：
//   1. BxLogManager 单例构造时 root 已可用（兜底 stdout）
//   2. Env::init / BxApplication::init 顺序、相对路径 cwd
//   3. BxConfig::LoadFromYaml 触发 LogIniter listener、root 被替换、async 生效
//   4. 配置热更新（reload）：新增/修改/删除 logger，level/appender/async 切换
//   5. 异步路径：BxFileLogAppender + async 真的批量落盘、shutdown 后无丢失
//   6. 自定义 BxConfigVar<vector<int>> 等容器配置同步生效
//   7. 错误 pattern 不致命；重复 LoadFromYaml 幂等

#include "app.h"
#include "async_logger.h"
#include "config.h"
#include "env.h"
#include "reactor.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ---- 简易断言 -------------------------------------------------------------
static int g_failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { ++g_failures; \
        std::cerr << "FAIL [" << __LINE__ << "] " << msg << "\n"; } \
    else { std::cout << "  ok [" << __LINE__ << "] " << msg << "\n"; } \
} while(0)
#define MUST(cond, msg) do { \
    if (!(cond)) { ++g_failures; \
        std::cerr << "FATAL [" << __LINE__ << "] " << msg << "\n"; \
        std::abort(); } \
    else { std::cout << "  ok [" << __LINE__ << "] " << msg << "\n"; } \
} while(0)

// ---- 工具 ----------------------------------------------------------------
static fs::path g_root;
static fs::path g_conf_dir;
static fs::path g_log_dir;

static std::string read_file(const fs::path& p) {
    std::ifstream ifs(p);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p);
    ofs << s;
}

static size_t count_lines_with(const fs::path& p, const std::string& needle) {
    auto s = read_file(p);
    size_t n = 0, pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// 在异步路径下，文件落盘需要等心跳/buffer 满/flush。我们调 flushAll 代替 sleep。
static void hard_flush() {
    bronx::BxAsyncLoggerMgr::GetInstance()->flushAll();
}

// ---- 自定义 BxConfigVar：用作 LoadFromYaml 影响业务配置的样例 ----------------
static auto g_pi_cfg   = bronx::BxConfig::Lookup<float>("extra.pi", 0.0f, "pi");
static auto g_name_cfg = bronx::BxConfig::Lookup<std::string>("extra.name", "", "name");
static auto g_tags_cfg = bronx::BxConfig::Lookup<std::vector<std::string>>(
    "extra.tags", std::vector<std::string>{}, "tags");

// 全局回调计数：用于校验 listener 真的被触发
static std::atomic<int> g_pi_cb_fired{0};
struct PiCbInstaller {
    PiCbInstaller() {
        g_pi_cfg->addListener([](const float& o, const float& n){
            ++g_pi_cb_fired;
            std::cout << "  [pi listener] " << o << " -> " << n << "\n";
        });
    }
};
static PiCbInstaller __pi_installer;

// =========================================================================
// 阶段 0：进入 main 之前 —— 验证静态初始化产物
// =========================================================================
static void stage0_before_main() {
    // BxLogManager 静态构造 + LogIniter::g_log_config listener 注册
    // 但还不应该有任何 user logger（除了构造函数里塞的 root）
    auto root = bronx::LoggerMgr::GetInstance()->getRoot();
    MUST(root != nullptr, "stage0: root logger exists");
    EXPECT(root->getName() == "root", "stage0: root name");
    EXPECT(root->getLevel() == bronx::BxLogLevel::DEBUG,
           "stage0: root default level == DEBUG");

    // root 自带 stdout appender，能立即写日志
    BRONX_LOG_INFO(root) << "stage0: pre-Env-init, fallback stdout root works";

    // BxConfigVar 已注册，默认值就位
    EXPECT(std::abs(g_pi_cfg->getValue() - 0.0f) < 1e-6f, "stage0: pi default 0");
    EXPECT(g_name_cfg->getValue().empty(), "stage0: name default empty");
    EXPECT(g_tags_cfg->getValue().empty(), "stage0: tags default empty");
    EXPECT(g_pi_cb_fired.load() == 0, "stage0: listener not fired yet");
}

// =========================================================================
// 阶段 1：基础同步配置（conf_a）
// =========================================================================
static void stage1_basic_sync() {
    fs::path file = g_conf_dir / "conf_a_basic.yml";
    auto sync_log = (g_log_dir / "sysA_sync.log").string();
    std::ostringstream y;
    y << "logs:\n"
      << "    - name: root\n"
      << "      level: info\n"
      << "      appenders:\n"
      << "          - type: BxStdoutLogAppender\n"
      << "            pattern: \"[BASIC-ROOT][%p] %c %T %m%n\"\n"
      << "    - name: sysA\n"
      << "      level: debug\n"
      << "      appenders:\n"
      << "          - type: BxStdoutLogAppender\n"
      << "          - type: BxFileLogAppender\n"
      << "            file: " << sync_log << "\n";
    write_file(file, y.str());

    bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string()));

    auto root = BRONX_LOG_ROOT();
    auto sysA = BRONX_LOG_NAME("sysA");

    EXPECT(root->getLevel() == bronx::BxLogLevel::INFO, "stage1: root level=INFO");
    EXPECT(sysA->getLevel() == bronx::BxLogLevel::DEBUG, "stage1: sysA level=DEBUG");

    BRONX_LOG_DEBUG(root) << "stage1-debug-root (should NOT appear; root=INFO)";
    BRONX_LOG_INFO(root)  << "stage1-info-root (visible)";
    BRONX_LOG_DEBUG(sysA) << "stage1-debug-sysA (visible)";
    BRONX_LOG_INFO(sysA)  << "stage1-info-sysA-into-file";

    // sync 模式下，写完即落盘；读文件直接验证
    EXPECT(fs::exists(sync_log), "stage1: sync file created");
    auto content = read_file(sync_log);
    EXPECT(content.find("stage1-debug-sysA") != std::string::npos,
           "stage1: sync file contains debug-sysA");
    EXPECT(content.find("stage1-info-sysA-into-file") != std::string::npos,
           "stage1: sync file contains info-sysA");
    EXPECT(content.find("stage1-debug-root") == std::string::npos,
           "stage1: sync file does NOT leak root debug (filtered)");
}

// =========================================================================
// 阶段 2：异步配置（conf_b）—— 重点
// =========================================================================
static void stage2_async() {
    fs::path file = g_conf_dir / "conf_b_async.yml";
    auto a_log   = (g_log_dir / "sysA_async.log").string();
    auto hot_log = (g_log_dir / "hot_async.log").string();
    std::ostringstream y;
    y << "logs:\n"
      << "    - name: root\n"
      << "      level: info\n"
      << "      appenders:\n"
      << "          - type: BxStdoutLogAppender\n"
      << "            pattern: \"[ASYNC-ROOT][%p] %c %T %m%n\"\n"
      << "            async: true\n"
      << "    - name: sysA\n"
      << "      level: debug\n"
      << "      appenders:\n"
      << "          - type: BxFileLogAppender\n"
      << "            file: " << a_log << "\n"
      << "            pattern: \"[ASYNC-A][%p] %c %T %m%n\"\n"
      << "            async: true\n"
      << "    - name: hot\n"
      << "      level: warn\n"
      << "      appenders:\n"
      << "          - type: BxFileLogAppender\n"
      << "            file: " << hot_log << "\n"
      << "            pattern: \"[ASYNC-HOT][%p] %m%n\"\n"
      << "            async: true\n";
    write_file(file, y.str());

    bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string()));

    auto sysA = BRONX_LOG_NAME("sysA");
    auto hot  = BRONX_LOG_NAME("hot");

    EXPECT(sysA->getLevel() == bronx::BxLogLevel::DEBUG, "stage2: sysA=DEBUG");
    EXPECT(hot->getLevel()  == bronx::BxLogLevel::WARN,  "stage2: hot=WARN");

    constexpr int N = 5000;
    for (int i = 0; i < N; ++i) {
        BRONX_LOG_INFO(sysA) << "stage2-async-A i=" << i;
        if (i % 3 == 0) BRONX_LOG_ERROR(hot) << "stage2-async-hot i=" << i;
        // 测试低于级别的日志被过滤
        if (i % 7 == 0) BRONX_LOG_INFO(hot) << "stage2-info-hot-FILTERED i=" << i;
    }

    // 必须经过 hard_flush 才能保证文件落盘
    hard_flush();

    EXPECT(fs::exists(a_log), "stage2: async A file exists");
    EXPECT(fs::exists(hot_log), "stage2: async hot file exists");

    auto a_content   = read_file(a_log);
    auto hot_content = read_file(hot_log);

    size_t a_count = count_lines_with(a_log, "stage2-async-A i=");
    size_t hot_err = count_lines_with(hot_log, "stage2-async-hot i=");
    size_t hot_info = count_lines_with(hot_log, "stage2-info-hot-FILTERED");

    EXPECT(a_count == (size_t)N,
           "stage2: ALL " + std::to_string(N) + " async A lines flushed (got "
           + std::to_string(a_count) + ")");
    size_t expected_hot = (N + 2) / 3; // i % 3 == 0
    EXPECT(hot_err == expected_hot,
           "stage2: hot ERROR lines = " + std::to_string(expected_hot)
           + " (got " + std::to_string(hot_err) + ")");
    EXPECT(hot_info == 0, "stage2: hot INFO filtered out (level=WARN)");

    EXPECT(a_content.find("[ASYNC-A]") != std::string::npos,
           "stage2: pattern [ASYNC-A] applied");
    EXPECT(hot_content.find("[ASYNC-HOT]") != std::string::npos,
           "stage2: pattern [ASYNC-HOT] applied");
}

// =========================================================================
// 阶段 3：异步路径多线程压力（共享 root + sysA）
// =========================================================================
static void stage3_concurrent() {
    auto sysA = BRONX_LOG_NAME("sysA");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 3000;

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t, sysA]{
            for (int i = 0; i < kPerThread; ++i) {
                BRONX_LOG_INFO(sysA) << "stage3-mt t=" << t << " i=" << i;
            }
        });
    }
    for (auto& th : ts) th.join();

    hard_flush();

    auto a_log = (g_log_dir / "sysA_async.log").string();
    size_t got = count_lines_with(a_log, "stage3-mt t=");
    size_t want = (size_t)kThreads * kPerThread;
    EXPECT(got == want,
           "stage3: concurrent async MT lines = " + std::to_string(want)
           + " (got " + std::to_string(got) + ")");
}

// =========================================================================
// 阶段 4：reload —— 修改 + 删除 + 新增 logger，自定义 BxConfigVar 同步生效
// =========================================================================
static void stage4_reload() {
    fs::path file = g_conf_dir / "conf_c_reload.yml";
    auto reload_log = (g_log_dir / "sysA_reload.log").string();
    std::ostringstream y;
    y << "logs:\n"
      << "    - name: root\n"
      << "      level: error\n"
      << "      appenders:\n"
      << "          - type: BxStdoutLogAppender\n"
      << "            pattern: \"[RELOAD-ROOT][%p] %m%n\"\n"
      << "    - name: sysA\n"
      << "      level: warn\n"
      << "      appenders:\n"
      << "          - type: BxFileLogAppender\n"
      << "            file: " << reload_log << "\n"
      << "            pattern: \"[RELOAD-A][%p] %m%n\"\n"
      << "            async: true\n"
      << "extra:\n"
      << "    pi: 3.1415\n"
      << "    name: \"bronx-it\"\n"
      << "    tags:\n"
      << "        - alpha\n"
      << "        - beta\n"
      << "        - gamma\n";
    write_file(file, y.str());

    int before = g_pi_cb_fired.load();
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string()));
    int after = g_pi_cb_fired.load();

    EXPECT(after == before + 1, "stage4: pi listener fired exactly once");
    EXPECT(std::abs(g_pi_cfg->getValue() - 3.1415f) < 1e-4f, "stage4: pi == 3.1415");
    EXPECT(g_name_cfg->getValue() == "bronx-it", "stage4: name == bronx-it");
    EXPECT(g_tags_cfg->getValue().size() == 3, "stage4: tags size 3");

    auto root = BRONX_LOG_ROOT();
    auto sysA = BRONX_LOG_NAME("sysA");
    auto hot  = BRONX_LOG_NAME("hot");

    EXPECT(root->getLevel() == bronx::BxLogLevel::ERROR, "stage4: root level=ERROR after reload");
    EXPECT(sysA->getLevel() == bronx::BxLogLevel::WARN,  "stage4: sysA level=WARN after reload");
    EXPECT(hot->getLevel()  == bronx::BxLogLevel::NOTSET,
           "stage4: hot logically deleted (level=NOTSET)");

    BRONX_LOG_INFO(root)  << "stage4-info-root-FILTERED (root=ERROR)";
    BRONX_LOG_ERROR(root) << "stage4-error-root-visible";
    BRONX_LOG_INFO(sysA)  << "stage4-info-sysA-FILTERED (sysA=WARN)";
    BRONX_LOG_WARN(sysA)  << "stage4-warn-sysA-visible";
    // hot 被逻辑删除：任何级别都应被吞掉
    BRONX_LOG_FATAL(hot)  << "stage4-fatal-hot-FILTERED (NOTSET)";

    hard_flush();

    EXPECT(fs::exists(reload_log), "stage4: reload async file exists");
    auto rc = read_file(reload_log);
    EXPECT(rc.find("stage4-warn-sysA-visible") != std::string::npos,
           "stage4: reload file has warn line");
    EXPECT(rc.find("stage4-info-sysA") == std::string::npos,
           "stage4: reload file does NOT have info-sysA");
    EXPECT(rc.find("[RELOAD-A]") != std::string::npos, "stage4: reload pattern applied");

    // 旧异步文件不应被覆盖（BxFileLogAppender 用 std::ios::app 重新打开）
    auto a_log = (g_log_dir / "sysA_async.log").string();
    auto old = read_file(a_log);
    EXPECT(old.find("stage2-async-A") != std::string::npos,
           "stage4: old async file preserved (append mode)");
}

// =========================================================================
// 阶段 5：错误 pattern + 同 yml 二次加载（幂等）
// =========================================================================
static void stage5_idempotent_and_badpattern() {
    fs::path file = g_conf_dir / "conf_d_bad.yml";
    std::ostringstream y;
    y << "logs:\n"
      << "    - name: badp\n"
      << "      level: info\n"
      << "      appenders:\n"
      << "          - type: BxStdoutLogAppender\n"
      << "            pattern: \"[BAD] %z %m%n\"\n";
    write_file(file, y.str());

    int before = g_pi_cb_fired.load();
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string()));
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string())); // 再来一遍

    auto badp = BRONX_LOG_NAME("badp");
    EXPECT(badp->getLevel() == bronx::BxLogLevel::INFO, "stage5: badp loaded");
    BRONX_LOG_INFO(badp) << "stage5-bad-pattern-survives";

    EXPECT(g_pi_cb_fired.load() == before,
           "stage5: pi listener NOT re-fired (extra unchanged)");

    // 加载相同 yml 多次应当幂等：sysA 等已有 logger 不应该重复添加 appender
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile((g_conf_dir / "conf_c_reload.yml").string()));
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile((g_conf_dir / "conf_c_reload.yml").string()));
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile((g_conf_dir / "conf_c_reload.yml").string()));
    auto sysA = BRONX_LOG_NAME("sysA");
    BRONX_LOG_WARN(sysA) << "stage5-after-3x-reload";
    hard_flush();

    auto reload_log = (g_log_dir / "sysA_reload.log").string();
    size_t n = count_lines_with(reload_log, "stage5-after-3x-reload");
    EXPECT(n == 1,
           "stage5: line written exactly once even after 3x identical reload (got "
           + std::to_string(n) + ")");
}

// =========================================================================
// 阶段 6：覆盖 extra（验证 vector<string> LexicalCast）
// =========================================================================
static void stage6_overwrite_extra() {
    fs::path file = g_conf_dir / "conf_e_extra.yml";
    write_file(file,
        "extra:\n"
        "    pi: 6.28\n"
        "    name: \"bronx-overwrite\"\n"
        "    tags:\n"
        "        - x\n"
        "        - y\n");
    int before = g_pi_cb_fired.load();
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string()));
    int after = g_pi_cb_fired.load();
    EXPECT(after == before + 1, "stage6: pi listener fires on change");
    EXPECT(std::abs(g_pi_cfg->getValue() - 6.28f) < 1e-4f, "stage6: pi=6.28");
    EXPECT(g_name_cfg->getValue() == "bronx-overwrite", "stage6: name overwritten");
    EXPECT(g_tags_cfg->getValue().size() == 2, "stage6: tags shrunk to 2");
}

// =========================================================================
// 阶段 7：BxApplication::init via -c <dir>，应当顺序加载 conf 目录
// =========================================================================
static void stage7_application_init() {
    // 单独一个 app conf 目录，不让前面阶段的 logger 干扰
    fs::path app_dir = g_root / "app_conf";
    fs::create_directories(app_dir);
    auto app_log = (g_log_dir / "app_async.log").string();
    write_file(app_dir / "00-base.yml",
        "extra:\n"
        "    pi: 1.111\n"
        "    name: \"app-base\"\n");
    std::ostringstream y;
    y << "logs:\n"
      << "    - name: appx\n"
      << "      level: info\n"
      << "      appenders:\n"
      << "          - type: BxFileLogAppender\n"
      << "            file: " << app_log << "\n"
      << "            pattern: \"[APP][%p] %m%n\"\n"
      << "            async: true\n"
      << "extra:\n"
      << "    pi: 9.99\n"
      << "    name: \"app-final\"\n";
    write_file(app_dir / "10-app.yml", y.str());

    char arg0[] = "test_chain_init";
    char arg1[] = "-c";
    std::string ds = app_dir.string();
    std::vector<char> arg2(ds.begin(), ds.end()); arg2.push_back('\0');
    char* argv[] = { arg0, arg1, arg2.data(), nullptr };
    int argc = 3;

    // 用一个独立 BxApplication 实例
    bronx::BxApplication app;
    bool ok = app.init(argc, argv);
    EXPECT(ok, "stage7: BxApplication::init returned true");

    EXPECT(bronx::EnvMgr::GetInstance()->get("c") == ds, "stage7: Env captured -c");

    EXPECT(std::abs(g_pi_cfg->getValue() - 9.99f) < 1e-4f,
           "stage7: 10-app.yml overwrote 00-base.yml (sorted load)");
    EXPECT(g_name_cfg->getValue() == "app-final", "stage7: name=app-final");

    auto appx = BRONX_LOG_NAME("appx");
    EXPECT(appx->getLevel() == bronx::BxLogLevel::INFO, "stage7: appx loaded");
    BRONX_LOG_INFO(appx) << "stage7-via-application-init";

    hard_flush();
    EXPECT(fs::exists(app_log), "stage7: app async file exists");
    EXPECT(count_lines_with(app_log, "stage7-via-application-init") == 1,
           "stage7: app async line landed exactly once");
}

// =========================================================================
// 阶段 8：在异步文件正写入时切回同步 — 不应丢失已 enqueue 的日志
//   注意：stage 7 加载的 app_conf 里没有 sysA，listener 已把 sysA 逻辑删除
//   （setLevel(NOTSET)+clearAppenders）。stage 8 必须自己重新激活 sysA。
// =========================================================================
static void stage8_async_to_sync_switch() {
    // 先重新把 sysA 配为 async file，独立的 stage8 文件，避免依赖前面遗留
    fs::path pre_file = g_conf_dir / "conf_f_stage8_async.yml";
    auto pre_log = (g_log_dir / "stage8_async.log").string();
    {
        std::ostringstream y;
        y << "logs:\n"
          << "    - name: sysA\n"
          << "      level: warn\n"
          << "      appenders:\n"
          << "          - type: BxFileLogAppender\n"
          << "            file: " << pre_log << "\n"
          << "            pattern: \"[STAGE8-ASYNC][%p] %m%n\"\n"
          << "            async: true\n";
        write_file(pre_file, y.str());
        bronx::BxConfig::LoadFromYaml(YAML::LoadFile(pre_file.string()));
    }

    auto sysA = BRONX_LOG_NAME("sysA");
    EXPECT(sysA->getLevel() == bronx::BxLogLevel::WARN, "stage8: sysA reactivated WARN");

    constexpr int N = 2000;
    for (int i = 0; i < N; ++i) {
        BRONX_LOG_WARN(sysA) << "stage8-pre-switch i=" << i;
    }

    // 不调 hard_flush；直接换配置：sysA 改成 sync + 新文件
    fs::path file = g_conf_dir / "conf_f_switch.yml";
    auto switch_log = (g_log_dir / "stage8_switch.log").string();
    std::ostringstream y;
    y << "logs:\n"
      << "    - name: sysA\n"
      << "      level: info\n"
      << "      appenders:\n"
      << "          - type: BxFileLogAppender\n"
      << "            file: " << switch_log << "\n"
      << "            pattern: \"[STAGE8-SYNC][%p] %m%n\"\n";
    write_file(file, y.str());
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string()));

    // 切换后写若干同步日志
    for (int i = 0; i < 100; ++i) {
        BRONX_LOG_INFO(sysA) << "stage8-post-switch i=" << i;
    }
    hard_flush();

    size_t pre = count_lines_with(pre_log, "stage8-pre-switch");
    size_t post = count_lines_with(switch_log, "stage8-post-switch");
    EXPECT(pre == (size_t)N,
           "stage8: ALL " + std::to_string(N) + " pre-switch async lines preserved (got "
           + std::to_string(pre) + ")");
    EXPECT(post == 100, "stage8: post-switch sync lines = 100");
}

int main() {
    g_root     = fs::path("/tmp") / ("bronx_chain_test_" + std::to_string(::getpid()));
    g_conf_dir = g_root / "conf";
    g_log_dir  = g_root / "logs";
    fs::create_directories(g_conf_dir);
    fs::create_directories(g_log_dir);

    // 严苛模式：通过环境变量启用 —— 把 buffer 调小到 8，心跳放慢到 5s，
    // 让 swap/back-pressure/free-list 频繁触发，便于暴露并发竞争
    if (const char* h = std::getenv("BRONX_HARD")) {
        if (std::string(h) == "1") {
            bronx::BxAsyncLogger::kBufferCapacity = 8;
            bronx::BxAsyncLogger::kCentralCapacity = 32;  // 必须是 2 的幂
            bronx::BxAsyncLogger::kHeartbeatMs = 5000;
            std::cout << "[HARD MODE] buffer=8 central=32 heartbeat=5000ms\n";
        }
    }

    std::cout << "==== Bronx Chain Integration Test ====\n";
    std::cout << "root=" << g_root << "\n\n";

    std::cout << "-- stage 0: pre-main static state --\n";
    stage0_before_main();

    std::cout << "\n-- stage 1: basic sync via LoadFromYaml --\n";
    stage1_basic_sync();

    std::cout << "\n-- stage 2: async appenders --\n";
    stage2_async();

    std::cout << "\n-- stage 3: concurrent async (8 threads x 3000) --\n";
    stage3_concurrent();

    std::cout << "\n-- stage 4: reload (modify + delete + extra) --\n";
    stage4_reload();

    std::cout << "\n-- stage 5: bad pattern + idempotent reload --\n";
    stage5_idempotent_and_badpattern();

    std::cout << "\n-- stage 6: overwrite extra (vector<string>) --\n";
    stage6_overwrite_extra();

    std::cout << "\n-- stage 7: BxApplication::init -c <dir> --\n";
    stage7_application_init();

    std::cout << "\n-- stage 8: async->sync switch keeps history --\n";
    stage8_async_to_sync_switch();

    std::cout << "\n-- stage 9: concurrent reload during async write --\n";
    {
        // 重新激活 sysA 为 async，准备压力测试
        fs::path file = g_conf_dir / "conf_g_storm.yml";
        auto storm_log = (g_log_dir / "stage9_storm.log").string();
        std::ostringstream y;
        y << "logs:\n"
          << "    - name: sysA\n"
          << "      level: info\n"
          << "      appenders:\n"
          << "          - type: BxFileLogAppender\n"
          << "            file: " << storm_log << "\n"
          << "            pattern: \"[STORM][%p] %m%n\"\n"
          << "            async: true\n";
        write_file(file, y.str());
        bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string()));

        auto sysA = BRONX_LOG_NAME("sysA");
        std::atomic<bool> stop{false};
        std::atomic<long> writes{0};

        // 4 个 producer 线程一直写
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t) {
            ts.emplace_back([&, t]{
                long i = 0;
                while (!stop.load(std::memory_order_acquire)) {
                    BRONX_LOG_INFO(sysA) << "stage9-storm t=" << t << " i=" << i;
                    ++i;
                    writes.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        // 主线程触发 10 次 reload
        for (int r = 0; r < 10; ++r) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            // 反复在 level 上摆动以触发 listener
            std::ostringstream y2;
            auto level = (r % 2) ? "info" : "warn";
            y2 << "logs:\n"
               << "    - name: sysA\n"
               << "      level: " << level << "\n"
               << "      appenders:\n"
               << "          - type: BxFileLogAppender\n"
               << "            file: " << storm_log << "\n"
               << "            pattern: \"[STORM-R" << r << "][%p] %m%n\"\n"
               << "            async: true\n";
            write_file(file, y2.str());
            bronx::BxConfig::LoadFromYaml(YAML::LoadFile(file.string()));
        }
        stop.store(true, std::memory_order_release);
        for (auto& th : ts) th.join();
        hard_flush();

        long w = writes.load();
        std::cout << "  stage9: total writes attempted = " << w << "\n";
        EXPECT(w > 0, "stage9: writes happened during storm");
        // 不要求所有 write 都落盘（reload 期间 level 切换可能过滤一部分），但
        // 程序必须依然存活、最终一致、文件可读
        EXPECT(fs::exists(storm_log), "stage9: storm file exists");
        size_t got = count_lines_with(storm_log, "stage9-storm");
        EXPECT(got > 0 && got <= (size_t)w,
               "stage9: storm lines in [1, " + std::to_string(w) + "] (got "
               + std::to_string(got) + ")");
    }

    std::cout << "\n==== shutdown ====\n";
    bronx::BxAsyncLoggerMgr::GetInstance()->shutdown();

    std::cout << "\n==== summary ====\n";
    std::cout << "failures=" << g_failures << "\n";
    if (g_failures == 0) {
        std::cout << "ALL OK\n";
        // 清理临时目录
        std::error_code ec;
        fs::remove_all(g_root, ec);
        return 0;
    }
    std::cerr << "see " << g_root << " for artifacts\n";
    return 1;
}
