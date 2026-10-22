# Paseo Passport 固件 — 二期说明（0.2.0-phase2）

在一期（配网 + relay/E2EE + fetch_agents 只读总览）之上完成设计文档
§10 的二期范围：按键接入、四屏可导航 UI（总览/结果/待审批/指令菜单）、
审批动作、发指令与新建任务。二期仍为**编译级验收**（物理设备不在
本机），协议行为对照 `../tools/ref_client.py` 与
`../research/samples/` 的真实报文样本实现。

设计依据同前：`../DESIGN_V1.md`、`../research/PROTOCOL_SUBSET.md` §4、
`../research/UNVERIFIED_RESOLVED.md`。

## 按键与长按约定

- UP/DOWN/OK 为 ADC 电阻梯按键（GPIO0/ADC1_CH0 共用）。板级回调不再
  指向未启动的 xiaozhi Application：`paseo_app` 在板构造前注册按键
  钩子，板级回调优先转发到 paseo 输入队列；钩子未注册时保留原
  xiaozhi 行为（音量/聊天），互不干扰。
- 短按/长按由 iot_button 框架区分：**长按阈值 2000 ms**，沿用基座
  惯例（`main/boards/common/button.cc` 中 AdcButton 的
  `long_press_time = 2000`）。短按 = 阈值前松手（click 事件），
  长按 = 按住达到阈值（long_press_start 事件），两者互斥。
- **开机进配网（BOOT 阶段判定，与运行期长按不冲突）**：已有配置时，
  开机后约 2.6 s 的 BOOT 窗口内若检测到 OK 长按（从上电起按住），
  强制进配网模式。窗口在进 station 模式前关闭，运行期各屏的长按
  OK 语义（见下表）不会触发配网。无配置时照旧自动进配网（不等待）。

## 操作表

### 总览屏
| 按键 | 行为 |
|---|---|
| UP / DOWN 短按 | 焦点上/下移一行（钳制不回绕） |
| UP / DOWN 长按 | 焦点跳移 5 行 |
| OK 短按 | 进结果屏（对焦点 agent 拉 timeline） |
| OK 长按 | 进待审批屏（无待审批时底部反馈一行提示） |

每行：`[状态] 标题 <待审批xN>`；顶部标题行含全局角标：连接状态、
agent 数、待审批总数（>0 才显示）。

### 结果屏
| 按键 | 行为 |
|---|---|
| UP / DOWN 短按 | 文本滚动 1 行 |
| UP / DOWN 长按 | 文本滚动 10 行 |
| OK 短按 | 进指令菜单 |
| OK 长按 | 返回总览屏 |

进屏自动发 `fetch_agent_timeline_request`（`direction:"tail"`、
`limit:5`、`projection:"projected"`，参数照子集文档 §4.5 与样本），
取 entries 中**最后一条** `assistant_message` 的 `text` 显示；长文在
客户端截断到 1536 B、UI 侧再按行宽折行（最多保留 200 行）滚动显示，
未知 item 类型一律跳过。发送指令后回到本屏会自动重新拉取。

### 待审批屏
| 按键 | 行为 |
|---|---|
| UP / DOWN 短按 | 切换条目（含末尾「← 返回总览」行） |
| UP / DOWN 长按 | 跳到首条 / 跳到「返回」行 |
| OK 短按 | **批准**焦点条目（「返回」行上则回总览） |
| OK 长按 | **驳回**焦点条目（「返回」行上则回总览） |

- 数据源：快照 `pendingPermissions`（fetch_agents/agent_update 时
  重建）+ `agent_permission_request` 事件新增、`agent_permission_resolved`
  事件移除，三者统一进客户端待审批缓存（上限 8 条，超限丢弃并打
  日志）。每条显示工具名 + 关键参数摘要（title → description →
  input 中 command/file_path/path/url/query/prompt/pattern 的首个
  字符串，截断显示）+ 所属 agent 与 kind。
- 批准/驳回发 `agent_permission_response`。**红线**（子集文档
  §4.4）：该报文 `requestId` 填权限请求自身 id，不是 RPC 关联 id；
  报文无 payload 包装、无响应报文。发出后等 resolved 事件刷新
  （条目消失即底部反馈「审批已生效」）；**10 s 未等到则自动发
  fetch_agents 重新对账**。同一条目处理中重复按键忽略。

### 指令菜单（结果屏 OK 进入）
| 按键 | 行为 |
|---|---|
| UP / DOWN 短按 | 切换菜单项（长按跳 5 项） |
| OK 短按 | 执行焦点项 |
| OK 长按 | 不执行，直接返回结果屏 |

发送类动作的结果在底部反馈行给出一行结果（发送中 → 已受理/失败
原因；RPC 25 s 超时同样反馈）。语音输入在三期接入同一菜单入口。

## 预置指令清单

| 菜单项 | 报文 | 内容 |
|---|---|---|
| 继续 | `send_agent_message_request` | text=`继续` |
| 汇报当前状态 | `send_agent_message_request` | text=`汇报当前状态` |
| 跑一遍测试 | `send_agent_message_request` | text=`跑一遍测试` |
| 停止当前任务 | `cancel_agent_request` | 对选中 agent，无正文 |
| 新建任务 | `agent.create.request` | config.provider/cwd 取 NVS 默认值（`def_prov` 预置 codex、`def_cwd` 配网页填），initialPrompt=`汇报当前状态`，带 idempotencyKey |
| 返回 | — | 回结果屏 |

