#include "app.h"
#include "async_logger.h"
#include "config.h"
#include "cpu_pool.h"
#include "log.h"
#include "sync.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

static std::mutex g_eventsMutex;
static std::vector<std::string> g_events;
static auto g_lifecycleConfig = bronx::BxConfig::Lookup<std::string>(
    "lifecycle.marker", "", "lifecycle load marker");

static void event(const char* name) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    g_events.emplace_back(name);
}

class LifecycleApp : public bronx::BxApplication {
public:
    explicit LifecycleApp(std::string marker) : marker_(std::move(marker)) {}

    int stopBeginCalls() const { return stopBeginCalls_; }
    bool endSawNoPool() const { return endSawNoPool_; }

protected:
    bool onInit() override {
        auto pool = bronx::BxCpuPool::GetDefaultPtr();
        if(!pool || pool->threadCount() != 1 || g_lifecycleConfig->getValue() != marker_) {
            return false;
        }
        event("cpu pool");
        event("onInit");
        return true;
    }

    bool onStart() override {
        event("onStart");
        auto pool = bronx::BxCpuPool::GetDefaultPtr();
        if(!pool || !pool->trySubmit([this] { taskDone_.notify(); })) return false;
        taskDone_.wait();
        stop();
        stop();
        return true;
    }

    void onStopBegin() override {
        ++stopBeginCalls_;
        event("onStopBegin");
    }

    void onStopEnd() override {
        endSawNoPool_ = bronx::BxCpuPool::GetDefaultPtr() == nullptr;
        event("cpu pool drain");
        event("onStopEnd");
        BRONX_LOG_INFO(BRONX_LOG_ROOT()) << marker_;
    }

private:
    std::string marker_;
    bronx::BxSemaphore taskDone_{0};
    int stopBeginCalls_{0};
    bool endSawNoPool_{false};
};

static bool hasMarker(const fs::path& path, const std::string& marker) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find(marker) != std::string::npos;
}

int main() {
    const fs::path dir = fs::temp_directory_path()
        / ("bronx_app_lifecycle_" + std::to_string(getpid()));
    fs::create_directories(dir);
    const fs::path cfg = dir / "bronx.yml";
    const fs::path log = dir / "lifecycle.log";
    const std::string marker = "lifecycle-async-flush";
    {
        std::ofstream out(cfg);
        out << "lifecycle:\n"
            << "  marker: " << marker << "\n"
            << "cpu_pool:\n"
            << "  threads: 1\n"
            << "  max_queue: 4\n"
            << "  name: lifecycle\n"
            << "logs:\n"
            << "  - name: root\n"
            << "    level: info\n"
            << "    appenders:\n"
            << "      - type: BxFileLogAppender\n"
            << "        file: " << log << "\n"
            << "        async: true\n";
    }

    g_lifecycleConfig->addListener([](const std::string&, const std::string&) {
        event("load config");
    });
    std::string path = cfg.string();
    char arg0[] = "test_app_lifecycle";
    char arg1[] = "-c";
    char* argv[] = {arg0, arg1, path.data(), nullptr};
    LifecycleApp app(marker);
    bool ok = app.init(3, argv) && app.run() == 0
        && app.stopBeginCalls() == 1 && app.endSawNoPool()
        && bronx::BxCpuPool::GetDefaultPtr() == nullptr
        && hasMarker(log, marker);

    const std::vector<std::string> expected{
        "load config", "cpu pool", "onInit", "onStart", "onStopBegin",
        "cpu pool drain", "onStopEnd"};
    {
        std::lock_guard<std::mutex> lock(g_eventsMutex);
        ok = ok && g_events == expected;
        if(ok) g_events.emplace_back("async log shutdown");
        ok = ok && g_events.back() == "async log shutdown";
    }

    fs::remove_all(dir);
    if(!ok) {
        std::cerr << "application lifecycle boundary failed\n";
        return 1;
    }
    return 0;
}
