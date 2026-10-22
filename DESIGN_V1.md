# AI Passport × Paseo 客户端固件 — 设计文档 V1

日期：2026-10-22
状态：已定稿开工（一期）
依据：research/PROTOCOL_SUBSET.md（协议子集，逐条对 getpaseo/paseo 源码核实）；本机联调 daemon v0.12.0 实跑验证 offer 生成。

## 1. 目标与非目标

目标：一台独立工作的 Paseo 遥控器。AI Passport（ESP32-C3）刷本固件后，作为正式 Paseo 客户端经 relay（E2EE）连 daemon，实现：
- 总览：各 agent 的状态（运行/空闲/出错/等待审批）
- 审批：查看待审批权限请求并批准/驳回
- 指令：对已有 agent 发文本指令（含按键语音输入）；按预置默认值新建 agent
- 结果：看某个 agent 最近输出的摘要文本

非目标：看 diff、翻完整日志、管理 worktree、语音播报（TTS）——屏幕与内存决定边界，那些留给手机 App。

## 2. 硬件基线（已核实，folo 分支板级代码）

- SoC：ESP32-C3（单核 RISC-V），无 PSRAM，可用 SRAM 约 400KB 级
- 屏：240×320 LCD（LVGL）
- 按键：UP/DOWN/OK 为电阻梯 ADC 按键（共用 GPIO0/ADC1_CH0），OK 键支持长按检测（周期采样）
- 音频：ES8311 + 麦克风 + 喇叭；电池 CW2017 电量计
- 定位：桌面常插电使用（WSS 长连接常开，不走 deep-sleep 模型）

## 3. 基座选型

基座 = 本地已有的 folo-ai-passport-xiaozhi 工作区（ESP-IDF），理由：同板已实机构建过固件；板级驱动（屏/ADC 按键/音频/电量）、LVGL、esp-wifi-connect 配网组件、NVS Settings 均为现成且验证过的。

保留：boards/folotoy/ai-passport 全部板级代码、display 层、audio 的录音通路、wifi-connect 配网框架、settings(NVS)、button 框架。
剥离：xiaozhi 协议层（protocols/）、OTA、MCP server、唤醒词/esp-sr、表情资产与聊天 UI（application.cc 的对话状态机只留壳参考，新写 Paseo 应用状态机）。
新增：paseo_client（relay+ E2EE+协议子集）、nacl（tweetnacl 移植）、config_ext（配网页扩展字段）、asr_client（硅基流动转写）、ui_paseo（三屏）。

## 4. 总体架构（任务划分）

- net_task：WiFi 连接/重连、relay WSS 连接、e2ee 握手、ping 保活（10s ping / 15s 超时 / 连失 2 次重连，官方客户端参数）
- paseo_rx：入站帧 → 解密 → 流式 JSON 解析 → 事件分发（帧长自设上限，超限丢弃并记录）
- ui_task：LVGL 三屏 + 按键事件（短按/长按区分）
- audio_task：按住 OK 录音（PCM 16kHz 单声道写 flash 文件）→ 松手触发 ASR 上传 → 文本回显待确认
- 存储：NVS 存配置与设备密钥对；录音文件存 flash 分区，上传后即删

## 5. 配置与配对（Leon 已定形态）

配网模式（开机长按 OK 进入，或无配置时自动进入）：
- 设备开 SoftAP，captive portal 在 esp-wifi-connect 现有配网页基础上扩展三个字段组：
  1. WiFi SSID/密码（原有）
  2. Paseo 配对 offer 链接（整段粘贴）→ 固件解 Base64 JSON 取 v/serverId/daemonPublicKeyB64/relay.endpoint+useTls
  3. ASR：端点（预置 https://api.siliconflow.cn/v1/audio/transcriptions）、模型（预置 XingChenAGI/XingChenASR-V3.2-Ultra）、API Key
  4. 新建 agent 默认值：provider（预置 codex）、cwd（文本）
- 全部存 NVS；设备 X25519 密钥对首次配网时生成并存 NVS，重置配网不轮换（除非用户选“重置密钥”）。

## 6. 协议实现要点（据 PROTOCOL_SUBSET.md）

