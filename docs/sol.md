# SOL 日志通道与板卡验收

## 功能与范围

`logctld` 收集本地日志，独立子进程通过 HTTP(S) 主动上传。Device Manager 的 SOL 页提供只读实时查看、按设备/启动/阶段回放、文本筛选和当前视图导出。它不执行远程命令，也不占用硬件 UART，因此不会干扰本地串口终端。

SPL、U-Boot 没有独立网络日志会话：Linux 启动后导入保留内存，联网后补传。ROM、TIFS、ATF 和未接入的交互 shell 输出不在采集范围内。启动到 Linux 之前失败、断电丢失 RAM 的日志不能保证回收。

## 凭据与配置

在主机执行一次，生成独立的设备上传凭据和只读查看凭据：

```sh
output/am625/host/bin/device-server sol-enroll \
  --root output/ota-validation/feed --device 1c-63-49-22-c4-f7
```

两种凭据分别存放于 feed 的 `sol-keys/<device-id>` 和 `sol-keys/viewer`，权限 0600。已存在的凭据不会被覆盖。设备只能提交自己的日志；查看凭据能读取所有已登记设备的日志，不能上传。

设备凭据应通过设备本地控制台或受信任的离线方式写到 `/data/device-manager/sol.key`，文件包含一行 64 位十六进制值，权限 0600、所有者 root。**不把凭据提交 Git 或放进可下载的固件包。** 当前测试设备的 `/etc/device-manager/sol.conf` 已随固件提供：

```ini
server=http://192.168.10.246:8090
device=1c-63-49-22-c4-f7
key-file=/data/device-manager/sol.key
interface=eth0
```

在确认 `/data` 已挂载到持久化介质后，可在设备串口准备目录和凭据：

```sh
mountpoint -q /data || exit 1
mkdir -p /data/device-manager
umask 077
cat > /data/device-manager/sol.key
# 粘贴主机 sol-keys/<device-id> 中的一行凭据，然后按 Ctrl-D。
sync
```

不要粘贴 viewer 凭据作为设备上传凭据。后台每 5 秒重试，首次凭据就绪后无需重启。

通用设备可省略 device 值，程序从指定接口 MAC 生成 ID。配置使用 `key=value`，没有 shell 展开。固件未提供 sol.conf 时保持本地采集；配置存在但凭据未就绪时后台等待，不阻塞 rcS 或固件健康检查。凭据就绪后自动重试。修改已生效的配置后重新启动 S01logctld。

临时功能验证可给服务端添加 `--sol-open-view`，Web 自动免凭据连接；设备上传仍必须提供自己的凭据。去掉此参数并重启服务即可恢复查看验证。当前实验服务器按用户要求已启用该选项。

正常验证模式下，打开 `http://192.168.10.246:8090/#sol`，在查看凭据栏填入服务端 `sol-keys/viewer` 内容。凭据只保留在当前页面内存；刷新后重新输入，标签页位置保留。设备停止上报超过 60 秒会提示暂未上报。

当前实验环境使用 HTTP，凭据和日志不加密。跨不可信网络时使用 HTTPS 反向代理，设备 server 改用 HTTPS，libcurl 默认验证服务器证书；不要关闭证书验证。协议不会跟随重定向传递凭据。

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
