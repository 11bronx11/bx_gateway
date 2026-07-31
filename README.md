# Bronx_gateway

Bronx 是一个运行在 Linux 上的 C++20 协程网络框架，底座是有栈协程(ucontext)+ epoll 反应堆 + N:M 调度 + cpu_pool 业务线程池;Bronx_gateway 基于它实现，是一个完整可部署的 HTTP/1.1 L7 反向代理，外加一个本地 IP-ban 控制面。路由、四种负载均衡、熔断三态、连接池、JWT/api_key 鉴权、轻量 WAF、路由级限流、动态 IP 封禁都在网关内，三个原生可执行文件 + YAML 即可跑，运行时零 JS/Python 依赖。

**定位:单机轻量化高性能 L7 网关。** 静态配置驱动，安全策略前置(名单/WAF/鉴权/限流按序短路，拒绝的请求不触达上游)，连接、路由、上游状态、风险规则、观测数据共用同一份运行时事实。"高性能"来自设计而非跑分:I/O 等待让协程让出不占线程、热路径原子快照零锁只读、请求响应体流式转发不整体缓冲、CPU 活 offload 出 reactor;真实吞吐仍需按部署压测，本文只给已跑出的测试数据。

**边界(对比 nginx/envoy/APISIX 等生产级网关)。** 优势是部署面小、依赖少、行为可追踪可回归、失败状态可见不静默降级;代价是当前只做 HTTP/1.1，不终止 TLS(委托前置层)、无 HTTP/2/3，不做动态服务发现与分布式配置中心，WAF 是固定特征轻量拦截不解析 body，IP-ban 是本机 UDS + SQLite 控制面而非跨机共识系统。它要解决的是"一台或一组 Linux 主机前放一层可控、可观测、可本机恢复的轻量入口"，不是替代大型 API 管理平台。

## 设计实现
框架层
- 有栈协程(ucontext)+ epoll 反应堆，N:M 调度，可绑核，I/O 等待即让出不占线程
- Hook libc 阻塞调用(sleep/read/write/connect/accept…)为协程让出，线程级开关，同步写法异步执行
- 优雅停机:停接新连接 → 踢空闲 keep-alive → close 经 owner reactor 路由 abortAll 唤醒阻塞协程带 ECANCELED 返回 → 超时兜底强关
- 进程外壳 + 信号治理:最早屏蔽信号子线程继承掩码，专用信号线程 sigtimedwait 同步收，INT/TERM 优雅停、HUP 热重载
- 栈池复用 + guard page 防溢出，栈溢出从静默踩内存变可诊断故障
- 业务线程池 cpu_pool + eventfd 桥:CPU 密集/同步阻塞挪出 reactor，完成回协程控制流
- 高性能内存池:TLS Bin 零锁 → per-class Slab → mmap PageHeap，整块空闲即还 OS
- 异步日志:TLS 双缓冲 + 无锁 MPSC 环 + 背压 + 令牌桶采样，支持结构化 JSON
- 扁平 kv 配置:YAML 解析，SIGHUP 热更新日志级别等节点
- 现代锁体系线程封装:TTAS 自旋、Ticket 公平锁、读写锁、信号量、协作取消，RAII 封装，握手，栈分配

网关层
- HTTP 边界:先分帧后业务，`Content-Length`/`chunked` 流式解析，CL 与 TE 冲突、非法分帧一律判歧义拒绝，入口挡请求走私(解析器 fuzz 2000 万+ 输入零 crash)
- 流式转发:请求/响应体分块边读边转，按上游分帧重编码，全程不整体缓冲，大 body 内存占用平
- config 快照原子替换:reload 在 CPU pool 建新快照，失败回退旧快照不污染在线流量
- 路由:exact 优先 prefix(整段匹配)，host/method/priority 仲裁，generation cache + ARC 路由缓存
- 负载均衡:RR / 加权 RR / 最少连接 / 加权最少连接
- 上游治理(endpoint 级):主动健康检查 + 熔断三态 + keep-alive 连接池 + `max_inflight` 并发上限
- 三级超时:total / connect / read 各自 deadline，慢上游不拖垮 worker
- 真实客户端 IP:只信 `trusted_proxies` 的 `X-Forwarded-For`，限流/封禁/日志/转发头共用一份解析，防伪造
- 路由级鉴权:每路由 `inherit`/`none`/`jwt`/`api_key`;JWT 验 HS256/RS256/ES256、防 `alg=none`、多 kid 轮转;api_key 走 SHA-256;按 scope/role 授权，可注入 `X-User-*` 透传上游
- 路由级限流:每路由独立令牌桶，key 按 `ip` 或 `user`，超限 `429` + `Retry-After`，反复超限自动举报坏 IP
- 轻量 WAF:扫 URL/query/头里的 SQLi/XSS/路径穿越/扫描器指纹，命中 `403` 并可报 daemon(不扫 body)
- 中间件洋葱链:固定装配顺序即安全模型，被名单/WAF/鉴权/维护拒绝的请求不触达上游
- WebSocket 透明隧道:Upgrade 握手转上游、校验 101，成功后双向字节泵对拷
- 可观测:`/stats` JSON + `/metrics` Prometheus + `/routes` 生效快照 + `/reload` 热重载 + `/maintenance` 维护开关

动态 IP 封禁
- 独立 daemon 决策，三条 UDS 职责分离:submit 收举报 / subscribe 订阅 / admin 由 `banctl` 人工增删
- 自定义线协议:20 字节定长帧头 + 变长 JSON body，手摆网络字节序，body 上限防超长分配，多种消息 kind
- 长连接 runtime:一连接一读一写协程，规则变更 Broadcaster 事件驱动唤醒增量推，不轮询
- 网关侧 Guard:静态 + 远程规则合并成不可变 `Snap`，原子替换热路径零锁只读，单 IP 进 hash O(1)，v4/v6 都支持
- SyncClient 单同步链:状态机 DIAL→HELLO→SYNC→OPEN，ACK/PONG 超时检测，断线退避重连
- epoch + version 对账:DELTA 带 prevVer 校验才应用增量，daemon 重启摇新 epoch 扛版本回退
- 举报不阻塞热路径:Reporter 有界队列 + 去重 + 退避，满了丢弃计数不卡业务
- SQLite 冷备:选择性落盘(短临时封不落) + offload 挪出 reactor，Hub `/healthz`、`/metrics` 观察

## 测试结果

