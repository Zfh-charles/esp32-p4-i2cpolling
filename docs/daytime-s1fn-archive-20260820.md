# 2026-08-20 日间 s1fn 基线存档

## 范围

- 用户实际验证窗口：2026-08-20 08:00–17:00。
- 该窗口板上运行的是 `boot_trace_v10_s1fn_m2c_visual_budget_life`，不是早晨失败的 s1fo M2d canary。
- 本存档保留之后确认属于行为守恒的代码味道整改：LVGL 锁名常量、PC MJPEG I/O 模块拆分、COM7 烧录脚本自杀修正。
- `S1FO_X_IDLE_LIFE_CANARY=0`，不启用未通过日间验证的 M2d idle-life canary。

## 可复现身份

原始日间 s1fn（真机验证过）：

- BIN SHA-256: `3C1410DE9217AC49497CE08EC6D5987AD84EF382383F66CF44E597BF3E3371E8`
- ELF SHA-256: `41E57DFFEB3EC99F9F26269D7A48480B2919794D2641EA7EAE2ED3B527C44AA4`

行为回归并保留 smell-clean 的重建版：

- Marker: `boot_trace_v10_s1fq_daytime_s1fn_smell_clean`
- BIN SHA-256: `A090683B21A2AD13BEF8E6EFE596029B627E26EABB195FAADFA7B56F31EDF335`
- ELF SHA-256: `5256EEF42DF5CC22AF254C4B3819AF461844E7E0556201B70307955FB901BFF7`
- 应用大小：`0x4621e0`，5MB 应用分区余量约 12%。

匹配二进制在本地 `fw_archive/xiaozhi_s1fq.bin`，匹配符号文件在 `elf_snapshots/xiaozhi_s1fq.elf`。二进制不提交 Git；Git 提交保存源码、工具、规则和本哈希清单。

## 验证状态

- V0：完整构建通过。
- 烧录：`otadata + OTA_0 + OTA_1` 单次写入，三段均 `Hash of data verified`。
- V1/V2：待物理断电冷启。RTS 热启后设备持续处于既有 `SW_SYS_RESET` 启动窗口；原始 s1fn 对照也复现，故不能归因于本次源码，但在 exact marker 出现前不得宣称上板通过。

冷启门：exact marker、`SD_MOUNT_OK`、`seed_stills done ok=6/6`、正常一轮对话、退出及二次唤醒；任一失败回烧上列原始 s1fn BIN。
