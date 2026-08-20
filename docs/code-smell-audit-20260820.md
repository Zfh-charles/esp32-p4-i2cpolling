# 代码味道审计（2026-08-20）

依据《坏代码的味道》/ 好代码坏代码，对当前工程做**审慎审视**。  
本批**不改固件热路径**（`s1fo` 占滚动 V3）；下列为债登记，另批行为守恒抽取。

## 严重度说明

- **P0**：伤可归因/可烧录/音频 → 独占批次  
- **P1**：显著可维护性债，行为守恒可抽 → 不占显示滚动门  
- **P2**：气味明显但非阻塞  

## 热点（按行数）

| 文件 | 约行 | 味道 | 建议 |
|------|------|------|------|
| `eezui_display_adapter.cc` | 4400+ | **上帝类**：Present / Tick / 字幕 / life / idle / 预算消费缠在一起 | 按「呈现仪式 / 预算读 / life 令牌 / 字幕」行为守恒抽 helper；禁顺手改时序 |
| `application.cc` | 2780+ | 上帝类倾向：状态机+会话+网络+调度 | 只抽正交边界；禁大挪 |
| `emotion_video_player.c` | 2670+ | 长过程 + 多职责（seed/decode_at/索引） | 解码 leaf 与策略入口分离（已部分 Presenter 化） |
| `face_mouth_layer.cc` | 830+ | 可接受；仍有加载环可抽 | 已有 LoadMouth/Eye bank；life 加载对称化 |
| `face_route_v2.h` | flag `a..x` | **开关命名空间膨胀** | 保留 R0 价值；另批做分组文档，**禁批量改名**（回退风险） |
| `audio_service.cc` | 1100+ | 与脸耦合点（mouth_level）须保持单向 | 继续禁止音频任务碰 LVGL |
| `dialogue_emotion_builder.py` | 35KB+ | 上帝脚本：扫描+生成+校验+life | PC 侧按 scan/generate/validate 拆模块 |

## 重复 / 死代码

| 气味 | 现状 | 收敛 |
|------|------|------|
| 串口读脚本三份 | `read-com` / `read_serial` / `serial_boot_capture` | 保留 boot_capture + `tools/stability/serial_monitor.py` |
| ROI 旧工具 | 工作区偶发只剩 `__pycache__` | 以 `emotion_tool_dev` / `tools/emotion_tool_dev` 为准 |
| known-good 叙事混用 | Life/Function/Archive 曾混写 | 已用规则 §0 三槽拆开 |

## 好味道（保留）

- route-v2 **R0 正交开关** + 独立日志标记  
- 音频 → `mouth_level` 原子 / Face Worker 唯一消费者  
- VisualBudget **先影子后消费**（M2a→b）  
- idle **令牌化**而非竞抢重试（s1ez/s1fa/s1fb→s1fo）  
- PC **不重编码**原始 MJPEG；contract / 语义批准门  

## 本归档明确不做

- 不拆 `eezui_display_adapter` 热路径（占滚动门风险）  
- 不改 flag 字母命名  
- 不整包合入远端混合 reminder 分支  

下一批可做（不占显示滚动门）：adapter 呈现仪式抽取（对标曾做过的 `ShowFaceCanvasLayers`）、builder 模块拆分、串口脚本收敛。
