# 2026-07-22 解析器 Fuzz 测试报告

## 结论

对自研 HTTP 请求头解析器（Mongrel 状态机 + `http_parse.cpp` 的 CL/TE 分帧判定）做覆盖引导
模糊测试。挂 libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer，喂任意畸形字节，
共执行 **20,126,804** 次，**零崩溃、零 UB、零 runtime error**。

这是功能测/压测/浸泡都挖不到的一类证据——请求头解析器是 L7 代理的信任根，畸形输入只在
攻击者构造下触发，正常流量永远走不到。本报告补上"解析器在任意畸形输入下不崩、不越界、
不 UB"这条负向证明；它不覆盖 body/chunked 解码，也不替代语义层走私专项。

## 运行配置

```bash
cd test/fuzz
make                 # 强制 clang++,编 fuzz_req_parser(挂 ASan+UBSan)
make run-req         # 用 seeds/ + corpus/ 起步持续跑,崩溃写 crashes/crash-*
# 本轮长跑:直接给可执行传 -max_total_time 跑到 2000 万次量级
```

编译单元全部零框架依赖，走独立 Makefile，不碰主 CMake / build-asan / build-tsan：

- `http_parse.cpp` —— HttpReqParser/HttpRespParser,Mongrel 回调 + 走私判定
- `http_msg.cpp` —— GwRequest/GwResponse 头容器
- `gw_parser/http_parser.cpp` + `httpclient_parser.cpp` —— Mongrel 状态机(ragel 生成)

harness 复刻 `conn.cpp` 契约：`find_header_end` 攒到完整 `\r\n\r\n` 头块 → **新建** parser
→ 一次性 `off=0` 喂完。不半包重入（半包重入会让 mark/query_start 残留偏移算出无符号溢出，
造假崩溃，非真实可达）。详见 `test/fuzz/README.md` 的「契约」节。

## Fuzz 结果

| 指标 | 结果 |
|---|---:|
| 输入数 | 20,126,804 |
| 平均速度 | 50,569 exec/s |
| 峰值速度 | 约 50,000 exec/s |
| 覆盖 edges | 6,234 |
| 覆盖 features | 14,683 |
| corpus 条目 | 4,204 |
| 峰值 RSS | 703MB |
| Sanitizer 报告 | 无 AddressSanitizer / UndefinedBehaviorSanitizer / runtime error |
| 崩溃文件 | 0 个 `crashes/crash-*` |

## 种子语料

`seeds/` 覆盖：正常 GET/POST/chunked、走私向量（CL+TE 共存 / 双 CL 不一致 / TE 混淆）、
超长 method、畸形版本、截断头、折叠头、超大 / 负 CL、裸 LF、超长 path。libFuzzer 从这些
起步变异，新覆盖路径自动存进 `corpus/` 复用。

## 已知边界

- **body/chunked 解码**（`body.cpp` readChunked）未 fuzz：绕不开 GwPool 内存池 + BxSocket
  符号，隔离代价高，当前由 `test_gw_body.cpp` e2e 覆盖。
- **响应头解析器**（HttpRespParser）编译单元已在，加 harness 即可，本轮未跑。
- **语义层走私**（受控前后端对，smuggler.py 级别）不在本报告范围，仅覆盖解析器层歧义拒绝；
  解析器层的 8 个 raw-socket 走私探针见集成功能探针（`gateway_probes.py::run_smuggling`）。

## 证据产物

- `test/fuzz/corpus/` —— 4,200+ 变异语料,可复跑复用
- `test/fuzz/crashes/` —— 崩溃工件目录,本轮为空
- 复现单个崩溃：`make repro-req CRASH=crashes/crash-xxxxxxxx`（带完整 ASan/UBSan 栈）
