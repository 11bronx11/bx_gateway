# gw 可管理高精度日志手册

本文说明 `gw` 的两类日志、admin 控制面、`/trace` 的完整参数、字段含义，以及如何据此区分上游问题和 gw 自身问题。以当前源码和 `api_gw/bin/bronx.yml` 为准。

## 1. 先建立全貌

一次业务请求有三层可观察性：

```text
客户端 -> gw 中间件链 -> 上游
              |               |
              |               +-- trace: 获取连接、请求、响应头、流式回写
              +-- system: 一条最终结构化访问记录

admin (/stats, /metrics, /routes): 当前计数、端点和熔断状态
```

- `gw_system.log` 始终记录系统事件和每条业务请求的最终结构化访问记录，适合全量统计和按 `request_id` 查找。
- `gw_trace.log` 默认关闭。它按条件记录一条请求从解析、各中间件、上游通信到流式回写的过程，适合一次具体故障的取证。
- `/stats` 和 `/metrics` 是运行中计数器，适合和日志交叉验证，但不携带单请求 ID，不能单独替代 trace。

## 2. 文件位置和用途

当前 `gw` 的框架日志配置在 `api_gw/bin/bronx.yml`。相对路径 `logs/...` 在当前运行方式下落在项目根的 `logs/` 目录。

| 文件 | logger | 内容 | 是否常开 |
| --- | --- | --- | --- |
| `logs/gw_system.log` | `system` | 结构化访问日志、配置 reload、trace 开关、维护模式、路由/上游/协议/WAF 等警告 | 是 |
| `logs/gw_trace.log` | `trace` | 命中过滤条件的全链路明细 | 文件常开，记录开关默认关 |
| `logs/gw.log` | `gw` | `api_gw/gw.cpp` 的进程启动、绑定、框架 reload、优雅停止等 | 是 |
| `logs/gw_root.log` | `root` | 没有命名 logger 的框架级输出 | 是 |
| `logs/hub_system.log`、`logs/hub.log`、`logs/hub_root.log` | hub | hub/IP-ban 控制面，不是 gw 请求链 | hub 运行时 |
| `logs/crash.log` | crash handler | 崩溃路径输出 | 按需 |

常用观察命令：

```bash
tail -F logs/gw_system.log
tail -F logs/gw_trace.log
tail -F logs/gw.log
```

日志框架默认前缀为：

```text
2026-07-25 08:38:03  54017  iom_0  55  [INFO]  [trace]  src/gateway/upstream.cpp:559  [c4#2.148] up   rsp ...
|时间(秒)              |线程ID |线程名 |fiber |级别    |logger |源文件:行号                         |正文
```

同一条 trace 可能跨 worker/fiber 落盘。文件使用异步 appender，且普通前缀的时间精度只有秒，所以不要只按文件物理行序或时间排序判断因果关系；应使用 trace 前缀中的序号，见第 6 节。

## 3. `gw_system.log` 的结构化访问记录

业务请求完成后，`system` logger 会写一条 JSON 正文，例如：

```json
{
  "ts":1784968397265,
  "request_id":"9739ffc3febe0000",
  "method":"POST",
  "path":"/v1/messages",
  "status":502,
  "latency_ms":2687,
  "duration_ms":2687,
  "ip":"127.0.0.1",
  "route":"/",
  "upstream":"headroom",
  "auth":"other",
  "auth_result":"ok"
}
```

