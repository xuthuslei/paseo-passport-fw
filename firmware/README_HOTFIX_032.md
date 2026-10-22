# 热修 0.3.2 (phase 3b)：E2EE 明文控制帧容错 + 单页配网

整包：`dist/Paseo-Passport-0.3.2-phase3b-full.bin`
（7,664,505 B，md5 `2cfe2c54216cc1830722d1c5f05bdf0d`，整包 0x0 刷入；
0.1.0/0.2.0/0.3.0/0.3.1 旧包未覆盖。构建日志 `build_hotfix032.log`，
BUILD_EXIT:0。本版同时包含下面两部分改动。）

启动横幅版本串：`Paseo Passport 0.3.2 (phase 3b)`（实机串口/屏显可核对）。

---

## 一、E2EE 明文控制帧容错（解密失败重连循环）

### 实机现象（0.3.0/0.3.1）

设备连上 relay、收到 `e2ee_ready`、进入 HELLO 等待后立刻报
`decrypt failed during hello wait` 并整链重连，无限循环，进不到 ONLINE。
同一 offer 用 PC 参考客户端（tools/ref_client.py）连同一 daemon 完全正常。

### 根因

固件在 E2EE 握手期每 1 秒重发一次 `e2ee_hello`（等不到 ready 就重发，
本意是抗慢配对）。协议设计上就预期客户端会重复发 hello：上游
getpaseo/paseo 的 `packages/relay/src/encrypted-channel.ts`（第 49-50 行
注释）明确写了客户端没看到 ready 时会重发 hello、daemon 应当重发
`e2ee_ready` 且**不轮换密钥**；其 EncryptedChannel 还专门有
`shouldIgnorePostHelloPlaintext` 逻辑，忽略通道建立后到达的明文
hello/ready 帧。

于是时序变成：设备第 2 次 hello → daemon 在已建立的通道上**明文**补发
一条 `e2ee_ready` → 这帧排进设备 rxq，落在 HELLO 等待阶段 → 旧代码把它
当作 base64 密文帧送 pb64/box 解密 → 必然失败 → 判整链失败重连 →
重连又重复同一过程，无限循环。官方客户端与 ref_client 只发一次 hello
或容忍明文帧，所以不触发。

### 改动（components/paseo_client/paseo_client.c）

- 新增统一入站帧分类 `plaintext_frame_type()`：在尝试解密**之前**先看
  帧形态——以 `{` 开头且能解析出字符串 `type` 的帧按明文控制帧处理；
  其余帧才走 base64 解码 + box 解密。分类只用帧首字符 + 轻量 JSON
  分词，不碰 paseo_box/pbase64（已主机交叉验证，不动）。
- HELLO 等待阶段：明文 `e2ee_ready` 打一行 INFO
  `duplicate e2ee_ready ignored` 并继续等待；其它明文 type 打 WARNING
  （只打 type 值，不打全文）并忽略；**不再因此判失败**。真正的解密失败
  仍按原逻辑 `goto fail_retry`。
- ONLINE 主循环：明文 `e2ee_ready` 同样忽略；其它明文帧警告并忽略，
  不断链（真故障仍由 RPC 25 秒超时机制兜底）。
- `e2ee_hello` 的 1 秒重发保留（协议允许且对慢配对有意义），握手时序
  其它部分未动。

### 现在日志长什么样（正常，不是故障）

```
paseo_client: e2ee_ready received
paseo_client: duplicate e2ee_ready ignored      <- 新增，重复 ready 被吞掉
paseo_client: server_info received; daemon accepted hello
paseo_app: paseo state: ONLINE
```

若出现 `ignoring unexpected plaintext frame: type=...` 的 WARNING，
说明 daemon 发了预期外的明文帧，已被忽略、不影响连接，把 type 值
反馈回来即可。

---

## 二、单页配网（配网只用一个页面）

### 改了什么

`components/esp-wifi-connect/assets/wifi_configuration.html`：

