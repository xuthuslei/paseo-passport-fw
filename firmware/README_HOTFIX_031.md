# Paseo Passport 0.3.1 (phase 3a) 热修说明

日期：2026-10-22
产物：`dist/Paseo-Passport-0.3.1-phase3a-full.bin`
（7,664,505 B，md5 `39238d0827eacb8d27e19d3cc8dffde4`，整包 0x0 刷入；
0.1.0/0.2.0/0.3.0 旧包未覆盖。构建日志 `build_hotfix031.log`，BUILD_EXIT:0）

## 背景（实机反馈）

Leon 刷 0.3.0 实机验收：WiFi 配网成功后设备又回到配网状态，且在配网页上
找不到填 offer / ASR Key 的地方。根因三条：

1. **/paseo 表单无入口**：Paseo 配置表单是独立页面 `/paseo`，而
   esp-wifi-connect 的主配网页上没有任何链接指向它，用户无从发现。
2. **done 页 1 秒自动退出**：WiFi 保存成功跳到 done 页后，页面在 1 秒后
   自动 POST `/exit` 退出配网模式——用户根本没机会再去填 offer。
3. **/paseo 页的“退出配网”是坏链接**：`/exit` 只注册了 HTTP POST，
   而 /paseo 页里放的是普通 GET 链接，点下去是 404，退不出去。

状态机本身行为正确（NVS 无合法 offer 就留在配网），只是流程把用户
挡在了第 2 步之外。

## 改动

### 1. 主配网页加第 2 步横幅
`components/esp-wifi-connect/assets/wifi_configuration.html`
（EMBED_TXTFILES 嵌入，改 assets 即生效，已验证新文案在 app bin 内）：
Wi-Fi 标签页顶部加橙色横幅——「本设备需要两步配置，缺一不可」+
第 1 步/第 2 步说明 + 整宽按钮「第 2 步：填写 Paseo 配置（粘贴 offer）→」
直达 `/paseo`。

### 2. done 页重做
`components/esp-wifi-connect/assets/wifi_configuration_done.html`：
- **移除 1 秒自动 POST /exit**（保留 `?message=` 参数覆盖主文案的原逻辑）。
- 主文案改「WiFi 已保存 ✓」，副文案明示还差第 2 步、只存 WiFi 就退出
  会再次进入配网。
- 加同款橙色按钮直达 `/paseo`；加「两步都完成了，退出配网模式」按钮
  （fetch POST /exit，点击后页面提示“已退出…可关闭本页”）。

### 3. /paseo 页退出改真按钮 + 错误回显加强
`main/paseo_app.cc`：
- 新增共享片段 `kExitButton`：按钮 + 内联 JS `fetch('/exit',{method:'POST'})`，
  表单页与提交成功页都改用它（替代原来 404 的 GET 链接）。
- 提交成功页继续回显 serverId / relay（offer 与 Key 永不回显的原则不变）；
  若本次没保存有效 offer，警告改为红色粗体，并写明“只填其它设置就退出
  会再次回到配网”。
- offer 解析失败页保留原错误原文回显，并补常见原因提示（粘贴不完整 /
  链接失效，请从 Paseo App 重新复制完整链接）。

### 4. 配网屏显文案三分
PROVISIONING 进屏文案按状态区分（`main/paseo_app.cc`）：
- 无 offer 且无已存 WiFi：「配网两步（缺一不可）：①连热点选 WiFi 点
  Connect 保存；②点配网页顶部橙色按钮粘贴 Paseo offer 并保存……」
- 无 offer 但 WiFi 已存（即 Leon 遇到的循环场景，依 SsidManager 判断）：
  「还差第 2 步：WiFi 已保存，但 Paseo offer 未配置。连接热点打开配网页，
  点顶部橙色按钮粘贴 Paseo offer 并保存。」
- offer 已存（强制重进配网改配置）：提示可在网页修改、完成后点按钮退出。
- 循环进配网的行为本身保留（无 offer 上线无意义），但用户能从屏幕上
  直接看到缺的是哪一步。

启动横幅版本串改为 `Paseo Passport 0.3.1 (phase 3a)` 便于实机核对。

## 新配网操作步骤（0.3.1 起）

1. 刷整包开机，无配置自动进配网，屏幕显示两步说明。
2. 手机连热点 `PaseoPassport-xxxx`，配网页自动弹出（或开 192.168.4.1）。
3. **第 1 步**：在配网页选 WiFi、输密码、点 Connect。跳到 done 页后
   **不要急着退出**（页面不再自动退出）。
4. **第 2 步**：点 done 页（或主配网页顶部）的橙色按钮
   「第 2 步：填写 Paseo 配置（粘贴 offer）→」，整段粘贴
   `https://app.paseo.sh/#offer=...`，按需填 ASR API Key、默认
   provider/cwd，点“保存 Paseo 配置”。成功页会回显 serverId 与 relay
   供核对；解析失败会红字给出原因。
5. 两步都完成后，在成功页或 done 页点「完成并退出配网模式」/
   「两步都完成了，退出配网模式」按钮，设备连 WiFi 上线。

## 验收状态

编译级验收：`idf.py build` 通过（app bin 0x203d20，分区余 31%）；
已用字符串检索确认四处新文案（主页横幅、done 页按钮、屏显“还差第 2 步”、
kExitButton JS）都在 app bin 与合并整包内。未上真机——首次实机验收点
不变：按上面 5 步走完配网 → 总览屏出 agent 列表。
