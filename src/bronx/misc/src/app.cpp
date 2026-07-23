#include "app.h"
#include "config.h"
#include "log.h"
#include "async_logger.h"
#include "cpu_pool.h"
#include "env.h"

#include <csignal>
#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

namespace bronx {

static BxLogger::ptr g_logger = BRONX_LOG_ROOT();
static auto g_cpu_threads = BxConfig::Lookup<uint32_t>(
    "cpu_pool.threads", 0, "cpu pool worker threads");
static auto g_cpu_max_queue = BxConfig::Lookup<uint32_t>(
    "cpu_pool.max_queue", 0, "cpu pool maximum pending tasks");
static auto g_cpu_name = BxConfig::Lookup<std::string>(
    "cpu_pool.name", "cpu", "cpu pool thread name prefix");

BxApplication::BxApplication() = default;

// 析构:确保即使未正常 run/stop,也置位停机标志、唤醒可能挂起的主线程并回收信号线程。
BxApplication::~BxApplication() {
    stopping_.store(true);
    notifyStop();
    joinSignalThread();
    stopCpuPool();
}

bool BxApplication::startCpuPool() {
    try {
        BxCpuPool::BxConfig cfg;
        cfg.threads = g_cpu_threads->getValue();
        cfg.maxQueue = g_cpu_max_queue->getValue();
        cfg.name = g_cpu_name->getValue();
        cpuPool_ = std::make_shared<BxCpuPool>(std::move(cfg));
        BxCpuPool::SetDefault(cpuPool_);
        return true;
    } catch(const std::exception& e) {
        BRONX_LOG_ERROR(g_logger) << "cpu pool start failed: " << e.what();
    }
    return false;
}

void BxApplication::stopCpuPool() {
    if(!cpuPool_) return;
    cpuPool_->drain();
    BxCpuPool::SetDefault(nullptr);
    cpuPool_.reset();
}

// 启动初始化。顺序有讲究:blockSignals 得最先,再解析命令行拿配置路径,然后起日志、加载配置,最后 onInit。
bool BxApplication::init(int argc, char** argv) {
    // 进程级屏蔽停止信号必须最早执行:此后创建的所有线程(异步日志线程、
    // BxIoManager worker、信号线程)都继承此掩码,确保 SIGINT/SIGTERM/SIGHUP 只能
    // 由专用 signalLoop 的 sigtimedwait 接收。否则 init 阶段早起的异步日志线程未屏蔽,
    // 停止信号可能投递到它并以默认处置(终止)杀进程,绕过优雅停机。
    blockSignals();

    auto* env = EnvMgr::GetInstance();
    if (!env->init(argc, argv)) {
        BRONX_LOG_ERROR(g_logger) << "environment init failed";
        return false;
    }

    std::string conf;
    if (env->has("c")) {
        conf = env->get("c");
    } else if (env->has("config")) {
        conf = env->get("config");
    } else {
        conf = env->getEnv("BRONX_CONFIG");
    }
    if ((env->has("c") || env->has("config")) && conf.empty()) {
        BRONX_LOG_ERROR(g_logger) << "config option needs a path";
        return false;
    }
    if (!conf.empty()) {
        configPath_ = conf;
        configPathRequired_ = true;
    }
    auto cwd = env->getCwd();
    if (!cwd.empty()) {
        std::filesystem::path path(configPath_);
        if (path.is_relative()) {
            configPath_ = (std::filesystem::path(cwd) / path).lexically_normal().string();
        }
    }

    LoggerMgr::GetInstance()->init();
    if (!loadConfig(configPathRequired_)) return false;
    if (!startCpuPool()) return false;
    if (!onInit()) {
        stopCpuPool();
        return false;
    }
    return true;
}

// 运行阶段,返回即进程退出。忽略 SIGPIPE、起信号线程、onStart 拉起服务,然后主线程挂在 cv_ 上等停。
// 主线程不参与协程调度(没用 use_caller),就干等着,真正的 I/O 都在 BxIoManager worker 上跑。
int BxApplication::run() {
    signal(SIGPIPE, SIG_IGN);
    installSignals();

    // init 与 run 之间若已收到停止信号(stopping_ 被置位),直接退出,不再 onStart。
    if (stopping_.load()) {
        joinSignalThread();
        stopCpuPool();
        onStopEnd();
        BxAsyncLoggerMgr::GetInstance()->shutdown();
        return 0;
    }

    if (!onStart()) {
        bool expected = false;
        if(stopping_.compare_exchange_strong(expected, true)) {
            onStopBegin();
        }
        notifyStop();
        joinSignalThread();
        stopCpuPool();
        onStopEnd();
        BxAsyncLoggerMgr::GetInstance()->shutdown();
        return 1;
    }

    // main thread waits here until stop() is called
    {
        BRONX_LOG_INFO(g_logger) << "main block waiting for stop signal";
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]{ return stopping_.load(); });
    }

    joinSignalThread();
    BRONX_LOG_INFO(g_logger) << "main block stopped, exiting";
    stopCpuPool();
    onStopEnd();
    BxAsyncLoggerMgr::GetInstance()->shutdown();
    return 0;
}

