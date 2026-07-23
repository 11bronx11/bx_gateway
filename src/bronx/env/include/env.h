#pragma once
#include "singleton.h"
#include "sync.h"
#include <map>
#include <string>

namespace bronx {

// 进程环境信息中心:命令行参数 + 可执行路径 + 系统环境变量。
// 启动早期解析 argv,把 -k v / --key val 存成 key->value 表,再记下 exe 的绝对路径和所在目录。
// 全局单例(EnvMgr),args_ 读多写少用读写锁,init 一般启动时只调一次。
class Env {
public:
    /// 解析命令行:取 argv[0] 的绝对路径填 exe_/cwd_,把 -k/--key 参数存入 args_。
    /// 参数非法(argc<=0 等)返回 false。调用链:BxApplication::init 早期调用。
    bool init(int argc, char** argv);

    /// 写入/覆盖一个参数(写锁)。
    void        set(const std::string& key, const std::string& val);
    bool        has(const std::string& key) const;
    /// 取参数值,不存在返回 def(读锁)。BxApplication 用它取配置路径 "c"/"config"。
    std::string get(const std::string& key, const std::string& def = "") const;

    /// 取系统环境变量(getenv 包装),不存在返回 def。
    std::string getEnv(const std::string& key, const std::string& def = "") const;

    const std::string& getExe()  const { return exe_; }   ///< 可执行文件绝对路径
    const std::string& getCwd()  const { return cwd_; }   ///< 可执行文件所在目录(末尾带 /)

private:
    using RWMutexType = BxRwMutex;
    mutable RWMutexType          mutex_;
    std::map<std::string, std::string> args_;
    std::string exe_;
    std::string cwd_;
};

/// 全局单例:全框架通过 EnvMgr::GetInstance() 访问环境信息。
using EnvMgr = Singleton<Env>;

} // namespace bronx
