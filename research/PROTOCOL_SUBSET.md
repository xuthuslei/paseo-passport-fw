# Paseo daemon 客户端协议子集（ESP32-C3 固件实现依据）

> 调研日期：2026-10-22。依据：github.com/getpaseo/paseo `main` 分支源码逐文件核实（每节标注出处文件）。
> 目标：固件只实现本文档列出的子集；未列出的报文一律忽略。拿不准处明确标「未核实」，实现时以实测为准。

## 1. 配对 offer 格式

依据：`packages/protocol/src/connection-offer.ts`

- 形态：`https://app.paseo.sh/#offer=<payload>`，payload 是 JSON 的 **base64url** 编码（解码规则：`-`→`+`、`_`→`/`、补 `=` 填充，再 UTF-8 解码 + JSON 解析，见该文件 `decodeBase64UrlToUtf8` / `parseConnectionOfferFromUrl`）。
- 当前只有 v2 一个版本（`ConnectionOfferSchema = ConnectionOfferV2Schema`），字段全集：

| 字段 | 类型 | 说明 |
|---|---|---|
| `v` | 字面量 `2` | 版本 |
| `serverId` | string（≥1） | daemon 的稳定标识（作用域为 daemon 的 `PASEO_HOME`），同时是 relay 会话标识 |
| `daemonPublicKeyB64` | string（≥1） | daemon 的 X25519 公钥，**标准 base64**（非 base64url），解码后 32 字节 |
| `relay.endpoint` | string（≥1） | relay 端点，`host:port` 形式（如官方 `relay.paseo.sh:443`，见 `packages/protocol/src/daemon-endpoints.ts` 的 `DEFAULT_RELAY_ENDPOINT`） |
| `relay.useTls` | boolean，可选 | 是否 TLS；缺省处理见下 |

- `useTls` 缺省时：`daemon-endpoints.ts` 有回退函数 `shouldUseTlsForDefaultHostedRelay`（注释标为旧 offer 迁移回退）——端口为 443 时按 TLS 处理。固件规则：`useTls` 为 true 或端点端口为 443 → 用 `wss://`。

## 2. 客户端连接时序

### 2.1 连 relay（WebSocket）

依据：`packages/protocol/src/daemon-endpoints.ts`（`buildRelayWebSocketUrl`）、`packages/relay/src/cloudflare-adapter.ts`

- URL：`wss://<endpoint>/ws?serverId=<serverId>&role=client&v=2`（`ws://` 同理）。
- 客户端**不带** `connectionId`：由 relay 分配，并通过控制通道通知 daemon；daemon 随即为该 connectionId 打开一条 server data socket 与客户端配对。
- 配对完成前客户端发出的帧会被 relay 缓冲（每 connectionId 最多 200 帧，`bufferFrame`），daemon data socket 接入后按序 flush。所以 WS 一连通就可以立刻发 `e2ee_hello`，不必等配对。
- 控制帧 `{"type":"sync",...}` / `{"type":"connected",...}` / `{"type":"disconnected",...}` **只发给 daemon 的控制 socket**（role=server 无 connectionId 的那条），客户端永远收不到，固件不要实现。
- daemon 侧掉线时，relay 会以关闭码 **1012**（reason `Server disconnected`）关掉客户端连接 → 固件须整链重连并重做 E2EE 握手。

### 2.2 E2EE 握手

依据：`packages/relay/src/encrypted-channel.ts`（`createClientChannel`）、`packages/relay/src/crypto.ts`

1. 客户端本地生成 X25519 密钥对（tweetnacl `nacl.box.keyPair()`，公私钥各 32 字节）。
2. 用 offer 中的 daemon 公钥派生共享密钥：`deriveSharedKey` = `nacl.box.before(peerPub, mySecret)`（即 NaCl box 预计算密钥，底层 XSalsa20-Poly1305；派生时先做 `scalarMult` 并拒绝全零结果）。
3. 客户端以**明文 WS 文本帧**发送：
   `{"type":"e2ee_hello","key":"<自己公钥的标准 base64>","capabilities":{"binaryCiphertext":true}}`
