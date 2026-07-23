# 测试索引

Bronx 的测试覆盖真实流量、浸泡、故障注入，以及框架、网关、IP-ban 的 C++ 单元和模块测试。
C++ 测试是独立可执行文件，不通过 CTest 发现。

```bash
cmake -S . -B build-bx-tests -DCMAKE_BUILD_TYPE=Debug -DBRONX_BUILD_TESTS=ON
cmake --build build-bx-tests --target test_gw_proxy --parallel 4
./build-bx-tests/test/gateway/test_gw_proxy
```

## 端到端、浸泡和压力验证

| 位置 | 用途 |
|---|---|
| `test/TEST_SCOPE.md` | 当前测试覆盖、可支持结论和明确未覆盖项。 |
| `test/TEST_REPORT_2026-07-22_K6.md` | 真实 gw/hub/双 upstream 的 k6 健康混合运行结果，含命令、负载、阈值、服务端快照和清理记录。 |
| `test/TEST_REPORT_2026-07-22_LOADFAULT.md` | vegeta 开环流量与 toxiproxy 延迟、断连、限带宽故障注入的详细结果、阈值和恢复证据。 |
| `test/TEST_REPORT_2026-07-22_FUZZ.md` | HTTP 请求头解析器的 libFuzzer、ASan/UBSan 运行配置、覆盖数据、种子和边界说明。 |
| `test/k6/` | 主端到端、韧性和健康长稳证据。专题脚本覆盖鉴权、限流、WAF、WebSocket、LB、reload、IP-ban、XFF；`run_evidence.sh` 分阶段编排并留存原始产物。 |
| `test/k6/run_evidence.sh` | 证据运行入口。记录 k6 版本、提交和配置 SHA、每个专题 JSON/日志，以及 gw/hub 的 RSS、线程、FD 前后快照；会按 `waf:<ip>` / `rate:<ip>` 清理测试风险规则并保留记录。 |
| `test/api_gw/run_soak.sh` | 隔离 gw/hub/mock 的浸泡测试，覆盖持续流量、热重载、故障场景和资源预算。 |
| `test/api_gw/gateway_probes.py` | 浸泡前的功能探针，检查 JWT、CORS、WAF、请求走私、IP-ban 和 metrics。 |
| `test_api_gw_journey` | 启动、日志、代理、封禁、限流、WAF、metrics、reload、停机的主链路。 |
| `test_api_gw_chain` | 代理头、WAF -> Reporter -> hub -> Guard、banctl、持久化、reload 和 metrics。 |
| `test_api_gw_lifecycle` | SIGHUP 与 admin reload、错误配置回滚、CORS、chunked、超时和 TERM。 |
| `test_api_gw_stress` | 并发新连接/keep-alive/POST 流量下 reload、hub 重启和 fd 增长。 |
| `test_api_gw_process` | 真 `gw`/`hub` 的启动停止、配置边界、SIGHUP、UDS、SQLite 和日志。 |
| `test/loadfault/` | vegeta + toxiproxy 故障注入，验证延迟、断连、限带宽和故障恢复。 |
| `test/fuzz/` | libFuzzer + ASan/UBSan 的请求头解析 fuzz，覆盖畸形输入和 CL/TE 分帧。 |

## C++ 单元与模块测试

### 框架，`test/bronx/`

| 范围 | 主要 target | 覆盖内容 |
|---|---|---|
| 配置和应用 | `test_app_env_config` `test_app_lifecycle` `test_chain_integration` `test_chain_e2e` | 环境、配置优先级、框架/业务配置分离、应用生命周期和端到端 echo。 |
| 网络和服务 | `test_address` `test_socket` `test_socket_smoke` `test_tcp_server_lifecycle` | 地址解析、TCP/UDP/Unix socket、真实 echo、连接计数、drain 和 stop。 |
| 调度和协程 | `test_scheduler_basic` `test_iomanager_cancel` `test_timer` `test_stack_pool` | 调度、cancel、超时、timer、协程栈复用和 guard page。 |
| 日志 | `test_log_async` `test_log_layer` `test_log_sampling` `test_log_json` `test_log_defaults` | 异步落盘、并发修改、采样、JSON formatter 和默认继承。 |
| 基础组件 | `test_bytearray_smoke` `test_bytearray_strict` `test_sync` `test_singleton` `test_cpu_pool` | 字节数组编解码、锁、单例、CPU pool 和 offload。 |
| 专项回归 | `test_offload_fd_leak` `test_socket_stream_semantics` `test_thread_caps` `test_crash_handler` | fd 复用、stream 返回语义、线程能力和 crash handler。 |
| 压力/基准 | `bench_async_log` `stress_async_log` `extreme_async_log` `fatal_barrier_async_log` | 异步日志吞吐、满队列、极端场景和 FATAL flush 屏障。 |

### 网关，`test/gateway/`

| 范围 | 主要 target | 覆盖内容 |
|---|---|---|
| 连接和代理 | `test_gw_connection` `test_gw_proxy` `test_gw_e2e` | keep-alive、流式 body、路由、中间件、上游转发和 YAML 驱动网关。 |
| 鉴权和内置能力 | `test_gateway_v2` `test_gw_core` `test_gw_builtin` | Router、JWT/API key、限流、IP filter、CORS、健康检查和安全头。 |
| 上游治理 | `test_gw_pool` `test_gw_metrics_upstream` `test_gw_reload_offload` | 内存池、连接池、熔断指标、reload 和 CPU offload。 |
| HTTP 解析和 body | `test_gw_parser` `test_gw_body` `test_gw_boundary` | 请求/响应解析、CL/TE 走私拒绝、chunked、buffer 和畸形输入边界。 |
| 缓存和指标 | `test_arc` `test_gw_metrics` | ARC 缓存和 admin metrics。 |
| 压力/工具 | `stress_gw_pool` `fakerisk` | 内存池并发压力，向 IP-ban daemon 注入模拟风险事件。 |

### IP-ban，`test/ipban/`

| 范围 | 主要 target | 覆盖内容 |
|---|---|---|
| 网关联动 | `test_ipban_gateway` `test_ipban_waf` `test_ipban_risk` `test_ipban_reload` | Ban middleware、WAF/限流举报、Guard 拦截和 reload。 |
| 同步和上报 | `test_ipban_report` `test_ipban_resync` `test_ipban_e2e` `test_ipban_restart` | Reporter、UDS 订阅、ACK/PING/PONG、重连和重启收敛。 |
| 持久化 | `test_ipban_db` `test_ipban_dbpolicy` `test_ipban_dbstress` `test_ipban_persist` | SQLite 存取、落盘策略、并发写和重启恢复。 |
| 规则和协议 | `test_ipban_proto` `test_ipban_guard` `test_ipban_judge` `test_ipban_delta` | IP/CIDR、协议帧、规则优先级、Snap/Delta 和 epoch。 |
| 管理和压力 | `test_ipban_ctl` `test_banhub` `test_ipban_stress` | admin UDS、hub 生命周期和规则并发压力。 |
