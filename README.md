# Device Manager

统一的嵌入式 Linux 设备管理组件。主机端提供管理页面与 OTA 发布，设备端负责固件事务、升级代理和本地日志。EDK 独立负责外设管理，两者通过明确的服务接口协作。

| 组件 | 运行位置 | 职责 |
| --- | --- | --- |
| `device-server` | 开发主机 | Device Manager Web、OTA 发布、设备升级记录与审计 |
| `ota-cli` | 开发主机 | 命令行上传、发布和查询 |
| `fwctl` | 设备 / initramfs | 固件验证、分区读写、A/B 切换、恢复与健康确认 |
| `fwctld` | 设备 | OTA 检查、下载、调用 fwctl、上报结果 |
| `logctl` | 设备 | 查询、实时查看及导出本地日志 |
| `logctld` | 设备 | 内核与服务日志采集、启动日志归档、轮转 |

## 目录

- `ota/`：服务端、CLI、升级代理、共享协议及 Web 页面。
- `firmware/`：独立固件操作引擎，不链接 EDK 库。
- `fwctl/`：命令行工具及外设监督服务的健康检查协议适配。
- `recovery/`：编入 `fwctl` 的 initramfs PID 1 恢复逻辑；`/init` 是调用 `fwctl init` 的脚本。
- `logging/`：本地日志组件及集成测试。
- `tests/`：固件事务、恢复和版本显示测试。

## 构建

在配套 workspace 中，`bsp-base-external/package/device-manager` 通过同一份包定义分别构建 host 和 target 产物。`BR2_PACKAGE_HOST_DEVICE_MANAGER_*` 控制主机工具，`BR2_PACKAGE_DEVICE_MANAGER_*` 控制设备工具：

```sh
make -C buildroot O="$PWD/output/am625" host-device-manager
make -C buildroot O="$PWD/output/am625" device-manager
```

主机程序安装到 `output/am625/host/bin/`；设备程序安装到 target 的 `/usr/sbin/`。独立开发可使用 `make host`、`make device` 和 `make test`，通过 `CC`、`CPPFLAGS`、`LDFLAGS` 指定工具链与依赖。host 需要 zlib、SQLite；fwctl 需要 libcurl、OpenSSL、libfdt 和 pthread；日志工具需要 libcurl。OTA 工具运行时需要 curl、sha256sum、tar 及对应 FIT 工具。`make -C ota test` 的日志格式回归使用 Node.js 18 或更新版本。

```sh
device-server --root /path/to/feed --bind 0.0.0.0 --port 8090
ota-cli --server http://192.168.10.246:8090 list
```

Web 使用左侧 OTA / SOL 导航。OTA 保留上传发布、升级历史、发布记录和审计标签；SOL 提供只读日志流、历史回放、阶段筛选与导出；当前调试配置的网页和设备均免凭据自动连接，不依赖 SSH。默认日志保留串口的正文格式，不显示采集元数据前缀。配置和验收见 [SOL 与板卡验收](docs/sol.md)。

## 设备日志

```sh
logctl status
logctl verify-storage
logctl verify-boot
logctl boots
logctl show --stage kernel
logctl show --boot previous
logctl follow --stage services
logctl export --boot previous --output /tmp/diagnostic.tar.gz
```

AM625 启动脚本只尝试挂载已有的 `/dev/mmcblk1p15` ext4 文件系统到 `/data`，**不会自动格式化**。可写时日志保存在 `/data/log/boots/<boot-id>/`，否则降级至 `/run/logctl/boots/` 并报告 volatile；后者重启或断电即丢失。新工厂镜像在 data 起点预建 256 MiB ext4；旧卡 OTA 不初始化该分区，首次部署仍需核实已有文件系统。

每个启动记录包含阶段日志、内核原始记录、服务日志、fwctl 版本输出和内核命令行。默认保留最近 4 次启动，每阶段日志保留 2 个不超过 2 MiB 的文件；pstore 单次归档限制 1 MiB。正常日志批量同步，事件日志立即同步。目录仅 root 可读。

SPL/U-Boot 通过板级缓存协议交接，Linux 从 `/sys/firmware/bootlog/` 读取已校验快照。SPL/U-Boot 必须更新后才能产生这些记录；OTA 仅更新 kernel/rootfs 时不能验证新引导程序。initramfs 的日志通过移动 `/run` 留给主系统。

采集范围是已接入的 UART 启动输出、kernel printk、启动脚本与 `/var/log/*.log` 服务输出，并非物理 UART 的无遗漏监听。ROM、TIFS、ATF 输出、未接入服务及任意交互 shell 的输出不在当前覆盖范围内。超过容量会截断或轮转；RAM 缓存与 ramoops 不保证断电保留，启动到 Linux 之前失败的日志也不能保证落盘。`show` 按阶段展示，跨阶段时间并不共享同一计时起点。

## 固件与验证

配置路径为 `/etc/device-manager/firmware.conf`。固件 tag 固定 `v0.0.1`，内容变化更新 SHA256；完整哈希用于验证和审计，普通日志显示前 8 位。发布后的设备健康确认才表示升级成功。

执行 `make -C logging test` 验证日志生命周期；`python3 tests/test_firmware.py` 验证固件引擎；`make -C ota test` 验证管理端和代理。OTA 的板级脚本测试需要相邻的 `bsp-base-external`；实际设备升级和 bootloader 接力需单独硬件验证。源码迁移出处见 [MIGRATION.md](MIGRATION.md)。
