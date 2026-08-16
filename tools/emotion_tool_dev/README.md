# emotion_builder（PC 表情包生成）

面向固件 **M1 layered**：静态情绪底图 + 独立嘴/眼小图 + 可选 life 轨。  
**不重编码**输入 MJPEG；只建索引、分段、ROI、RGB565、校验。

源码目录：`emotion_tool_dev/`。GitHub 镜像：`tools/emotion_builder/`。

## 推荐发布包

| 包 | 本地路径 | 角色 |
|----|----------|------|
| **v4 temporal feather** | `mjpeg_ai_dialogue_v4_temporal_feather` | 当前推荐：canonical 预合成 + A8 羽化 + 双 48 行 life 轨 |
| v2 canonical | `mjpeg_ai_dialogue_v2_canonical` | 无 life 的基线 layered 包 |
| posebank | `mjpeg_ai_dialogue_v2_posebank` | **实验**，眨眼已回退，勿当正式包 |
| 旧 v2 / v3 | `mjpeg_ai_dialogue_v2` / `_v3_life_v1` | 归档参考，勿再发布 |

设备部署路径必须是 `/sdcard/dialogue_v2/`（整包拷贝）。

## 用法

```bat
run_dialogue_v2.bat
```

```powershell
pip install -r requirements.txt
python dialogue_emotion_builder.py --input "C:\bake\mjpeg_ai" --output "C:\bake\mjpeg_ai_dialogue_v4_temporal_feather"
```

全画幅 AI 素材常被判 `layered_safe=false`；强制 layered 须肉眼查 `mouth_roi_preview.png`：

```powershell
python dialogue_emotion_builder.py --input "C:\bake\mjpeg_ai" --output "C:\bake\out" --force-layered
```

`--profile` 可覆盖自动 ROI/分段；改完必须重校验。

## 输出要点

- `pack_manifest.json`：六表情入口与 render 能力
- 每情绪：`frames.mjpeg`（原样拷贝）+ `manifest.json` + `mouth/` + `eye/` RGB565
- v4：`life/track_0`、`life/track_1`（各 ≤48 行，预合成到 canonical hold base）
- `analysis_report.csv` / `validation_report.json`

固件契约：每 tick **最多一条** life 或嘴带；latest/drop-old；**禁止**再解 480×480 抠嘴；life 直接覆盖 ROI，**禁止**与上一帧累计 alpha。

冷启关键字：`!!FACE_S1CR h=1 pack=1` → 说话期 `mouth_arm` / `mouth_blit`。

## 与旧工具的关系

`emotion_roi_builder.py` 是更早的「自动找统一 ROI」脚本，**不能**替代本 builder。  
工作区 `tools/emotion_roi_builder/` 若只剩 `__pycache__`，视为死目录，勿再发布。
