# SOL 日志通道与板卡验收

## 功能与范围

`logctld` 收集本地日志，独立子进程通过 HTTP(S) 主动上传。Device Manager 的 SOL 页提供只读实时查看、按设备/启动/阶段回放、文本筛选和当前视图导出。它不执行远程命令，也不占用硬件 UART，因此不会干扰本地串口终端。

SPL、U-Boot 没有独立网络日志会话：Linux 启动后导入保留内存，联网后补传。ROM、TIFS、ATF 和未接入的交互 shell 输出不在采集范围内。启动到 Linux 之前失败、断电丢失 RAM 的日志不能保证回收。

## 串口格式

默认视图与文本导出只显示日志正文：保留缩进、空行和程序自己的 `[fwctl] [INFO]` 等前缀；不显示采集阶段、序号、接收时间或 `source=...` 标签。内核 `/dev/kmsg` 元数据转换为 `[    秒数.微秒] 消息`。长行使用横向滚动，不强制折行；回车和退格按纯文本终端方式显示。

新采集记录使用明确的字节长度，网络分段和半行输出不会额外补换行。完整元数据仍保留在本地归档和服务端 SQLite 中。`events` 收集器内部诊断不混入默认视图，可通过阶段筛选单独查看。没有接入采集的启动阶段不会填充虚假日志。

旧归档仍可去除元数据前缀，但旧采集器已补入的换行无法可靠撤销。initramfs 旧式缓存及启动早期服务缓存只保存整块导入时间，没有逐条发生时间；跨来源的排列为尽力还原，不代表与串口逐字节完全一致。此页面不是完整 ANSI 终端模拟器，不提供交互 shell。

## 当前调试配置：直接连接

当前 AM625 调试环境同时关闭网页查看与设备上传的凭据验证。打开 `http://192.168.10.246:8090/#sol` 即自动连接；设备联网后自动上传，不需要注册、密钥文件或手工写入凭据。首页和 SOL 页面不显示凭据输入框。

服务端启动参数：

```sh
output/am625/host/bin/device-server --root output/ota-validation/feed \
  --bind 0.0.0.0 --port 8090 --sol-debug-no-auth
```

板级 overlay 已提供 `/etc/device-manager/sol.conf`：

```ini
server=http://192.168.10.246:8090
auth=none
device=
interface=eth0
```

空 device 值表示按接口 MAC 自动生成设备 ID，也可以指定已有 ID。`auth=none` 不读取密钥文件，也不发送 Authorization。该配置只影响日志连接，不改变 OTA 完整性验证、A/B 升级和健康检查。

页面保留设备、启动记录、阶段和日志筛选；设备首次上线会自动加入列表。60 秒未上报时显示暂未上报。没有可用持久化存储时，仍可使用 RAM 日志并进行网络转发。

## 可选：以后恢复凭据验证

无需删除已有凭据；调试模式不使用它们。以后去掉服务端 `--sol-debug-no-auth`，在设备配置中改成 `auth=key` 并指定 `key-file=/data/device-manager/sol.key` 即可恢复验证。`--sol-open-view` 只放开网页查看，不放开设备上传。

需要凭据时，主机执行 `device-server sol-enroll --root DIR --device DEVICE_ID` 生成 `DIR/sol-keys/DEVICE_ID` 和 `DIR/sol-keys/viewer`。设备密钥单独配置，不能放进固件包或 Git；网页使用 viewer 凭据。缺少必要凭据时后台等待，不阻塞本地采集或 rcS。配置采用 key=value，无 shell 展开；修改已生效配置后重启 S01logctld。

## 续传与容量

服务端使用独立 SQLite `sol.db`，批次和续传位置在同一 FULL synchronous 事务内提交。设备以启动 ID、阶段、日志文件首条序号和字节偏移定位上传。重试、客户端重启和服务端重启会先读取服务器确认位置，不重复插入已确认批次。

