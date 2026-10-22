# Paseo Passport 固件 — 一期说明（0.1.0-phase1）

AI Passport（ESP32-C3）× Paseo 客户端固件。基座为 folo-passport-xiaozhi-clock
工作区（ESP-IDF v6.0.1，板型 CONFIG_BOARD_TYPE_FOLOTOY_AI_PASSPORT），
在其上裁剪 xiaozhi 应用层并新增 Paseo 协议子集客户端。

设计依据：`../DESIGN_V1.md`、`../research/PROTOCOL_SUBSET.md`、
`../research/UNVERIFIED_RESOLVED.md`（0.5 期实测结论）；
PC 参考实现：`../tools/ref_client.py`（C 实现逐项与之对照）。

## 一期范围

- 应用状态机：BOOT →（无 Paseo 配置）PROVISIONING /（有配置）CONNECTING
  → ONLINE；WiFi 断开超过 25 s 宽限期后停客户端并重连，站点 60 s 连不上
  回落配网模式。
- 配网页扩展：Paseo offer 粘贴、ASR 配置、默认 provider/cwd（见下）。
- paseo_client：relay WSS → E2EE（NaCl box）→ daemon hello →
  【红线】等 server_info 后才发 RPC → 事件订阅 + fetch_agents →
  10 s 应用层 ping 保活 → agent 列表/状态经回调上屏。
- 总览屏（复用现有 display 层）：连接状态行 + agent 列表
  （状态 + 标题 + 待审批计数）+ 总数。

二期及以后（不在本包内）：待审批屏与审批动作、发指令、结果屏、按键语音
（录音 → 硅基流动 ASR → 确认 → 发送）。ASR 的端点/模型/Key 一期只负责
在配网页收集并存 NVS，固件尚不调用。

## 裁剪 / 保留清单

保留并实际使用：

- 板级驱动 `main/boards/folotoy/ai-passport`（240×320 屏、ADC 按键、
  ES8311、CW2017）与 `main/boards/common` 的 Board/Button/Backlight
- display 层（LVGL 聊天 UI 骨架，一期借其状态行与消息区做总览屏）
- esp-wifi-connect 配网框架（vendored 到 `components/esp-wifi-connect`，
  见下）
- Settings（NVS）

编译保留但从不初始化（裁剪方式 = 启动路径停用，未删代码）：

- xiaozhi `Application` 与设备状态机（`main.cc` 改为直接调
  `paseo_app_run()`，不再 `Application::Initialize/Run`）
- `main/protocols/`（xiaozhi WebSocket/MQTT 协议层）、OTA、MCP server、
  音频服务与唤醒词/esp-sr、表情/聊天对话逻辑、PassportDisplay 的天气/
  时钟主页刷新（依赖 Application 状态，一期不刷新）

已知副作用：板级按键回调（音量/开关等）仍指向未启动的 xiaozhi
Application，一期按键无作用（总览屏为只读显示）；这不影响配网与连接。

vendored 组件改动（`components/esp-wifi-connect`，源自 managed
78/esp-wifi-connect 3.3.1，本地组件同名覆盖 managed 版）：

- `WifiConfigurationAp::RegisterExtraUriHandler()`：允许应用注册自有 URI
  handler，在 StartWebServer 中于内置页面之后、captive 兜底重定向之前
  注册；`max_uri_handlers` 24 → 40。
- `WifiManager::AddConfigApUriHandler()`：转发登记（StartConfigAp 前调用
  即可，内部只应用一次，防重复注册）。

新增组件：

- `components/tweetnacl`：TweetNaCl 20140427 官方版本
  （tweetnacl.cr.yp.to），`randombytes` 接 `esp_fill_random` 硬件 RNG
  （安全红线：box nonce 与设备密钥都依赖它）。
- `components/paseo_client`：offer 解析（base64url + 轻量 JSON）、
  paseo_box（beforenm 预计算 + afternm 封装，填充约定已用 PyNaCl 在主机
  侧双向交叉验证，见 `firmware/host_test/`）、
  pjson（jsmn 式零拷贝分词，只取 id/title/status/pendingPermissions
  计数）、客户端状态机。依赖官方 `espressif/esp_websocket_client`。

## 配网操作步骤

1. 刷入整包 bin 后开机。无配置时自动进入配网模式，屏幕显示
   “Provisioning”。
2. 手机/电脑连接设备热点 `PaseoPassport-xxxx`，浏览器打开配网页
   （captive portal 一般自动弹出，地址 192.168.4.1）。
3. 在配网页（首页）填 WiFi SSID/密码并保存（原 esp-wifi-connect 流程）。
4. 打开 `http://192.168.4.1/paseo`：
   - **Paseo 配对 offer 链接**：整段粘贴 `https://app.paseo.sh/#offer=...`
     固件当场解 Base64+JSON 校验（v=2、serverId、daemonPublicKeyB64、
     relay.endpoint/useTls），失败会回显原因、不保存。
   - ASR API Key（硅基流动）、ASR 端点/模型（已预置，可改）、
     新建 agent 默认 provider（预置 codex）/cwd。
   - 点“保存 Paseo 配置”→ done 页回显解出的 **serverId 与 relay 端点**
     （offer 原文与 API Key 永不回显、永不打日志，日志只打印 serverId）。
