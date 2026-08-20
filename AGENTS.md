# AGENTS.md — 长期工程契约

> 本文件只保留**长期有效**的契约与项目概览。可执行规则已分层（见下「分层规则」）；单次实验/阶段 marker/完整 panic 记录写日志或 `face-emotion-sd.mdc` 战史，不写本文件。冲突时以本文件与用户当前要求为准。

## 每次任务的强制启动检查

- 收到新任务、动手前，先读**作战卡** `.cursor/rules/p4-session-preflight.mdc`（每回合唯一必读）。
- 再按任务类型按需读：改 `main/**` 前读 `p4-hard-constraints.mdc`；崩溃/表情读 `face-emotion-sd.mdc`；换阶段/定原则读 `xiaozhi-p4-core.mdc`；要方法论读 `p4-strategy-philosophy.mdc`；「我在哪」读 `p4-current-state.mdc`。
- 读规则不等于扩大修改范围。开始修改前明确本次目标、预计修改文件清单、不在范围的文件；只改完成目标所必需的最小集合。
- **禁**「顺手整理」、批量格式化、清目录、移文件、改无关配置、更新依赖或动第三方/managed component。根因确在依赖里，须先说明证据/拟改文件/回退方式并获用户同意。
- 不覆盖/删除/还原用户已有改动；发现无关改动保留原样；与目标重叠且无法安全绕开时停下说明。
- 每次修改后列出实际改动文件；出现计划外变化先停止查明来源，恢复到任务边界内。
- 用户指定「只改某文件」时该指令是硬边界；除该文件外不产生任何写入（含规则/日志/快照/构建产物/依赖），除非追加授权。

## 分层规则

| 层 | 文件 | 职责 |
|----|------|------|
| 作战卡（唯一必读） | `.cursor/rules/p4-session-preflight.mdc` | 四问 / 批次准入 / 弹药预算 / 分层索引 |
| 冻结事实 | `.cursor/rules/p4-hard-constraints.mdc` | SD / AFE 栈 / UDP Opus / 硅片 / 烧录 / 显示 |
| 战术+战史 | `.cursor/rules/face-emotion-sd.mdc` | 崩溃归因序 + 账本 + 演进阶梯 |
| 原则 A–G + 阶段 | `.cursor/rules/xiaozhi-p4-core.mdc` | 跨阶段可执行原则 |
| 方法论 | `.cursor/rules/p4-strategy-philosophy.mdc` | 世界观 / 第一性原理 |
| 当前状态（常改） | `.cursor/rules/p4-current-state.mdc` | 阶段 / known-good / marker / 下一刀 |

## 项目概览

基于 ESP-IDF 5.4.x、面向 ESP32-P4 与 EP Chat P4 ML307 的小智语音终端固件。ML307 4G、480×480 屏、SD 卡表情。能力：小智云语音会话与 TTS；本地闹钟；HTTP 提醒轮询 + MQTT 即时唤醒；MCP 查询提醒队列；LVGL + EEZ UI；SD MJPEG 表情；串口/崩溃/看门狗/长期稳定性诊断。工程根 `xiaozhi-p4-epdainaozhong/`。先读 `README.md`、`docs/team-onboarding.md`、`docs/architecture-reminder-poll-mcp.md`。

## 关键代码地图

| 模块 | 路径 |
|------|------|
| 应用状态机 | `main/application.cc` |
| 提醒轮询 / MQTT 唤醒 | `main/reminder/reminder_poller.cc` · `reminder_mqtt_wake.cc` |
| 本地闹钟 | `main/alarm/general_timer.cc` |
| MQTT/UDP 协议 | `main/protocols/mqtt_protocol.cc` |
| 显示适配 / Presenter | `main/boards/ep-chat-p4-ml307/eezui_display_adapter.cc` · `screen_presenter.cc` |
| 表情解码 / 嘴部分层 | `main/boards/ep-chat-p4-ml307/emotion_video_player.c` · `face_mouth_layer.c` |
| SD 卡 | `main/boards/ep-chat-p4-ml307/sd_scanner.c` |

