# 当前测试范围、结论和边界

本文件回答三件事：当前实际测了什么；测试通过能说明什么；哪些结论不能从这些结果推出。

“可以保证”只表示：在报告记录的二进制、配置、负载和本机 loopback 环境中，指定输入得到指定输出，
并且对应断言、状态计数或 sanitizer 检查通过。它不表示任何环境下都没有缺陷。

## 证据与测试资产

- [k6 健康混合运行报告](TEST_REPORT_2026-07-22_K6.md)、[隔离浸泡报告](TEST_REPORT_2026-07-22_SOAK.md)、[故障注入报告](TEST_REPORT_2026-07-22_LOADFAULT.md) 和 [解析器 Fuzz 报告](TEST_REPORT_2026-07-22_FUZZ.md) 是已运行的结果证据。
- `test/k6/scenarios/`、`test/api_gw/`、`test/loadfault/`、`test/fuzz/` 和 C++ target 是可复跑的测试资产。脚本或 target 存在不等于某个专题已有一次可引用的运行结果；引用具体结论时以结果报告和原始产物为准。

## k6 健康双 upstream 运行

**实际做了什么**

- 使用真实 `gw`、`hub` 和两个独立 mock upstream；k6 发出 50,468 个 HTTP 请求、41,912 次迭代，88,327 项检查全部通过，`http_req_failed=0`。
- 36,000 个正常 JWT 请求全部返回 200；正常 JWT 的 p95 为 37.40ms，p99 小于 250ms 阈值。
- expired、bad_secret、alg:none 三种 token 各请求 4,501 次，均返回 401。
- 一次 WAF 注入返回 403；限流阶段收到 64 个预期 429，解除突发后恢复请求均为 200。
- 完成 1,347 次 WebSocket 101 握手和 echo，收发 2,694 帧；120 次 `/stats` 轮询均为 200。
- 两个 upstream 分别成功处理 19,068 和 19,118 个请求；结束快照中 route 5xx、`upstream_fail`、`circuit_open` 均为 0，IP-ban 链路为同步且 open。

**通过能说明什么**

- 在健康双 upstream 配置下，正常 JWT 业务转发、三种指定的非法 JWT 拒绝、WAF 拦截、限流后的恢复、WebSocket 隧道、管理统计和 Hub 到网关的 IP-ban 同步可以同时工作。
- 在这组负载和配置下，网关没有出现客户端请求失败、路由 5xx、上游失败或熔断；两个 endpoint 都实际收到业务流量。

**本次没有做什么**

- 没有在这次 k6 运行中注入上游 503、断连、慢响应、Hub 重启或热重载；这些状态转换由隔离浸泡和故障注入结果补充。
- 没有逐个执行并留存 `test/k6/scenarios/01` 到 `15` 的全部专题结果；这些脚本不能单独作为已验证结论。
- 没有测容量上限、跨机网络、TLS 前置层或跨日稳定性。

## 隔离浸泡和控制面

**实际做了什么**

- 在独立运行根目录启动真实 `gw`、`hub`、`banctl` 与 A/B/C 三台 mock。测试独立创建 YAML、UDS、端口、日志和 SQLite，不改 checkout 的运行配置或数据库。
- 基线使用 20 worker，完成 23,773 个请求，实际 79.24 QPS、传输错误 0、p99 46.27ms；覆盖 JWT 200、缺失 Bearer 401、JWT POST 200、未命中路由 404、伪造 JWT 401 和客户端主动断连。
- 三上游预检得到 1,350/1,350 个 200，A/B/C 分别处理 450 个请求；突发阶段 1,600 个 JWT 请求全部 200。
- 对 A 注入 503 后，A 熔断器进入 open，B/C 承接流量；恢复阶段 400/400 个请求返回 200，三个 endpoint 的熔断器均回到 closed。
- 注入传输故障后得到 380 个 200、4 个预期 502、16 个预期 504，benchmark 传输错误仍为 0；恢复阶段 400/400 个请求为 200。
- 85 个控制面探针全部通过：管理 `/healthz`、`/routes`、维护模式、`/stats`、Gateway/Hub Prometheus metrics、CORS、JWT/API key 鉴权、WebSocket、认证信息剥离、8 个请求走私原始报文、WAF 上报到 Hub 再同步 Guard、管理员封禁/解封、限流举报自动封禁、SQLite 持久化、Hub 重启后规则恢复和网关重连。
- 测试期间执行 admin reload、SIGHUP 日志配置重载、日志轮转和优雅停机检查；资源快照为 gw FD 18 -> 24、峰值 58，RSS 13,692KB -> 14,560KB、峰值 15,028KB，Hub FD 17 -> 17。

