# s1ft verified archive bundle

This file identifies the PC-side tooling and SD asset pack paired with the
validated `boot_trace_v10_s1ft_m2c_visual_budget_life` firmware archive.

## Firmware

- Source commit: `56984e932786f1d70b1972cf22c9213f4a9d2218`
- Binary: `../fw_archive/xiaozhi_s1ft.bin`
- Binary SHA-256: `10f163327fcfc1b36beec857c2591f8627cc81763e97b86671d9d191f62419aa`
- ELF: `../elf_snapshots/xiaozhi_s1ft.elf`
- ELF SHA-256: `256ec10db29628057e0a9a7572e5b1f1bbd8ee90ae9e396bf9918b395d33157d`

## PC automation tool

- Repository path: `emotion_tool_dev/`
- Workspace source: `C:\bake\xiaozhi-p4-epdainaozhong0109\emotion_tool_dev`
- Files: `9` (excluding `__pycache__` and `*.pyc`)
- Bytes: `96683`
- Tree SHA-256: `6455bbaf779aeacbbc401520086ada53f8cb8a0941662142a82503f3b5c7a10f`

## SD dialogue pack

- Repository path: `mjpeg_ai_dialogue_v5p3_mouth_focus/`
- Workspace source: `C:\bake\xiaozhi-p4-epdainaozhong0109\mjpeg_ai_dialogue_v5p3_mouth_focus`
- SD destination: `/sdcard/dialogue_v2/`
- Files: `424`
- Bytes: `45726024`
- Tree SHA-256: `83edc1b001bd1ce7c71b002089433be6aa29d957b12d03ef2500aad5f3241422`

The tree hash is SHA-256 over UTF-8 lines sorted by relative path, formatted
as `<lowercase file sha256><two spaces><forward-slash relative path>\n`.
The repository copies were compared file-by-file against the workspace sources:
missing `0`, extra `0`, content mismatch `0`.