| 字段 | 含义 | 取证时的注意点 |
| --- | --- | --- |
| `ts` | 请求进入 `ReqCtx` 时的 Unix epoch 毫秒时间戳 | 比日志行前缀精确；不是响应结束时刻 |
| `request_id` | `X-Request-Id`。客户端带了就沿用；未带则由 gw 生成 16 位十六进制值，同时注入上游请求和响应头 | 是跨 system、客户端和上游日志最实用的关联键；不是 trace 的 `[c...#...]` 编号 |
| `method` | HTTP 方法 | 例如 `GET`、`POST`、`HEAD` |
| `path` | HTTP 路径，不带 query string | trace 的 `req head` 和 `up req` 会带 query，二者不能按全文直接比较 |
| `status` | gw 最终尝试发送给客户端的 HTTP 状态码 | 它可能是上游原样响应，也可能由 gw 中间件/代理层合成，不能只凭这个字段定责 |
| `latency_ms` | 从 `ts` 到响应被提交（普通响应已发送，流式响应头已提交）的时间 | 对流式响应通常接近首字节/响应头时间，不等于整条流结束 |
| `duration_ms` | 从 `ts` 到整个中间件链返回的时间 | 流式响应包含 body 转发时间，通常大于或等于 `latency_ms` |
| `ip` | 解析后的客户端 IP | 只有对端位于 `trusted_proxies` 时才采用受信任的转发地址；否则是直连 peer IP |
| `route` | 命中路由的 `routeKey` | 空字符串表示请求在路由前短路，或未写入路由结果；此时不要把它当成上游请求 |
| `upstream` | 路由选中的上游组名称 | 仅代表路由选择，**不证明** gw 已连接到上游，更不证明上游返回了该状态码 |
| `auth` | 认证方案标签 | 常见为 `jwt`、`api_key`、`other`；在认证中间件之前短路时可能为空 |
| `auth_result` | 认证/授权结果 | `ok`、`missing`、`bad_token`、`bad_key`、`bad_sig`、`expired`、`bad_iss`、`bad_aud`、`bad_alg`、`scope`、`role`、`bad_policy`；未执行时为空 |

### 3.1 先用 system 做什么

```bash
# 只看最终 5xx 访问记录（保留带 JSON 的行）
grep '"status":5' logs/gw_system.log

# 按请求 ID 定位一条请求
grep -F '9739ffc3febe0000' logs/gw_system.log

# 看管理动作：trace、维护模式、reload
grep -E 'trace (ON|OFF)|maintenance mode|gateway config reloaded' logs/gw_system.log
```

`system` 足以告诉你“哪条请求最终是什么状态、匹配了哪个路由”。如果要回答“502 是上游 HTTP 响应，还是 gw 在连接/发送/解析时合成的”，需要当次请求的 trace，或结合第 8 节的 metrics。

## 4. Admin 控制面

当前运行配置把 admin 绑定在 `127.0.0.1:9090`。admin 服务没有独立的 HTTP 认证层，必须保持在 loopback 或由受控网络层保护，不能直接暴露公网。

```bash
export GW_ADMIN=http://127.0.0.1:9090
```

| 方法和路径 | 是否改变状态 | 用途 |
| --- | --- | --- |
| `GET /healthz` | 否 | 进程存活探针，返回 `OK` |
| `GET /trace` | 否 | 查看 trace 是否开启及当前过滤器；同时结算已过期 TTL |
| `POST /trace/on?...` | 是 | 用完整参数集替换当前 trace 规则并开启 |
| `POST /trace/off` | 是 | 立即关闭 trace，后续在途请求打点也停止 |
| `GET /stats` | 否 | JSON 汇总计数、路由统计、上游统计和连接数 |
| `GET /metrics` | 否 | Prometheus 文本指标，包含上游失败原因和熔断状态 |
| `GET /routes` | 否 | 当前快照中的路由、端点、连接池、健康和熔断配置/状态 |
| `POST /reload` | 是 | 重载 `gateway.yml`，不是日志开关；排障时不要为了看日志而调用 |
| `GET /maintenance` | 否 | 查看维护模式 |
| `POST /maintenance/on`、`POST /maintenance/off` | 是 | 开/关维护模式；会影响业务请求，排障时谨慎使用 |

建议先只读确认：

```bash
curl -fsS "$GW_ADMIN/trace"
curl -fsS "$GW_ADMIN/stats"
curl -fsS "$GW_ADMIN/routes"
curl -fsS "$GW_ADMIN/metrics"
```