每批最多 4096 字节；服务器保留每设备最近 4 次启动、每阶段最多 4 个文件代际、最多 2048 个数据块，约 8 MiB 原始日志。SQLite 额外索引/元数据占用不包含在 8 MiB 中。空间会复用；超过保留范围的记录会删除。客户端每轮每文件最多上传 128 KiB，每轮间隔约 5 秒。掉线期间仍受本地轮转限制，不能保证无限期离线无损。

页面每次读取最多 128 块，每两秒增量刷新，只在 SOL 页面且实时跟随开启时读取；屏幕保留最近 2 MiB 日志正文。导出的是当前筛选视图，不是无限期全量档案。SPL/U-Boot 按启动阶段排列；Linux 日志按内核原生时间和用户态采集时间合并，不能保证还原物理 UART 上逐字节交错的顺序。

## 持久化验证

新工厂 `sdcard.img` 在固定 data 起点 320 MiB 放置 256 MiB 空 ext4，已有 A/B 偏移不变。只对重新烧录的卡初始化；OTA 只更新 kernel/rootfs，不写 data，也不会格式化已有数据。

在设备串口执行：

```sh
logctl status
logctl verify-storage
logctl boots
```

`storage=persistent` 仅说明当前介质非 RAM。`verify-storage` 写入并同步当前启动标记；重启后再次执行，出现 `cross-boot=verified` 表示能读到前一次启动的标记。这不能替代断电可靠性测试。SOL 元数据也会报告存储类型、各启动的 durable-marker 和已捕获的引导阶段。

若是旧卡且 data 未格式化，先备份并确认 `/dev/mmcblk1p15` 数据用途。工具不会自动创建文件系统；可以在主机上重新烧录**新卡**的完整工厂镜像进行验证，避免覆盖当前卡的数据。运行中写整个 sdcard.img 会破坏系统，禁止这样操作。

## SPL/U-Boot 验证

仅更新 kernel/rootfs 的 OTA 不会升级引导程序。`bootloader.itb` 和原始 tiboot3/tispl/u-boot 产物已独立提供。AM625 ROM 固定读取 tiboot3_a；更新备用槽位不能证明首级 SPL 已更新。刷写前应备份当前启动分区，并核对镜像与写入后分区的 SHA256。

新引导程序启动后，在 Linux 执行：

```sh
logctl verify-boot
logctl show --stage r5-spl
logctl show --stage a53-spl
logctl show --stage uboot
```

在本次缓存尚未清理时，三个阶段应显示 `handoff=verified-by-kernel`；v2 缓存归档清理后改为明确显示已 acknowledged。日志中应能区分 R5 SPL、A53 SPL、U-Boot。Linux 驱动检查记录头、长度、阶段和 CRC32；仅仅存在 sysfs 目录不能判定通过。

### AM625 早期 v1 实机结果（2026-11-03）

将串口接入主机 `/dev/ttyACM2` 后，发现板上仍运行旧 SPL/U-Boot，三个阶段均为 `unavailable`。U-Boot TFTP 因 PHY 自协商超时未能下载，未写入启动分区。随后在 Linux 侧下载并验证 `bootloader.itb`，通过 `fwctl update bootloader` 只更新启动链；写入后逐一读回校验 `tiboot3_a`、`tispl_a` 和 `uboot_a`。deployment 0 启动、健康检查通过，已标记 `confirmed`。

连续两次正常重启均验证 R5 SPL、A53 SPL 和 U-Boot 接力：`logctl verify-boot` 的三个结果全部为 `verified-by-kernel`；内核报告三段各 `0 dropped`。第二次启动的服务器 SOL 归档与物理串口捕获逐字节一致，分别为 324、227、2907 字节。Device Manager 页面能从归档中去掉采集元数据前缀，按阶段显示原始正文。两次启动有独立启动 ID，没有混入上次启动记录。

本次验证覆盖正常重启；断电冷启动和容量溢出仍需单独测试。设备的 data 文件系统尚未配置，`logctl` 本地存储状态为 `volatile`；服务器 SQLite 已收到并保存这些日志，但不能据此认定设备本地具备断电持久性。ROM、TIFS 和 ATF 输出仍不属于当前接力范围。

