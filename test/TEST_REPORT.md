# 测试报告

## 结论

真实 `gw`、`hub`、受控 upstream 和客户端流量已覆盖网关主链路、IP-ban 控制面、多上游分流、
故障恢复、鉴权、WAF、限流、WebSocket、热重载和资源采样。两组运行结果均完成预期检查，未出现
网关进程崩溃。

## k6 综合运行

| 指标 | 结果 |
|---|---:|
| HTTP 请求 | 50,468，56.02 req/s |
| 迭代 / 中断 | 41,912 / 0 |
| Checks | 88,327 通过，0 失败 |
| `http_req_failed` | 0.00%，0 / 50,468 |
| 正常 JWT | 36,000 次全部 200，p95 37.40ms，p99 < 250ms 阈值通过 |
| 鉴权攻击 | expired、bad_secret、alg:none 各 4,501 次，全部 401 |
| WAF 和限流 | 1 次预期 403，64 次预期 429，恢复请求全部 200 |
| WebSocket | 1,347 次 101 握手，2,694 帧收发 |
| Gateway `/stats` | 路由 5xx=0，`upstream_fail=0`，`circuit_open=0` |
| Hub 同步 | `ipban_synced=1`，link state=`open`，协议错误和 ACK/PONG 超时均为 0 |

两个 upstream 分别成功处理 19,068 和 19,118 请求，结束时均为 healthy、circuit closed。测试配置、
进程和临时目录均已清理。

完整的运行配置、负载组成、阈值和服务端快照见
[k6 健康混合运行报告](TEST_REPORT_2026-07-22_K6.md)。

## 隔离浸泡

该流程在独立运行根目录中启动真实 `gw`、`hub`、`banctl` 和三台 mock；运行 YAML、UDS、端口、
日志和 SQLite 均与 checkout 隔离。

| 阶段 | 结果 |
|---|---|
| 基线流量 | 23,773 请求，79.24 QPS，传输错误 0，p99 46.27ms。 |
| 三上游分布 | 1,350/1,350 为 200，A/B/C 各处理 450 请求。 |
| 突发 | 1,600 个 JWT 请求全部 200，159.82 QPS，传输错误 0。 |
| 熔断故障和恢复 | A 注入故障后为 open，B/C 承接流量；恢复阶段 400/400 为 200，三个 endpoint 回到 closed。 |
| 传输故障和恢复 | 380 个 200、4 个预期 502、16 个预期 504；benchmark 传输错误为 0，恢复阶段 400/400 为 200。 |
| 控制面 | 功能、限流举报、持久化、Hub 重启恢复探针合计 85/85 通过。 |
| 资源 | gw FD 18 -> 24，峰值 58；RSS 13,692KB -> 14,560KB，峰值 15,028KB；Hub FD 17 -> 17。 |

主动断连阶段的 mock `ConnectionResetError` 与客户端提前关闭连接一致；它不是 benchmark 传输错误，也不是gateway 崩溃。

执行结果见[隔离浸泡结果报告](TEST_REPORT_2026-07-22_SOAK.md)。

## 故障注入

`test/loadfault/run_loadfault.sh` 使用 200 rps 开环流量和 toxiproxy 注入上游故障：

| 场景 | 成功率 | p50 / p99 | `circuit_open` |
|---|---:|---|---:|
| baseline | 100% | 约 42ms / 45ms | 0 |
| 上游 +1500ms 延迟 | 100% | 1490ms / 1600ms | 93 |
| 上游断连 | 0%，符合该故障预期 | 4000ms / 4008ms | 892 |
| 1KB/s 限速 | 96.4% | 180ms / 184ms | 910 |
| 故障解除后 | 100% | 1.3ms / 3.3ms | - |

完整的运行配置、故障拓扑、每个场景的判定和恢复结果见
[开环压测和故障注入报告](TEST_REPORT_2026-07-22_LOADFAULT.md)。

## 解析器 Fuzz

解析器 fuzz 使用 libFuzzer、ASan 和 UBSan，覆盖 HTTP 头解析和 CL/TE 分帧路径。

| 指标 | 结果 |
|---|---:|
| 输入数 | 20,126,804 |
| 平均速度 | 50,569 exec/s |
| 覆盖 | 6,234 edges，14,683 features，4,204 corpus entries |
| 峰值 RSS | 703MB |
| Sanitizer | 未报告 AddressSanitizer、UndefinedBehaviorSanitizer 或 runtime error |

种子包含正常 GET/POST/chunked、CL+TE、双 Content-Length、TE 混淆、截断头、裸 LF 和异常
Content-Length。

完整的 harness 契约、libFuzzer 覆盖数据、语料和未覆盖项见
[解析器 Fuzz 报告](TEST_REPORT_2026-07-22_FUZZ.md)。

## C++ 单元和模块测试

当前 CMake 清单定义 71 个 C++ 可执行测试、压力或基准 target；这些测试覆盖从纯逻辑到 loopback TCP、
UDS 和真实子进程边界。测试为直接可执行文件，不通过 CTest 发现。

| 目录 | target 数 | 主要覆盖 |
|---|---:|---|
| `test/bronx/` | 31 | 调度、协程、socket、配置、应用生命周期、日志、bytearray、CPU pool。 |
| `test/gateway/` | 16 | HTTP 解析、body、代理、鉴权、路由、限流、熔断、连接池、缓存、metrics、reload。 |
| `test/ipban/` | 19 | 规则、协议、Guard、同步、SQLite、持久化、WAF/限流举报、hub 和 admin。 |
| `test/api_gw/` | 5 | 真实 `gw`/`hub`/`banctl` 的启动、链路、压力、生命周期和 journey。 |

具体 target 和覆盖点见 [TEST_MAP.md](TEST_MAP.md)。

## 覆盖边界

- 开环压测已验证至 200 rps，未测系统饱和点。
- 运行环境为本机 loopback；生产网络和 TLS 前置层需要独立数据。
- 具体测试范围、可支持结论和未覆盖项见 [TEST_SCOPE.md](TEST_SCOPE.md)。