## 5. `/trace` 的完整用法和参数

### 5.1 三个接口

```bash
# 查看当前状态
curl -fsS "$GW_ADMIN/trace"

# 开启。必须是 POST；所有参数都在 query string。
curl -fsS -X POST \
  'http://127.0.0.1:9090/trace/on?level=1&ip=127.0.0.1&path=/v1/messages&sample=1&ttl=120'

# 关闭。应在一次观察结束后执行。
curl -fsS -X POST "$GW_ADMIN/trace/off"
```

`GET /trace` 在关闭时返回：

```json
{"on":false,"level":0}
```

开启时会额外返回 `ip`、`path`、`sample`、`expires_in_ms`。`expires_in_ms=-1` 表示用 `ttl=0` 配置的永久 trace。

### 5.2 参数表

`POST /trace/on?level=<...>&ip=<...>&path=<...>&sample=<...>&ttl=<...>` 会**整体替换**上一条规则，不会对旧规则增量合并。

| 参数 | 默认值 | 允许/实际行为 | 匹配与注意事项 |
| --- | --- | --- | --- |
| `level` | `1` | 只会得到 `1` 或 `2`。小于 1 的值被钳为 1，大于 2 的值被钳为 2 | `level=0` 不是关闭；它仍会开启 level 1。关闭只能调用 `POST /trace/off` |
| `ip` | 空 | 任意非空值按字符串精确匹配 | 空表示不按 IP 过滤；不是 CIDR，也不支持通配符。该 IP 是 gw 解析后的 client IP，受 `trusted_proxies` 影响 |
| `path` | 空 | 请求 path 的字节前缀匹配 | 空表示不按路径过滤。`path=/api` 同时匹配 `/api/x` 和 `/api2`；要限制路径段时用 `path=/api/`。不匹配 query string |
| `sample` | `1` | `1/N` 采样，`1` 是每条匹配请求 | 先通过 `ip` 和 `path` 后才参与采样；首次匹配请求命中，之后按全局匹配序号每 N 条命中一次。只应传 `1` 到 `4294967295` 的普通十进制整数 |
| `ttl` | `600` | 秒。`0` 表示不自动过期 | 非数字、负数或会造成毫秒截止时间溢出的值返回 HTTP 400。过期后 trace 自动关闭 |

参数解析的细节：

- 未提供或提供空值时使用默认值/空过滤器。
- 未识别的参数被忽略；同名参数出现多次时，解析器取第一个。
- query 参数**不做 URL 解码**。普通路径中的 `/` 可以原样传；不要把 `/v1/messages` 写成 `%2Fv1%2Fmessages`，后者不会匹配实际路径。
- `level` 和 `sample` 当前使用宽松整数转换。为了得到可预测结果，传正常十进制；特别不要传超过 `uint32_t` 的 `sample`，否则强制转换可能绕回为 `0`，等价于全量而非预期采样。
- `ttl=0` 会一直开启到显式 `/trace/off`、进程退出或下一次 `/trace/on` 覆盖规则为止。生产排障应优先用有限 TTL。

### 5.3 level 1 和 level 2 的差别

| 内容 | level 1 | level 2 |
| --- | --- | --- |
| 请求头摘要、链开始/结束、每层中间件回程、上游获取/请求/响应、客户端响应、最终收尾 | 有 | 有 |
| 中间件进入行 `mw N name ->` | 无 | 有 |
| 每次客户端 socket `recv` 和 body buffer 到达 | 无 | 有 |
| 流式 body 中间进度 | 每 50 块或距离上次记录满 1 秒 | 每 10 块或距离上次记录满 1 秒 |
| 首块和末块 body 汇总 | 有 | 有 |
| 建议 | 常规单请求排障 | 短时间、窄过滤的字节/分块/中间件时序排障 |

