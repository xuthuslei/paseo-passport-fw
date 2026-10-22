# 热修 0.3.4 (phase 3d)：paseo_client 常驻内存瘦身（plain/toks 改每帧按需）

整包：`dist/Paseo-Passport-0.3.4-phase3d-full.bin`
（7,664,505 B，md5 `8413c47e680563659a815015155d7d60`，整包 0x0 刷入；
0.1.0–0.3.3 旧包未覆盖。构建日志 `build_hotfix034.log`，BUILD_EXIT:0；
已用字符串检索确认新横幅与新日志串都在整包内、旧横幅已消失，且整包
0x20000 处 app 区与本次构建的 xiaozhi.bin 逐字节一致。）

启动横幅版本串：`Paseo Passport 0.3.4 (phase 3d)`（实机串口/屏显可核对）。

---

## 一、实机证据（为什么改这里）

Leon 刷 0.3.3 后的真机日志（2026-10-22 21:54）：

- `server_info received; daemon accepted hello` → 状态到 ONLINE →
  `fetch_agents_response: 1 agents, 0 perms`——**0.3.3 的按需分配修复
  已坐实有效**，握手与首帧解密全通。
- 随后（第一轮约 28 秒、之后每轮重连数秒内）反复出现：

  ```
  E (...) Dynamic Impl: alloc(16749 bytes) failed
  E (...) esp-tls-mbedtls: read error :-0x008D
  E (...) websocket_client: esp_transport_read() failed with -141 ...
  ```

  WS 读失败 → 断线 → 重连，无限循环，设备无法稳定在线。

- **16,749 B 的来源账**：mbedTLS 的 Dynamic Impl（动态缓冲）在读一条
  TLS 记录时，按 `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384` 申请一整块
  输入缓冲，16,749 = 16,384 + 365 B 实现开销。它申请失败，意味着上线
  后堆里连一块 **16.7 KB 的连续空闲块** 都凑不出——不是总量问题，是
  常驻占用 + 碎片化。
- **头号元凶**：paseo_client 启动时常驻、整个进程生命周期不释放的两块
  各约 48 KB 的工作区（0.3.3 只修了 decrypt_frame 的临时 raw 缓冲，
  这两块没动）：
  - `c->plain`：49,153 B（MAX_FRAME_BYTES+1 的明文缓冲）；
  - `c->toks`：49,152 B（3072 × sizeof(pj_tok_t)=16 B 的 JSON token 数组）。
  - 合计 **98,305 B（约 98 KB）** 纯常驻。ESP32-C3 无 PSRAM、总 DRAM
    约 400 KB，再叠加 WiFi/WSS/mbedTLS/LVGL 与配网残留碎片，最大连续
    块被压到 16.7 KB 以下完全吻合实机表现。

## 二、本版改动（仅 components/paseo_client/paseo_client.c + 横幅）

1. **plain 改每帧按需**：`decrypt_frame()` 新签名带出参
   `char **plain_out`。输出缓冲按解码后的密文长度分配（密文长即明文
   上界，真实首帧只需 2,607 B），NUL 结尾后把指针交给调用方；调用方
   在解析/分发完成后 `free`。失败路径（raw/b64/plain alloc/box）各自
   释放已分配的部分，`*plain_out` 保持 NULL。`paseo_client_start` 中
   plain 的常驻 malloc 与结构体字段已删除。
2. **toks 改每帧按需 + 可增长**：新增 `root_parse()` 按需分配——初始
   cap **512** 个 token（8,192 B），`pj_parse` 返回 -2（token 不足）时
   翻倍重解析，上限仍 MAX_TOKENS=3072；结果存 `c->toks` 供本次消息
   处理期间使用，处理结束由调用方统一 `parse_release()` 释放并置
   NULL。toks 分配失败打
   `toks alloc failed (cap=.. free=.. largest=..)` 并跳过该帧（返回
   -3，上层按不可解析帧处理，不断线、不崩溃）。两个使用点都已照顾：
   hello 等待里的 server_info 检查、主循环的 dispatch_message。
