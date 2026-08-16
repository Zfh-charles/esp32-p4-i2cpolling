# tools/ 布局（存档约定）

坏味道对策：一种职责一个目录；死脚本不进 GitHub；大日志/构建产物永不入库。

```
tools/
  emotion_tool_dev/                    # PC 自动化工具源码（= 工程根 emotion_tool_dev）
    dialogue_emotion_builder.py        # 主生成脚本
    README.md                          # 使用说明
  mjpeg_ai_dialogue_v4_temporal_feather/  # 当前推荐表情资源包
  emotion_builder/                     # 同上工具的镜像名（兼容旧路径）
  emotion_assets/
    dialogue_v2_canonical/             # 无 life 的基线 layered 包
  experimental/
    dialogue_v2_posebank/              # 实验，非发布
  stability/                           # reboot_audit / soak / serial 监控
```

设备部署：把 `mjpeg_ai_dialogue_v4_temporal_feather/` 整包拷到 SD `/sdcard/dialogue_v2/`。

## 明确不入库

- `serial_monitor.log` 及根目录 `build_*.log` / `flash_*.log`
- `build/`、`managed_components/`、`sdkconfig`（用 `sdkconfig.defaults*`）
- `__pycache__`、`.pyc`、固件 `.bin` 快照、`elf_snapshots/`
