# SOL 日志通道与板卡验收

## 功能与范围

`logctld` 收集本地日志，独立子进程通过 HTTP(S) 主动上传。Device Manager 的 SOL 页提供只读实时查看、按设备/启动/阶段回放、文本筛选和当前视图导出。它不执行远程命令，也不占用硬件 UART，因此不会干扰本地串口终端。

SPL、U-Boot 没有独立网络日志会话：Linux 启动后导入保留内存，联网后补传。ROM、TIFS、ATF 和未接入的交互 shell 输出不在采集范围内。启动到 Linux 之前失败、断电丢失 RAM 的日志不能保证回收。

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

页面每次读取最多 128 块，每两秒增量刷新，只在 SOL 页面且实时跟随开启时读取；屏幕保留最近 256 KiB。导出的是当前筛选视图，不是无限期全量档案。阶段之间不共享启动时钟，接收顺序不等于跨阶段发生时间。

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

本次 OTA 不升级引导程序。`bootloader.itb` 和原始 tiboot3/tispl/u-boot 产物已独立提供。`fw update tftp <bootloader.itb>` 更新选择的引导槽位；AM625 ROM 固定读取 tiboot3_a，单次更新备用槽位不能保证 ROM 首级已经换新。完整验收建议在备用 SD 卡上烧录本次 `sdcard.img`，保留原卡作为恢复介质。

新引导程序启动后，在 Linux 执行：

```sh
logctl verify-boot
logctl show --stage r5-spl
logctl show --stage a53-spl
logctl show --stage uboot
```

三个阶段都应显示 `handoff=verified-by-kernel`，日志中应能区分 R5 SPL、A53 SPL、U-Boot。Linux 驱动检查记录头、长度、阶段和 CRC32；仅仅存在 sysfs 目录不能判定通过。至少再冷启动、正常重启各一次，核对没有混入前次启动记录，并执行容量溢出测试。未连接串口、未安装新引导程序时，这项必须保留为待实机验证。