- 连接：WSS `wss://<relay.endpoint>/ws?serverId=<id>&role=client&v=2`（不带 connectionId）
- 握手：明文文本帧 `{"type":"e2ee_hello","key":"<设备公钥 b64>","capabilities":{"binaryCiphertext":true}}`，可 1s 间隔重发，等 `e2ee_ready`
- 加密帧：`[24B 随机 nonce ‖ NaCl box 密文]` → Base64 → WS 文本帧；共享密钥用 box.before 预计算一次
- 通道内首帧：daemon 层 hello（clientType 用 "cli"，protocolVersion 1，声明 hello_rejection 与 explicit_event_subscriptions 能力——须与目标 daemon 版本对齐，联调靶子为 v0.12.0）
- 订阅面（严控）：session.events.set_subscription 只订 agent_attention_required / agent_permission_request / agent_permission_resolved / status.server_info
- 功能报文：fetch_agents_request、send_agent_message_request、agent.create.request（provider+cwd+initialPrompt）、agent_permission_response（注意：其 requestId 填权限请求自身 id）、fetch_agent_timeline_request（必须 tail + 小 limit + projected，禁用 limit:0）
- 等待审批判定：协议无独立状态，靠快照 pendingPermissions 非空 / attentionReason="permission" 聚合
- 入站纪律：单帧上限自设（拟 48KB，超限截断报错）；JSON 流式解析（cJSON 流式或 jsmn 类），禁止整包 DOM 解析大快照；agent 列表只保留 id/标题/状态/待审批数等摘要字段

## 7. 加密与安全

- tweetnacl 单文件 C 移植（Curve25519 + XSalsa20-Poly1305），randombytes 必须接 esp_random 硬件 RNG（安全红线：nonce 为纯随机 24B，无计数器兜底）
- offer 链接等同凭证：只存 NVS，不回显全量、不打日志
- ASR API Key 同上；ASR 走 HTTPS（内置 CA 证书包校验 api.siliconflow.cn）

## 8. 内存预算（拟定，实测校准）

- TLS 两路并发上限（relay WSS + ASR HTTPS 不同时在线：ASR 上传前可保持 WSS，mbedTLS 双连接约 80–110KB，紧张但可行；若超标则 ASR 时短暂挂起 relay 重连）
- 入站帧缓冲：48KB 上限 + 解析工作区复用
- LVGL：部分刷新小缓冲（240×40 行级）
- 录音：PCM 直写 flash 文件（32KB/s），时长上限 60s（约 1.9MB），不占 RAM
- 栈与堆保留量在一期实测后回填本节

## 9. UI 三屏（240×320）

- 总览屏：agent 列表（名称/状态点/待审批角标），UP/DOWN 选，OK 进结果屏；顶部状态栏：连接状态、电量、时间
- 待审批屏：有待审批时总览屏角标提醒；本屏列请求摘要（工具名+关键参数截断显示），OK=批准，长按 OK=驳回，UP/DOWN 切换条目
- 结果屏：选中 agent 的最近输出摘要（timeline tail 投影文本），UP/DOWN 滚动；OK 长按返回
- 指令流：任意屏按住 OK 录音 → ASR 文本上屏 → OK 发送给当前选中 agent（总览屏未选中时发给默认/最近活跃 agent，规则一期定版时明确）→ 上下键取消

## 10. 分期与验收

- 0.5 期（先行）：PC 端 Python 参考客户端实现同一协议子集，经 pr-worker 连本机联调 daemon，跑通 hello/拉列表/订阅/发指令/审批全链路并抓样本帧（补核 PROTOCOL_SUBSET.md 文末 7 项未核实，重点 timeline item 结构）。验收：样本帧存档 + 未核实项清零或标注结论。
- 一期：配网页扩展 + WiFi + relay/E2EE 打通 + fetch_agents 上屏（总览屏只读）。验收（Leon 实机）：配网粘 offer 后连上联调 daemon，总览屏显示 agent 数与状态。
- 二期：待审批屏 + 审批动作 + 发指令（预置文本/列表选）+ 结果屏。验收：实机对联调 daemon 完成一次审批与一次指令往返。
- 三期：按键语音全链路（录音→ASR→确认→发送）。验收：实机语音指令被 daemon 执行并回显结果摘要。
- 交付：每期出烧录包（整包 bin + 刷机/配网说明），版本递增不覆盖。

## 11. 风险与未核实

- 协议演进快：子集须与 daemon 版本对齐（当前 0.12.0）；升级 daemon 前先跑 0.5 期参考客户端回归
- XSalsa20 在 C3 上对大帧的解密耗时未测：一期用 48KB 上限帧实测
- timeline item 内部字段未展开：0.5 期抓帧补核
- 本机联调 daemon 直连官方 relay 被本机出口代理干扰 TLS：开发链路 daemon 侧改指 pr-worker + 本地 CONNECT 隧道；设备侧在 Leon 家庭网络直连官方 relay/pr-worker 均可（不经此代理）
- 物理设备在 Leon 手上：每期验收依赖 Leon 刷机反馈（沿用既有协作节奏）