`send_agent_message_request` 固定 `attachments: []`、带随机
messageId 去重、不填 activeTurnBehavior（走服务端默认）——均按
子集文档 §4.3 与 0.5 期实测结论。新建任务时若 `def_cwd` 为空，
不发送并反馈「未配置默认 cwd」。

取舍说明：任务定义里总览屏 OK 与「OK 进指令菜单」有重叠，本期定为
总览 OK = 结果屏、结果屏 OK = 指令菜单（总览长按 OK 给待审批屏），
一条按键路径可达全部功能。

## 实现要点（纪律延续）

- 出站通路：`paseo_client` 新增 outbox 队列（上限 8）。UI 侧的
  `paseo_client_*` API 只负责拼内层报文 + 入队，**真正发送只发生在
  客户端任务的 ONLINE 主循环**——hello 被接受（server_info 到达）
  前绝不发 RPC 的一期红线不变。断开时未发送队列与未决 RPC 一并
  收尾（回调报 disconnected），不跨连接补发。
- 入站纪律不变：单帧 48 KB 上限、agent_stream 只读信封即丢弃、
  JSON 零拷贝分词提取（jsmn 式），不做整包 DOM。timeline 解析只
  遍历 entries/items 骨架，除最后一条 assistant 文本外全部字段
  流式丢弃；单条文本上限 1536 B，条数/行数在 UI 侧再设上限。
- 待审批/agent 缓存都在客户端任务内维护，回调把快照拷给 UI；所
  有 LVGL 操作只在应用任务上做（回调不碰显示），UI 状态全部在
  `paseo_ui`（`main/paseo_ui.cc`）内管理，不依赖 xiaozhi
  Application。UI 为文本模式四屏（整屏 overlay + 头/正文/脚三段
  label，行宽按 ASCII=1、宽字=2 截断折行），离开在线状态自动
  拆除 overlay 回到一期的 stock UI（配网提示等)。
- RPC 关联：timeline/send/create 各只跟踪一个未决请求（requestId
  匹配 payload 回显），后发覆盖先发；25 s 超时回调报错。

## 与一期的差异

- 板级按键：无作用 → 全功能（钩子转发，未注册时保留 xiaozhi 旧
  行为）；OK 新增长按回调注册。
- 总览屏：聊天 UI 追加气泡 → 专用 overlay 列表（可导航、焦点、
  角标），agent 更新原地刷新不再堆气泡。
- 客户端：只读订阅 → 双向（outbox + 6 类新报文处理：permission
  request/resolved、attention、timeline 响应、send 响应、create
  响应；快照里解析 pendingPermissions 明细）。
- 开机长按 OK 进配网由隐含行为改为显式 BOOT 窗口判定（2.6 s）。
- 应用版本串 0.1.0 → 0.2.0；一期整包保留在 dist 不覆盖。

## 已知限制

- 编译级验收：未在真机/真 daemon 上联调。审批链路沿用 0.5 期未
  覆盖项——本机无可用 provider 凭证造不出真实权限请求，请求对象
  字段以子集文档 §4.4 的源码结构为准（样本里 pendingPermissions
  为空数组）；首次实机验收点：对联调 daemon 完成一次审批 + 一次
  指令往返（设计文档二期验收标准）。
- timeline limit=5：最后 5 条 entries 里没有 assistant_message 时
  结果屏显示「（暂无文本输出）」，不自动扩大窗口重拉。
- 待审批角标数 = 已缓存的待审批条数（全局上限 8）；agent 上限
  仍为 20（fetch page.limit=20，无分页）。
- cancel_agent_request 在协议里无文档化的配对响应：菜单反馈为
  「已发送停止」，实际停止以随后 agent_update 的状态为准。
- 标题/摘要/结果文本按显示宽度截断；基座动态字库之外的字形仍
  可能缺字（与一期相同）。
- 「新建任务」的 initialPrompt 固定为预置「汇报当前状态」，自定
  义首条指令需等三期语音/文本输入。
- 报文子集与 daemon v0.12.0 对齐；升级 daemon 前先跑 0.5 期参考
  客户端回归（一期纪律延续）。

## 构建与产物

```sh
source ~/esp/esp-idf-v6.0.1/export.sh
cd firmware
idf.py build
cd build
python3 -m esptool --chip esp32c3 merge_bin -o ../dist/Paseo-Passport-0.2.0-phase2-full.bin @flash_args
```

本期产物（2026-10-22 构建）：应用 xiaozhi.bin 0x1f7120 B（应用分区
0x2f0000 B，余 33%）；整包 7,664,505 B（0x74f379），
md5 `3f6d5036dfac5734683d48b60a4049f2`。
构建日志 `build_phase2.log`（含 BUILD_EXIT:0）。

刷机：`esptool.py --chip esp32c3 write_flash 0x0 dist/Paseo-Passport-0.2.0-phase2-full.bin`
（整包一次写入；会清掉旧 NVS 配置，需重新配网。）