4. daemon 回**明文文本帧**：`{"type":"e2ee_ready"}`，同意二进制密文时带 `"capabilities":{"binaryCiphertext":true}`。
5. 官方客户端每 **1000ms** 重发 hello 直到收到 ready（`HANDSHAKE_RETRY_MS`）；握手未完成时应用层 send 会被排队（上限 200 条，`MAX_PENDING_SENDS`）。
6. 固件简化选项：hello **不带** `capabilities` 字段 → 全程走 base64 文本帧模式（见 2.3），逻辑最简，daemon 兼容此形态（ready 不带 capabilities 即表示未协商二进制）。

### 2.3 加密帧格式（通道建立后）

依据：`packages/relay/src/crypto.ts`（`encrypt`/`decrypt`）、`packages/relay/src/encrypted-channel.ts`（`EncryptedChannel.send/handleMessage`）

- 每次加密**随机生成 24 字节 nonce**（`nacl.randomBytes(24)`），密文 = `nacl.box.after(明文, nonce, sharedKey)`（XSalsa20-Poly1305，含 16 字节 Poly1305 MAC）。
- 线束格式（二进制）：`[nonce 24B][ciphertext...]`，总开销固定 40 字节（`ENCRYPTED_PAYLOAD_OVERHEAD_BYTES = 40`）。
- **没有序号、没有计数器型 nonce**：nonce 是纯随机的，收发双方不维护任何同步状态；加密层本身不做重放防护。固件不需要维护计数器，但必须保证随机数源质量（见风险 3）。
- WS 帧表示规则：
  - 明文是**字符串**（daemon 协议的 JSON 报文全部是字符串）→ 线束做**标准 base64** 后以 **WS 文本帧**发送。即使协商了 `binaryCiphertext`，字符串仍走 base64 文本帧；只有明文为 ArrayBuffer 且已协商二进制时才发 WS 二进制帧。
  - 固件结论：**只实现 base64 文本帧收发即可覆盖全部 JSON 协议**。二进制帧只承载终端流/文件传输/宿主路由（均不在子集内）；固件若收到二进制帧直接丢弃即可（未协商 binaryCiphertext 时，上游 legacy 接收路径也只认文本帧的 base64）。
- 解密失败、或在加密通道上收到明文 JSON 应用帧，官方实现视为致命错误并以 **1011** 关连接。固件同策略：出错断开、整链重连，不要试图带病继续。

### 2.4 daemon 协议握手（加密通道之上）

依据：`packages/protocol/src/messages.ts`（`WSHelloMessageSchema` 等 WS 层 schema）、`packages/client/src/daemon-client.ts`（`sendHelloMessage`）

加密通道一 open，客户端立即发送（加密后的 JSON 文本）：

```json
{"type":"hello","clientId":"<稳定字符串>","clientType":"cli","protocolVersion":1,
 "capabilities":{"hello_rejection":true,"explicit_event_subscriptions":true}}
```

- `clientId` 必填（min 1）；`clientType` 枚举为 `mobile|browser|cli|mcp|hub`，没有嵌入式专属值——固件借用 `cli`（这也是官方 DaemonClient 的默认值，属实现选择，非协议规定）。
- `protocolVersion` 当前为 `1`（整数）。
- `capabilities` 是 passthrough 对象，以上两个键是能力线名（`packages/protocol/src/client-capabilities.ts`：`hello_rejection`、`explicit_event_subscriptions`）。`voice`、`pushNotifications` 等其余键均可选，固件不声明可减少 daemon 给我们推大块数据的面。
- 可选 `auth`：daemon 设了密码时为 `{"kind":"password","password":"..."}`，或 `{"kind":"localCredential","token":"..."}`。自用无密码 daemon 不需要。
- 拒绝报文（顶层，非 session 信封）：`{"type":"hello.rejected","reason":"password_required"|"incorrect_password"|"incompatible_protocol","accepts":["password"]}`。
- 成功后，daemon 以 session 信封推送服务器信息（`ServerInfoStatusPayloadSchema`）：