**通过能说明什么**

- 三个等权 upstream 在健康时都能接收流量；在此处配置的 503、断连、超时条件下，异常 endpoint 会被隔离，故障解除后可重新加入服务。
- 被测试的认证、授权、CORS、维护模式和 WebSocket 拦截发生在上游转发之前；测试断言了被拒绝请求不访问 mock upstream。
- 已验证 WAF 和限流事件能够经过 Reporter、Hub、Guard 和 metrics 形成封禁；`banctl` 删除或解封规则后，业务路径重新放行。
- 已验证 Hub 的 SQLite 规则在重启后恢复，网关可重新建立订阅并收到规则；已测试的重载、日志轮转和停机路径能完成而不破坏当时的业务检查。
- 本轮资源数字证明该场景下没有观察到 FD 或 RSS 的持续失控；它们是快照和峰值，不是资源上限证明。

**本次没有做什么**

- 不是跨日或 7x24 长稳，不能据此证明长期无内存泄漏、FD 泄漏或性能漂移。
- 使用受控 mock upstream、SQLite 和 loopback，未覆盖真实业务上游、外部身份服务、远端数据库、DNS、负载均衡器或生产网络故障。
- 8 个走私探针覆盖解析器层的 CL/TE 歧义拒绝，不覆盖受控前后端之间的全部语义层走私组合。

## 开环压测和上游故障注入

**实际做了什么**

- 用 vegeta 以恒定开环速率向真实 `gw` 发压，在网关到 mock upstream 的一跳使用 toxiproxy 注入故障；熔断配置为 `slow_ms=1000`、`failure_threshold=5`、`min_requests=10`、`failure_rate=50`。
- baseline 为 200 rps，成功率 100%，p50 约 42ms、p99 约 45ms、`circuit_open=0`。
- 注入 1500ms 延迟后，成功率 100%，p99 1600ms，`circuit_open=93`。
- 直接断开上游连接后，成功率 0% 是预期故障结果，p99 4008ms，`circuit_open=892`；故障移除后的 200 rps 恢复为成功率 100%、p99 3.3ms。
- 限制上游带宽至 1KB/s 后，成功率 96.4%，p99 184ms，`circuit_open=910`。

**通过能说明什么**

- 在这一组熔断阈值、开环速率和上游故障下，延迟、断连和低带宽会走到可观测的熔断或超时路径；故障解除后，业务请求和延迟可以恢复。
- baseline 的延迟来自恒定速率开环压测，不是由闭环客户端吞吐反推；可作为本机 200 rps 下的基线数据。

**本次没有做什么**

- 未搜索饱和点，不能把 200 rps 当作容量上限或生产 SLA。
- 故障只注入网关到上游的一跳；客户端到网关的断网、代理断连、慢客户端和互联网丢包不由这份结果证明。
- 未覆盖所有熔断阈值、所有 HTTP 错误码组合、多 upstream 同时故障或真实服务端的业务级失败。

## HTTP 请求头解析器 Fuzz

**实际做了什么**

- 对 Mongrel 状态机和 `http_parse.cpp` 的请求头及 CL/TE 分帧判定运行 libFuzzer，并启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。
- 从正常 GET/POST/chunked、CL+TE、双 Content-Length、Transfer-Encoding 混淆、截断头、折叠头、裸 LF、异常 Content-Length 和长路径等种子开始变异。
- 执行 20,126,804 个输入，得到 6,234 edges、14,683 features 和 4,204 个 corpus 条目；未生成 crash 文件，未出现 ASan、UBSan 或 runtime error。
- harness 复刻线上请求头契约：先积累完整 `\r\n\r\n` 头块，再新建 parser 一次性输入，而不是让同一 parser 半包重入。

**通过能说明什么**

