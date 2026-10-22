# 解密失败二分报告（DECRYPT_BISECT）

日期：2026-10-22
现象：真机 0.3.2 连 Leon 的 daemon，relay/e2ee 握手完成、重复 e2ee_ready
已被忽略，但 daemon 第一帧真密文仍报 `decrypt failed during hello wait`，
随即整链重连、无限循环。同一 offer 用 tools/ref_client.py 连同一
daemon 完全正常。

## 结论先行

- **固件组件代码无罪**：用组件**原文件**（paseo_box.c / pbase64.c /
  tweetnacl.c，非 tn_test 的副本）在主机上解密探针抓到的**真实第一
  帧**，成功且明文与 PyNaCl 逐字节一致；paseo_box_derive 的 shared
  与 PyNaCl 的 crypto_box_beforenm 输出逐字节一致。
- **密钥材料无罪（方向性证明）**：daemon 只在成功解密客户端 hello
  后才回这帧（hello 前探针等 3 秒，0 帧到达）；它回了，说明设备→
  daemon 方向的加密被 daemon 正常解开。NaCl box 双向共用同一
  shared，shared 若错则双向皆错——故 offer 公钥/NVS 往返/设备私钥
  这条线基本排除。
- **失败被限定在设备接收路径的运行时**：WS 组装或 decrypt_frame 的
  运行时环境。头号嫌疑是 **decrypt_frame 固定 malloc 48KB 在真机
  碎片化堆上失败**（详见第 4 节内存专节）；次嫌疑是组装器在真机
  实际投递模式下的字节问题（逻辑走查正确，未能在主机证伪/证实）。

## 1. 工具与用法

| 工具 | 作用 |
|---|---|
| `tools/ref_probe.py --offer '<url>'` | 与 ref_client 同链路（隧道 127.0.0.1:8443、SNI=pr-worker.178960.xyz）只读连接：e2ee 握手 → 发 daemon hello → 抓第一帧密文原文并用 PyNaCl 解密，四样数据（会话私钥 b64、daemon 公钥 b64、第一帧密文 b64 原文、明文）落 `tools/probe_out.json`（**含会话私钥，只留本机**） |
| `tools/ws_frame_probe.py --offer '<url>'` | 原始 socket 手工解析 WS 帧头，观察 relay 实际投递的分帧（fin/opcode/长度），并做 hello 前主动性测试 |
| `tools/host_fw_decrypt.c`（+`randombytes_shim.c`，/dev/urandom） | 主机夹具：直接编译组件原文件，复刻 paseo_client.c 的 decrypt_frame 管线（pb64_decode → paseo_box_derive → paseo_box_decrypt），输入 probe_out.json，shared 与明文写 bin 供比对 |

构建夹具：

```sh
FW=~/workspace/paseo-passport-fw/firmware
gcc -O1 -o tools/host_fw_decrypt tools/host_fw_decrypt.c tools/randombytes_shim.c \
  $FW/components/paseo_client/paseo_box.c $FW/components/paseo_client/pbase64.c \
  $FW/components/tweetnacl/tweetnacl.c \
  -I$FW/components/paseo_client -I$FW/components/paseo_client/include \
  -I$FW/components/tweetnacl
./tools/host_fw_decrypt tools/probe_out.json shared.bin plain.bin
```

## 2. 二分证据

### 2.1 真实第一帧（探针实测）

daemon 在 hello 后回的第一帧即失败帧本体：

- base64 文本长 **3,476** 字符；解码后 **2,606** B（24 nonce + 2,582
  密文 = 2,566 明文 + 16 MAC，自洽）；明文 **2,566** B，
  type=`session`、内层 `status`（server_info）。
- WS 分帧（ws_frame_probe 实测）：**单帧** fin=1、op=1、len=3,476，
  daemon/relay/Cloudflare 任一跳都未做 WS 层分片。
- 主动性：e2ee_ready 后、发 daemon hello 前等待 3 秒，**0 帧**到达；
  hello 发出后该帧随即到达。

### 2.2 组件原文件解密（夹具实测）

- `pb64_decode(frame)` → rlen **2,606**（与探针一致）
- `paseo_box_decrypt` → plen **2,566**，明文 sha256 前 8 位
  `e1e6880e`，与 PyNaCl 解密结果**逐字节一致**