```json
{"type":"session","message":{"type":"status","payload":
  {"status":"server_info","serverId":"...","protocolVersion":1,"version":"...","features":{...}}}}
```

  其中 `features` 是大量可选布尔门（如 `providersSnapshot`），固件只需取 `serverId` 做一致性校验。**未核实**：server_info 的确切发送时机（hello 后无条件主动推送，还是需先订阅 `status.server_info` 事件）未在 server 源码逐行核实——稳妥做法是 hello 后紧接着按 4.2 发事件订阅且把 `status.server_info` 包含在内，两种时机都能覆盖。

## 3. 通用信封与 RPC 约定

依据：`packages/protocol/src/messages.ts`（`WSInboundMessageSchema` / `WSOutboundMessageSchema`）、`packages/client/src/daemon-client.ts`

- 除少数顶层报文外，所有会话报文都包在信封里：`{"type":"session","message":{...}}`，内层以 `message.type` 判别。
- 顶层（非 session）报文只有：`ping`、`pong`、`hello`、`hello.rejected`、`recording_state`（出站方向为 `pong`、`hello.rejected`、`session`）。
- 请求类报文带 `requestId`（客户端生成的字符串，官方用 UUID）；响应报文的 `payload.requestId` 回显同一值做关联。响应报文 type 与请求成对出现。
- 官方客户端 RPC 默认超时 60s（`DEFAULT_SESSION_RPC_TIMEOUT_MS`）。固件可设 15–30s，超时即认为连接异常。

## 4. 最小功能报文子集

以下每条均出自 `packages/protocol/src/messages.ts` 对应 schema（名字在括注中）。所有请求/响应都是 §3 的 session 信封内层报文。

### 4.1 拉取 agent 列表及状态

- 请求 `fetch_agents_request`（`FetchAgentsRequestMessageSchema`）：
  `{type, requestId, scope?:"active", includeBackground?:bool, page?:{limit, cursor?}, subscribe?:{subscriptionId?}, sort?, filter?}`
- 响应 `fetch_agents_response`（`FetchAgentsResponseMessageSchema`）：
  `payload{requestId, subscriptionId?, entries:[{agent, project, syncSeq?}], pageInfo:{nextCursor, prevCursor, hasMore}}`
- `entries[].agent` 是 agent 快照（`AgentSnapshotPayloadSchema`），固件关心的字段：
  `id`、`provider`、`cwd`、`model`、`title`、`createdAt`/`updatedAt`/`lastUserMessageAt`、`status`、状态
  `pendingPermissions[]`、`requiresAttention`、`attentionReason`、`attentionTimestamp`、`lastError`、`persistence`（续接历史会话用的句柄，可空）。
- **状态枚举**（`AgentStatusSchema` → `packages/protocol/src/agent-lifecycle.ts` 的 `AGENT_LIFECYCLE_STATUSES`，全集）：
  `initializing` | `idle` | `running` | `error` | `closed`
- 注意：**「等待审批」不是独立状态值**。agent 处于 `running`（或 `idle`）且 `pendingPermissions` 非空、或
  `requiresAttention=true` 且 `attentionReason="permission"`（枚举 `finished|error|permission`）即表示等审批。屏幕上的状态灯按这个组合判断。

### 4.2 订阅状态变更推送

两条路线，固件用 (b) 为主、(a) 的响应报文也要能认：

(a) 目录订阅：`fetch_agents_request` 带 `subscribe` 字段后，daemon 持续推送变更，报文包括：
- `agent_update`（`AgentUpdateMessageSchema`）：`payload{kind:"upsert", agent:<完整快照>, project?, seq?}` 或 `{kind:"remove", agentId}`
- `agent_status`（`AgentStatusMessageSchema`）：`payload{agentId, status:<字符串>, info:<完整快照>}`
- 注意这两种推送**每次都带完整 agent 快照**，没有增量瘦身版。

