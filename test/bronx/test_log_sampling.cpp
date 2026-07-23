// E1-a 令牌桶日志采样测试。
// 验证:rate=0 直通 / rate=N 每秒约放行 N 条 / 桶容量突发上限 / 补充随时间恢复。
// 直接测 BxLogRateLimiter(不经真实落盘,断言可控),再顺带验 BxLogger 的 ERROR 豁免路径。
#include "log.h"
#include "test_util.h"
#include <thread>

using namespace bronx;

int main(){
    // 1) rate=0:不限流,allow() 恒真
    {
        BxLogRateLimiter rl;
        bool allTrue = true;
        for(int i = 0; i < 10000; ++i){
            if(!rl.allow()) allTrue = false;
        }
        TEST_CHECK_MSG(allTrue, "rate=0 应全部放行");
        TEST_CHECK_EQ(rl.getDropped(), (uint64_t)0);
    }

    // 2) rate=100:桶初值满(=100),瞬间连打 1000 条,应约放行 100 条(突发上限),其余丢弃。
    //    时间几乎不流逝 => 补充量可忽略,放行数落在 [100, 105] 的小窗内。
    {
        BxLogRateLimiter rl;
        rl.setRate(100);
        int passed = 0;
        for(int i = 0; i < 1000; ++i){
            if(rl.allow()) ++passed;
        }
        TEST_CHECK_MSG(passed >= 100 && passed <= 110, "突发放行应约等于桶容量 100");
        TEST_CHECK_EQ(rl.getDropped(), (uint64_t)(1000 - passed));
    }

    // 3) 补充恢复:rate=50,先抽干桶,睡 200ms 后应补回约 50*0.2=10 个令牌(±容差)。
    {
        BxLogRateLimiter rl;
        rl.setRate(50);
        // 抽干初始桶(50 个)
        for(int i = 0; i < 50; ++i){ rl.allow(); }
        // 此刻桶应基本空,立即再取大概率失败
        // 睡 200ms 让令牌补充
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int refilled = 0;
        for(int i = 0; i < 50; ++i){
            if(rl.allow()) ++refilled;
        }
        // 200ms @ 50/s => ~10 个;给宽容差 [5, 20] 吸收调度抖动
        TEST_CHECK_MSG(refilled >= 5 && refilled <= 20, "200ms 后应补回约 10 个令牌");
    }

    // 4) BxLogger 集成:ERROR/FATAL 豁免。设一个极小采样率,ERROR 必须全放行。
    //    通过 getSampledDropped 观测:大量 ERROR 不应产生任何丢弃。
    {
        auto lg = std::make_shared<BxLogger>("sampling_test");
        lg->setSampleRate(1);   // 每秒仅 1 条低级别配额
        lg->setLevel(BxLogLevel::DEBUG);
        // 打 100 条 ERROR(高于阈值,应全部豁免,dropped 保持 0)
        for(int i = 0; i < 100; ++i){
            BRONX_LOG_ERROR(lg) << "err " << i;
        }
        TEST_CHECK_EQ(lg->getSampledDropped(), (uint64_t)0);

        // 再打 100 条 INFO(低级别),配额=1,应丢弃约 99 条
        for(int i = 0; i < 100; ++i){
            BRONX_LOG_INFO(lg) << "info " << i;
        }
        TEST_CHECK_MSG(lg->getSampledDropped() > 90, "低级别应被大量丢弃");
    }

    return TEST_SUMMARY();
}