trace 不记录请求体和响应体内容，也不记录原始 header 值；但 `req head` 和 `up req` 会记录包含 query string 的请求行。不要把 token、密钥或用户隐私放在 URL query 中，并把 trace 文件按敏感运行日志管理。

### 5.4 直接可用的命令

```bash
# 1. 只看某个客户端和某个 API，常用且低噪音
curl -fsS -X POST \
  'http://127.0.0.1:9090/trace/on?level=1&ip=127.0.0.1&path=/v1/messages&sample=1&ttl=120'

# 2. 不看 IP，捕捉 /v1 下每 10 条中的 1 条，保留 5 分钟
curl -fsS -X POST \
  'http://127.0.0.1:9090/trace/on?level=1&path=/v1/&sample=10&ttl=300'

# 3. 不看 IP、也不限制 path，短时间全量高精度捕捉
#    这会显著增加写盘和日志量，只应短时使用。
curl -fsS -X POST \
  'http://127.0.0.1:9090/trace/on?level=2&sample=1&ttl=300'

# 4. 跟随记录，等待复现
tail -F logs/gw_trace.log

# 5. 复现结束后立即关闭，并确认状态
curl -fsS -X POST "$GW_ADMIN/trace/off"
curl -fsS "$GW_ADMIN/trace"
```

## 6. trace 行前缀和时序

请求级前缀形如 `[c4#2.148]`：

| 部分 | 含义 |
| --- | --- |
| `c4` | 当前进程内的 trace 连接序号 |
| `#2` | 同一 keep-alive 连接上的第 2 个 HTTP 请求 |
| `.148` | 此连接上的第 148 条 trace 行序号，用于重建顺序 |

连接级事件没有 `#请求号`，例如 `[c4.197] cli recv ...`、`[c4.198] conn close ...`。连接级标识不是 `request_id`，它只在当前进程生命周期内有意义。

异步落盘和 fiber 换 worker 会让同一请求的行出现“后发生的行先写入文件”。按某一请求重建顺序时，先筛选其前缀，再按 `.序号` 数值排序：

```bash
# 将 c4 上第 2 个请求的 trace 按内部序号还原
grep -F '[c4#2.' logs/gw_trace.log \
  | sed -E 's/.*#[0-9]+\.([0-9]+)\].*/\1 &/' \
  | sort -n -k1,1 \
  | cut -d' ' -f2-
```

上面的命令只用于查看；日志文件本身不需要也不应为了排序而改写。

## 7. trace 事件和字段字典

### 7.1 请求、中间件和连接事件

| 事件 | 字段解释 |
| --- | --- |
| `req  head  METHOD /path?query framing=... clen=N hdrs=N ka=0|1` | 请求行摘要。`framing` 为 body 定界：`none`、`CL`、`chunked`、`until_close`；`clen` 为解析的 Content-Length；`hdrs` 为 header 数；`ka=1` 表示 keep-alive |
| `chain start mws=N` | 进入 N 个中间件组成的请求链 |
| `mw I NAME ->` | level 2 的进入行。`I` 是链中下标，`NAME` 是中间件名 |
| `mw I NAME pass|SHORT|end status=S self=Xms tot=Yms` | 回程行。`pass` 表示该层调用了 `next()`；`SHORT` 表示该层没有继续且响应状态改变；`end` 表示未继续且状态未改变。`self` 不含下游耗时，`tot` 含下游耗时 |
| `chain end depth=A/B status=S state=... cost=Xms` | 实际走到的深度/总层数、最终上下文状态、整条中间件链耗时。状态可为 `open`、`sent`、`stream`、`tunnel`、`write_fail` |
| `req  done  status=S cost=Xms cut=... [write_fail] [stop]` | 连接处理循环的收尾。`cut` 为 `none`、`bad_header`、`body_too_large`、`headers_too_large`、`bad_body`、`no_response`；`write_fail` 表示向客户端写失败；`stop` 表示此连接不再复用 |
| `cli  recv n=N buf=N [errno=N]` | level 2，客户端 socket 读到的字节数和输入缓冲可读字节数；负数并带 `errno` 是读失败/超时线索 |
| `cli  body recv buf=N` | level 2，收到请求体数据时的当前 buffer 大小，不记录 body 内容 |
| `conn close reqs=N` | 连接关闭，累计处理了 N 个 HTTP 请求 |

