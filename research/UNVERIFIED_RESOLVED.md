# PROTOCOL_SUBSET.md 未核实清单 — 0.5 期实测结论

> 实测日期：2026-10-22。环境：本机联调 daemon v0.12.0 经自有 relay
> pr-worker.178960.xyz:8443（本地 CONNECT 隧道中转）连接，参考客户端
> `tools/ref_client.py` 实跑两轮（建 codex agent + 追发消息 + timeline +
> 90 秒无 ping 探针）。样本帧见 `research/samples/`（密钥材料已打码）。

## 链路（先记清，后面每项的前提）

daemon 侧：本机出网走 CONNECT 代理且域名被 fake-IP 解析劫持，Node 直连
外网 TLS 报 wrong version number。最终形态：
`daemon --TLS(wss, SNI=pr-worker.178960.xyz)--> 127.0.0.1:8443 隧道
（tools/connect_tunnel.py，经 $https_proxy CONNECT 到 pr-worker:443）
--> Cloudflare pr-worker --> daemon 的 relay 控制/数据 socket`。
daemon 进程在独立 mount namespace 中以 bind 挂载的 /etc/hosts 解析
pr-worker.178960.xyz → 127.0.0.1（/etc/hosts 本体是只读 squashfs），
启动脚本 `tools/run_dev_daemon.sh`；config 改动（已备份
config.json.bak-20261022-prworker）：daemon.relay.endpoint /
publicEndpoint = pr-worker.178960.xyz:8443，useTls / publicUseTls = true，
`paseo reload` 提示 relay 变更必须 `daemon restart` 才生效。
daemon.log 出现 `relay_control_connected`，`paseo daemon status` 显示
relay endpoint 已切换，判定连通。

## 逐项结论

1. **server_info 时机 —— 已解决：hello 后主动推送，无需订阅。**
   客户端发送 daemon hello 后、发任何订阅之前（5 秒观察窗内），
   session/status(server_info) 即到达（serverId、version=0.12.0、
   protocolVersion=1）。固件 hello 后直接等它做一致性校验即可，
   订阅 status.server_info 只是冗余保险。样本：samples/status.json。

2. **fetch_agents 目录订阅的推送组合 —— 已解决（实测范围）。**
   fetch_agents_request 带 subscribe 后，本轮 agent 生命周期内收到：
   `agent_update`（完整快照 upsert，共 8 条）与 `agent_stream`（时间线
   流式增量，共 9 条）；未单独出现 `agent_status` 报文（本轮状态变化
   都经 agent_update 承载）。注意：**agent_stream 不需要单独订阅
   agent.timeline 也会随目录订阅到达**——固件若不想处理它，必须在
   解析层直接丢弃（内容可含大工具输出，见子集文档 §4.5 的风险）。
   节流行为未见（量级小）；agent 多时的节流仍未核实，固件按流式
   解析 + 自设帧上限防御即可。样本：samples/agent_update.json、
   samples/agent_stream.json。

3. **send_agent_message 的 attachments 空形态 —— 已解决：`[]` 可接受。**
   send_agent_message_request 带 `"attachments": []` 发送，daemon 回
   send_agent_message_response `accepted=true, error=null`。
   固件固定填 [] 即可。样本：samples/send_agent_message_response.json。

4. **activeTurnBehavior 枚举 —— 已解决（源码）：`interrupt` | `steer`。**
   取值二选一（@getpaseo/protocol
   dist messages.d.ts 的 ActiveTurnBehaviorSchema）。固件不填走
   服务端默认即可；若将来做「打断」键，用 interrupt。

5. **timeline item 内部结构 —— 已解决（源码 + 实测双证）。**
   item 是按 `type` 判别的联合：`user_message{text, messageId?,
   clientMessageId?}`、`assistant_message{text, messageId?}`、
   `reasoning{text}`、工具调用型（ToolCallTimelineItem）、`todo`、
   `error{message}`、`notification{level, message}`、compaction、
   plugin。**抽取最近输出的路径就是 `entries[].item.type ==
   "assistant_message"` 时的 `item.text`**，实测样本中 user_message
   与 assistant_message 的键集合与源码声明一致。
   样本：samples/fetch_agent_timeline_response.json。
   ⚠️ 本轮 codex 因无凭证 401，assistant_message 内容是
   `[System Error] ... 401 Unauthorized ...`——结构有效，但真实
   工具调用型 item（tool call）本轮未抓到样本，固件解析器对未知
   item.type 一律跳过即可（联合类型本就开放）。

6. **daemon 对不发应用层 ping 的客户端 —— 已解决（90 秒窗口内无服务端断开）。**
   探针连接完成 hello + 一次 fetch 后静默 90 秒（不发应用层 ping、
   无任何流量），之后再次 fetch_agents 正常返回。结论：至少 90 秒
   尺度上 daemon 不因缺少应用层 ping 强制断开；更长尺度未测，
   固件仍建议照官方 10s ping 节奏（成本极低，且 pong 样本已抓：
   samples/pong.json，本轮主连接收到 pong ×13）。
   另附带踩坑（固件必须规避）：**hello 被 daemon 接受之前发出的
   任何 RPC 都会被拒**——daemon 对 pending 连接的非 hello 报文记
   `Rejected pending message before hello` 并以 Invalid hello 关闭
   连接，relay 随之以 1012 关掉客户端。固件时序必须是：发 hello →
   等 server_info 到达 → 再发订阅/RPC（参考客户端第一版探针就死
   在这里，已修正为等 3 秒/等 server_info）。

7. **legacy create_agent_request 的响应类型 —— 已解决（源码）：无配对
   RPC 响应，成功/失败以事件报文 `agent_created` / `agent_create_failed`
   推送。** 固件不用 legacy 路径，此项仅存档。

## 本轮未覆盖（如实标注）

- **审批链路（agent_permission_request → agent_permission_response）
  未实测**：本机 codex 未登录 OpenAI（codex login status = Not
  logged in、无 API Key），每个 turn 在调模型时即 401，agent 从未
  执行到工具调用，自然没有权限请求产生；显式订阅的
  agent_attention_required 事件本轮亦未触发（0 次）。报文结构仍以
  PROTOCOL_SUBSET.md §4.4 的源码结论为准；待任一 provider 有可用
  凭证后，用 ref_client.py --create 一轮即可补测（客户端已内置
  --auto-approve 自动批准逻辑与样本抓取）。
- agent 多时的目录订阅节流、32KB 以上大帧在 relay 上的实际表现，
  未测（固件侧按子集文档 §6 防御策略实现）。

## 进程与恢复（本机重启/会话结束后如何拉起来）

1. 隧道：`cd tools && setsid nohup python3 connect_tunnel.py > tunnel.log 2>&1 &`
2. hosts 覆盖文件：/tmp/hosts.new 需含 `127.0.0.1 pr-worker.178960.xyz`
   （VM 重启后 /tmp 清空，需重建——从 /etc/hosts 复制后追加一行）。
3. daemon：`setsid nohup tools/run_dev_daemon.sh > tools/daemon-run.log 2>&1 &`
   （脚本内含 bind 挂载 + getent 自检 + paseo daemon run 前台运行）。
4. 参考客户端：`python3 tools/ref_client.py --pair-file /tmp/pair.json --create`
   （offer 也可经 --offer 或 --home 现场生成）。