// 触发优雅停机。可被信号线程或任意线程调用。
// 用 CAS 保证 onStop + 唤醒只执行一次(重复信号/析构并发调用都安全)。
void BxApplication::stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) return;

    BRONX_LOG_INFO(g_logger) << "BxApplication stopping";
    onStopBegin();
    notifyStop();
}

// 配置热加载。由 signalLoop 收到 SIGHUP 触发:重载 YAML 成功后回调 onReload。
void BxApplication::reload() {
    BRONX_LOG_INFO(g_logger) << "BxApplication reloading config";
    if (loadConfig(true)) {
        onReload();
    } else {
        BRONX_LOG_ERROR(g_logger) << "reload config failed";
    }
}

// 持锁 notify_all 唤醒 run() 中挂在 cv_ 上的主线程。
void BxApplication::notifyStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
}


// 加载配置到 BxConfig。configPath_ 既可是单个 YAML 文件,也可是目录(按名排序后逐个 LoadFromYaml)。
// required=true 时,路径缺失视为错误返回 false;否则仅告警。
bool BxApplication::loadConfig(bool required) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(configPath_);

    if (fs::is_regular_file(p, ec)) {
        try {
            BxConfig::LoadFromYaml(YAML::LoadFile(p.string()));
        } catch (const std::exception& e) {
            BRONX_LOG_ERROR(g_logger) << "loadConfig failed: " << e.what();
            return false;
        }
        return true;
    }

    if (!fs::is_directory(p, ec)) {
        if (required) {
            BRONX_LOG_ERROR(g_logger) << "config path not found: " << configPath_;
            return false;
        }
        BRONX_LOG_WARN(g_logger) << "config path not found: " << configPath_ << ", skipping";
        return true;
    }

    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(p, ec)) {
        auto ext = entry.path().extension().string();
        if (ext != ".yml" && ext != ".yaml") continue;
        files.push_back(entry.path());
    }
    if (ec) {
        BRONX_LOG_ERROR(g_logger) << "iterate config path failed: " << configPath_
                                  << ", error=" << ec.message();
        return false;
    }
    std::sort(files.begin(), files.end());

    std::vector<std::pair<fs::path, YAML::Node>> loaded;
    loaded.reserve(files.size());
    for (auto& file : files) {
        try {
            loaded.emplace_back(file, YAML::LoadFile(file.string()));
        } catch (const std::exception& e) {
            BRONX_LOG_ERROR(g_logger) << "failed to parse " << file << ": " << e.what();
            return false;
        }
    }

    for (auto& item : loaded) {
        try {
            BxConfig::LoadFromYaml(item.second);
            BRONX_LOG_INFO(g_logger) << "loaded config: " << item.first;
        } catch (const std::exception& e) {
            BRONX_LOG_ERROR(g_logger) << "failed to load " << item.first << ": " << e.what();
            return false;
        }
    }
    return true;
}

// 进程级屏蔽 SIGINT/SIGTERM/SIGHUP。在 init 最开头调用,子线程继承该掩码。
void BxApplication::blockSignals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

// 再次确认屏蔽掩码,然后创建专用信号线程运行 signalLoop。
void BxApplication::installSignals() {
    blockSignals();
    signalThread_ = std::make_shared<BxThread>([this]{ signalLoop(); }, "signal");
}

void BxApplication::joinSignalThread() {
    if (signalThread_) {
        signalThread_->join();
        signalThread_.reset();
    }
}

// 信号线程主体。sigtimedwait 同步收被屏蔽的信号,躲开异步 handler 的种种限制。
// 100ms 超时轮询是为了 stopping_ 一置位就能退出。SIGINT/SIGTERM 走 stop(),SIGHUP 走 reload()。
void BxApplication::signalLoop() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);

    while (!stopping_.load()) {
        timespec timeout{0, 100 * 1000 * 1000};
        int sig = sigtimedwait(&set, nullptr, &timeout);
        if (sig == -1) {
            if (errno == EAGAIN || errno == EINTR) continue;
            BRONX_LOG_ERROR(g_logger) << "sigtimedwait failed, errno=" << errno;
            continue;
        }

        if (sig == SIGINT || sig == SIGTERM) {
            BRONX_LOG_INFO(g_logger) << "received signal " << sig << ", stopping";
            stop();
            break;
        } else if (sig == SIGHUP) {
            BRONX_LOG_INFO(g_logger) << "received signal " << sig << ", reloading";
            reload();
        }
    }
}

} // namespace bronx
