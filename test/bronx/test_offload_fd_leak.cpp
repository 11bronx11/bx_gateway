// 验证 offload EventFd 析构是否泄漏 BxFdCtx(fd 号复用隐患)。
// 隐患链:EventFd::close 只 ::close(fd),不 FdMgr::del(fd) => fds_[fd] 残留旧 ctx。
// fd 号复用时 get(fd,true) 命中残留槽位,新 fd 被当已初始化的旧对象处理。
//
// 本测试直接观测:构造 EventFd -> ctx 在册 -> 析构 -> 查 ctx 是否残留;
// 再开新 fd 复用同号,查是否拿到"陈旧"ctx(残留 => 修前 FAIL,修后 PASS)。
#include "offload.h"
#include "fd_context.h"
#include "test_util.h"
#include <sys/eventfd.h>
#include <unistd.h>

using namespace bronx;

int main(){
    int leakedFd = -1;

    // 1) 构造 EventFd,确认其 fd 已在 FdMgr 注册
    {
        detail::EventFd ev;
        leakedFd = ev.fd();
        TEST_CHECK_MSG(leakedFd >= 0, "eventfd 应有效");
        auto ctx = FdMgr::GetInstance()->get(leakedFd, false);
        TEST_CHECK_MSG(ctx != nullptr, "构造后 FdMgr 应有该 fd 的 ctx");
    }
    // 2) 析构后:若走了 FdMgr::del,槽位应被清空(get(false)==nullptr)。
    //    修前:EventFd 只 ::close,ctx 残留 => 此断言 FAIL。修后 PASS。
    {
        auto ctx = FdMgr::GetInstance()->get(leakedFd, false);
        TEST_CHECK_MSG(ctx == nullptr, "析构后 FdMgr 该 fd 的 ctx 应已清除(不残留)");
    }

    // 3) fd 号复用场景:开一个新 fd,若拿到同号且 FdMgr 里是残留旧 ctx(isInit 已设),
    //    则新 fd 被误当已初始化对象。这里验证复用同号时 get 出来的 ctx 是新鲜的。
    {
        int newFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        TEST_CHECK_MSG(newFd >= 0, "新 eventfd 应有效");
        if(newFd == leakedFd){
            // 命中复用:此时槽位若残留,get(false) 会返回非空的陈旧 ctx
            auto stale = FdMgr::GetInstance()->get(newFd, false);
            TEST_CHECK_MSG(stale == nullptr,
                "复用同 fd 号时不应存在陈旧 ctx(否则新 fd 被当旧 socket 处理)");
        } else {
            BRONX_LOG_INFO(bronx_test::logger())
                << "新 fd=" << newFd << " 未复用 leakedFd=" << leakedFd << ",复用路径本次未触发";
        }
        ::close(newFd);
    }

    return TEST_SUMMARY();
}
