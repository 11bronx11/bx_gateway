// E1-b JSON formatter 测试。
// 验证:输出是合法紧凑 JSON 一行;字段齐全;特殊字符(引号/反斜杠/换行/控制字符)正确转义。
// 不引 jsoncpp,靠子串断言 + 手工挑关键片段验证。
#include "log.h"
#include "test_util.h"
#include <sstream>
#include <yaml-cpp/yaml.h>

using namespace bronx;

// 在字符串里找子串
static bool has(const std::string& hay, const std::string& needle){
    return hay.find(needle) != std::string::npos;
}

int main(){
    BxJsonFormatter fmt;

    // 1) 普通事件:字段齐全,合法 JSON 骨架
    {
        auto ev = std::make_shared<BxLogEvent>(
            "svc.api", BxLogLevel::INFO, "src/foo.cpp", 42,
            0, 1234, 56, time(0), "worker-1");
        ev->getSS() << "hello world";

        std::ostringstream os;
        fmt.format(os, ev);
        std::string j = os.str();

        TEST_CHECK_MSG(!j.empty() && j.front() == '{', "应以 { 开头");
        TEST_CHECK_MSG(j.find("}\n") != std::string::npos, "应以 }\\n 结尾");
        TEST_CHECK_MSG(has(j, "\"level\":\"INFO\""), "level 字段");
        TEST_CHECK_MSG(has(j, "\"logger\":\"svc.api\""), "logger 字段");
        TEST_CHECK_MSG(has(j, "\"thread\":1234"), "thread 数值不加引号");
        TEST_CHECK_MSG(has(j, "\"thread_name\":\"worker-1\""), "thread_name 字段");
        TEST_CHECK_MSG(has(j, "\"fiber\":56"), "fiber 数值");
        TEST_CHECK_MSG(has(j, "\"file\":\"src/foo.cpp\""), "file 字段");
        TEST_CHECK_MSG(has(j, "\"line\":42"), "line 数值");
        TEST_CHECK_MSG(has(j, "\"msg\":\"hello world\""), "msg 字段");
    }

    // 2) 转义:msg 含引号、反斜杠、换行、tab、控制字符
    {
        auto ev = std::make_shared<BxLogEvent>(
            "root", BxLogLevel::ERROR, "a.cpp", 1,
            0, 1, 0, time(0), "t");
        // 内容:  say "hi"\n	backslash\ and ctrl(0x01)
        std::string raw = "say \"hi\"\n\tback\\slash";
        raw.push_back((char)0x01);
        ev->getSS() << raw;

        std::ostringstream os;
        fmt.format(os, ev);
        std::string j = os.str();

        TEST_CHECK_MSG(has(j, "\\\"hi\\\""), "引号应转义为 \\\"");
        TEST_CHECK_MSG(has(j, "\\n"), "换行应转义为 \\n");
        TEST_CHECK_MSG(has(j, "\\t"), "tab 应转义为 \\t");
        TEST_CHECK_MSG(has(j, "back\\\\slash"), "反斜杠应转义为 \\\\");
        TEST_CHECK_MSG(has(j, "\\u0001"), "控制字符应转义为 \\u00XX");
        // 转义后不应出现裸的未转义换行(除结尾的 }\n)
        std::string body = j.substr(0, j.size() - 2);  // 去掉结尾 }\n
        TEST_CHECK_MSG(body.find('\n') == std::string::npos, "JSON 体内不应有裸换行");
    }

    // 3) 运行时配置导出应保留 sample_rate 和 JSON formatter 类型
    {
        auto logger = std::make_shared<BxLogger>("roundtrip");
        logger->setSampleRate(17);
        auto appender = std::make_shared<BxStdoutLogAppender>();
        appender->setFormatter(std::make_shared<BxJsonFormatter>());
        logger->addAppender(appender);

        YAML::Node node = YAML::Load(logger->toYamlString());
        TEST_CHECK_MSG(node["sample_rate"].IsDefined(), "sample_rate 应被导出");
        TEST_CHECK_EQ(node["sample_rate"].as<uint64_t>(), (uint64_t)17);
        TEST_CHECK_MSG(node["appenders"].IsDefined() && node["appenders"].size() == 1,
                       "应导出一个 appender");
        auto appenderNode = node["appenders"][0];
        TEST_CHECK_MSG(appenderNode["format"].IsDefined(), "JSON format 应被导出");
        TEST_CHECK_MSG(appenderNode["format"].as<std::string>() == "json",
                       "formatter 类型应保持为 json");
        TEST_CHECK_MSG(!appenderNode["pattern"].IsDefined(),
                       "JSON formatter 不应退化为空 pattern");
    }

    return TEST_SUMMARY();
}