### 7.2 上游和客户端回写事件

| 事件 | 字段解释 |
| --- | --- |
| `up   acquire ep=HOST:PORT reused=0|1 cost=Xms` | 已获得上游连接。`reused=1` 是连接池复用，`cost` 包含取得/新建连接所花时间 |
| `up   acquire FAIL why=... cost=Xms` | 未取得上游连接。`why` 见下表；该路径没有收到上游 HTTP 响应 |
| `up   req METHOD /path?query HTTP/1.1 head=NB cost=Xms [fresh_conn]` | 发往上游的实际请求行，已反映 strip/rewrite；`head` 是请求头总字节数；`fresh_conn` 表示不是复用连接 |
| `up   reqbody NB/Kblk framing=A->B cost=Xms` | 请求体已转发的总字节/块数，以及客户端到上游的定界转换 |
| `up   rsp S ttfb=Xms framing=... clen=N sse=0|1 hdrs=N mark=ok|fail` | 已成功解析到上游响应头。`S` 是上游原始 HTTP 状态；`ttfb` 是从开始转发到读完上游响应头；`sse=1` 表示 Content-Type 以 `text/event-stream` 开头；`mark` 是熔断统计判定，不会改写该 HTTP 状态 |
| `cli  rsp S framing=... hdrs=N` | gw 已准备向客户端回写的响应头。`S` 是该响应的状态，`framing` 是客户端侧使用的 body 定界 |
| `cli  body#1 n=N t=Xms` | 客户端真正收到的第一个 body 块。`t` 相对响应 body 开始，不是整个请求开始 |
| `cli  body~ blk=N bytes=N t=Xms` | 流式 body 的聚合进度，`blk`/`bytes` 为累计值 |
| `cli  bodyend blk=N bytes=N cost=Xms ok|body_error|client_write_fail` | 响应 body 收尾。`client_write_fail` 是客户端断开/写失败，并不等于上游请求失败 |
| `up   finish mark=... why=... status=S cost=Xms reusable=0|1` | 上游 body 已处理后的最终归还结果。`reusable=1` 才会放回连接池 |
| `up   end RESULT mark=... why=... status=S cost=Xms reusable=0|1` | 转发的异常出口。`RESULT` 说明 gw 内部转发结论，通常 `status=0` 表示根本没有解析到上游 HTTP 响应 |

`why` 的取值：

| `why` | 含义 |
| --- | --- |
| `none` | 无失败原因 |
| `connect` | 建连上游失败 |
| `send` | 向上游发送请求头或请求体失败 |
| `timeout` | connect/read/send 或总截止时间超时 |
| `bad_resp` | 上游响应头/协议无法解析或不合规 |
| `bad_body` | body 传输/读取异常 |
| `status` | 已收到上游 HTTP 响应，但该状态被当前熔断配置列为失败状态 |
| `busy` | 上游并发/获取限制拒绝 |
| `open` | 熔断器处于打开状态 |
| `down` | 没有可用端点或端点被判不健康 |
| `deadline` | 总请求截止时间已到 |

`mark` 的取值为 `ok`、`fail`、`skip`。特别地，`mark=fail why=status` 不等于 gw 合成了错误：它表示 gw 已读到上游状态，且该状态被熔断策略计为失败；gw 随后会把该上游状态复制到客户端响应。

## 8. 用日志判断 502、503、504 的责任边界

access log 的 `upstream="headroom"` 只说明路由选了 `headroom`。要定责，按以下优先级看 trace：

