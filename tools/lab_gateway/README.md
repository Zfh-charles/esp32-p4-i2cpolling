# P4 云端构建 / Surface 实验台 v1

## 推荐入口：无需常驻服务器（GitHub Actions）

2026-09-11 按用户选择，默认采用本节。后文relay模式保留为可选方案，不需要部署。
Codex云端负责改代码和离线测试；GitHub Actions是独立的云端编译执行器，两者不是同一台机器。
Actions固件和ELF作为限期产物保存，不进入Git源码；Surface仅下载固件包。

新增`.github/workflows/p4-cloud-build.yml`，只支持手动workflow_dispatch：
审核分支 → 提供私有输入commit → 主机测试 → IDF 5.4.1同图构建 → P4镜像门 → 两份独立artifact。
保留7天。artifact访问权限跟随仓库权限，公开仓库的产物不等于私密存储；
当前配置包含派生凭据盐，因此本workflow强制只在私有仓库执行。
公开仓库的新分支仅保存工具模板，不生成公开BIN/ELF；私有构建执行端尚待接通。

上线前需配置GitHub environment `p4-cloud-build`：

- variable `P4_INPUTS_REPOSITORY`：已有的私有构建输入仓库owner/name。
- secret `P4_INPUTS_READ_TOKEN`：仅能读取该输入仓库的凭据。
- 输入仓库包含sdkconfig、dependencies.lock、需要的本地components，以及完整哈希清单
  build-inputs.sha256.json。prepare_inputs.py逐个验证后复制，拒绝覆盖不同的源码。
- environment可设置审批；只有审核过的分支使用输入凭据。当前未配置远端environment。
- workflow文件需经审核上库；本地项目当前无.git，不能直接把整个C盘工程推上去。

在Surface先由用户完成GitHub CLI登录（`gh auth login`），下载权限只需Actions read。
构建完成后，指定准确run ID与源码commit运行：

```powershell
& C:/Users/0000/.espressif/python_env/idf5.4_py3.14_env/Scripts/python.exe tools/lab_gateway/github_stage.py --run RUN_ID --commit FULL_COMMIT_SHA --state C:/bake/xiaozhi-p4-epdainaozhong0109/lab-node-state
```

该命令校验运行成功、workflow路径、仓库、源码commit、run attempt、GitHub压缩包digest、
内部固件SHA256、marker和P4布局；只接受两个指定文件，不解压任意路径。
最终生成`run-ID-ATTEMPT/firmware.bin`、job.json与stage-result.json。
没有自动选择latest，也没有触碰COM口。stage-result保存在本地，当前不自动上传GitHub。
下载超时可重跑，任务目录内容不同时拒绝覆盖；大ELF继续留在云端。

本地验证：`python -m unittest -v test_gateway test_github_delivery`。
2026-09-11：22项测试PASS（10.542秒），Python/YAML语法及产物分离配置检查PASS。
当前仍需首次真实Linux构建和下载验收；自动烧录/日志回传属于后续接线，不算已完成。
GitHub官方接口依据：https://docs.github.com/en/rest/actions/artifacts 。

本目录是可运行的首版交付通道：云端同图构建 → HTTPS 中转 → Surface
下载和校验 → 持久结果回传。标准库 Python 3.10+，默认仅 STAGE。
不依赖托管 Codex 支持自定义 MCP；以后 MCP 可包装同一执行端。

## 当前边界

- 实现并测试：双角色鉴权、固件哈希、P4 padding 门、marker 检查、5MiB应用上限、
  幂等提交、节点单实例锁、结果离线重传、执行中断后禁止自动重做。
- 本地固定烧录 adapter 是扩展接口，**当前未接线**；不会触碰 COM7。
  节点锁排斥本工作流；flash前还会获取当前monitor共享的COM7字节锁，
  monitor活跃则报告BLOCKED，不杀进程。其他不遵守此锁的串口程序仍需适配器处理。
- 未部署云端服务、未完成 Linux 固件构建、未烧录、未证明任何硬件行为。
- v1 每个 relay 服务一个 Surface/一块板。多节点必须使用独立 relay、token、存储。
- 下载失败从头重试，成功后按哈希复用缓存；v1 没有 HTTP Range 断点续传。
- relay 是内网/loopback 服务，公网需要现有 HTTPS 反向代理、请求限流和磁盘配额。
  两个 token 使用独立随机值，至少32字符，由环境变量注入；不提交到 Git。
  上传者可信：哈希保证传输一致性，不是发布者签名或编译证明。

## 本轮验收意图

H：任务与结果持久化能把不可靠网络和不可重复烧录隔离。
M+：完整下载后才交给adapter；丢失回执后只回传旧结果；重启遇到未完成执行记录时报告未知。
M-：哈希错误仍执行、重复烧录、远端注入命令/地址/COM、伪报硬件验证通过。
回滚：停止节点和relay，恢复现有本地开发流程；不修改固件、分区或SD。

## 1. 本地测试

从本目录运行：

```powershell
& C:/Users/0000/.espressif/python_env/idf5.4_py3.14_env/Scripts/python.exe -m unittest -v test_gateway
```

测试启动临时 loopback HTTP 服务并自动退出，只使用合成镜像，不读写串口。

2026-09-11 本地验收：Windows/Python 3.14，15项测试通过（10.204秒）；
6个Python文件语法检查通过，两个Bash入口分别通过bash -n。
覆盖真实HTTP交付、缓存、完整性/身份拒绝、鉴权角色、节点/monitor锁、
重复提交、执行中断、结果上传中断和本地烧录策略；不包含Linux交叉构建与真机验收。

## 2. Ubuntu 中转服务