(b) 显式事件订阅（需 hello 里声明 `explicit_event_subscriptions` 能力，v0.8.0 起的模型）：
- 请求 `session.events.set_subscription.request`（`SessionEventsSetSubscriptionRequestSchema`）：
  `{type, requestId, events:[<事件名>...], notifications?:bool}`
- 响应 `session.events.set_subscription.response`
- 事件名全集（`SessionEventSubscriptionSchema`）：`project.update`、`providers_snapshot_update`、`agent_attention_required`、`agent_permission_request`、`agent_permission_resolved`、`checkout_status_update`、`script_status_update`、`workspace_setup_progress`、`agent.provider_subagents.update`、`terminal_attention_required`、`status.server_info`、`status.daemon_config_changed`、`status.plugin_catalog_changed`、`status.plugin_settings_changed`、`activity_log`、`hub.execution.agent.update`、`hub.execution.agent.stream`
- 固件只订阅四个：`agent_attention_required`、`agent_permission_request`、`agent_permission_resolved`、`status.server_info`。相关推送报文：
  - `agent_attention_required`：`payload{agentId, reason:"finished"|"error"|"permission", timestamp, shouldNotify, notification?:{title, body, data{...}}}`
  - `agent_permission_request`：`payload{agentId, request:<权限请求对象，见 4.4>}`
  - `agent_permission_resolved`：`payload{agentId, requestId:<权限请求 id>, resolution:<回应对象>}`
- 单个 agent 的时间线流订阅是另一条 `agent.timeline.set_subscription.request`（`{type, agentIds:[...], requestId}`）——固件默认不用（见 4.5 的 payload 风险）；只有用户在设备上打开某个 agent 详情时才临时订阅这一个 agent。

### 4.3 给 agent 发送文本指令

续接已有会话（`SendAgentMessageRequestSchema`）：
- 请求 `send_agent_message_request`：
  `{type, requestId, agentId, text, messageId?, sessionMode?, activeTurnBehavior?, images?, attachments}`
  - `agentId` 服务端可解析全 ID、唯一前缀或精确标题（schema 注释），固件应使用全 ID。
  - `messageId` 是客户端提供的去重 ID，建议填，避免重连重发造成重复执行。
  - `attachments` 在 schema 中**不是** `.optional()`（对比 `agent.create.request` 里它是可选数组）——即该报文字段必填。**未核实**：`AgentAttachmentsSchema` 的空值形态（空数组是否可接受）未展开核实，实现时先给 `[]` 并实测。
  - `activeTurnBehavior`：agent 正忙（running）时新消息的排队/打断策略，枚举未展开核实，固件可不填走服务端默认。
- 响应 `send_agent_message_response`：`payload{requestId, agentId, accepted:bool, deliveryStatus?:"queued", error:string|null}`

新建会话（`AgentCreateRequestSchema` = `CreateAgentRequestMessageSchema` 扩展）：
- 请求 `agent.create.request`：
  `{type, requestId?, config:{provider, cwd, modeId?, model?, thinkingOptionId?, title?, systemPrompt?...}, initialPrompt?, workspaceId?, idempotencyKey?, agentId?:<uuid>, subscribe?:bool, attachments?:[...]}`
  - `config.provider` 与 `config.cwd` 是必填的两个字段（`AgentSessionConfigSchema`），其余皆可选。
  - `initialPrompt` 即首条指令，一条报文完成「建会话 + 下指令」。
  - `idempotencyKey`（≤512 字符）防重连重复建会话，建议填。
- 响应 `agent.create.response`（`AgentCreateResponseSchema`）：`payload{requestId, agent:<快照|null>, error:string|null, creation?}`
- 另有 legacy 的 `create_agent_request`（同 config 结构）；其响应报文类型**未核实**，固件用上面的新版即可。