- `paseo_box_derive` 的 shared sha256 前 8 位 `1124a409`，与
  PyNaCl `crypto_box_beforenm(daemon_pub, dev_sec)` 输出**逐字节
  一致**

判定：base64 解码层、shared 派生层、box 解密层——**三层全部无罪**
（且这次是组件原文件，不是 tn_test 的副本代码）。

### 2.3 方向性证明（排除密钥材料）

2.1 的主动性测试表明该帧是 daemon 对 hello 的应答，而 hello 本身
在 box 内，daemon 必须先成功解密才能应答。设备日志亦显示 daemon
的应答帧在 hello 发出后约 0.5 秒即到达并触发失败——不是 20 秒
HELLO_WAIT 超时。结论：设备上行加密正确 ⇒ 设备侧 shared 正确 ⇒
失败只可能发生在设备收到字节之后、box 验 MAC 之前的环节（组装
字节不完整/被改，或 decrypt_frame 根本没跑到 box）。

### 2.4 组装器走查（针对该帧长度）

- 常量：MAX_FRAME_BYTES = 49,152；该帧 3,476 远低于上限。
- esp_websocket_client 的 buffer_size = **2,048 < 3,476**：该帧在
  设备上必然被切成 2 个 DATA 事件（payload_len 恒为 3,476，
  payload_offset 0 → 2,048，data_len 2,048 + 1,428）。托管组件
  esp_websocket_client.c 的投递语义已逐行核对（payload_len 为整帧
  长、payload_offset 为帧内偏移），固件组装器对此模式的
  reset/append/完成判定逻辑走查**正确**。
- 但注意：这是本会话中第一个 >2,048 的帧——握手期所有帧
  （e2ee_ready 等）都是几十字节的单事件帧。失败恰好首现于第一个
  需要跨事件组装的帧，这一相关性使组装路径仍留嫌疑（见第 5 节）。

## 3.（并入第 4 节）

## 4. 内存假设专节：decrypt_frame 的固定 48KB 分配

背景：paseo_client.c 的 decrypt_frame() 每次调用固定
`malloc(MAX_FRAME_BYTES)` = 49,152 B 作 raw 缓冲；malloc 失败、
pb64_decode 失败、paseo_box_decrypt 失败**统一返回 -1**，上层只打
同一条 `decrypt failed during hello wait`，现有日志无法区分阶段。

### 4.1 实际需求 vs 固定分配

对真实第一帧（2.1）：

| 项 | 字节 |
|---|---|
| pb64_decode 实际产出（raw 真实需求） | **2,606** |
| 按 b64 长度的上界估算（3,476 × 3/4） | 2,607 |
| decrypt_frame 固定分配 | **49,152（实际需求的 18.9 倍，单次浪费 46,546）** |
| paseo_box_decrypt 内部 calloc+malloc（clen=2,622 ×2） | ≈5,244 |
| 该帧解密路径瞬时需求：现状 / 按需分配 | **≈54.4 KB / ≈7.9 KB** |

即：这一帧的解密本只需要约 8KB 瞬时内存，现实现却要求一块
49,152 B 的**连续**堆块（malloc 语义是连续块，不是总量）。

### 4.2 客户端常驻占用（失败发生前已先吃掉的堆）

paseo_client_start 在建连前已常驻分配：

| 项 | 字节 |
|---|---|
| c->plain（MAX_FRAME_BYTES+1） | 49,153 |
| c->toks（MAX_TOKENS 3,072 × pj_tok_t 16 B） | 49,152 |
| c->tl_text（PASEO_TIMELINE_TEXT_CAP） | 1,536 |
| 合计 | **≈99,841（约 97.5 KB）** |

外加 client 任务栈 10,240、ws 任务栈 8,192、asm_buf（本帧 3,477）
及 WSS/TLS 侧的 mbedTLS 缓冲。ESP32-C3 无 PSRAM，总 DRAM 约
400KB，IDF+WiFi/LWIP+LVGL（小智基座）稳态后可用堆本就有限，且
真机在此前刚经历完整的配网周期（SoftAP+httpd+DNS+站点切换）与
一次 TLS 握手——堆碎片化显著，**最大连续块远小于空闲总量**是
这类设备上的常态。在这种堆状态下，49,152 B 连续分配失败是
完全现实的可能。

### 4.3 能否解释"每轮都在同一位置失败"——能，且高度吻合

