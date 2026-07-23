#include "test_util.h"
#include "sync.h"
#include "util.h"
#include <atomic>
#include <csignal>
#include <pthread.h>
#include <regex>
#include <thread>
#include <unistd.h>
#include <vector>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

static void test_current_time_string_format(){
    std::string s = bronx::GetCurrentTimeString();
    std::regex re("^[0-9]{4}\\.[0-9]{2}\\.[0-9]{2} [0-9]{2}:[0-9]{2}$");
    TEST_CHECK_MSG(std::regex_match(s, re), "unexpected time string: " << s);
}

static void test_current_time_string_concurrent(){
    std::atomic<bool> bad{false};
    std::vector<std::thread> threads;
    for(int i = 0; i < 8; ++i){
        threads.emplace_back([&](){
            for(int j = 0; j < 5000; ++j){
                std::string s = bronx::GetCurrentTimeString();
                if(s.size() != 16){
                    bad = true;
                    break;
                }
            }
        });
    }
    for(auto& t : threads){
        t.join();
    }
    TEST_CHECK_MSG(!bad.load(), "concurrent GetCurrentTimeString produced malformed output");
}

static void noop_signal_handler(int) {
}

static void test_semaphore_wait_retries_eintr(){
    struct sigaction oldact;
    struct sigaction act;
    act.sa_handler = noop_signal_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    TEST_CHECK_MSG(sigaction(SIGUSR1, &act, &oldact) == 0, "install SIGUSR1 handler");

    bronx::BxSemaphore sem(0);
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::atomic<bool> failed{false};
    std::thread waiter([&](){
        entered = true;
        try{
            sem.wait();
            returned = true;
        } catch(...){
            failed = true;
        }
    });

    while(!entered.load()){
        usleep(1000);
    }
    usleep(20 * 1000);
    TEST_CHECK_MSG(pthread_kill(waiter.native_handle(), SIGUSR1) == 0,
                   "send SIGUSR1 to waiter");
    usleep(50 * 1000);
    TEST_CHECK_MSG(!returned.load() && !failed.load(),
                   "BxSemaphore::wait should continue waiting after EINTR");

    sem.notify();
    waiter.join();
    TEST_CHECK_MSG(returned.load() && !failed.load(),
                   "BxSemaphore::wait returns after notify");

    sigaction(SIGUSR1, &oldact, nullptr);
}

int main(){
    BRONX_LOG_INFO(g_logger) << "=== test_util start ===";
    test_current_time_string_format();
    test_current_time_string_concurrent();
    test_semaphore_wait_retries_eintr();
    return TEST_SUMMARY();
}