附带（可选）：`cancel_agent_request` `{agentId, requestId?}` 可用于「停止」键；`fetch_agent_request` `{requestId, agentId}` 拉单个 agent 的最新快照（审批前刷新详情用）。历史（已持久化）会话的恢复是 `resume_agent_request` `{handle:<快照里的 persistence>, requestId}`，固件子集可暂不实现，标可选。

### 4.4 审批 / 驳回权限请求

依据：`AgentPermissionRequestPayloadSchema`、`AgentPermissionResponseMessageSchema`、`AgentPermissionResponseSchema`（均在 messages.ts）

- 待审批请求的来源：快照里的 `pendingPermissions[]`，或 4.2 的 `agent_permission_request` 推送。请求对象字段：
  `{id, provider, name, kind:"tool"|"plan"|"question"|"mode"|"other", title?, description?, input?:<任意对象>, detail?:<工具调用详情>, actions?:[{id, label, behavior:"allow"|"deny", variant?, intent?}], suggestions?, metadata?}`
  - ⚠️ `input` 与 `detail` 可以很大（完整工具参数/diff），固件只取 `name`/`kind`/`title`/`description` 显示，其余字段在流式解析时跳过。
- 回应请求 `agent_permission_response`：
  `{type, agentId, requestId:<这里填权限请求对象的 id>, response:{behavior:"allow"} 或 {behavior:"deny", message?, interrupt?}}`
  - 可选 `response.selectedActionId`：当请求带 `actions` 列表时，用它指明选了哪个动作（比裸 behavior 更精确）。
  - ⚠️ 易踩坑：此报文的 `requestId` **不是** RPC 关联 ID，而是权限请求自身的 id（schema 中它与 `agentId` 并列、没有单独的 payload 包装，也没有响应报文与之关联——它是单向通知型报文）。

### 4.5 拉取某个 agent 的最近输出（摘要）

依据：`FetchAgentTimelineRequestMessageSchema` / `FetchAgentTimelineResponseMessageSchema`

- 请求 `fetch_agent_timeline_request`：
  `{type, agentId, requestId, direction:"tail", limit:<小正整数>, projection:"projected"}`
- 响应 `fetch_agent_timeline_response`：
  `payload{requestId, agentId, agent:<快照|null>, direction, projection, epoch, reset, staleCursor, gap, window:{minSeq, maxSeq, nextSeq}, startCursor, endCursor, hasOlder, hasNewer, entries:[{provider, item, turnId?, timestamp, seqStart, seqEnd, ...}], error}`
- ⚠️ 巨型 payload 风险（固件必须避开）：
  1. 请求里 `limit: 0` 的语义是「返回该窗口全部匹配行」（schema 注释原文），**绝不能用 0 或省略后不设防**；即便 limit 给 10，单条 entry 的 `item` 也可内嵌完整工具输出、文件内容或 diff，整帧仍可能达几十 KB 以上。固件策略：`direction:"tail"` + `limit ≤ 10` + `projection:"projected"`，解析时只抽取最近的 assistant 文本、其余字段流式丢弃；显示层再截断到屏幕可读长度（数百字）。
  2. `entries[].item` 的内部字段结构（文本片段的确切字段名）本次**未逐项核实**，写解析器前需对着 `AgentTimelineItemPayloadSchema` 再核一遍或用真实 daemon 抓一条样本帧。
  3. `agent_stream` 推送（时间线流式增量）频率高、总量大，固件不订阅时间线就不会收到；不要为了「实时感」打开它。
  4. `fetch_agents_response` 在 agent 多时整包同样可观：每个 entry 都是完整快照（含 `availableModes`、`pendingPermissions[].detail` 等大字段）。用 `page.limit`（见 §5）压到一屏量（10–20）并流式解析。
  5. `providers_snapshot_update`（模型/Provider 目录快照）体积大且与固件功能无关：不要订阅 `providers_snapshot_update` 事件、hello 里不声明 `all_providers` 能力。