1. **首次调用即此帧**：整个会话里 decrypt_frame 第一次被调用
   就是这一帧（此前全是明文帧）。它在任何一版固件上从未在真机
   成功过——与"48KB 分配在这个堆状态下从来拿不到"完全一致，
   也与"更小的帧先行成功、到这帧才暴露"的观察不矛盾（小帧根本
   不经过 decrypt_frame）。
2. **确定性复现**：每轮重连走完全相同的序列——同样的常驻分配、
   同样的配网后堆状态、同样的 TLS 握手分配模式——decrypt_frame
   申请 48KB 时面对的堆图谱高度可复现，失败因此每轮必现、且
   都在帧到达后约 0.5 秒内（即时 -1，不是超时），与日志时间线
   吻合。
3. **对照组**：主机的夹具在堆不受限的环境下用同一份代码成功；
   ref_client 亦然。差异变量只剩运行环境，堆是其中最具体的一个。

保留意见：主机侧无法证实真机当时最大连续块的确切数值；也不能
排除组装字节在真机上确有损坏（4.3-1 的"首次调用"论点对组装
嫌疑同样成立）。两者靠现有日志不可区分——这正是 5.2 诊断版
要一次测清的。

### 4.4 修复/诊断建议（按任务要求，未改固件代码，待确认）

按优先级：

1. **先出诊断版（一次实测定位）**：decrypt_frame 的 -1 拆成三种
   日志——alloc 失败（打印申请字节、esp_get_free_heap_size()、
   heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)）、b64 失败
   （打印 b64len 与 rlen）、box 失败（打印 rlen）；并在 hello
   wait 入口打印一次 free heap / largest block。有这四个数，
   下一轮真机日志即可定罪。
2. **raw 按需分配**：容量改为按输入计算（b64len × 3/4 + 4），或
   改为与 c->plain 并列的常驻复用缓冲（启动时一次分配、每帧
   复用）——本帧场景瞬时需求从 ≈54.4KB 降到 ≈7.9KB，且不再有
   每帧大块申请。
3. **顺带**：ws buffer_size 2,048 → 8,192，使 ≤8KB 的帧单事件
   投递，消除跨事件组装边界（代价 +6KB 常驻）；组装嫌疑若被
   诊断版排除，此项可降级为可选。
4. 中长期：toks 的 49,152 B 常驻（3,072 token）对只需取少数字段
   的解析偏奢侈，可评估下调 MAX_TOKENS 或改为按帧临时分配，
   给堆留更多余量（仅建议，未评估解析上限影响）。

## 5. 真机侧剩余嫌疑排序

| # | 嫌疑 | 评估 |
|---|---|---|
| 1 | decrypt_frame 的 49,152 B malloc 在碎片化堆上失败（与 box/b64 失败同日志） | **高**。见第 4 节：量级浪费 18.9 倍、常驻已占 ≈97.5KB、确定性复现、首次调用即失败 |
| 2 | 组装器在真机实际事件序列下字节损坏（chunk 尺寸/事件次序与主机假设不同） | 中低。逻辑走查正确、分帧事实明确（单帧 3,476），但真机 esp_transport 的实际 chunk 切分未经日志证实；若诊断版显示 alloc 正常且 rlen 异常，回到此项 |
| 3 | decrypt_frame 忽略 b64len、以组装器保证的 NUL 结尾为前提 | 低。组装器确实 NUL 结尾且缓冲独立（len+1），仅在 #2 成立时才连带出问题 |

已排除：box 加解密逻辑、shared 派生、base64 编解码（2.2 组件
原文件实测）；offer 解析与 NVS 往返、设备密钥材料（2.3 方向性
证明）；relay/daemon 侧行为（ref_client 与探针对同一 daemon
全程正常）。

## 6. 隐私与产物

- probe_out.json 含一次性会话私钥，只留在本机 tools/，未外传；
  该会话已结束、私钥无后续价值。比对用的 shared/plain bin 已
  删除。本报告只记录长度与 sha256 前 8 位，不含密钥与明文全文；
  未引用 Leon daemon 中任何 agent 的内容（仅 frame type 与字节
  数）。
- 新增文件：tools/ref_probe.py、tools/ws_frame_probe.py、
  tools/host_fw_decrypt.c、tools/randombytes_shim.c（+ 编译产物
  tools/host_fw_decrypt）。