## 核心架构原则（长期）

- **UI**：React 类比（不引入运行时）——开机 seed → 状态增量 Commit → 局部刷新。「局部」由**素材能力**定义（`full_frame_clip`/`fixed_roi_clip`/`layered`/`static`），不由固定行数定义；≥400 行条带＝打折全屏。刷新成本量子＝显示缓冲整条刷带（全宽×50 行），横向缩框不省钱。全屏事务单例 + 安全窗。**PSRAM 容量非瓶颈，约束是单位时间总线突发**。禁旁路抢面板；禁无预算全屏 burst / 持续 inject。
- **通信**：MQTT=控制 JSON；UDP=加密 Opus（上行麦 + 下行 TTS）。云端识别语音必须 UDP 上行 Opus；`SEND_WAKE_WORD_DATA=y` 须先发 wake opus。禁为省资源关上行 Opus 却宣称云端可对话。
- **主动提醒**：默认 `MQTT wake/HTTP poll → mcp_wake → wake opus+short detect → MCP 读队列 → cloud TTS → ack`。MQTT 秒级门铃，HTTP 轮询兜底。`mcp_wake` 只发短 `wake_text`，正文由 MCP 读同一 HTTP 队列。无提醒必须静默；TTS 未真正收到音频不提前 ACK。遵守 session/preemption。
- **SD / AFE / 共享对象 / 硅片 / 烧录**：见 `p4-hard-constraints.mdc`（命令式冻结表），不在此重述。

## 对话情绪与嘴部动画演进原则（长期，落地阶梯见 current-state M0–M3）

- 情绪表达与嘴部开合是**两条独立可合成的数据流**：情绪状态机负责意图与阶段，音频包络只负责即时嘴部等级；**不得用音量直接切换情绪**。
- 情绪资源描述 `enter/hold/exit` 三段；情绪状态至少 `requested/pending/committed`，新请求先 pending，只在安全点提交，允许覆盖尚未提交的旧请求，不排队播过期情绪。
- TTS 生命周期控制情绪阶段：开始前进入情绪、播放维持 hold、结束或被抢占走 exit/过渡回 standby 或下一 pending。
- **音频任务只算平滑 RMS/包络并发布 `closed/small/medium/large`**，不解 JPEG、不调 LVGL、不等显示完成；嘴部事件用 latest-value/覆盖式队列，慢则丢旧。
- **Face Worker 是显示数据流唯一消费者**：读已提交情绪 + 最新嘴部等级，预算允许时更新嘴部小区域；服从 UI 所有权与线程约束；需足量任务栈（≥8192，深处采样判栈）。
- 嘴部更新设迟滞/最短保持/静音回落/最大刷新率；音频·SD·PSRAM·显示总线紧张时优先降嘴部刷新率或退静态，不得影响 Opus/AFE/TTS 连续性。
- **嘴/眼必须是独立小图**；从整帧实时抠取只减上屏拷贝，不减解码——缺分层资源时退**静态情绪**，不解全帧再裁。
- **低频 life 轨必须通过语义门**：帧差/运动分数只能产出候选，正式包须由 profile 明确批准轨道及语义；头发、饰品或背景的单一区域持续重复运动默认拒绝，除非产品明确把它定义为该表情的主体动作。优先保留呼吸、轻微身体重心、面部小动作等不抢嘴部注意力的轨；不同素材必须重新批准，禁在固件按固定 track id 硬编码。工具须输出批准状态、语义标签和预览，未批准时允许静态/单轨回退。
- **先判视觉预算再解码**；预算至少考虑音频采集与播放队列深度、近期欠载、显示耗时、是否已有全屏事务。
- 实时等级：音频采集与播放 > 会话与网络控制 > 必显 UI 状态 > 情绪静态切换 > 表情动画帧。低级流可丢/可冻/可降。
- 素材清单声明渲染能力（`full_frame_clip`/`fixed_roi_clip`/`layered`/`static`）；只有通过稳定性检测的素材才启用 layered 嘴部；头部位移/镜头运动/口部遮挡明显时保留全帧或静态回退。PC 工具生成帧索引/分段/嘴部 ROI/嘴形候选/哈希/风险报告，自动结果只是建议，可 profile 覆盖并重校验；**禁为取索引重编码原始 MJPEG**。固件读 manifest v2 按能力执行，无 manifest 或校验失败走 legacy 六表情，不因素材升级导致无表情或阻塞启动。
- **存活门优先（v1.0 硅片现实）**：每个视觉特性挂 R0 flag；每一级升阶判据＝ per-marker 平均寿命不回退，视觉指标次要。阶段落地（M0 静态优先 / M1 仅 speak 嘴部 / M2 五档预算 / M3 manifest v2）见 `p4-current-state.mdc`。

