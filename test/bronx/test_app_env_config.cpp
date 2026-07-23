#include "app.h"
#include "config.h"
#include "cpu_pool.h"
#include "env.h"
#include "fiber.h"
#include "io_hook.h"
#include "log.h"
#include "tcp_listener.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

static auto g_source = bronx::BxConfig::Lookup<std::string>(
    "app_test.source", "", "application config source");

class ConfigApp : public bronx::BxApplication {
public:
    ConfigApp(std::string source, std::string fallback = {}, size_t threads = 1)
        : source_(std::move(source)), threads_(threads) {
        if(!fallback.empty()) setConfigPath(std::move(fallback));
    }

protected:
    bool onInit() override {
        auto pool = bronx::BxCpuPool::GetDefaultPtr();
        return pool && pool->threadCount() == threads_ && g_source->getValue() == source_;
    }

private:
    std::string source_;
    size_t threads_;
};

static bool writeConfig(const fs::path& path, const std::string& source) {
    std::ofstream out(path);
    out << "app_test:\n"
        << "  source: " << source << "\n"
        << "cpu_pool:\n"
        << "  threads: 1\n";
    return out.good();
}

static bool initApp(const std::string& expected, const std::string& fallback,
                    const std::vector<std::string>& args, size_t threads = 1) {
    std::vector<std::string> values{"test_app_env_config"};
    values.insert(values.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(values.size() + 1);
    for(auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    ConfigApp app(expected, fallback, threads);
    return app.init(static_cast<int>(values.size()), argv.data());
}

static bool testConfigPriority(const fs::path& dir) {
    const auto fallback = dir / "fallback.yml";
    const auto env = dir / "env.yml";
    const auto longOpt = dir / "long.yml";
    const auto shortOpt = dir / "short.yml";
    if(!writeConfig(fallback, "fallback") || !writeConfig(env, "env")
       || !writeConfig(longOpt, "long") || !writeConfig(shortOpt, "short")) {
        return false;
    }
    if(::setenv("BRONX_CONFIG", env.c_str(), 1) != 0) return false;

    bool ok = initApp("short", fallback.string(),
                      {"-c", shortOpt.string(), "--config", longOpt.string()})
        && initApp("long", fallback.string(), {"--config", longOpt.string()})
        && initApp("env", fallback.string(), {})
        && ::unsetenv("BRONX_CONFIG") == 0
        && initApp("fallback", fallback.string(), {});
    return ok;
}

static bool testEnvExeDirectory(const fs::path& dir) {
    const auto exeDir = dir / "fake-bin";
    const auto confDir = exeDir / "conf";
    const auto exe = exeDir / "fake-app";
    fs::create_directories(confDir);
    std::ofstream(exe).put('\n');
    if(!writeConfig(confDir / "framework.yml", "relative")) return false;

    std::string exePath = fs::canonical(exe).string();
    char* argv[] = {exePath.data(), nullptr};
    ConfigApp app("relative");
    if(!app.init(1, argv)) return false;

    const auto* env = bronx::EnvMgr::GetInstance();
    return env->getExe() == exePath
        && env->getCwd() == fs::canonical(exeDir).string() + "/";
}

static bool testRelativeConfigSources(const fs::path& dir) {
    const auto exeDir = dir / "relative-bin";
    const auto otherDir = dir / "other";
    const auto exe = exeDir / "fake-app";
    fs::create_directories(exeDir);
    fs::create_directories(otherDir);
    std::ofstream(exe).put('\n');
    if(!writeConfig(exeDir / "arg.yml", "relative-arg")
       || !writeConfig(exeDir / "app.yml", "relative-app")) {
        return false;
    }

    const auto original = fs::current_path();
    fs::current_path(otherDir);
    std::string exePath = fs::canonical(exe).string();
    char arg[] = "-c";
    char relative[] = "arg.yml";
    char* argv[] = {exePath.data(), arg, relative, nullptr};
    ConfigApp argApp("relative-arg");
    const bool argOk = argApp.init(3, argv);

    char* appArgv[] = {exePath.data(), nullptr};
    ConfigApp appPath("relative-app", "app.yml");
    const bool appOk = appPath.init(1, appArgv);
    fs::current_path(original);
    return argOk && appOk;
}

static bool testBronxConfigAndBusinessIsolation() {
    const fs::path framework = fs::canonical("api_gw/bin/bronx.yml");
    g_source->setValue("");
    if(!initApp("", "", {"-c", framework.string()}, 2)) return false;

    auto cpuThreads = bronx::BxConfig::Lookup<uint32_t>("cpu_pool.threads", 99, "test");
    auto cpuQueue = bronx::BxConfig::Lookup<uint32_t>("cpu_pool.max_queue", 99, "test");
    auto cpuName = bronx::BxConfig::Lookup<std::string>("cpu_pool.name", "bad", "test");
    auto stackSize = bronx::BxConfig::Lookup<uint32_t>("fiber.stack_size", 0, "test");
    auto stackPool = bronx::BxConfig::Lookup<uint32_t>("fiber.stack_pool_size", 0, "test");
    auto guardPage = bronx::BxConfig::Lookup<bool>("fiber.guard_page", false, "test");
    auto connectTimeout = bronx::BxConfig::Lookup<int>("tcp.connect.timeout", 0, "test");
    auto recvTimeout = bronx::BxConfig::Lookup<uint64_t>("tcp_server.recv_timeout", 0, "test");
    auto logs = bronx::BxConfig::LookupBase("logs");

    // Force the production translation units that own these registrations into this test.
    bronx::set_hook_enable(false);
    (void)bronx::BxFiber::Current();
    bronx::BxTcpServer probe(nullptr, nullptr);

    bool ok = cpuThreads->getValue() == 2 && cpuQueue->getValue() == 0
        && cpuName->getValue() == "cpu" && stackSize->getValue() == 131072
        && stackPool->getValue() == 8 && guardPage->getValue()
        && connectTimeout->getValue() == 5000 && probe.getRecvTimeout() == 120000
        && recvTimeout->getValue() == 120000 && logs;

    if(!ok || bronx::BxConfig::LookupBase("server.address")
       || bronx::BxConfig::LookupBase("server.io_workers")
       || bronx::BxConfig::LookupBase("upstreams")
       || bronx::BxConfig::LookupBase("routes")) {
        return false;
    }
    bronx::BxConfig::LoadFromYaml(YAML::LoadFile("api_gw/bin/gateway.yml"));
    return !bronx::BxConfig::LookupBase("server.address")
        && !bronx::BxConfig::LookupBase("server.io_workers")
        && !bronx::BxConfig::LookupBase("upstreams")
        && !bronx::BxConfig::LookupBase("routes")
        && cpuThreads->getValue() == 2 && guardPage->getValue();
}

int main() {
    const fs::path dir = fs::temp_directory_path()
        / ("bronx_app_env_" + std::to_string(getpid()));
    fs::create_directories(dir);

    const bool ok = testConfigPriority(dir)
        && testEnvExeDirectory(dir)
        && testRelativeConfigSources(dir)
        && testBronxConfigAndBusinessIsolation();
    fs::remove_all(dir);
    if(!ok) {
        std::cerr << "application environment/config boundary failed\n";
        return 1;
    }
    return 0;
}