- 在本次经过的请求头输入空间和真实调用契约下，没有发现可被 ASan、UBSan 或运行时错误检测到的越界、未定义行为或崩溃。
- 已覆盖的 CL/TE 歧义、截断和畸形头会持续作为 fuzz corpus 与回归输入保留。

**本次没有做什么**

- 没有 fuzz `body.cpp` 的 body/chunked 解码，也没有对响应头解析器建立本轮 harness。
- 没有覆盖语义层请求走私、TLS 解密、HTTP/2、HTTP/3 或所有可能的协议互操作。
- fuzz 未发现问题不等于所有输入、所有平台或所有并发时序都不存在解析、安全或内存问题。

## C++ 单元、模块和进程回归

**实际做了什么**

- 当前 CMake 清单定义 71 个直接可执行的 C++ 测试、压力或基准 target：`test/bronx/` 31 个、`test/gateway/` 16 个、`test/ipban/` 19 个、`test/api_gw/` 5 个。
- 框架层覆盖调度和协程、socket/UDS、配置、应用生命周期、日志、bytearray 和 CPU pool；网关层覆盖 HTTP 解析/body、代理、鉴权、路由、限流、熔断、连接池、缓存、metrics 和 reload。
- IP-ban 层覆盖规则、协议帧、Reporter/Guard、订阅 ACK/PING/PONG、SQLite、持久化、hub/admin 和压力；进程层启动真实 `gw`、`hub`、`banctl` 验证启动、链路、压力、生命周期和 journey。

**通过能说明什么**

- 每个直接执行 target 通过时，能保证其自身断言覆盖的函数、协议或 loopback 进程边界没有回归；它们为 k6、浸泡、故障注入和 fuzz 提供模块级回归保护。
- 具体 target、输入和覆盖点见 [测试索引](TEST_MAP.md#c-单元与模块测试)；应按改动所在模块选择对应 target 复跑。

**本次没有做什么**

- 单元和模块用例不能替代真实生产拓扑、全量依赖版本组合或全量并发时序。
- C++ target 的通过不等于整个仓库的所有路径都由同一条命令覆盖；没有对应运行产物的专题不应借用其他 target 的结果宣称通过。

## 整体可以作出的结论

- 网关和 Hub 的核心业务链路在受控真实进程中已同时验证成功流量、预期拒绝、安全拦截、上游治理、控制面同步、持久化、重载和恢复。
- 健康多 upstream 的选路，以及指定的 503、断连、慢响应和限带宽故障下的隔离与恢复，均有真实流量、服务端状态或指标作为证据。
- IP-ban 从 WAF/限流风险上报到 Hub、Guard 执行、管理员删除/解封、SQLite 恢复和网关重连已有端到端证据。
- 请求头解析器在本轮大规模 fuzz 输入下未发现 sanitizer 问题；C++ 测试为框架、网关和 IP-ban 的关键模块补充了回归覆盖。

## 不能作出的结论

- 不能保证项目没有全部功能、并发、安全或内存缺陷，也不能保证未覆盖配置、平台和依赖版本的行为。
- 不能保证容量上限、生产 SLA、跨日稳定性、真实网络质量、TLS 前置层、HTTP/2/HTTP/3 或完整的浏览器和第三方 WebSocket 兼容性。
- 不能将现有 k6 专题脚本、fuzz 种子、C++ target 或故障注入脚本的存在误写成所有对应场景已运行；具体结论必须能回溯到一份结果报告或保存的原始产物。

## 结论适用条件

上述结论仅适用于各报告记录的二进制、运行配置和受控环境。修改网关、Hub、路由 YAML、依赖版本、运行参数或部署拓扑后，应复跑受影响的测试并保存新结果，才能把结论延续到变更后的版本。

相关入口：[测试报告](TEST_REPORT.md)、[测试索引](TEST_MAP.md)、[k6 结果](TEST_REPORT_2026-07-22_K6.md)、[隔离浸泡结果](TEST_REPORT_2026-07-22_SOAK.md)、[故障注入结果](TEST_REPORT_2026-07-22_LOADFAULT.md)、[解析器 Fuzz 结果](TEST_REPORT_2026-07-22_FUZZ.md)。
