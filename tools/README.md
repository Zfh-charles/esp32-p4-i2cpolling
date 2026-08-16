# tools/ 布局（存档约定）

坏味道对策：一种职责一个目录；死脚本不进 GitHub；大日志/构建产物永不入库。

```
tools/
  emotion_builder/          # PC 生成器（= emotion_tool_dev）
  emotion_assets/
    dialogue_v2_canonical/  # 基线 layered 包
    dialogue_v4_temporal_feather/  # 当前推荐（life + 羽化）
  experimental/
    dialogue_v2_posebank/   # 实验，非发布
  stability/                # reboot_audit / soak / serial 监控（收敛目标）
  mcp-* / inference-api-demo / 提醒联调脚本  # 原厂 demo，勿与表情生成混放
```

## 明确不入库

- `serial_monitor.log` 及根目录 `build_*.log` / `flash_*.log`
- `build/`、`managed_components/`、`sdkconfig`（用 `sdkconfig.defaults*`）
- `__pycache__`、`.pyc`、固件 `.bin` 快照（gitignore）
- `_ui_backup/`、`backups/`、`fw_archive/` 本地烧录产物

## 已知重复（待收敛，本存档不删运行中脚本）

| 味道 | 现状 | 收敛方向 |
|------|------|----------|
| 重复的串口读 | `read-com.py` / `read_serial.py` / `serial_boot_capture.py` | 保留 `serial_boot_capture.py` + 根目录 `serial_monitor.py` |
| 死目录 | 工作区 `tools/emotion_roi_builder/` 仅 pycache | 以 `emotion_builder/` 为准 |
| 上帝文件 | `dialogue_emotion_builder.py` 扫描+生成+校验 | 下次按模块拆，行为守恒 |
