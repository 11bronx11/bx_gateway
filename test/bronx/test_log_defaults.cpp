#include "async_logger.h"
#include "config.h"
#include "log.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

int main() {
    auto root = bronx::LoggerMgr::GetInstance()->getRoot();
    auto inherited = bronx::LoggerMgr::GetInstance()->getLogger("test_log_defaults_inherited");

    root->setLevel(bronx::BxLogLevel::INFO);
    bool ok = inherited->getLevel() == bronx::BxLogLevel::INFO;

    inherited->setLevel(bronx::BxLogLevel::DEBUG);
    root->setLevel(bronx::BxLogLevel::WARN);
    ok = ok && inherited->getLevel() == bronx::BxLogLevel::DEBUG;

    auto late = bronx::LoggerMgr::GetInstance()->getLogger("test_log_defaults_late");
    ok = ok && late->getLevel() == bronx::BxLogLevel::WARN;

    inherited->setLevel(bronx::BxLogLevel::NOTSET);
    root->setLevel(bronx::BxLogLevel::DEBUG);
    ok = ok && inherited->getLevel() == bronx::BxLogLevel::NOTSET;

    const fs::path file = fs::temp_directory_path()
        / ("bronx_log_defaults_" + std::to_string(getpid()) + ".log");
    const std::string marker = "root-appender-inheritance";
    bronx::BxConfig::LoadFromYaml(YAML::Load(
        "logs:\n"
        "  - name: root\n"
        "    level: info\n"
        "    appenders:\n"
        "      - type: BxFileLogAppender\n"
        "        file: " + file.string() + "\n"
        "        async: true\n"));
    auto fileInherited = bronx::LoggerMgr::GetInstance()->getLogger("test_log_defaults_file_inherited");
    BRONX_LOG_INFO(fileInherited) << marker;
    bronx::BxAsyncLoggerMgr::GetInstance()->flushAll();
    std::ifstream in(file);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ok = ok && fileInherited->getLevel() == bronx::BxLogLevel::INFO
        && text.find(marker) != std::string::npos;
    fs::remove(file);

    if(!ok) {
        std::cerr << "logger default inheritance failed\n";
        return 1;
    }
    return 0;
}
