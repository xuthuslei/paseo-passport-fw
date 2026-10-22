# 热修 0.3.3 (phase 3c)：decrypt_frame 按需分配 + 三阶段失败日志

整包：`dist/Paseo-Passport-0.3.3-phase3c-full.bin`
（7,664,505 B，md5 `f808aef376e49011616028b9ef9a20b3`，整包 0x0 刷入；
0.1.0–0.3.2 旧包未覆盖。构建日志 `build_hotfix033.log`，BUILD_EXIT:0；
已用字符串检索确认新日志串与新横幅都在整包内，且整包 app 区与本次
构建的 xiaozhi.bin 逐字节一致。）

启动横幅版本串：`Paseo Passport 0.3.3 (phase 3c)`（实机串口/屏显可核对）。

本版只验证一个假设（内存），刻意只动一处逻辑：websocket
buffer_size、帧组装器、paseo_box/pbase64/tweetnacl、握手时序、
0.3.2 的明文帧容错全部未动。

---

## 一、二分证据摘要（为什么改这里）

`research/DECRYPT_BISECT.md` 的结论：

- **组件代码无罪**：用组件原文件（paseo_box.c / pbase64.c /
  tweetnacl.c，非副本）在主机上解密从 Leon daemon 抓到的真实第一
  帧，成功且明文与 PyNaCl 逐字节一致；paseo_box_derive 的 shared
  与 PyNaCl 的 crypto_box_beforenm 输出逐字节一致。base64、shared
  派生、box 解密三层全清。
- **密钥材料无罪（方向性证明）**：daemon 只在成功解密客户端 hello
  后才回这帧，它回了 ⇒ 设备→daemon 方向加密正确；NaCl box 双向
  共用同一 shared，shared 错则双向皆错。故 offer 公钥、NVS 往返、
  设备私钥一并排除。
- **真实第一帧尺寸**：base64 文本 3,476 字符，解码后 2,606 B
  （24 nonce + 2,582 密文），明文 2,566 B（session/status=
  server_info）。WS 层单帧投递、无分片。
- **旧实现的浪费**：decrypt_frame 每次固定 `malloc(MAX_FRAME_BYTES)`
  = 49,152 B 作 raw 缓冲，是这帧实际需求（2,606 B）的 **18.9 倍**。
  而客户端在建连前已常驻占约 97.5 KB（c->plain 49,153 + c->toks
  49,152 + tl_text 1,536），ESP32-C3 无 PSRAM、总 DRAM 约 400 KB，
  又刚经历配网（SoftAP+httpd+DNS）与一次 TLS 握手，堆碎片化下
  「空闲总量够、最大连续块不够」是常态——49,152 B 连续分配失败
  完全现实，且每轮重连的堆图谱高度可复现，正好解释「每轮都在
  同一位置、帧到后约 0.5 秒内即时失败」。
- **瞬时需求对比**（这一帧的解密路径）：旧实现约 **54.4 KB**
  （固定 49,152 + box 内部约 5,244）；按需分配后约 **7.9 KB**
  （raw 按公式上界 2,611 B + box 内部约 5,244）。
- **旧日志不可判读**：malloc 失败、pb64_decode 失败、box 失败
  三种情况统一返回 -1，上层只打一条 `decrypt failed during
  hello wait`，无法区分是哪一阶段——本版一并修掉。

## 二、本版改动（仅两个文件）

### components/paseo_client/paseo_client.c

1. **decrypt_frame 改按需分配**：raw 缓冲大小按 b64 文本长度计算
   上界 `need = ((b64len + 3) / 4) * 3 + 4`，不再固定申请
   MAX_FRAME_BYTES。`b64len <= 0` 或 `need > MAX_FRAME_BYTES`
   （49,152，帧上限不变）时明确报错拒绝：
   `decrypt: bad frame size (b64len=… need=… cap=…)`
2. **三阶段失败分开打日志**，每条带关键数值：
   - 分配失败：`decrypt: raw alloc failed (need=… b64len=… free=… largest=…)`
   - base64 失败：`decrypt: base64 decode failed (b64len=… need=… rlen=…)`
   - box 失败：`decrypt: box open failed (rlen=… free=… largest=…)`

   其中 free = `esp_get_free_heap_size()`，largest =
   `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`（为此新增
   include esp_system.h / esp_heap_caps.h）。成功路径不加任何
   新日志。
3. pb64_decode 的输出上限随缓冲改为 need（解码器自身的越界检查
   语义不变）；c->plain 仍是 MAX_FRAME_BYTES 常驻缓冲，box 输出
   上限不变。

### main/paseo_app.cc

- 启动横幅改为 `Paseo Passport 0.3.3 (phase 3c)`。

## 三、实机复验步骤与判读

刷机（整包刷入会清 NVS，刷完重新走单页配网，粘同一个 offer）：

```sh
esptool.py --chip esp32c3 write_flash 0x0 dist/Paseo-Passport-0.3.3-phase3c-full.bin
```

开机别按住按键。串口重点看 paseo_client 的日志序列。

**成功应出现**：

```
paseo_client: relay websocket connected: pr-worker.178960.xyz:443
paseo_client: e2ee_ready received
paseo_client: duplicate e2ee_ready ignored        <- 可能出现，正常（0.3.2 的容错）
paseo_client: server_info received; daemon accepted hello
paseo_app: paseo state: ONLINE
```

总览屏出 agent 列表，且不再出现 `decrypt failed during hello wait`。

**若仍失败**：这次日志会直接点名阶段——把含 `decrypt:` 的那几行
（连同前后几行）发回即可，判读方式：

- `raw alloc failed` → 内存假设坐实，但连按需的约 2.6 KB 都拿不到：
  看 free/largest 两个数，下一步就是给常驻占用瘦身（toks/plain）。
- `base64 decode failed` → rlen=-1，组装出的字节不是合法 base64：
  嫌疑转到帧组装器，下一步查真机的 WS 事件切分。
- `box open failed` → 解密本身失败：rlen 应为 2606（与二分实测
  一致）；若 rlen 对而 box 仍失败，则与主机结论矛盾，需要做
  字节级内容比对（把设备收到的帧与探针帧逐字节对）。
- `bad frame size` → 到达的帧长异常（b64len 远超 3,476 或为 0）：
  同样指向组装/投递路径。

## 四、验收级别

编译级验收：构建一次通过、新串检索入包、整包 app 区与构建产物
逐字节一致。未上真机——解密是否恢复、以及若未恢复到底卡在哪个
阶段，等本版实机日志定论。
