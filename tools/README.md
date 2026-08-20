# tools/ 布局（存档约定）

```
tools/
  emotion_tool_dev/                       # PC 自动化工具源码（权威）
    dialogue_emotion_builder.py
    README.md
  emotion_builder/                        # 兼容旧路径（与 emotion_tool_dev 同步）
  mjpeg_ai_dialogue_v4_temporal_feather/  # 曾推荐 v4 包（归档）
  emotion_assets/
    dialogue_v2_canonical/                # 无 life 基线包
  experimental/
    dialogue_v2_posebank/                 # 实验，非发布
  stability/                              # reboot_audit / soak / serial
```

板上当前常用 SD 包可能是工作区 `mjpeg_ai_dialogue_v5p3_mouth_focus`（以 current-state §0 为准）；部署路径仍是 `/sdcard/dialogue_v2/`。

代码味道审计：仓库根 `docs/code-smell-audit-20260820.md`。

## 明确不入库

- `serial_monitor.log`、`build_*.log`、`flash_*.log`
- `build/`、`managed_components/`、`sdkconfig`、`elf_snapshots/`、`fw_archive/`
- `__pycache__`、`.pyc`