## 证据驱动诊断 / 变更批次 / 排查顺序

- 修改前写清：可证伪假设 `H` → 必现 `M+` / 应缺 `M-` → 本次唯一变量 → 失败如何回滚。证据须可重复+可定位+可对照+单变量。见 `p4-strategy-philosophy.mdc` 实验循环。
- 四问 / 批次准入 / 回滚三层 / 弹药预算：见作战卡 `p4-session-preflight.mdc` 与 core 原则 G。**贵的是验证不是编码**，纪律约束不可回滚性与不可分辨性，不是改动数量。
- 崩溃/WDT/panic **统一归因顺序**（-1 硅片 → 0 PANIC → 1 panic 路径 → 2 CACHE → 3 `mtval≈sp` → 4 HEAP → 5 addr2line → …）：见 `face-emotion-sd.mdc`，不可跳步。
- 「没表情」先查 `SD_MOUNT_OK`+`seed_stills`+能力，再改 ROI；「云端不听/无 stt」先查 `Session ID`+wake opus+listening 上行 Opus，再看 `type=stt`，不先改 MQTT JSON/表情。

## 沟通与工作方式

- 简体中文回复；代码/符号/日志标记英文。
- 先读相关代码/文档/日志再下结论。诊断请求只定位解释；用户明确要求才改代码。
- 不主动 git commit/push/建 PR，除非明确要求。保留用户已有改动。
- 优先可回滚可验证的改动；一次烧录可含多项正交改动，但一个因果结论不得由多个变量认领。

## 完成标准

改动完成至少：相关目标能构建；新固件确实上板并出现预期 `FW_MARKER`（需真机时）；预定义 `M+`/`M-` 日志闭环；未破坏 SD / UDP Opus / AFE 栈 / 可烧录性 / 会话抢占；说明尚未做的真机步骤与剩余风险。

涉表情与音画调度额外要求：

- 音频侧无 I2S 欠载、无 TTS 断续、上行 Opus 不积压、Listening 期 STT 不退化。
- 显示侧嘴部延迟 <120ms、静音后 ~150ms 闭嘴、不补播过期口型、字幕正常更新、情绪切换无回待机闪帧、无上下条带拼接。
- 稳定性用**每固件标记平均存活时间**对标既有基线判断，不用「最近几次没崩」或「动画看起来能动」代替。
- 覆盖冷启动、多轮长对话、主动提醒、待机唤醒混合场景并持续足够长时间。

无设备/串口或用户未冷启时，可做静态检查与构建，但不得声称真机问题已验证解决。

## 规则维护

- 本文件只留长期契约，不记「当前主矛盾」或易过期的 `s1xx` 阶段状态（写 `p4-current-state.mdc` **§0**）。
- 态势真源：`boot_trace.cc` 的 `FW_MARKER` > current-state §0 > 作战卡摘要。known-good 分 **KG-Life / KG-Function / KG-Archive** 三槽。
- 经重复真机证据确认的新硬约束更新 `p4-hard-constraints.mdc`；单次实验/panic/误判写 `face-emotion-sd.mdc` 战史。
- 可变层保持一页；日流水只增不删。代码味道债见 `docs/code-smell-audit-20260820.md`。
- 旧规则备份见 `.cursor/_rules_backup_*/`（历史依据）。