5. 点页内“完成并退出配网模式”（或首页的退出），设备连 WiFi 并自动上线；
   屏幕状态行依次显示 relay → e2ee → daemon hello → online 与 agent 数。

已有配置时开机直接连 WiFi 上线；WiFi 60 s 连不上会回落配网模式，
可在 /paseo 页更新 offer（留空 = 不修改已存 offer/Key）。

## NVS 配置键（命名空间 `paseo`）

| 键 | 类型 | 说明 |
|---|---|---|
| `srv_id` | str | daemon serverId（来自 offer） |
| `relay_ep` | str | relay 端点 host:port |
| `relay_tls` | i32 | 1=用 wss |
| `da_pub_b64` | str | daemon X25519 公钥（标准 base64，32 B） |
| `dev_pub` / `dev_sec` | blob 32B | 设备 X25519 密钥对（首次启动/连接时生成，重置配网不轮换） |
| `asr_key` | str | 硅基流动 API Key（一期仅存储） |
| `asr_ep` | str | ASR 端点，默认 https://api.siliconflow.cn/v1/audio/transcriptions |
| `asr_model` | str | ASR 模型，默认 XingChenAGI/XingChenASR-V3.2-Ultra |
| `def_prov` | str | 新建 agent 默认 provider，默认 codex |
| `def_cwd` | str | 新建 agent 默认 cwd |

另：WiFi 凭据由 esp-wifi-connect 的 SsidManager 存其自有命名空间。

## 协议实现要点（与 0.5 期结论对齐）

- relay URL：`wss://<endpoint>/ws?serverId=<id>&role=client&v=2`（不带
  connectionId）；TLS 用 ESP 证书包校验。
- e2ee_hello 为明文帧且**不带 capabilities**（与参考客户端一致，全程
  base64 文本帧模式），1 s 间隔重发直到 e2ee_ready。
- 加密帧 = base64( [24B 随机 nonce ‖ NaCl box 密文] )，WS 文本帧发送；
  共享密钥 box.before 预计算一次；派生时拒绝全零 scalarmult 结果。
- daemon hello：clientType `"cli"`、protocolVersion 1、能力
  `hello_rejection` + `explicit_event_subscriptions`；clientId 为
  `paseo-passport-<MAC>`（稳定）。
- **时序红线**：server_info 到达前绝不发任何 RPC（0.5 期实测：提前发会被
  daemon 以 Invalid hello 拒绝并连累 relay 以 1012 关连接）。
  server_info 的 serverId 与 offer 不一致时断开重连。
- 订阅面：仅 `agent_attention_required`、`agent_permission_request`、
  `agent_permission_resolved`、`status.server_info` 四个事件 + fetch_agents
  目录订阅（page.limit=20，scope=active）。
- 入站纪律：单帧上限 48 KB（组帧阶段超限即整帧丢弃）；二进制帧丢弃；
  `agent_stream` 只读信封类型即丢弃，不解析其内容（可内嵌大工具输出）；
  JSON 零拷贝分词提取，不做整包 DOM。解密失败 = 致命错误，整链重连
  （与官方客户端一致）。daemon 以 1012 关闭（daemon 掉线）同样整链重连。
- 保活：应用层 `{"type":"ping"}` 每 10 s（加密发送），daemon 回 pong
  （忽略）；无 pong 超时判定一期未做（0.5 期结论 #6：daemon 对 90 s
  静默不主动断开）。

## 已知限制

- 一期仅编译级验收：未在真机/真 daemon 上联调（物理设备不在本机）。
  首次实机验收点：配网粘 offer → 连上联调 daemon → 总览屏显示 agent
  数与状态（设计文档一期验收标准）。
- 总览屏借用 stock 聊天 UI 的消息区追加显示，agent 更新会新增一条消息
  气泡而非原地刷新；专用三屏 UI 在后续期次。
- fetch_agents 固定 limit=20，最多显示 20 个 agent；无分页拉取。
- 待审批计数来自快照 pendingPermissions；`agent_permission_request`
  事件本身一期只记录不处理（审批动作二期）。
- 设备的 daemon hello 无密码字段：目标 daemon 须未设访问密码。
- 板级按键一期无功能（见裁剪清单的已知副作用）。

## 构建命令

```sh
source ~/esp/esp-idf-v6.0.1/export.sh
cd firmware
idf.py reconfigure   # 首次或组件变更后
idf.py build
```

整包合并（在 `build/` 目录内执行，flash_args 内为相对路径）：

```sh
cd build
python3 -m esptool --chip esp32c3 merge_bin -o ../dist/Paseo-Passport-0.1.0-phase1-full.bin @flash_args
```

本期产物（2026-10-22 构建）：应用 xiaozhi.bin 0x1f2f30 B（应用分区
0x2f0000 B，余 34%）；整包 7,664,505 B（0x74f379），
md5 `b788d4ae8f9ffc938de0dd6dc3f98490`。

刷机：`esptool.py --chip esp32c3 write_flash 0x0 dist/Paseo-Passport-0.1.0-phase1-full.bin`
（整包已含 bootloader/分区表/应用/assets，偏移 0x0 一次写入；会清掉旧
NVS 配置，需重新配网。）