| 观察到的证据 | 结论 |
| --- | --- |
| `up rsp 502 ...`，后面有 `cli rsp 502 ...` | gw 已收到直接上游的 HTTP 502 并原样回给客户端。gw 到该上游这一跳不是连接/解析失败；如果该上游本身又是代理，还需查看它自己的日志定位更下游原因 |
| `up rsp 503 ...` | 同理，是直接上游返回 503 |
| `up acquire FAIL why=connect|timeout|busy|open|down|deadline` | 没有拿到可用上游连接。属于 gw 的上游获取/治理路径；最终客户端状态要结合后续 `mw` 和 system 的 `status` 看 |
| `up end CONNECT_FAIL|SEND_FAIL|BAD_RESPONSE ...` | gw 没有可用的上游 HTTP 响应，代理中间件会合成 HTTP 502 |
| `up end UPSTREAM_TIMEOUT ...` | gw 的代理中间件会合成 HTTP 504 |
| `up end UPSTREAM_BUSY ...` | gw 的代理中间件会合成 HTTP 503 |
| `mw ... maintenance SHORT status=503`，同时 system 的 `route`、`upstream` 为空且 `latency_ms=0` | gw 维护模式在路由/上游前短路，503 是 gw 自己返回 |
| `mw ... router SHORT status=503` | 路由已到达但无上游注册表/上游组，503 是 gw 自己返回 |
| `cli bodyend ... client_write_fail` | 客户端写失败或断开；不要误归为上游 5xx |

源码中的实际映射是：`CONNECT_FAIL`、`SEND_FAIL`、`BAD_RESPONSE` -> 502；`UPSTREAM_TIMEOUT` -> 504；`UPSTREAM_BUSY` -> 503。上游已经产生的普通 HTTP 响应不走这组映射。

## 9. `/stats` 和 `/metrics` 如何配合

### 9.1 先看摘要

```bash
curl -fsS "$GW_ADMIN/stats"
```

重点字段：

| 字段 | 含义 |
| --- | --- |
| `requests`、`1xx` 到 `5xx` | 请求和最终响应分类计数 |
| `upstream_ok`、`upstream_fail` | 上游转发统计结果计数 |
| `upstream_acquire_fail` | 获取上游连接失败次数 |
| `no_healthy_endpoint` | 无健康端点次数 |
| `circuit_open` | 熔断打开次数 |
| `maint_blocked` | 维护模式拦截次数 |
| `routes` | 按路由聚合的请求和响应类计数 |
| `upstreams[].requests` | 按上游端点的 `result` 和 `reason` 聚合 |
| `upstreams[].rejected` | 获取阶段被拒绝的原因聚合 |
| `upstreams[].circuit` | 当前熔断状态与滚动窗口统计 |

### 9.2 精确看上游失败原因

```bash
curl -fsS "$GW_ADMIN/metrics" \
  | grep -E 'gateway_upstream_requests_total|gateway_upstream_rejected_total|gateway_upstream_acquire_fail_total|gateway_no_healthy_endpoint_total|gateway_circuit_state|gateway_maintenance_blocked_total'
```

最重要的 Prometheus 指标：

```text
gateway_upstream_requests_total{
  upstream="headroom",
  endpoint="127.0.0.1:8788",
  result="fail",
  reason="status"
} 4
```

这表示当前端点累计有 4 次“已收到上游状态、该状态被判失败”的结果；若同一时间段 system 中恰有四条该路由的 502，即可强力支持“上游返回状态码”的结论。若 `reason` 为 `connect`、`send`、`timeout`、`bad_resp` 等，则是 gw 的上游通信路径失败。

指标是累计快照，没有 `request_id`，而且端点对象可能随配置 reload 重建。因此它用于趋势和与同一观察窗口的日志计数对账；对某一条历史请求的最终判定，仍以当次 trace 为准。

