# API 网关串行复现套件

该目录用于在单机、小型服务、单核和 512 MiB/1 GiB/2 GiB 总预算下，串行执行 Bronx、
Spring Cloud Gateway、Kong DB-less 和 Kong + PostgreSQL 的完整 API 请求链测试。测试对象
不会并发运行，每次命令都会创建新的结果目录，成功与失败证据都不会覆盖。

Nginx 不在比较对象中。本套件比较的是包含 JWT、WAF、IP 封禁、限流、路由、指标、日志、
WebSocket、在线变更和转发的 API 网关链，而不是裸反向代理。

## 一键命令

在本目录执行：

```bash
# 20 秒夹具验证，不得作为正式性能证据
./run.sh --case smoke-20s --products bronx,spring,kong-dbless,kong-postgres

# 正式串行测试：每个产品预热 60 秒、固定 1000 RPS 综合负载 5 分钟
./run.sh --case full-5m --profile 512m
./run.sh --case full-5m --profile 1g
./run.sh --case full-5m --profile 2g

# 单核内存档位；默认是 512m
./run.sh --case smoke-20s --profile 1g
./run.sh --case smoke-20s --profile 2g

# 只执行指定对比线
./run.sh --case full-5m --products bronx,spring
./run.sh --case full-5m --products bronx,kong-dbless,kong-postgres
```

产品顺序按 `--seed` 确定性打乱并写入 `product-order.txt`，但始终严格串行。可用
`--products` 固定参与者，用 `--results-root` 指定证据根目录。命令退出码非零表示至少一个产品、
事件、在线断言、汇总或 manifest 失败。

## 前置条件

- Linux、cgroup v2、可用的 user systemd。
- 当前仓库 `bin/gw`、`bin/hub`、`bin/banctl` 已构建。
- Java 21、Maven、Docker、Python 3、PyYAML、k6、Vegeta、curl、jq、gzip、sha256sum。
- Docker 能拉取 `kong:3.9.1-ubuntu` 和 `postgres:17-alpine`；实际 image digest 会逐轮保存。
- 默认被测 CPU 是 CPU 2，可用 `--target-cpu` 修改。

Spring 依赖会由 Maven 固定版本构建。Kong 镜像按 manifest digest 固定。第一次运行需要下载
依赖和镜像，准备/构建时间不计入 5 分钟正式负载窗口。可提前执行以下命令拉取全部 Spring
运行依赖和 Maven 插件，并用离线构建确认本地仓库完整：

```bash
mvn -f products/spring/pom.xml -DskipTests dependency:go-offline
mvn -o -f products/spring/pom.xml -DskipTests clean package
```

## 每轮证据

```text
results/<UTC>-<随机后缀>/
  case.json
  metadata.env
  product-order.txt
  source.sha256
  source/                    # 本轮 runner、适配器、产品补充源码和 mock 源码
  host/                      # CPU、内存、内核、Git 状态和工作树补丁
  products/<product>/
    capabilities.json
    configs/                 # 渲染配置、源码/插件和镜像/JAR 摘要
    events.jsonl
    events/
    logs/
    raw/                     # k6/Vegeta 原始输出或 gzip
    resources.csv            # 每秒进程与实际 cgroup 样本
    runtime/                 # 隔离运行根；PostgreSQL/SQLite 状态也保留
    snapshots/
    summary.json
    status.txt
  report.md
  summary.json
  status.txt
  manifest.sha256
```

`manifest.sha256` 在所有进程停止、日志封存后生成。可校验：

```bash
cd results/<run-id>
sha256sum -c manifest.sha256
```

容器进程属于其他 UID 时，普通用户通常无法读取 `/proc/<pid>/smaps_rollup`，因此 Kong 的
PSS 显示为 `null`；容器实际 cgroup `memory.current`/峰值仍被采样，RSS 作为补充保留。不能把
缺失 PSS 解读为零占用。

正式流量、事件和资源采样共享 `formal-start-epoch-ms.txt` 中的绝对时钟。资源采样严格覆盖从
该锚点开始的用例时长，不包含锚点前 5 秒等待和计时后的 graceful stop。`summary.json` 的
`http_rps` 按 `http_requests / measurement_duration_s` 计算；k6 把 setup 等待计入其 rate 的
原始值另存为 `k6_reported_http_rps`，不能用后者进行横向比较。

## 能力口径

- Bronx：网关能力和 Hub 控制面均计入资源。
- Spring：HTTP/WS 与路由使用 Spring Cloud Gateway；JWT/scope 使用 Spring Security OAuth2
  Resource Server + Nimbus；限流使用 SCG `RequestRateLimiter` + Bucket4j/Caffeine；熔断使用
  Spring Cloud CircuitBreaker + Resilience4j；负载均衡和健康检查使用 Spring Cloud
  LoadBalancer；指标使用 Actuator/Micrometer。IP 策略由官方 `XForwardedRemoteAddr` 谓词和
  Actuator RouteDefinition 组合。请求防火墙使用官方 `StrictServerWebExchangeFirewall`，但
  本轮四类攻击匹配仍是测试规则配置，只能视为部分 WAF，不能描述成完整 WAF/OWASP CRS。
- Kong：JWT、IP restriction、rate limiting、Prometheus 等使用官方插件；WAF/scope 使用本目录
  Lua 插件，标记为 `PASS-CUSTOM`。Kong rate limiting 是固定窗口语义。
- Kong + PostgreSQL：两个容器合计限制为 1 CPU/512 MiB，并汇总两个 cgroup。
- Bronx 高精度 trace 在事件窗口手动开启；Spring 和 Kong CE 都没有等价的运行时高精度
  trace 开关，明确记录为 `UNSUPPORTED`，不伪造日志或状态变化。

正常、路由、解封后的 ban 和限流放行请求不只检查 200，还检查上游实例名、方法、改写后路径、
配置版本头、内部头删除和 request-id 端到端一致性。正常、路由、WebSocket、故障期可用率和
恢复期可用率分别设硬门禁，不能被总体语义成功率平均掉。

`smoke-20s` 只证明脚本、事件、清理和证据链可运行。发布性能结论必须使用 `full-5m`，并遵守
上级 [测试设计书](../TEST_DESIGN.md) 的复测、能力等价和结论边界。

## 清理边界

每个 adapter 只停止带本轮哈希名称的 systemd unit 或 Docker container，只删除本轮 Bronx
创建的三个 `/tmp/bronx_cmp_*` socket。不会修改 `api_gw/bin/*.yml`、生产 SQLite、现有 Demo、
其他容器或其他结果目录。运行目录和所有失败证据默认保留。