先有可引用的运行结果，再提供复现入口。结论仅适用于报告记录的二进制、YAML 和本机 loopback
环境，不把脚本存在误写成所有场景都已经运行;完整复现命令见后面的[测试复现](#测试复现)与[测试索引与证据](#测试索引与证据)。

| 已通过的验证 | 实际结果 | 证明的范围 |
|---|---|---|
| k6 健康混合运行 | 真实 `gw`、`hub`、双 upstream ；50,468 HTTP 请求、41,912 迭代、88,327 checks 全部通过，`http_req_failed=0`。JWT、三类非法 token、WAF、限流恢复、WebSocket、admin 和 Hub 同步同时验证。 | 健康多 upstream 条件下的持续主链路和安全拒绝能并行工作；不等价于故障注入或跨日稳定性。 |
| 隔离浸泡与控制面 | 三 upstream、真实 gw/hub/banctl；基线 23,773 请求、p99 46.27ms、传输错误 0；85/85 功能与控制面探针通过。验证了均衡、503/断连/超时后的熔断恢复、Hub 重启、SQLite 恢复、reload、日志轮转和停机。 | 网关、Hub、Guard、Reporter、SQLite 与规则管理的端到端闭环。 |
| 开环压测和故障注入 | vegeta 200 rps baseline 成功率 100%、p99 约 45ms；延迟、断连、限带宽注入均触发可观测的熔断/超时，解除后 200 rps 成功率恢复 100%、p99 3.3ms。 | 上游侧延迟、断连、低带宽下的熔断与恢复路径。 |
| 请求头解析 fuzz | libFuzzer + ASan + UBSan 执行 20,126,804 个输入，6,234 edges、14,683 features，无 crash、ASan、UBSan 或 runtime error。 | 当前请求头解析与 CL/TE 分帧调用契约下的内存安全负向证据。 |
| C++ 单元、模块和进程回归 | CMake 定义 71 个直接执行 target，覆盖框架 31、网关 16、IP-ban 19、真实 API-gateway 进程 5。 | 对协程、socket、配置、日志、HTTP、路由、认证、熔断、SQLite、协议和真实子进程边界提供模块级回归保护。 |


## 构建

目标平台为 Linux。需要 CMake 3.20+、支持 C++20 的编译器、yaml-cpp、OpenSSL、jsoncpp 和 pthread。SQLite amalgamation 与 jwt-cpp 头文件随仓库提供，CMake 优先用系统 jwt-cpp，找不到再回退仓库副本。

```bash
sudo apt install cmake g++ libyaml-cpp-dev libssl-dev libjsoncpp-dev

cmake -S . -B build-bx -DCMAKE_BUILD_TYPE=Debug
cmake --build build-bx --parallel 4
```

产物固定输出到仓库根目录 `bin/gw`、`bin/hub`、`bin/banctl` 和 `lib/libbronx_core.a`。Sanitizer 用独立构建目录避免产物互相覆盖，ASan/UBSan 产物落在 `bin-address-undefined/`，TSan 落在 `bin-thread/`：

```bash
cmake -S . -B build-bx-asan -DBRONX_BUILD_TESTS=ON -DBRONX_SANITIZE=address,undefined
cmake -S . -B build-bx-tsan -DBRONX_BUILD_TESTS=ON -DBRONX_SANITIZE=thread
```

## 完整演示

仓库内置一套隔离的真实进程 Demo，覆盖 HTTPS/HTTP2、路由与改写、JWT、限流、
WAF、动态 IP-ban、加权负载均衡、熔断恢复、SSE、热重载、WSS、Prometheus
指标、Grafana 大盘以及 Alloy -> Loki 日志关联。它使用 `demo/` 下的独立 YAML、
日志、SQLite 和 PID，不读写 `api_gw/bin/gateway.yml` 或正式 Hub 数据库。

```bash
./demo/up.sh
sudo ./demo/up.sh nginx
./demo/status.sh
./demo/demo.sh       # 全部 16 项
./demo/demo.sh 15    # 只演示 WSS
./demo/demo.sh 16    # 只演示日志关联
```

依赖准备、端口、SSH 隧道、Grafana/Loki 日志查询和资源回收见
[Demo 运行手册](demo/README.md)。`DEMO_PLAN.md` 是施工历史，不是当前运行说明。

## 部署与上手

`gw` 和 `hub` 根据 `/proc/self/exe` 锚定运行根，因此直接运行构建输出的 `bin/gw`、`bin/hub` 即可，进程会自动 chdir 到仓库根，从 `api_gw/bin/` 读取 YAML。

**最小可跑配置**：只反代一个上游，不启用 IP-ban/Hub。把下面写进 `api_gw/bin/gateway.yml` 即可（框架配置 `bronx.yml` 保持仓库默认，不必改动）：

```yaml
server:
  address: "0.0.0.0:8090"        # 业务口
  admin_address: "127.0.0.1:9090" # 管理口，只绑 loopback

upstreams:
  - name: backend
    lb: round_robin
    endpoints:
      - { host: "127.0.0.1", port: 8080 }   # 你的真实后端

routes:
  - name: all
    match_type: prefix
    path: /
    upstream: backend
```

启动并验证（Hub 只在需要动态 IP 封禁时才启，基础代理不需要）：

```bash
./bin/gw
curl http://127.0.0.1:8090/            # 反代到 127.0.0.1:8080
curl http://127.0.0.1:9090/healthz     # 存活
curl http://127.0.0.1:9090/routes      # 当前生效的路由/上游快照
curl http://127.0.0.1:9090/stats       # JSON 指标
curl http://127.0.0.1:9090/metrics     # Prometheus 指标
curl -X POST http://127.0.0.1:9090/reload   # 热重载 gateway.yml
```

要启用动态封禁，先起 `hub`，在 `gateway.yml` 补 `ip_policy` 段填 UDS 路径（与 `hub.yml` 一致），再用 `banctl` 管理规则：

```bash
./bin/hub
./bin/banctl deny 203.0.113.9/32 10m manual-check
./bin/banctl list
```

管理口和 Hub 观察口**都没有内建认证**。保持它们绑定 loopback 或受信任私网，用防火墙/前置代理限制访问，不要直接暴露公网。完整字段、默认值和热更新边界见下文[核心讲解](#核心讲解)之后的[配置参考](#配置参考)。

## 核心讲解

### 1. 为什么选择协程 reactor 而不是线程池

**设计问题**：一个反向代理需要同时处理数千个并发连接，每个连接都可能在等待上游响应、客户端发送 body 或定时器到期。用”一连接一线程”模型会消耗大量内存（每个栈 2-8MB）、频繁上下文切换、且难以在超时或停机时精确取消所有等待中的任务。用无栈协程（如 C++20 coroutine）需要把所有异步逻辑改写成 `co_await` 链，代码可读性和错误处理都会恶化。

**设计选择**：Bronx 选择**有栈协程 + N:M 调度 + libc hook** 的组合：每个连接或任务是一个协程（有独立栈），多个协程由少量 worker 线程调度，I/O 系统调用被 hook 透明替换为”注册事件 + 让出执行权”。这让业务代码保持同步形态（`read(fd)` 直接返回数据或错误），同时获得异步 I/O 的并发能力和资源效率。

**实现要点**：

```text
业务协程调用 read / write / connect
  -> hook 遇到 EAGAIN 或 EINPROGRESS
  -> 在 BxIoManager 上注册 fd 的读或写事件，并挂起当前协程
  -> worker 回到 epoll_wait，继续服务别的连接或任务
  -> epoll 事件、超时或取消到达
  -> 调度器重新 post 原协程，调用继续
```

几个直接影响稳定性的细节：

- **edge-triggered epoll + tickle pipe**：投递新任务、最早定时器变化或停机时，向 pipe 写一字节唤醒阻塞的 worker，避免等到超时才感知工作。
- **单读写约束**：每个 fd 的读/写方向只允许一个等待者，事件上下文明确记录应恢复的协程，避免多协程无序竞争同一 socket。
- **owner reactor 路由**：fd 记录所属的 `BxIoManager`。关闭连接时，框架把 `abortAll` 路由回持有该 fd 的 reactor，让挂起协程收到 `ECANCELED` 而不是永久睡眠。
- **超时统一管理**：`connect` 超时受框架 timer 控制，不会因为上游不响应而卡住 worker。

**收益**：高连接数下的并发组织能力和可取消性。一个 4 核 8G 虚拟机可以同时处理数千个长连接，超时和停机时能精确取消所有挂起任务。这**不是**承诺某个固定 QPS——实际吞吐仍取决于 CPU、协议报文、上游延迟、日志量和部署参数。

#### 框架层关键设计

| 设计 | 当前实现 | 对网关的价值 |
|---|---|---|
| 有栈协程 + N:M 调度 | `BxFiber` 基于独立栈保存/恢复上下文；多个协程由少量 worker 调度。可选绑核，`gw`/`hub` 默认不绑。 | 避免”一连接一线程”的资源占用，同时保留同步代码形态。 |
| libc I/O hook | `read`/`write`/`connect`/`accept`/`close` 等被 hook 透明替换。开关是 `thread_local`，只有 reactor worker 走协程让出。 | 业务代码以同步调用编程，hook 在底层消化 `EAGAIN`，普通线程不受影响。 |
| 栈池与 guard page | 协程栈按尺寸在**每线程** pool 中复用，免去高并发下反复 `mmap`；可配置 `PROT_NONE` guard page。 | 减少协程创建抖动，把栈溢出从静默踩内存变成可诊断的 SIGSEGV。 |
| 异步日志管线 | 每线程 TLS 双 buffer + 有界 MPSC ring + 背压。环满时生产者等待，writer 批量写 appender。 | 业务线程不与文件 I/O 争锁；有界队列避免日志洪峰无限占内存；停机时 flush TLS 残留。 |
| 日志质量控制 | 令牌桶 `sample_rate` 限制低级别日志，`ERROR`/`FATAL` 豁免。File appender 支持结构化 JSON。 | 流量风暴时限制写盘，同时保留关键错误；JSON 可直接接入日志平台。 |
| 分层同步原语 | `BxSpinLock`(TTAS+退避)、`BxTicketLock`(FIFO 公平)、`BxMutex`、读写锁、信号量。 | 短临界区、公平需求、读多写少分别用相称原语，避免一把重锁扩散到整个框架。 |
| CPU offload | `offload()` 将任务投给 `BxCpuPool`，以 eventfd 通知等待协程。无 pool 时退化为当前线程执行。 | YAML 解析、DNS、SQLite 写不会长时间占住 I/O worker，完成后仍回到协程控制流。 |

这几项组合起来形成 Bronx 的基础运行时：I/O 等待让协程让出，CPU 工作显式离开 reactor，日志以有界异步通道输出，状态读写按访问模式选择原语。它们不是孤立”工具箱”，而是网关和 Hub 所有热路径的共同约束。

### 2. HTTP 边界：在网关入口统一拒绝协议歧义

**设计问题**：HTTP/1.1 请求走私（request smuggling）的根源是前后端对"请求在哪里结束"理解不同。当代理允许 `Content-Length` 与 `Transfer-Encoding: chunked` 同时存在、或允许多个不一致的 `Content-Length`，攻击者可以构造一个请求在代理眼中是一条、在上游眼中是两条，从而绕过认证、注入恶意请求或污染缓存。

**设计选择**：Bronx 在进入路由前先完成 HTTP/1.1 分帧判定，对协议歧义**零容忍**：`Content-Length` 与 chunked 同时出现、多个不一致的 `Content-Length`、无效数字或不合法的 `Transfer-Encoding` 会被视为歧义并**立即拒绝 400**，而不是猜测一个解释继续转发。这是一条重要的代理边界：前后端对请求边界理解不同是走私的前提，Bronx 不把这类解析错误延迟到上游处理，而是在网关入口统一拒绝。

**实现要点**：

- 解析器基于 Mongrel 状态机，支持 `Content-Length` 与 `Transfer-Encoding: chunked` 两种分帧；响应没有明确分帧时按连接关闭结束。
- `Content-Length` 与 `chunked` **互斥**：同时出现或多个不一致的 CL 都是 400。
- 转发前做**头部卫生**：逐跳头（`Connection`、`Keep-Alive`、`Upgrade`、`TE`）、客户端自带的 `X-Forwarded-*` 和不支持的 `Expect` 不会直接传给上游；网关根据已解析的真实客户端地址重新构造转发头。
- 请求/响应 body **流式转发**：`readChunk` 循环读取客户端 body，per-frame 重新编码给上游，不会把整个 body 缓冲到内存。

**测试覆盖**：

- **解析器 fuzz**（[测试报告](test/TEST_REPORT_2026-07-22_FUZZ.md)）：libFuzzer + ASan/UBSan 挂 Mongrel 状态机，覆盖畸形请求行、头部、chunked 编码；1000万次输入无崩溃。
- **走私集成测试**（`test/gateway/test_gw_smuggle.cpp`）：覆盖 CL+TE、双 CL、CL 非数字等 8 种走私变体，全部被拒绝 400。

这条边界保证：客户端不能通过协议歧义绕过路由、认证或 WAF；路由的 header 改写不会绕过协议边界；上游不需要再防御代理层传来的协议歧义。

### 3. 中间件顺序是安全合约，而不是可任意拼装的插件链

**设计问题**：可配置的插件系统灵活但危险——一旦顺序配错，安全策略就会静默失效而不报错。比如：WAF 如果在路由后执行，已知恶意 IP 会消耗完整路由计算资源；限流如果在 JWT 验证前执行，`user` 限流维度会因为没有 `jwt_sub` 而静默降级为 IP 维度；认证如果在 IP-ban 后才执行，被封禁的 IP 却仍然要走完鉴权逻辑。

**设计选择**：Bronx 的中间件**顺序固定，不可配置**。`gateway.yml` 只控制开关和参数，不控制顺序。每次启动或 reload 时由代码按确定顺序组装。固定顺序换来的是：**顺序本身定义了安全模型**，可以回归测试，行为可以推理。

**顺序与设计理由**：

| 阶段 | 中间件 | 为什么放在这里 |
|---|---|---|
| 1 | RequestId | 最早注入，后续所有日志和 header 都带同一 trace id。 |
| 2 | **ClientAddr** | 解析真实客户端 IP——TCP peer vs XFF 由 `trusted_proxies` 决定。**一次解析，全链共用**，IP-ban、限流、日志、转发头使用相同客户端身份，不会出现”日志记 A、限流限 B”的策略分裂。 |
| 3 | StructuredAccessLog | 紧跟 ClientAddr，后续被短路也能记到客户端地址。 |
| 4 | SecurityHeaders / CorsPrepare / Finish | 响应头和记账钩子，即使后续请求被拒绝也能生效。 |
| 5 | **BanMiddleware / IPFilter** | **最早拒绝**：被封禁的来源在这里 403，不进入 WAF、认证、路由或上游资源。 |
| 6 | HealthCheck / CorsPreflight / Maintenance | 运维接口和维护模式，不需要路由匹配。 |
| 7 | **WAF** | 在路由前扫描 URL/query/headers，命中 403 并报 daemon，不消耗后续路由和上游资源。 |
| 8 | **Router** | 匹配路由，后续中间件读取 `ctx.route().rule`。 |
| 9 | **RouteAuth** | 读取 `rule.authPolicy`，验 JWT/API key，成功写 `jwt_sub`。认证必须在限流前——`user` 维度限流需要 `jwt_sub` 到位。 |
| 10 | **per-route RateLimit** | 令牌桶，key 是 `ip` 或 `user`（后者依赖 step 9 已写入 `jwt_sub`）。超限 429 + `Retry-After`，累计命中可自动向 daemon 举报。 |
| 11 | WSTunnelStub / **Proxy** | 只有前置安全、路由和策略全部通过的请求才连接上游。 |

两个顺序依赖值得特别说明：

1. **RouteAuth → RateLimit** 的先后不可调换：如果限流先于认证，`user` 维度限流会因为缺 `jwt_sub` 静默降级为 IP 维度，效果完全不同。
2. **WAF 在路由前**：WAF 不需要路由上下文；放路由前意味着已封 IP 由 BanMiddleware 拦截，WAF 只扫漏掉的流量，最小化正则开销。

这不是一个提供任意第三方代码插槽的插件平台，换来的是行为可预期、安全模型可测试，以及每种拒绝类型都能在 `/stats` 中单独追踪。

### 4. 配置热更新：原子快照 + 失败回滚，保证在线请求不受影响

**设计问题**：传统服务器热重载（如 nginx `SIGHUP`）通常在信号处理函数里直接重建配置，有几个风险：一是信号处理函数不 async-signal-safe 的操作（malloc、YAML 解析、DNS）会 UB；二是新配置加载失败时旧配置可能已被部分修改；三是框架资源（工作线程数、监听端口）和业务配置（路由/上游）混在一起，一处出错会影响另一处。

**设计选择**：Bronx 将配置分为两个平面，用不同信号/接口独立控制：

```text
SIGHUP  -> BxApplication::onReload()    (框架平面)
  -> 重载 bronx.yml / hub_bronx.yml
  -> 框架资源：日志 appender、CPU pool、timer、hook timeout

POST /reload  -> GatewayServer::reload()  (业务平面)
  -> 重建 business snapshot
  -> 业务语义：路由、上游、认证、CORS、IP-ban UDS、rate limit
```

业务 reload 的完整流程：

1. 在 `BxCpuPool` offload 线程里做所有耗时工作：读取 YAML 文件、解析上游地址、构建路由表和中间件链。
2. 全部完成后，以 `atomic<shared_ptr<ConfigSnapshot>>` **原子替换**当前快照。
3. 正在处理中的请求持有旧快照的 `shared_ptr`，不受影响，引用计数归零时自然释放。
4. 若任意步骤失败（YAML 格式错、上游地址无法解析、中间件构建异常），**不替换**当前快照，admin 返回 `500`，旧快照继续服务流量。

路由缓存失效也以同一机制处理：每次替换 route table 递增 `generation`，旧 generation 的 ARC cache key 自然 miss——不需要在 reload 时冒险清空一个正在被请求读取的全局缓存，新旧请求各持自己的快照，互不干扰。

**收益**：配置变更的爆炸半径被隔离到”加载失败时 admin 返回错误”，而不是”半份配置进入流量路径”或”重载过程中旧配置被部分破坏”。监听地址、I/O worker 数、CPU pool 大小属于进程启动资源，不在热更新范围内——这是有意为之的边界，而不是遗漏。

### 5. 上游治理：endpoint 是分析和决策的最小单元

**设计问题**：一个 upstream group 有三台机器，其中一台 OOM 后响应变慢但不断连。如果把健康状态、熔断、连接池、并发上限都以”服务组”为粒度，三台机器对外表现为一个整体，排障时无法分辨是哪台出问题，也无法把坏的一台隔离出去而不影响整组。

**设计选择**：每个 endpoint 独立持有完整的治理状态：

```text
endpoint
  +-- active_connections / max_inflight (并发上限)
  +-- idle keep-alive connection pool   (连接复用)
  +-- health check: continuous pass/fail counters (主动探测)
  +-- circuit breaker: CLOSED <-> OPEN <-> HALF_OPEN (被动熔断)
  +-- request / error / latency statistics
```

**选路逻辑**：先剔除不可用候选（health FAIL、circuit OPEN、达到 `max_inflight`），再把剩余 endpoint 交给 load balancer。可用的算法是 `round_robin`、`weighted`、`least_conn`、`weighted_least_conn`。注：代码中存在 `consistent_hash` 占位类，但它没有进入可支持的 YAML 集合，当前**不可用**。

**健康检查 vs 熔断**：两者解决的是不同问题，缺一不可：

| 机制 | 触发方式 | 适用场景 |
|---|---|---|
| 主动健康检查 | 周期性 HTTP GET，连续成功/失败达阈值才改变状态 | 探测 endpoint 是否存活（例如机器宕机、端口未监听） |
| 熔断器 | 真实代理请求的失败率/慢请求率超窗口阈值 | 检测 endpoint 虽然存活但业务请求不可靠（例如上游 OOM 后响应极慢） |

连接复用时只复用**已安全完成、响应边界明确**的 keep-alive 连接；连接失败、超时、上游异常和不可复用响应会被驱逐而不是塞回池中，避免把坏连接传染给下一个请求。

**三级超时收敛**：`total_timeout`（请求总预算）、`connect_timeout`（拨号）、`read_timeout`（读响应头），三个 timer 的最小剩余值在每次 I/O 操作前收敛为实际等待上限。这保证上游慢响应不会占住协程超过总预算。

**可观测性**：`/routes` 给出每条路由的配置和当前 endpoint 状态；`/stats`、`/metrics` 给出请求数、在途数、健康状态、熔断转移次数和延迟桶。排障不需要猜”请求是否落到某台上游”——endpoint 级指标直接回答这个问题。

### 6. 客户端身份、认证与 WAF：把信任来源显式写进配置

**ClientAddr：XFF 信任链的显式控制**

反向代理最容易被忽略的安全问题：所有客户端都能伪造 `X-Forwarded-For`。如果代理无条件信任 XFF，攻击者只需在请求头加一个 IP 就能绕过基于 IP 的所有策略（封禁、限流、WAF）。

Bronx 的规则是：**只有 TCP peer 地址匹配 `trusted_proxies` 配置列表时，才从 XFF 解析客户端地址；否则直接使用 TCP peer 作为客户端地址**。`trusted_proxies` 是显式配置的 CIDR 列表，配置不存在等于没有可信代理。

解析在 `ClientAddr` 中间件里**做一次**，结果存入 `ReqCtx`。后续 IP-ban、限流、WAF、访问日志和上游转发头全部读取同一个解析结果——不会出现”限流用 A、日志记 B、封禁判断 C”的策略分裂。

**路由级认证：每条路由显式声明信任策略**

`authPolicy` 有四个选项：

| 选项 | 含义 | 适用场景 |
|---|---|---|
| `inherit` | 继承全局 JWT 默认值 | 大多数接口统一鉴权时 |
| `none` | 显式公开，不做认证 | 公共接口、健康检查 |
| `jwt` | JWT 验签 + 校验过期/issuer/audience/scope/role | 需要用户身份的接口 |
| `api_key` | 读取 header，SHA-256 比对，校验 scopes | 服务间调用 |

JWT 鉴权三个防御要点：

- **alg-lock**：配置里 `algorithm` 字段决定接受哪种算法（`HS256`/`RS256`/`ES256`），不接受 `alg=none`，不接受与配置不匹配的算法——防止攻击者把签名算法降级。
- **multi-kid 轮转**：支持配置多个 `kid`→`secret/pubkey` 映射，key rotation 期间新旧 key 并存，不强制客户端重新登录。
- **身份注入**：认证成功后可删除原始 `Authorization` header，向上游注入由网关生成的 `X-User-*` header——上游不需要信任客户端自带的身份声明。

**WAF：明确范围，不过度承诺**

轻量 WAF 扫描 URL path、query string、`User-Agent`、`Referer`、`Cookie` 中的 SQLi / XSS / 路径穿越 / 扫描器特征，命中后 403 并可向 daemon 上报风险（`Src::WAF`）。它**不扫描 request body**，也不是深度包检测或完整 WAF。

这个边界是有意为之的：request body 扫描需要完整缓冲（内存压力）或流式分段（状态机复杂度），收益不对称——绝大多数 SQLi/XSS 攻击通过 URL 或参数注入，body 里的攻击通常针对具体应用框架，不是网关层的防御重点。明确范围让部署者能正确评估还需要哪些额外防护，而不是误以为网关已经覆盖。

### 7. IP-ban 控制面：gateway/daemon 分离 + 零锁热路径 + epoch 对账

**为什么把封禁控制面做成独立 daemon 而不是网关内部模块？**

如果 IP 规则完全在网关内部维护，`banctl` 加规则需要和 gateway 进程通信（IPC 或 REST），规则生效需要等待网关下一次读锁，规则持久化需要在 gateway 进程里管理 SQLite——三个职责混在一个进程里，任何一个出故障都会影响网关主路径。

Bronx 的选择是职责分离：

```text
gw Reporter   --(submit_sock, UDS)--> Hub PolicyEngine     (上报风险)
gw Guard      <-(subscribe_sock, UDS)-- Hub Broadcaster    (接收规则快照/增量)
banctl        --(admin_sock, UDS)-->   Hub admin handler   (手动增删查)
                         |
                    SQLite persistence
```

三条 Unix domain socket 完全独立，每条只做一件事。网关不持有规则的写路径——只订阅和使用；Hub 不处理业务请求——只维护规则和推送。这让两边都可以单独重启而不互相破坏。

**热路径零锁：Guard + 原子快照**

网关侧的热路径只做两件事：取当前快照（一次原子 load）+ 查询（hash 精确匹配或 CIDR 向量扫描）。

```text
atomic<shared_ptr<Snap>> current_snap
  每次规则更新:
    合并静态 YAML 规则 + Hub 远程规则
    -> 编译新的不可变 Snap
    -> atomic store 替换
  请求处理:
    shared_ptr<Snap> snap = current_snap.load()  // 一次原子读
    -> snap->check(clientAddr)                    // hash v4/v6 精确 + CIDR vec 扫描
```

“不可变快照”的关键是：业务请求读取的 `Snap` 不会中途被修改。更新时生成新的 `Snap`，原子替换指针——正在用旧快照的请求继续用旧的，不持锁、不阻塞。Hub 断开时仍按最后有效快照工作，到期规则由网关本地 timer 兜底过期。

**自定义二进制协议而不是 HTTP/JSON**

ipban 的线路协议是固定 20 字节 header（magic `BAN1` + version + kind + bodyLen + seq）+ JSON body（≤8MB）。共 15 种消息类型，覆盖 HELLO/SNAP/DELTA/ACK/PING/PONG/REPORT/DENY 等。

选择自定义协议而不是 HTTP 的理由：长连接上需要双向推送（Hub → gateway 推规则，gateway → Hub 推风险），HTTP/1.1 不支持服务端主动推；WebSocket 可以双向但引入了一层握手和帧格式；自定义协议的 header 固定 20 字节，可以边读边解析，无需等完整 HTTP header。

**SyncClient 状态机与 epoch 对账**

SyncClient 有明确的状态机：`DOWN → DIAL → HELLO → SYNC → OPEN → BACKOFF`。在 `OPEN` 状态收 SNAP 或 DELTA，版本不匹配时请求重新同步（回到 `SYNC`），Hub 重启时 epoch 改变，SyncClient 识别后完整重新同步。

epoch 的必要性：Hub 每次启动生成新的随机 epoch，gateway 订阅时带上已知的 epoch 和 version。如果 epoch 不匹配，说明 Hub 重启过，旧的 DELTA 序列无效——gateway 请求完整 SNAP 重建，而不是把旧 DELTA 拼到新进程的规则序列上。

**SQLite 选择性持久化**

PolicyEngine 对规则落盘有阈值控制：只落永久规则（TTL=0）或**原始时长 ≥ `persistMinTtlMs=300000`（5分钟）** 的规则；短临时封（如”30秒令牌桶超限”）不落盘——重启窗口内这类规则多半已过期，落盘只增加 I/O 而没有价值。

SQLite 写经 `offload()` 移出 Hub reactor——持锁做 SQLite I/O 会死锁（同线程另一个协程等不到锁）。Hub 重启后从 SQLite load 过滤已过期的规则，epoch 重摇但 version 接上，保证重启后规则连续性。

### 8. 优雅停机：把取消协议延伸到整个生命周期

**信号隔离：屏蔽主线程，专用线程接收**

进程启动时，`BxApplication::init()` 在创建任何 worker 线程前先用 `pthread_sigmask` 屏蔽 `SIGINT`/`SIGTERM`/`SIGHUP`。所有后续创建的 worker（I/O reactor、CPU pool、日志 writer）都继承这个 mask，保证信号不会随机投递到某个正在写文件或持有锁的线程——那样会跳过清理逻辑直接终止进程。

随后由专用的 `signalLoop()` 线程调用 `sigtimedwait` 同步接收信号，根据信号类型调用 `onStopBegin` / `onReload`，清理流程在可控的调用栈里完成。

**drain → abortAll → ECANCELED 链路**

网关停机时：

1. **drain 监听器**：`TcpServer::drain()` 关闭监听 socket，停止 accept 新连接。
2. **等待活跃连接**：在配置的期限内（如 30s）等待活跃连接自然结束。期限到后：
3. **强制取消**：`TcpServer::closeAllConnections()` 遍历连接表，对每个连接调用 `socket->close()`。
4. **owner reactor 路由**：`BxSocket::close()` 读取该 fd 记录的 owner reactor，把 `abortAll(READ|WRITE)` 路由回 owner 的 `BxIoManager`。
5. **协程收到取消**：挂起在该 fd 上的读/写协程被 resume，系统调用返回 `ECANCELED`，业务代码看到错误后清理资源、退出协程。

这条链路的价值是：慢上游、keep-alive 连接、等待客户端 body 的协程都不会永久阻塞退出流程——它们会在 drain 期限到后收到显式取消，而不是被 `SIGKILL` 粗暴杀死。

Hub 的停机流程类似：先 drain HTTP 观察口（如果开了 `/healthz`），再 drain UDS 会话，最后停止 daemon 和 I/O manager。

**崩溃处理器：async-signal-safe 写栈到 crash.log**

`gw` 和 `hub` 在初始化完成后安装崩溃信号处理器（`SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL`/`SIGBUS`），捕获后：

1. 用 `backtrace()` + `backtrace_symbols_fd()` 把栈帧写到 `logs/crash.log`（这两个函数是 async-signal-safe）。
2. 恢复默认处理器，`raise()` 重新触发信号，让进程生成 core dump。

这不替代系统 core dump 或外部守护进程重启，但让异常至少有一个与实例日志同位置的诊断入口——尤其在没有配置 core pattern 的环境里。

### 9. 可观测性：运行状态直接来自业务对象，而不是旁路采集

**设计选择**：`/stats` 和 `/metrics` 不维护独立的 metrics 数据库，也不异步采集后写入时序库——它们直接从活跃的业务对象读取并格式化：

- `GatewayMetrics`：请求总数、各拒绝类型计数（IP-ban、WAF、认证失败、限流 429、维护模式、解析错误……），以 C++ 原子变量维护，读取时无锁。
- 当前 `ConfigSnapshot`：每条路由配置，每个 endpoint 当前状态（健康/熔断/连接数）。
- `CircuitBreaker` 每个 endpoint：当前状态（CLOSED/OPEN/HALF_OPEN），窗口失败率，上次转移时间。
- `Guard`/`SyncClient`：ipban 是否已同步，上次接收时间，ACK/PONG 超时次数，风险队列积压深度。
- `GatewayMetrics` 的 WebSocket 计数：当前连接数，累计 open/close，平均生命周期（histogram bucket）。

这一设计的好处是：指标永远反映当前实时状态，没有采集延迟；不需要额外进程或网络请求；指标不会"丢失"（进程在，数据就在）。代价是进程重启后历史数据不保留——这是已知的 trade-off，适合本机部署的单机网关场景。

`/metrics` 输出 Prometheus 文本格式，可直接接 scrape；`/stats` 输出 JSON，方便脚本消费。

**排障时能直接回答的问题**：

- 请求被哪一层拒绝？（解析错误 / IP-ban / WAF / 认证失败 / 限流 / 维护模式）
- 某路由某 endpoint 是否健康，是否在熔断 OPEN 状态，当前并发是否达到 `max_inflight`？
- 延迟在哪一段恶化？（网关处理 vs 上游连接 vs 上游响应）
- Hub 是否已同步，上次接收规则是多久前，风险队列是否积压？
- WebSocket 连接数是否异常，某长连接生命周期是否超预期？

**日志设计**：访问日志经 `StructuredAccessLog` 中间件以 JSON 格式输出，包含 `client_addr`、`request_id`、`route_id`、`upstream_addr`、响应码、各阶段耗时。每 logger 的令牌桶 `sample_rate` 在进程内控制低级别日志速率（`ERROR`/`FATAL` 豁免），File appender 定期 reopen（logrotate 无缝轮转），文件大小和保留期由部署环境的 logrotate 配置管理。

### 10. WebSocket 透明隧道：handshake 代理 + 双向流

WebSocket 升级不是常规 HTTP 请求——它需要网关代理 Upgrade 握手，验证服务端响应，然后把连接转为双向字节流，既不能按 HTTP request/response 来处理，也不能完全透传（网关需要判断升级是否成功）。

**实现流程**：

1. 检测请求带有 `Connection: Upgrade` + `Upgrade: websocket`（在 RateLimit 之后、Proxy 之前拦截）。
2. 向上游转发完整 Upgrade 请求，包含 `Sec-WebSocket-Key`，移除和重写逐跳头。
3. 读取上游响应，验证：状态码 `101`、`Connection: Upgrade`、`Upgrade: websocket`、`Sec-WebSocket-Accept`（base64(SHA1(key + GUID)) 必须匹配）。
4. 向客户端发送 `101`，随后启动**双向泵**（pump）：两个协程分别负责 client→upstream 和 upstream→client，任意一侧读到 EOF 或错误时关闭双方，退出协程。

对网关而言这是一条"终结链路"的路径——WebSocket 隧道建立后，该连接不再参与 HTTP 中间件链的后续，直到隧道关闭。泵内的读写是原始字节转发，WebSocket 帧格式由客户端和上游自行协商，网关不解析帧。

这个设计让 WebSocket 应用不需要配置额外端口或 TCP 穿透——使用与 HTTP 相同的路由规则、相同的认证（在 Upgrade 前已完成）、相同的 endpoint 治理（连接算 active_connections）。

### 归纳：核心设计选择与必须接受的边界

| 设计选择 | 核心收益 | 必须接受的边界 |
|---|---|---|
| 有栈协程 + epoll + libc hook | I/O 等待不占线程；超时、取消回到同一调度模型；同步代码形态 | 只面向 Linux/POSIX；性能仍以真实业务压测为准 |
| 协议歧义零容忍 | 请求走私在网关入口被拒绝，不传染给上游 | 严格解析会拒绝一些畸形但"能用"的客户端 |
| 固定中间件顺序 | 顺序即安全模型，行为可测试、可推理 | 不是任意加载第三方代码的插件平台 |
| 原子 business snapshot + 失败回滚 | 热更新失败不污染在线配置 | 监听地址、worker 数、栈资源不是在线伸缩能力 |
| endpoint 级治理 | 健康、熔断、连接池、限流、指标精确到单台上游 | 不提供服务发现、跨地域调度、全局负载均衡 |
| XFF 信任链显式配置 | 客户端无法伪造 IP 绕过 IP 策略 | 依赖运维正确配置 `trusted_proxies` |
| gateway/daemon 分离 + 零锁快照 + epoch 对账 | 风险闭环本机低延迟；两边独立重启；规则可恢复 | 不是跨节点共识/分布式策略系统 |
| drain → abortAll → ECANCELED | 停机时挂起协程收到显式取消，不被强杀 | drain 期限需按业务连接特征配置 |
| 指标直接来自业务对象 | 无采集延迟、无额外进程、指标不丢 | 进程重启后历史数据不保留 |

这些是本项目最有价值的工程决策：不是用更多组件堆砌功能，而是在一个可本机部署的 C++ 网关里，把并发 I/O、协议边界、安全策略、上游故障、规则同步、运行观测和停机清理组织成可追踪、可测试的闭环。

### 明确不做什么

- **不是 API 管理平台**：没有租户、开发者门户、计费、配额计划、动态服务发现或分布式配置中心。
- **业务面是 HTTP/1.1**：不做 TLS 终止、HTTP/2、HTTP/3；HTTPS/TLS 应在受信任的前置层（nginx/envoy）终止。
- **Hub 是本机控制面**：UDS + SQLite，不是跨节点共识、跨机集群或远程策略管理系统；多节点策略统一需要上层负责。
- **WAF 是轻量特征拦截**：不解析 request body，不能替代专业 WAF、完整漏洞防护或安全审计。
- **性能需实测**：吞吐上限、跨日稳定性、生产网络延迟、跨机丢包和前置 LB 兼容性需按实际部署单独压测。

## 配置参考

以下是完整的 YAML 配置字段说明。项目仓库里的 `api_gw/bin/*.yml` 是**占位示例**，不是可直接部署的生产配置——部署时按下表填写，发布后用 `GET /routes` 核对实际生效项。

### 配置文件与重载

配置分为框架层和业务层，不能混为一个 YAML：

| 进程 | 框架配置 | 业务配置 | 用途 |
|---|---|---|---|
| `gw` | `api_gw/bin/bronx.yml` | `api_gw/bin/gateway.yml` | I/O、协程栈、日志、CPU pool；路由、上游、鉴权和 IP-ban。 |
| `hub` | `api_gw/bin/hub_bronx.yml` | `api_gw/bin/hub.yml` | I/O、协程栈、日志、CPU pool；UDS、HTTP 观察端口和 SQLite。 |

`-c <path>`、`--config <path>` 或 `BRONX_CONFIG=<path>` 只覆盖框架 YAML。网关和 Hub 的业务 YAML 路径由进程入口固定在运行根的 `api_gw/bin/` 下。

重载也分两类：

```bash
# 重新加载 gw 的 gateway.yml。新快照构建失败会返回 500，旧快照继续服务。
curl -X POST http://127.0.0.1:9090/reload

# 重新加载 gw 的 bronx.yml。
kill -HUP "$(pidof gw)"
```

`SIGHUP` 会重新加载框架 YAML；当前日志配置和 `tcp.connect.timeout` 有运行时监听器。
CPU pool worker 数、I/O worker 数和协程栈分配器属于启动期资源，修改后应重启对应进程。修改
`hub.yml` 后同样应重启 `hub`；Hub 收到 `SIGHUP` 时只重载其框架 YAML。

生产测试不要直接修改 checkout 的 YAML 或 SQLite。`test/api_gw/run_soak.sh` 和 `test/loadfault/run_loadfault.sh` 会在 `/tmp` 创建隔离运行根、端口、UDS、日志和数据库。

### 网关 YAML

`api_gw/bin/gateway.yml` 是部署示例，不是 schema 的全部要求：除 `server.address`、
`server.io_workers` 和每条已启用路由所引用的上游外，其余字段可按需要省略并使用下列默认值。
当前业务解析器只读取九个顶层段：`server`、`upstreams`、`routes`、`rt_cache`、
`trusted_proxies`、`cors`、`auth`、`ip_filter`、`ip_policy`。未列出的业务字段不会产生功能，
也不会作为未知键报错；部署后应以 `GET /routes` 核对实际生效的路由、上游和治理参数。

九个顶层段各字段的默认值和约束按下面分表说明；最小可跑示例见前面的[部署与上手](#部署与上手)。

#### `server`：监听与启动期资源

`server` 由 `gw` 在启动时读取，不参与 `POST /reload`。改业务或管理监听地址、业务 worker 数、
启动维护态后必须重启 `gw`。

| 字段 | 默认值 | 约束和作用 |
|---|---:|---|
| `server.address` | `0.0.0.0:8090` | 业务 HTTP 监听地址。不能为空，否则启动失败。 |
| `server.admin_address` | `127.0.0.1:9090` | 管理 HTTP 监听地址。绑定失败会禁用管理口，但不阻止业务监听启动。只能放在 loopback 或受控私网。 |
| `server.io_workers` | `4` | 业务 `BxIoManager` worker 数，必须大于 `0`。管理口固定使用独立的一个 worker。 |
| `server.maintenance` | `false` | 启动时是否进入维护模式；开启后业务请求返回 `503` 和 `Retry-After: 30`，`/healthz` 仍可用。运行时用 admin 接口切换。 |

#### `upstreams[]`：上游、连接池、健康检查与熔断

每个元素是一组可由路由通过名字引用的 endpoint。加载时会解析每个 `host`；空 host 或无法解析的
endpoint 被跳过。组本身仍会存在，但没有可用 endpoint 的路由无法代理。所有 `*_ms` 均为毫秒。

| 字段 | 默认值 | 约束和作用 |
|---|---:|---|
| `name` | 无 | 非空上游组名；`routes[].upstream` 必须能引用它。 |
| `lb` | `round_robin` | 仅支持 `round_robin`、`weighted`、`least_conn`、`weighted_least_conn`；其他值记录警告并回退轮询。`weighted*` 才使用 endpoint `weight`。 |
| `timeout.total_ms` | `30000` | 单次代理请求总期限；小于 `1` 时钳制为 `1`。 |
| `timeout.connect_ms` | `3000` | 新建上游 TCP 连接期限；小于 `1` 时钳制为 `1`。 |
| `timeout.read_ms` | `30000` | 读取上游响应期限；小于 `1` 时钳制为 `1`。 |
| `limits.max_inflight` | `0` | 每个 endpoint 的最大在途连接数；`0` 表示不设限制，负值钳制为 `0`。到达上限的 endpoint 不参加选路。 |
| `connection_pool.max_idle` | `32` | 每个 endpoint 保留的最大空闲 keep-alive 连接数；`0` 表示不缓存空闲连接。 |
| `connection_pool.idle_timeout_ms` | `30000` | 空闲连接复用期限；小于 `1` 时钳制为 `1`。 |
| `health_check.enabled` | `false` | 启用后立即执行一次，并按周期发送 `GET <path>`。只把 `2xx`、`3xx` 视为成功。 |
| `health_check.path` | `/healthz` | 健康探测 HTTP 路径；空字符串实际请求 `/`。 |
| `health_check.interval_ms` | `5000` | 探测周期，至少 `1`。 |
| `health_check.timeout_ms` | `1000` | 单次 connect/send/recv 期限，至少 `1`。 |
| `health_check.healthy_threshold` | `1` | 连续成功达到此值才标记健康，至少 `1`。 |
| `health_check.unhealthy_threshold` | `2` | 连续失败达到此值才标记不健康，至少 `1`。 |
| `endpoints[].host` | 无 | 要解析的 hostname 或 IP；配置重载时重新解析。 |
| `endpoints[].port` | `80` | endpoint TCP 端口。 |
| `endpoints[].weight` | `1` | 加权策略的权重；`<=0` 钳制为 `1`，大于 `1000000` 钳制为该上限。 |

`circuit_breaker` 是 endpoint 级别，不是整个 upstream group 级别。它将连续失败、滚动窗口的失败率
和慢响应率组合使用；进入 OPEN 后等待，再以有限请求进入 HALF_OPEN，达到成功阈值才回 CLOSED。

| `circuit_breaker` 字段 | 默认值 | 作用与范围 |
|---|---:|---|
| `failure_threshold` | `5` | 连续失败阈值，至少 `1`。 |
| `window_ms` / `buckets` | `10000` / `10` | 失败率、慢响应率的滚动窗口和分桶数；二者至少 `1`，`buckets` 最大 `1024`。 |
| `min_requests` | `20` | 窗口样本不足时不按比例触发，至少 `1`。 |
| `failure_rate` / `slow_rate` | `50` / `50` | 失败率和慢响应率百分比，钳制在 `0..100`。 |
| `slow_ms` | `1000` | 响应达到此耗时即计为慢请求，至少 `1`。 |
| `open_timeout_ms` | `10000` | 初次 OPEN 的等待时长，至少 `1`。 |
| `max_open_timeout_ms` | `60000` | 反复失败时 OPEN 等待的上限，不能小于 `open_timeout_ms`。 |
| `half_open_max_requests` | `1` | HALF_OPEN 同时允许的探测请求数，至少 `1`。 |
| `half_open_successes` | `1` | 回到 CLOSED 所需连续成功数，范围 `1..half_open_max_requests`。 |
| `failure_statuses` | `[500,502,503,504]` | 计为上游业务失败的 HTTP 状态列表；仅保留 `100..599`，非法项跳过。 |

#### `routes[]` 与 `rt_cache`：匹配、改写、认证和限流

路由按 **exact 优先于 prefix** 匹配；prefix 只匹配完整路径段（`/api` 匹配 `/api/x`，不匹配
`/apix`）。同类规则优先顺序是：指定 `host`、指定 `methods`、更大的 `priority`、YAML 中更靠前。

| 字段 | 默认值 | 约束和作用 |
|---|---:|---|
| `name` | 空 | 用于指标、限流 bucket 和 `/routes` 展示；推荐唯一。 |
| `match_type` | `prefix` | `exact` 或 `prefix`；其他值会跳过整条路由。 |
| `path` | `/` | 匹配路径；空值回退 `/`，缺少前导 `/` 时自动补上。 |
| `upstream` | 无 | 被引用 upstream 必须存在，否则整条路由跳过。 |
| `host` | 空 | Host 匹配条件；空表示任意 host。 |
| `methods` | 空数组 | 允许的 HTTP 方法；空表示任意方法。 |
| `priority` | `0` | 同类候选的降序优先级。 |
| `strip_prefix` | `false` | 转发前去除匹配的 route path；去除后仍保证路径以 `/` 开头。 |
| `rewrite_prefix` | 空 | 在 `strip_prefix` 后再加此前缀；缺少前导 `/` 自动补上。 |
| `request_headers.set` | 空 map | 在转发前设置/覆盖请求头。 |
| `request_headers.remove` | 空数组 | 在 `set` 后删除指定头，因此同名字段以 `remove` 为准。逐跳头、客户端的转发头和 `Expect` 不会原样透传。 |
| `auth` 或 `auth.policy` | `inherit` | `inherit`、`none`、`jwt`、`api_key`。未知策略失败关闭，返回拒绝而非放行。 |
| `require_scopes` | 空数组 | JWT 要同时拥有列出的全部 scope；API key 也按其 `scopes` 检查。非标量或空项会跳过整条路由。 |
| `require_roles` | 空数组 | JWT 只要命中任一 role；API key 不支持 role，因此配置 role 的 API-key 路由会返回 `403`。 |
| `rate_limit.enabled` | `false` | 启用路由独立令牌桶。 |
| `rate_limit.capacity` / `refill_per_sec` | `0` / `0` | 桶容量与每秒回填量；负值钳制为 `0`。容量为 `0` 的启用路由会立即被限流。 |
| `rate_limit.key` | `ip` | `ip` 或 `user`；`user` 取 JWT `sub`，不存在时回退真实客户端 IP；其他值回退 `ip`。 |
| `rt_cache.cap` | `1024`（未配置时采用 Router 默认） | ARC 路由缓存容量；显式配置为 `0` 会使加载失败，旧快照继续服务。 |

#### 客户端 IP、CORS、认证和 IP-ban

| 字段 | 默认值 | 作用与限制 |
|---|---:|---|
| `trusted_proxies` | 空数组 | 可信前置代理 CIDR 列表。只有 peer 在此列表才采信 XFF；无效 CIDR 记录警告后跳过。不要写入不受控的网段。 |
| `cors.enabled` | `true` | 是否添加 CORS 响应头和处理预检。 |
| `cors.allow_origin` | `*` | 单一允许 origin；仅在 `allow_origins` 未设置时使用。 |
| `cors.allow_origins` | 空数组 | 多 origin 白名单；含 `*` 时，若 `allow_credentials=true`，实际回显请求 Origin，不能同时返回通配符凭据。 |
| `cors.allow_methods` / `allow_headers` | `GET, POST, PUT, DELETE, OPTIONS` / `Content-Type, Authorization` | 允许的方法和请求头字符串，原样写入预检响应。 |
| `cors.allow_credentials` | `false` | 允许浏览器携带凭据。 |
| `cors.max_age` | `86400` | 预检缓存秒数；钳制到 `0..INT_MAX`。 |
| `cors.short_circuit_preflight` | `true` | 合法预检是否不进入业务代理；关闭时仍设置 CORS 头，但请求继续经过后续链路。 |
| `auth.jwt.enabled` | `false` | 是否启用全局 JWT；路由 `inherit` 在启用时等价于 `jwt`，否则等价于 `none`。 |
| `auth.jwt.algo` | `HS256` | 仅 `HS256`、`RS256`、`ES256`，大小写无关；非法值使 JWT 认证拒绝。 |
| `auth.jwt.secret` | 空 | HS256 对称密钥。不要把真实密钥提交到仓库。 |
| `auth.jwt.public_key` | 空 | RS256/ES256 PEM 公钥。 |
| `auth.jwt.issuer` / `audience` / `leeway_sec` | 空 / 空 / `0` | 非空时校验 issuer/audience；`leeway_sec` 是过期和生效时间的时钟容忍。 |
| `auth.jwt.keys[]` | 空数组 | 公钥轮换项：每项必须有唯一 `kid`、`public_key`，且 `algo` 与全局算法一致；有效的 `keys` 不能同单一 `public_key` 混用。 |
| `auth.api_key.header` | `X-API-Key` | 读取 API key 的请求头名。 |
| `auth.api_key.keys[]` | 空数组 | 推荐 `{id, hash, enabled, scopes}`，`hash` 必须是 64 位 SHA-256 hex；标量或 `{key: 明文}` 是兼容格式，会记录弃用警告。 |
| `auth.forward.enabled` | `false` | 将通过认证的身份注入上游头；客户端预先伪造的同前缀身份头会先被清除。 |
| `auth.forward.strip_bearer` / `strip_api_key` | `true` / `true` | JWT 或 API-key 成功认证后，是否删除原始凭据头再转发。 |
| `auth.forward.prefix` | `X-User-` | 注入身份头前缀，例如 `X-User-Id`、`X-User-Scopes`、`X-User-Roles`。 |
| `ip_filter.enabled` | `false` | 是否启用静态 IP 规则；解析成功后装入网关 Guard。 |
| `ip_filter.mode` | `denylist` | `denylist` 命中拒绝；`allowlist` 未命中拒绝；其他值回退 `denylist`。 |
| `ip_filter.cidrs` | 空数组 | IPv4 CIDR 列表；任一非法 CIDR 会让本次 business snapshot 构建失败。 |
| `ip_policy.submit_sock` | 空 | Reporter 提交 WAF/限流风险的 UDS；应与 Hub `submit_sock` 一致。留空时只本地拦截、不提交风险。 |
| `ip_policy.subscribe_sock` | 空 | Guard 订阅 Hub 快照/增量的 UDS；应与 Hub `subscribe_sock` 一致。 |
| `ip_policy.instance_id` | `gw-1` | 订阅客户端标识；多个网关实例使用相同 Hub 时必须不同。 |
| `ip_policy.waf.enabled` / `ban_ms` | `false` / `600000` | 轻量 WAF 开关与建议封禁时长。WAF 只扫描 URL、query、`User-Agent`、`Referer`、`Cookie`，不扫描 body；命中返回 `403`。 |
| `ip_policy.rate_report.enabled` / `hits` / `window_ms` / `ban_ms` | `false` / `20` / `60000` / `300000` | 同一客户端在窗口内达到指定 429 次数时提交一次限流风险；需同时有可用 Reporter 才会送 Hub。 |

网关还有固定启用但**当前不提供 YAML 参数**的行为：`/healthz` 业务健康端点、`X-Request-Id`、结构化
访问日志、安全响应头、WebSocket Upgrade 透明隧道和代理转发头卫生。它们是当前中间件链的一部分，不要
凭空添加诸如 `security_headers`、`websocket`、`redirects` 或 `tls` 段；这些键不会启用对应功能。

### Hub YAML

`api_gw/bin/hub.yml` 只读取 `server` 段；其余顶层字段和 `server` 下未列字段均不生效。
这些值只在 Hub 启动时读取，修改后重启 `hub`。Hub 的框架 YAML 是另一个文件
`api_gw/bin/hub_bronx.yml`。

```yaml
server:
  submit_sock: "/tmp/bronx_ip_submit.sock"
  subscribe_sock: "/tmp/bronx_ip_subscribe.sock"
  admin_sock: "/tmp/bronx_ip_admin.sock"
  http_address: "127.0.0.1:9091"
  db_path: "api_gw/bin/db/hub.db"
  iom_workers: 2
```

| 字段 | 默认值 | 作用与部署要求 |
|---|---:|---|
| `server.submit_sock` | `/tmp/bronx_ip_submit.sock` | 网关 Reporter 写入 WAF/限流风险的 UDS；不能为空。应只允许本机可信 `gw` 进程访问。 |
| `server.subscribe_sock` | `/tmp/bronx_ip_subscribe.sock` | 网关 Guard 订阅规则快照、Delta、ACK/PING/PONG 的 UDS；不能为空。 |
| `server.admin_sock` | `/tmp/bronx_ip_admin.sock` | `banctl` 使用的管理 UDS。任何可以连接它的本机账号都能修改封禁规则，应以目录权限限制。 |
| `server.http_address` | `127.0.0.1:9091` | 只提供只读 `/healthz`、`/metrics` 的 HTTP 观察口。绑定失败会使 Hub 启动失败。 |
| `server.db_path` | 空 | SQLite 数据库路径；空值只使用内存规则，Hub 重启后丢失。相对路径相对运行根解析；生产应使用独立数据目录和备份策略。 |
| `server.iom_workers` | `2` | Hub I/O worker 数，必须大于 `0`。 |

Hub 和网关必须使用同一组 `submit_sock`、`subscribe_sock`。若放宽 UDS 文件权限、把 `admin_sock` 暴露给不可信用户，等同于授予对封禁规则的管理权限。

### Bronx 框架 YAML

`bronx.yml` 与 `hub_bronx.yml` 使用同一套已注册框架配置。下表是当前 `bronx_core` 注册的**全部
框架 YAML 键**；业务 YAML 中的 `server`、`routes`、`upstreams` 不会被框架配置系统消费。
框架加载器将键名折叠为小写，未注册的键会被忽略，因此拼错键名不会替你启用功能。

```yaml
cpu_pool:
  threads: 2
  max_queue: 0
  name: cpu

fiber:
  stack_size: 131072
  stack_pool_size: 8
  guard_page: true

tcp:
  connect:
    timeout: 5000

tcp_server:
  recv_timeout: 120000

logs:
  - name: root
    level: info
    sample_rate: 0
    appenders:
      - type: BxFileLogAppender
        file: logs/gw.log
        pattern: "[%d{%Y-%m-%d %H:%M:%S}][%p][%c] %m%n"
        async: true
      - type: BxStdoutLogAppender
        format: json
        async: false
```

| 配置 | 默认值 | 生效时机和作用 |
|---|---:|---|
| `cpu_pool.threads` | `0` | CPU offload worker 数；`0` 时取硬件并发数的一半，最低为 `1`。CPU pool 只在进程初始化时创建，改后重启。 |
| `cpu_pool.max_queue` | `0` | CPU pool 等待队列上限；`0` 表示无上限。启动期读取，改后重启。 |
| `cpu_pool.name` | `cpu` | CPU pool 线程名前缀。启动期读取，改后重启。 |
| `fiber.stack_size` | `131072` | 新协程默认栈字节数。进程运行后会用于新建协程；不把它当作已有协程栈的热更新。 |
| `fiber.stack_pool_size` | `8` | 每线程、每种栈尺寸缓存的空闲栈数。首次创建子协程时锁定栈池配置，改后重启。 |
| `fiber.guard_page` | `true` | 是否为协程栈设置 guard page；首次创建子协程时锁定，生产建议保持启用。 |
| `tcp.connect.timeout` | `5000` | hook 后非阻塞 `connect` 的超时毫秒数。已注册变更监听器，`SIGHUP` 成功加载框架 YAML 后会更新。 |
| `tcp_server.recv_timeout` | `120000` | 新建 `BxTcpServer` 的默认接收超时毫秒数；服务器对象构造时拷贝，改后重启服务进程。 |
| `logs` | 空 | logger 定义数组；加载或 `SIGHUP` 时按名称创建、更新或删除 logger 和 appender，是可热更新项。 |
| `logs[].name` | 必填 | logger 名称。常用 `root`、`system`、`gw`、`hub`；缺失会使这份日志配置加载失败。 |
| `logs[].level` | 未知级别 | `debug`、`info`、`warn`、`error`、`fatal`；建议明确配置，不依赖未识别级别。 |
| `logs[].sample_rate` | `0` | **每个 logger 每秒**允许输出的低级别日志数量；`0` 不限流。`ERROR` 和 `FATAL` 不受采样丢弃影响。该项会随框架热重载立即更新。 |
| `logs[].appenders[]` | 空 | 一个 logger 的输出目标列表。没有 appender 的具名 logger 回退 root。 |
| `appenders[].type` | 必填 | 只能是 `BxFileLogAppender` 或 `BxStdoutLogAppender`；其他值被跳过。 |
| `appenders[].file` | 必填（File） | 文件输出路径；相对路径相对进程运行根。日志目录需要由部署提前创建且可写。 |
| `appenders[].pattern` | 默认 formatter | 自定义文本格式。 |
| `appenders[].async` | `false` | 是否异步写此 appender。 |
| `appenders[].format` | 空 | 仅 `json` 启用 JSON formatter，且 JSON 优先于 `pattern`。 |

### 管理接口和规则管理

所有 HTTP 管理接口都**没有内建认证**，也不应由业务公网入口反代暴露。`gw` admin 和 Hub HTTP
管理口应只绑定 loopback，或在受控私网中再由带认证的反向代理、ACL 和防火墙保护。

先设置地址变量：

```bash
GW_ADMIN=http://127.0.0.1:9090
HUB_ADMIN=http://127.0.0.1:9091
```

#### 网关 admin HTTP

| 请求 | 成功响应 | 用途和使用时机 |
|---|---|---|
| `GET /healthz` | `200 OK`，正文 `OK` | 仅检查 admin server 可接受请求。用于进程/监听存活探针，不等价于上游或 Hub 健康。`curl -fsS "$GW_ADMIN/healthz"`。 |
| `GET /stats` | `200`，JSON | 运行快照：总请求和 1xx-5xx、每条路由 2xx/4xx/5xx、每 endpoint 健康/在途/连接池/熔断、认证、限流、WAF、WebSocket、延迟、route cache、IP-ban 同步状态。排障时先看 `upstream_fail`、`no_healthy_endpoint`、`circuit_open`、`ipban_link_state`。`curl -fsS "$GW_ADMIN/stats"`。 |
| `GET /metrics` | `200`，Prometheus text `version=0.0.4` | 给 Prometheus scrape。包含 `gateway_requests_total`、`gateway_request_seconds_*`、`gateway_upstream_*`、`gateway_circuit_*`、`gateway_active_connections`、认证/CORS/维护状态及 IP-ban 指标。`curl -fsS "$GW_ADMIN/metrics"`。 |
| `GET /routes` | `200`，JSON | 查看**当前快照实际生效**的路由与上游，含匹配条件、限流参数、健康检查、endpoint 权重、连接池和熔断状态。配置发布后用它检查路由没有因无效字段/未解析上游被跳过。`curl -fsS "$GW_ADMIN/routes"`。 |
| `POST /reload` | 成功 `200 reloaded`；失败 `500 reload failed` | 重新读 `gateway.yml` 并构建新业务快照。只成功时原子切换；失败保留旧快照。它重载上游、路由、CORS、认证、IP 规则和 IP-policy，不重新绑定 `server` 地址、不改变 worker 数、不重设启动维护态。`curl -fsS -X POST "$GW_ADMIN/reload"`。 |
| `GET /maintenance` | `200`，`{"maintenance":true|false}` | 查询当前业务维护态。 |
| `POST /maintenance/on` | `200 maintenance on` | 立即让业务请求短路为 `503`，用于发布/故障隔离；admin `/healthz` 仍可用。`curl -fsS -X POST "$GW_ADMIN/maintenance/on"`。 |
| `POST /maintenance/off` | `200 maintenance off` | 立即恢复业务放行。`curl -fsS -X POST "$GW_ADMIN/maintenance/off"`。 |

管理口只接受上述路径和方法；其他请求返回 `404`。`/stats` 与 `/routes` 是运行数据，不应把响应中的
endpoint、路由和内部拓扑公开给不可信调用方。

#### Hub HTTP 与 `banctl`

| 请求 | 成功响应 | 用途 |
|---|---|---|
| `GET /healthz` | `200 ok` | Hub daemon 正常且 SQLite 已启用时数据库可用；Hub 未启动或数据库失败返回 `503 db failed`。`curl -fsS "$HUB_ADMIN/healthz"`。 |
| `GET /metrics` | `200`，Prometheus text | Hub 版本、规则数、订阅会话、风险提交、规则变更、SQLite 写入/恢复及错误计数。`curl -fsS "$HUB_ADMIN/metrics"`。 |

规则变更只通过 `banctl` 的 admin UDS，不通过 Hub HTTP：

```bash
# 读取当前规则；--json 便于自动化解析
./bin/banctl list
./bin/banctl --json list

# 手工允许或封禁 CIDR；TTL 省略、0 或 perm 都表示永久
./bin/banctl allow 198.51.100.0/24 1h trusted-proxy
./bin/banctl deny  198.51.100.9/32 30m manual-block

# 按 CIDR 删除 admin 规则，或按 list 输出的规则 ID 删除
./bin/banctl unban 198.51.100.9/32
./bin/banctl del admin:198.51.100.9

# 非默认 UDS 和连接/接收超时
./bin/banctl --sock /run/bronx/hub-admin.sock --timeout 5000 list
```

TTL 支持 `30s`、`10m`、`2h`、`7d`；纯数字按毫秒解释。退出码 `0` 表示成功，`1` 用法错误，`2`
为 Hub 拒绝，`3` 为 UDS 不可达，`4` 为协议错误。处理应急封禁时先 `list`，再执行变更并再次 `list`
确认版本和规则；不要依赖 HTTP 观察口完成写操作。

## 测试复现

测试默认不参与构建，且不是 CTest 注册测试；构建 target 后直接运行可执行文件：

```bash
cmake -S . -B build-bx-tests -DCMAKE_BUILD_TYPE=Debug -DBRONX_BUILD_TESTS=ON
cmake --build build-bx-tests --target test_gw_proxy test_ipban_resync --parallel 4
./build-bx-tests/test/gateway/test_gw_proxy
./build-bx-tests/test/ipban/test_ipban_resync
```

API gateway 进程测试使用真实 `gw`、`hub`、`banctl`：

```bash
cmake --build build-bx-tests --target \
  test_api_gw_process test_api_gw_chain test_api_gw_stress \
  test_api_gw_lifecycle test_api_gw_journey --parallel 4
./build-bx-tests/test/api_gw/test_api_gw_chain
```

隔离浸泡使用真实二进制并自行创建临时 YAML、端口、UDS、日志和 SQLite，不修改 checkout 的运行配置：

```bash
API_GW_SOAK_GW_BIN="$PWD/bin/gw" \
API_GW_SOAK_HUB_BIN="$PWD/bin/hub" \
API_GW_SOAK_BANCTL_BIN="$PWD/bin/banctl" \
SOAK_SECONDS=300 bash test/api_gw/run_soak.sh
```

故障注入会自行启动隔离网关，需要预先安装 `vegeta` 和 toxiproxy：

```bash
LF_GW_BIN="$PWD/bin/gw" bash test/loadfault/run_loadfault.sh
```

fuzz 使用独立 Makefile 和 clang 的 ASan/UBSan，不复用主 CMake 的 sanitizer 构建：

```bash
make -C test/fuzz
make -C test/fuzz run-req
```

k6 证据编排要求你先在**隔离测试环境**启动匹配的 gw、hub 和至少两个受控 mock upstream，并显式提供
PID、mock 端口和慢上游路径；它会记录版本、配置 SHA、每阶段 JSON/log 和进程资源快照：

```bash
# 将 PID、端口和慢路径替换为隔离测试环境的实际值
K6_GW_PID=1234 K6_HUB_PID=1235 \
K6_MOCK_PORTS=18080,18081 K6_SLOW_API_PATH=/controlled/slow-path \
K6_EVIDENCE_SOAK_DURATION=15m bash test/k6/run_evidence.sh
```

不要把 k6 的 WAF、限流、重载或 IP-ban 场景直接指向生产网关；脚本会主动制造风险事件和规则，再执行
清理。需要单独运行健康混合脚本时，参见 [k6 报告](test/TEST_REPORT_2026-07-22_K6.md) 中的运行配置。

## 测试索引与证据

- [测试索引](test/TEST_MAP.md)
- [测试报告](test/TEST_REPORT.md)
- [测试范围和边界](test/TEST_SCOPE.md)
- [k6 结果](test/TEST_REPORT_2026-07-22_K6.md)
- [隔离浸泡结果](test/TEST_REPORT_2026-07-22_SOAK.md)
- [故障注入结果](test/TEST_REPORT_2026-07-22_LOADFAULT.md)
- [解析器 Fuzz 结果](test/TEST_REPORT_2026-07-22_FUZZ.md)