## 10. 规则切换、TTL 和并发语义

`POST /trace/on` 并不是逐字段修改共享对象：它在互斥锁内构造并替换一份不可变 `TraceFilter` 快照，然后重置采样序号并以原子 level 开关发布。因此新请求看到的是完整的旧规则或完整的新规则，不会看到半写入的 `ip/path/sample/ttl` 组合。

不过它也不是“对所有在途请求重新匹配”的规则引擎：

- 请求头解析后，gw 对该请求调用一次 `want(ip, path)`，把是否命中记在 request/connection 的 trace tag 上。
- 后续 `/trace/on` 改规则不会让此前未命中的在途请求开始记录，也不会按新 `ip/path` 重新过滤此前已命中的请求。
- `/trace/off` 和 TTL 过期通过全局 level 生效，已命中的在途请求后续打点会立刻停止。现有 `test_api_gw_trace` 覆盖了这两个行为。
- `GET /trace` 会主动结算 TTL，因此即使一直没有新业务请求，也能正确报告到期后为 `on:false`。

## 11. 推荐排障流程

1. 先读 `system` 和 admin：确定时间段、`request_id`、最终状态、路由、上游，以及有无维护模式、熔断或端点不健康。
2. 对将要复现的问题，先开一个最窄的 level 1 trace，设置有限 `ttl`；只有需要逐次 recv 或更密的流式进度时再升 level 2。
3. 复现一次。用 `[c#req.seq]` 重建该请求链，先找 `up rsp` 或 `up end`，再看 `cli rsp` 和 `req done`。
4. 立即 `/trace/off`，保存对应的 system/trace 时间窗和 `/metrics` 输出。
5. 若 trace 显示 `up rsp 5xx`，再去查直接上游的日志；若显示 `acquire FAIL` 或 `up end`，则从 gw 的连接池、端点健康、网络和 timeout 配置查起。

## 12. 日志轮转和运行注意事项

仓库提供 `api_gw/bin/logrotate.conf`，目标是项目根 `logs/*.log`：单文件超过 50 MiB 轮转，保留 7 份，压缩历史文件。它只是 logrotate 规则，不代表已经安装了定时任务；state 文件路径由调用时的 `-s` 参数决定。

当前规则没有 `copytruncate`，也没有在轮转后通知 gw 重新打开文件描述符的 `postrotate`。因此不要把一次在线 `logrotate` 当成无需验证的无中断操作：先在部署方式中明确“进程何时 reopen 日志”（受控重启或已有的 reopen 机制），再启用轮转。下面命令只做解析演练，不实际轮转：

```bash
logrotate -d -s logs/.logrotate.state api_gw/bin/logrotate.conf
```

trace logger 必须保持独立文件、异步写、`info` 级别；不要给它配置额外的 logger `sample_rate`，否则会静默丢失 trace 行，破坏一条请求的时序证据。

## 13. 源码入口

| 主题 | 源码 |
| --- | --- |
| admin 路由、参数解析、`/stats`、`/metrics`、`/routes` | `src/gateway/admin.cpp` |
| trace 过滤器、级别、前缀、采样、TTL、并发语义 | `src/gateway/include/trace.h`、`src/gateway/trace.cpp` |
| 请求头、socket 读取、请求收尾 trace | `src/gateway/conn.cpp` |
| 中间件链 trace 和状态语义 | `src/gateway/mw.cpp` |
| 上游连接、转发、TTFB、流式 body、失败原因 | `src/gateway/upstream.cpp`、`src/gateway/include/up_ret.h` |
| gw 合成 502/503/504 的映射 | `src/gateway/include/middlewares/proxy.cpp` |
| structure access log 的 JSON 字段 | `src/gateway/include/middlewares/builtin.cpp` |
| trace 运行配置和文件路径 | `api_gw/bin/bronx.yml` |
| admin trace 进程级回归测试 | `test/api_gw/test_api_gw_trace.cpp` |
