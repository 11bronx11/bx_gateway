#include "env.h"
#include <climits>
#include <cstdlib>
#include <cstring>

namespace bronx {

// 解析命令行:realpath(argv[0]) 拿到 exe_,截掉文件名得 cwd_;
// 再扫 argv,-k/--key 当键,后面紧跟的非 '-' token 当值(没有就是开关型)。
bool Env::init(int argc, char **argv) {
  if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
    return false;
  }

  char buf[PATH_MAX] = {0};

  if (realpath(argv[0], buf)) {
    exe_ = buf;
    cwd_ = buf;
    auto pos = cwd_.rfind('/');
    if (pos != std::string::npos) {
      cwd_ = cwd_.substr(0, pos + 1);
    }
  } else {
    exe_ = argv[0];
    cwd_.clear();
  }

  std::map<std::string, std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.size() < 2 || a[0] != '-') {
      continue;
    }

    std::string key = (a.size() > 2 && a[1] == '-') ? a.substr(2) : a.substr(1);
    if (key.empty()) {
      continue;
    }

    std::string val;
    if (i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '-') {
      val = argv[++i];
    }
    args[key] = val;
  }

  {
    RWMutexType::WriteLock lock(mutex_);
    args_.swap(args);
  }

  return true;
}

void Env::set(const std::string& key, const std::string& val) {
    RWMutexType::WriteLock lock(mutex_);
    args_[key] = val;
}

bool Env::has(const std::string& key) const {
    RWMutexType::ReadLock lock(mutex_);
    return args_.count(key);
}

std::string Env::get(const std::string& key, const std::string& def) const {
    RWMutexType::ReadLock lock(mutex_);
    auto it = args_.find(key);
    return it != args_.end() ? it->second : def;
}

std::string Env::getEnv(const std::string& key, const std::string& def) const {
    const char* v = ::getenv(key.c_str());
    return v ? v : def;
}

} // namespace bronx
