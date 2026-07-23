// test_singleton.cpp — E2 单例现代化验证
//  ① Singleton<T> / LazySingleton<T> 返回同一实例
//  ② LazySingleton 并发 GetInstance 仅构造一次(call_once)
//  ③ AtShutdown 进程退出按 LIFO(后注册先跑)执行 —— fork 子进程验回放顺序
#include "singleton.h"
#include "test_util.h"
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>

using namespace bronx;

struct Counted { static std::atomic<int> ctor; Counted(){ ctor.fetch_add(1); } int v = 42; };
std::atomic<int> Counted::ctor{0};

// ① 单例性
static void test_identity() {
    TEST_CHECK_MSG(Singleton<Counted>::GetInstance() == Singleton<Counted>::GetInstance(),
                   "Singleton returns same instance");
    TEST_CHECK_MSG(LazySingleton<int>::GetInstance() == LazySingleton<int>::GetInstance(),
                   "LazySingleton returns same instance");
}

// ② call_once:8 线程并发取,构造函数只跑一次
static void test_call_once() {
    Counted::ctor.store(0);
    std::vector<std::thread> ths;
    std::atomic<Counted*> seen[8];
    for (int i = 0; i < 8; ++i)
        ths.emplace_back([i, &seen]{ seen[i].store(LazySingleton<Counted>::GetInstance()); });
    for (auto& t : ths) t.join();
    TEST_CHECK_EQ(Counted::ctor.load(), 1);
    for (int i = 1; i < 8; ++i)
        TEST_CHECK_MSG(seen[i].load() == seen[0].load(), "all threads see same instance");
}

// ③ AtShutdown LIFO:子进程注册 1,2,3 号回调,退出时应按 3,2,1 写入文件
static void test_shutdown_lifo() {
    const char* path = "/tmp/bronx_shutdown_order.txt";
    ::remove(path);
    pid_t pid = fork();
    TEST_CHECK_MSG(pid >= 0, "fork ok");
    if (pid == 0) {
        int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        AtShutdown([fd]{ ::write(fd, "1", 1); });
        AtShutdown([fd]{ ::write(fd, "2", 1); });
        AtShutdown([fd]{ ::write(fd, "3", 1); ::fsync(fd); });
        std::exit(0);   // 触发 atexit → RunAll 逆序回放
    }
    int st = 0; waitpid(pid, &st, 0);
    FILE* f = fopen(path, "r");
    char buf[16] = {0};
    if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    TEST_CHECK_MSG(std::string(buf) == "321", std::string("LIFO order, got=") + buf);
    ::remove(path);
}

int main() {
    test_identity();
    test_call_once();
    test_shutdown_lifo();
    return TEST_SUMMARY();
}