- 0.3.1 在页首加的「第 2 步」横幅/跳转按钮**已移除**，改为页首一条
  简短提示；Wi-Fi 表单下方新增内嵌的 **Paseo 配置区**（同一页）：
  offer 粘贴 textarea、ASR API Key、ASR 端点与模型（预置默认值
  `https://api.siliconflow.cn/v1/audio/transcriptions` /
  `XingChenAGI/XingChenASR-V3.2-Ultra`，可改）、默认 provider
  （预置 `codex`）、默认 cwd（空，带占位提示）。字段名与
  `/paseo/submit` 处理端完全一致
  （`offer/asr_key/asr_ep/asr_model/def_prov/def_cwd`）。
- 配置区底部是总按钮 **「保存全部并退出配网」**，点击后由页面 JS
  （`submitPaseoAll`）严格顺序执行：
  1. 按组件原有方式 POST `/submit` 保存 Wi-Fi（与原 Connect 按钮同一
     JSON 接口与校验，但成功后**不跳 done 页**）；
  2. POST `/paseo/submit` 保存 Paseo 字段（该处理端失败时也回 200 的
     HTML 结果页，JS 按页面中的「解析失败 / 尚未保存」标记判定成败）；
  3. 两步都成功才 POST `/exit` 退出配网。
  任一步失败：停在配网模式不退出，页面上以中文明确报错（如密码错、
  offer 解析失败需整段重粘），改好后可再次点击。
- 原 Wi-Fi 的 Connect 按钮保留（只存 Wi-Fi 的老路径，仍会跳 done 页），
  总按钮是主路径，页面文案已写明顺序。
- `/paseo` 独立页保留不动，作为兜底入口。
- done 页（wifi_configuration_done.html）：总按钮流程**不会经过**它
  （第 1 步成功不跳转），所以不会被它打断；它只在老路径（只点
  Connect）时出现，文案已调整为提示回配网页同页完成或去独立配置页，
  其上的独立配置页按钮与手动退出按钮保留作兜底。

### 最终用户路径（配网操作步骤）

1. 刷整包开机，无配置自动进配网（热点 `PaseoPassport-xxxx`），
   或开机 2.6 秒窗口内长按 OK 强制进配网。
2. 手机连热点，配网页自动弹出（或浏览器开 `192.168.4.1`）。
3. 在**同一个页面**上：上方列表里点选 Wi-Fi 并输入密码；往下到
   「Paseo 配置」区粘贴 offer（ASR 等其余项用预置默认值即可，
   ASR Key 可留空后补）。
4. 点橙色大按钮 **「保存全部并退出配网」**，页面会依次显示
   第 1/3、2/3、3/3 步进度。
5. 全部成功后设备自动退出配网、连接 Wi-Fi 并上线
   （总览屏出 agent 列表），本页可以关闭。若中途报错，设备仍在
   配网模式，按提示改好再点一次按钮即可。

---

## 三、实机复验步骤（两部分一起验）

1. 整包刷入 0.3.2，开机确认版本串 `0.3.2 (phase 3b)`。
2. 走上面「最终用户路径」完成单页配网（offer 用 Leon 自己的 daemon
   那个，relay 为 pr-worker.178960.xyz:443）。
3. 看串口日志确认连接走完四段：relay connected → `e2ee_ready received`
   →（可能有一行 `duplicate e2ee_ready ignored`，正常）→
   `server_info received` → ONLINE；**不应再出现**
   `decrypt failed during hello wait`。
4. 总览屏应出现 agent 列表（状态 + 标题 + 待审批数）；停留几分钟，
   不应出现无故断线重连。
5. 顺带回归：结果屏拉时间线、待审批屏、预置指令各点一次（与二期
   验收点相同）。

未上真机——以上为本版首次实机验收点。如连接仍卡住，把新日志
（特别是 `paseo_client` 与 `paseo_app` 的行）整段发回。