## 5. 心跳 / 保活

依据：`packages/protocol/src/messages.ts`（`WSPingMessageSchema`/`WSPongMessageSchema`）、`packages/client/src/daemon-client.ts`（`LIVENESS_*` 常量）

- 应用层心跳：客户端发顶层报文 `{"type":"ping"}`（无其他字段），daemon 回 `{"type":"pong"}`（同样无字段）。
- 官方客户端节奏：每 **10s** 发一次 ping；pong 超时 **15s**；连续 **2** 次失败判定连接死亡、断开重连（`LIVENESS_HEARTBEAT_INTERVAL_MS=10_000`、`LIVENESS_HEARTBEAT_TIMEOUT_MS=15_000`、`LIVENESS_FAILURE_RECONNECT_THRESHOLD=2`）。
- **未核实**：daemon 侧是否对长时间不发 ping 的客户端强制断开，未在 server 源码逐行确认；固件照官方节奏发 ping 即可（成本极低，且加密后每帧仅约 50 字节量级）。
- 注意区分：WebSocket 协议层 ping/pong 帧由 WS 库自动应答，与上面的应用层 `{"type":"ping"}` 是两回事，后者必须在应用层自己发。
- relay 侧对客户端没有应用层心跳要求（adapter 里的 control keepalive 只针对 daemon 的控制 socket）。

## 6. 报文大小与分页

依据：`packages/protocol/src/websocket-message-limit.ts`、`messages.ts`、cloudflare-adapter.ts

- 协议硬上限：daemon 接受的单条 WS 报文最大 **100MB**（`MAX_WEBSOCKET_MESSAGE_BYTES`，超限以关闭码 1009 断开）；relay 接受的加密帧最大 **32MB**（`MAX_RELAY_WEBSOCKET_MESSAGE_BYTES`）。这两个上限对 C3 都没有实际意义——固件的真实约束是自身内存，必须**自设入站帧长上限**（建议 32KB 量级，按堆余量定）并配合流式 JSON 解析；超限帧丢弃并考虑断开重连、靠重新拉列表收敛状态。
- 分页参数：
  - `fetch_agents_request.page.limit`：正整数，**schema 上限 200**（`.max(200)`），配 `page.cursor` 翻页；响应用 `pageInfo{nextCursor, prevCursor, hasMore}` 指示。另有 `fetch_agent_history_request`（历史列表）是同一套 page/pageInfo 形态，固件可暂不实现。
  - `fetch_agent_timeline_request`：`direction`（`tail|before|after`）+ `cursor` + `limit` 的窗口机制，响应用 `window{minSeq,maxSeq,nextSeq}` 与 `startCursor/endCursor`、`hasOlder/hasNewer` 指示可继续翻页。
  - 除上述两个列表类接口外，子集内其他报文无分页概念。
- relay 缓冲上限：daemon 未接入期间客户端帧最多缓冲 200 帧（超出丢最旧），固件重连后不要依赖「relay 会补发」，状态一律以重新拉取为准。

## 7. 未核实清单（实现/联调时逐项实测）

1. `server_info` 的确切发送时机（hello 后主动推送 vs 订阅后到达）——§2.4 已给双保险做法。
2. `fetch_agents_request` 带 `subscribe` 后的增量推送组合与节流行为（`agent_update`/`agent_status` 与目录订阅的绑定关系未逐行核实）。
3. `AgentAttachmentsSchema` 的空形态（`send_agent_message_request.attachments` 必填字段给什么）。
4. `activeTurnBehavior` 的枚举取值。
5. timeline `item` 内部字段结构（抽 assistant 文本的确切路径）。
6. daemon 对不发应用层 ping 的客户端是否有服务端超时断开。
7. legacy `create_agent_request` 的响应报文类型（固件不用，仅记录）。