3. **堆遥测**：新增 `log_heap()` helper，打
   `heap: free=%u largest=%u min=%u`（free=当前空闲总量、largest=最大
   连续空闲块、min=开机以来最低空闲水位）。进入 ONLINE 时打一次
   （后缀 `online`），主循环每 30 秒打一次（后缀 `main loop`）。
   alloc 失败日志沿用 0.3.3 的数值风格。

**常驻账**：start 路径现只剩 `tl_text` 1,536 B 一块小常驻（未动）；
plain+toks 常驻立减 **98,305 B**。瞬时占用：典型帧 plain 约 2.6 KB +
toks 初始 8 KB（512 token），大帧 token 翻倍时瞬时最高 49 KB 但用完
即放——且是在消息到达的那一刻才申请，此时 TLS 输入缓冲不在途。

## 三、释放路径自查（逐路径核对，无泄漏/无重复释放）

- **hello 等待**：重构为单次迭代内清理——每帧解密成功后，无论提取
  结果是 continue（非 server_info/session）、got_info、还是 fatal
  （hello.rejected / serverId mismatch），都先落到迭代底部统一
  `parse_release(c) + free(plain)`，再由 fatal 标志决定是否
  `goto fail_retry`。明文帧路径（duplicate e2ee_ready）不经
  decrypt/parse，只 free 帧本身。解密失败路径 plain 恒 NULL，直接
  goto，无可漏。
- **主循环**：解密失败 → plain 恒 NULL、断链，无可漏；解密成功 →
  `dispatch_message()` 返回后**无条件** `parse_release(c) +
  free(plain)`（含 dispatch 返回 false 的断链路径，经 tear_down 标志
  在释放后才 break）。
- **root_parse 内部**：翻倍重试时先 free 旧数组再置 NULL 重申请；
  分配失败返回 -3 且 `c->toks == NULL`，调用方 parse_release 对
  NULL 安全。
- **dispatch_message**：只借用 plain 与 c->toks，全程不释放，归还
  调用方统一处理；其早退（tokens<=0）在 toks 已分配/未分配两种情况
  下都由主循环的 parse_release 兜底。

## 四、刻意未动

sdkconfig（工程规则禁止手改）、TLS/mbedTLS 参数、websocket
buffer_size(2048)、帧组装器、paseo_box/pbase64/tweetnacl、握手时序、
0.3.2 明文帧容错、0.3.3 的 raw 按需分配与三阶段日志——全部未动。
本版只验证一个假设：常驻瘦身后 TLS 的 16.7 KB 连续块能否稳定拿到。

## 五、实机复验步骤与判读

1. 整包刷入（0x0，先全擦更稳），开机会进配网（NVS 已清）。
2. 单页配网：同页选 WiFi 输密码 + 粘 Leon 自己 daemon 的 offer +
   ASR Key，点「保存全部并退出配网」。
3. **成功判读**（缺一不可）：
   - 日志走完 `e2ee_ready received` → `server_info received; daemon
     accepted hello` → `paseo state: 4`（ONLINE）→ 紧跟一行
     `heap: free=.. largest=.. min=.. (online)`；
   - `fetch_agents_response: 1 agents, 0 perms`，总览屏出 agent 列表；
   - **上线后持续在线数分钟以上，不再出现 `Dynamic Impl: alloc(16749
     bytes) failed`** 与随之而来的 WS 断线重连；
   - 每 30 秒一行的 `heap: ... (main loop)` 中 **largest 应明显大于
     16,749**（这是 TLS 缓冲的硬门槛）；free/min 供观察趋势；
   - 可操作：总览上下选 agent、发一条预置指令（如「汇报当前状态」）、
     有回复上屏。
4. **若仍断线**：把断线前最近的几行 `heap:` 和新的 alloc 失败行发回
   即可——若 largest 仍 <16,749，下一步就查 paseo_client 之外的常驻
   大户（LVGL/音频侧）；若 heap 数字健康却仍 read error，则转查
   mbedTLS/TLS 侧配置（届时另开一版，不在本版范围）。

**验收级别：编译级**（构建通过 + 串检索入包 + 整包与构建产物一致 +
释放路径逐行自查），未上真机。