将本目录代码部署到受控服务器。通过服务器的秘密管理方式设置
`LAB_PRODUCER_TOKEN` 和 `LAB_NODE_TOKEN`，然后运行：

```sh
python3 relay.py --state /var/lib/p4-lab --port 8768
```

运行用户需拥有该状态目录。服务绑定127.0.0.1，反向代理将专属HTTPS域名转发到该端口。
代理需要允许8MiB以内请求，转发Authorization，禁止缓存 /next 和 /results。
用服务管理器保持进程运行；SQLite与固件目录必须持久化。
不要将标准库HTTP服务直接绑定公网。生产应增加磁盘水位/容量配额、任务有效期和审计保留策略。

## 3. Surface 节点

把 `surface.example.json` 复制成 `surface.local.json`，设置relay为真实HTTPS域名。
通过本地秘密存储注入 `LAB_NODE_TOKEN` 后运行：

```powershell
& C:/Users/0000/.espressif/python_env/idf5.4_py3.14_env/Scripts/python.exe surface.py --config surface.local.json
```

`--once` 可处理一个任务然后退出。空闲每5秒查询，故障退避到120秒。
节点状态目录不可在任务中途删除或多进程使用不同副本；删除运行账本会失去去重证据。
默认不自动启动、不变更休眠设置；安排长泡时需保证Surface保持运行。

## 4. Codex 云端构建

官方云端安装脚本入口可运行 `bash tools/lab_gateway/setup-cloud.sh`。
在环境设置同时配置 `LAB_IDF_PATH`；安装阶段的export不自动传入任务阶段。
系统需已有git、Python、CMake/Ninja及ESP-IDF Linux前置依赖。
固定ESP-IDF v5.4.1，任务阶段每次通过build-cloud.sh加载export环境。

必须先在私有环境准备 **经过审核的完整构建输入**：

- `sdkconfig` 与 `dependencies.lock`；当前公开仓库忽略了它们。
- 修改过的本地组件，例如本地化的LVGL port；核对实际生产源码后封存，不能下载原版替代。
- `build-inputs.sha256.json`：私有输入每个文件的项目相对路径到SHA256映射，须包含上述配置、
  本地组件、生成前必需的资源和其他Git未追踪输入。它本身也应在本地/私有harness管理，
  并列入该云端checkout的.git/info/exclude，避免公开提交和未追踪文件阻断发布。
- 一份只包含必要工程约束的私有云端harness，不向公开main提交历史rules。

构建入口要求源码Git根就是ESP-IDF项目根，工作树干净；输出路径使用仓库外目录。
先检查计划，再明确执行：

```sh
bash tools/lab_gateway/build-cloud.sh --project "$PWD" --output /tmp/p4-release --id trial-001
bash tools/lab_gateway/build-cloud.sh --project "$PWD" --output /tmp/p4-release --id trial-001 --execute
```

这可能触发首次完整构建。构建日志位于输出目录的trial-001.build.log；任务期间可另开终端
读取尾部确认进度。产物通过P4镜像门后才生成trial-001目录，包含firmware.bin、firmware.elf、
job.json；ELF保存在云端产物存储，Surface仅接收BIN。
本入口尚未通过真实Linux构建验收。私有输入清单是受控输入契约，需要部署前审核完整性。

## 5. 提交及回传

设置云端发布端 `LAB_PRODUCER_TOKEN` 后：

```sh
python tools/lab_gateway/submit.py --relay https://LAB_DOMAIN --bundle /tmp/p4-release/trial-001
python tools/lab_gateway/submit.py --relay https://LAB_DOMAIN --result trial-001
```

托管Codex任务阶段凭据供应需单独验证：官方环境secrets只提供给setup阶段，不能假定
任务中自动有发布token。可让可信CI发布产物/任务，或接入经过验证的短期授权服务；
不要把长效token硬写进源码或输出日志。先用本地提交端也可完成第一条真实链路。

`STAGED`仅表示下载、哈希、布局和内嵌marker通过；不表示已上板。
待处理的坏任务会阻塞本单板FIFO；v1应由维护者审核处理，不静默丢掉。
服务不会删除历史产物；上线前配置存储容量与保留策略。

## 6. 接入真实烧录的剩余工作

节点只调用本地配置中的固定参数数组 `flash_adapter`，shell=False，stdin传入JSON：
job、已验证firmware_path、由本地配置指定的port。远端不能指定命令、烧录偏移或COM。
必须同时开启allow_flash并在approved_flash_sha256中明确列出允许的候选hash。

接线前，adapter需要独立通过以下真机验收：

1. 确认设备身份、当前槽和known-good归档；只写授权候选槽，保护回退槽、bootloader和assets。
2. 和现有唯一monitor正确交接、持有物理串口排他权；异常退出仍能恢复采集。
3. 单次esptool失败立即停，不自动抢窗或重枚举。
4. 保存完整本地启动证据，exact marker/运行槽命中后才记录上板；冷启和会话另验。
5. 自动回退只在确认可安全通信时执行；写超时/设备消失要转人工断电，不能连续重烧。

flash_adapter运行期间，节点持有monitor_lock；adapter不要再次获取同一锁，也不要
在锁释放前启动monitor。当前v1会在monitor占用时拒绝任务，尚未实现自动交接。

v1的adapter成功结果固定为ADAPTER_COMPLETED_DEVICE_VALIDATION_PENDING；
超时/异常为UNKNOWN，均不会谎报hardware_verified=true。
适配器需自行管理子进程树，避免超时留下烧录子进程；当前禁止未经验收开启。

后续需要真实HTTPS地址、云端仓库访问与私有输入供应、Surface monitor交接接口，
才能完成端到端部署。当前代码可以先验证交付通道，不占用固件稳定性试验窗口。
