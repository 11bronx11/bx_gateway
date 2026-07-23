#pragma once
// 把进程锚到 repo 根: 读 /proc/self/exe 定位可执行文件(不靠 argv[0] 也不靠 cwd),
// exe 在 <root>/bin/xxx, 故 root = exe 目录的上一级。chdir 过去, 让业务配置 / 日志 /
// db 那堆相对路径无论从哪个目录启动都锚在 root。返回 root 绝对路径, 失败返回空串。
#include <filesystem>
#include <string>
#include <climits>
#include <unistd.h>

namespace bronx {

inline std::string anchorRoot() {
    char buf[PATH_MAX] = {0};
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if(n <= 0) return {};
    buf[n] = '\0';
    // exe = /root/bin/gw  ->  parent = /root/bin  ->  root = /root
    std::filesystem::path root = std::filesystem::path(buf).parent_path().parent_path();
    if(root.empty() || ::chdir(root.c_str()) != 0) return {};
    return root.string();
}

} // namespace bronx