### 双缓冲 v2 更新

当前 AM625 已采用两组扩容的 SPL/U-Boot/kernel 启动缓存，正常健康确认及服务器持久归档后清理，下次从第一组开始。失败启动与恢复后的正常启动分别保存，不冒充同一次 boot ID；data 已完成持久存储验证。v1 段落保留为历史记录，不能用于描述当前双缓冲布局。配置、清单和限制见 [双缓冲启动日志](startup-buffers.md)，#43 实机结果见 workspace 的 `docs/am62x/bootlog-double-buffer-validation.md`。

## 删除启动记录

SOL 的启动记录下拉列表每行右侧有“×”。确认后删除该条服务器 SOL 记录与正文；正在采集的本次启动、仍未完成上传的快照禁止删除。删除其他行保留当前选择，删除正在查看的历史记录则切换到剩余记录。

服务端 `DELETE /api/sol/records/<device>/<boot>` 要求 `X-OTA-Request: 1`，校验同源请求；普通模式需要 viewer 凭据，`--sol-open-view` 仅允许读取，调试免凭据模式可直接操作。删除在 SQLite 事务中完成，保存删除时间、来源 IP 和原字节数；删除标记与有限续传游标防止设备重试、进程重启或 ACK 丢失后重新生成记录。

设备本地文件不删除，独立保存的 OTA 审计和事故附件不受影响。SQLite 删除页可供后续日志复用，不在每次操作时执行阻塞的 VACUUM。本功能只修改管理端，不要求更新设备固件。测试使用 `python3 ota/tests/test-sol-delete.py`，不删除生产设备历史。

## OTA 异常附件与 SOL 跳转

OTA 异常详情现在分别列出“前次异常启动”和“回退后恢复启动”，提供查看日志、下载附件及“在 SOL 中打开”。下载文件包含 OTA 事务目标版本、短固件 ID、归档接收时间与短 capture ID；恢复启动实际使用的镜像身份以恢复日志为准。原始数据、完整事务标识和完整恢复启动 ID 保留在数据库中。

服务端以设备、OTA publication/source boot、完整恢复启动 ID，以及两组缓存的相邻 generation/不同 slot 建立关系，不按八位固件哈希猜测。关联前重新核对四阶段长度与 CRC；缺少缓存对、身份关系不明确或归档字节损坏时不自动关联。同一恢复上下文被多个异常事务占用时，附件关系隐藏等待核实。capture ID 不等于 Linux boot ID，也不能仅凭未确认标记认定发生 panic。

已确认关系的归档复制到独立事故表中，每个事务最多保存一对有限快照；早期快照按各阶段既有容量限制，独立于原有实时日志附件限额。SOL 列表删除、实时窗口轮转、进程重启均不删除这些副本。归档和异常事件不论先后到达都可关联；服务启动时回填已有记录，并在处理 SOL 删除前完成可确认的历史关联。

`GET /api/sol/incidents/<device>/<publication>/<source>` 增加 `startup_archives`：capture、failed-startup/recovery-startup、恢复上下文、接收时间、字节数及四阶段 CRC/drop 状态。原有 `boots` 实时片段仍保留。附件通过原有 `/api/sol/incidents/.../<capture>/<cursor>` 读取，有限阶段返回 `raw=true`；完整性状态只描述已接入的阶段，不声称覆盖 ROM/ATF 或全部物理 UART 输出。

SOL 链接使用 `/?sol_device=...&sol_boot=...&sol_publication=...&sol_source=...#sol`，打开后选中指定设备和记录，刷新保留。原始记录已经删除时，从独立事故附件回退读取并明确标注；事故副本的 × 禁用。指定记录不可用时显示缺失，不自动改看另一场启动。没有完整恢复关系的旧事故仍可保留原因及已有实时片段。

测试：`python3 ota/tests/test-startup-incident.py` 覆盖关联、回填、删除与重启、错误/歧义上下文、截断状态及归档 CRC 失败。本功能只更新 host 管理端，不需要升级设备固件。
