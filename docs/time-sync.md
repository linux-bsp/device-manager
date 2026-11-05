# 设备校时与归档时间

Device Manager 提供简易 HTTP 校时服务，复用现有管理端口（开发环境为 8090）。设备联网后由独立的 `timectld.service` 取时、写 RTC 并上报状态，不依赖 logctld/SOL，不阻塞本地采集、启动健康检查或 OTA。标准 NTP 客户端不能直接使用这个 HTTP 接口。设备也支持从外部标准 NTP 服务器取时；管理服务不占用 UDP 123 端口。

## 设备行为

AM625 默认本地时区为 UTC+8。Buildroot 使用 `BR2_TARGET_TZ_INFO=y`、
`BR2_TARGET_TZ_ZONELIST="etcetera"`、`BR2_TARGET_LOCALTIME="Etc/GMT-8"`，
安装 `/etc/localtime`；此 IANA 名称的负号表示东八区，不使用夏令时。
`date`、校时成功日志、`clock-synced-local` 以及网页校时/OTA/设备时间均显示 UTC+8。
网页不跟随浏览器时区。`date -u` 仍显示 UTC；NTP/HTTP 时间戳、数据库 Unix 秒和 RTC
均保持 UTC，不在校准数值上额外加八小时。kernel 的启动秒数保持原始格式。

`timectl status` 中的 `clock-timezone=+0800` 和带偏移的 `clock-synced-local`
用于检查显示；`clock-synced-at` 继续为原始 Unix 秒，便于不同时间源交叉核对。

AM625 的 `/etc/device-manager/time.conf` 默认包含：

```ini
server=http://192.168.10.246:8090
auth=none
interface=eth0
source=-
enabled=1
interval=3600
```

网卡 link UP 且有 IP 后进行三次带随机 nonce 的请求，选择往返延迟最小的有效样本，以半程延迟估算返回时刻。服务器时间必须在 2020–2099 年，纳秒字段和 nonce 必须匹配；超时、无效内容和重定向不会被用于设时。

首次联网校时，默认随后每小时校正（网页可设为 60–86400 秒）；检测到系统时间相对单调时间跳变超过两秒时提前重试，失败每 30 秒重试。偏差超过 100 ms 才调用 `clock_settime`。采样、重试和周期使用 CLOCK_BOOTTIME，不受日历时间调整影响；停止服务会取消正在进行的请求。

校准成功后尝试把 UTC 写入 `/dev/rtc0`；`rtc-state=written` 仅说明写操作成功。内核开启 `CONFIG_RTC_HCTOSYS=y` 读取 RTC，但本板 AM625 片内 RTC 在软件复位后实测回到初始值，因此可靠来源是每次联网重新校准，不能宣称跨重启保时。RTC 写失败不撤销已成功的网络校时，状态会单独标记。

TI 的 [AM625 硬件设计说明 SPRAD05E，第 7.11 节](https://www.ti.com/lit/ug/sprad05e/sprad05e.pdf) 说明片内 RTC 没有用于跨复位保时的独立供电，软件或外部冷/热复位可能使计时值复位。本轮不改变系统复位方式来绕过此硬件边界，也没有把复位后的 1970 年时间当作已校准时间。

```sh
timectl sync           # 请求 timectld 使用当前时间源立即校时
timectl status         # 查看守护进程、校时和 RTC 状态
timectl source set ntp://192.168.10.1
systemctl status timectld
date -u
cat /sys/class/rtc/rtc0/date
cat /sys/class/rtc/rtc0/time
```

`clock-offset-ms` 是最近一次校准前测得的修正量，首次从 1970 年校准时数值会很大，不是当前剩余误差。

`enabled=0` 关闭自动校时，守护进程仍处理网页配置和显式的 `timectl sync` 请求。只运行 SOL 上传程序不会触发设时。

状态原子写入 `/run/timectl/clock-status`，包括 `clock-state`、`clock-source`、`clock-synced-at`、`clock-offset-ms`、`clock-uncertainty-ms`、`rtc-state`。网络不通时保留已有系统时间；从未成功时标为 waiting-network/unsynchronized，有既往样本时标为 holdover。精度取决于管理服务器时钟和网络延迟，往返延迟估计不是高精度时间保证。

## 服务接口

`GET /api/time/<32位小写十六进制nonce>` 返回：

```text
DMTIME1 <原nonce> <Unix秒> <纳秒>
```

接口只读，禁用缓存；支持 HEAD，其他方法返回 405。服务器自身时间超出合理范围时返回 503。设备使用配置的管理服务器，不跟随重定向到其他时间源；沿用当前调试网络配置。

## 网页时间状态与时间源配置

打开 `http://<管理服务器>:8090/#clock`，侧栏选择“时间同步”。页面显示管理服务器时间、设备最近一次上报时的时间、实际协议、有效时间源、周期、最近成功校时、修正量、网络延迟估计和 RTC 状态。统一显示北京时间；设备时间是上报快照，不冒充持续读取的实时时钟。超过 30 秒没有回报标记为过期。

时间源可选：

- 当前 Device Manager：使用设备 `time.conf` 中的 server，配置值 `-`。
- 外部标准 NTP：如 `ntp://192.168.10.1:123`、`ntp://clock.example.org`，支持域名及 IPv4/IPv6，默认 UDP 123。
- 外部 Device Manager：如 `http://192.168.10.2:8090` 或 HTTPS origin；必须提供上述 DMTIME1 接口，普通 HTTP 网站不适用。HTTPS 需要设备已有合理时间及受信 CA；本板复位后时钟可能回到 1970 年，建议用 NTP 或内网 HTTP 完成首次校时。

保存配置同时触发一次校时，随后按所设周期运行；“立即校时”可用相同配置再次请求采样。关闭自动校时后设备仍接收配置与上报状态。源不可达时保留已有系统时间，显示失败；不会偷偷换用其他服务器。时间源配置不修改 OTA / SOL 的管理地址。

后台先记录持久化请求，网页显示等待设备应用。只有设备实际返回后才显示完成或 errno；当前生效源单独来自设备回报，不能把期望配置当作已生效配置。设备离线时可先下发，联网后处理。服务端 `clock.db` 保存请求、首次结果与设备状态；重复回报不会覆盖首次结果。

设备将源、启停、周期及已执行请求原子保存到 `/data/timectl/source.conf`（`TIMECTL_STATE_FILE` 可覆盖路径），对文件和目录做 fsync，进程重启/OTA 后继续使用。临时 RAM 文件系统不接受“持久化成功”；data 不可写时回报配置失败，保留原配置。`time.conf` 给出出厂默认值，已有持久化源优先；管理通道即使关闭自动校时也继续处理网页配置。

`timectl sync` 请求正在运行的 timectld 使用当前生效源取时；CLI 不直接修改系统时钟。`timectl source set URI` 持久化本地时间源，保留启停/周期与已处理的远程请求编号；后续新的网页配置请求可以覆盖它。保存源不代表同步成功，结果由 status 查看。网页“立即校时”目前要求启用自动校时。

### 生命周期与旧配置迁移

源码位于 `time/`，`BR2_PACKAGE_DEVICE_MANAGER_TIMECTL` 独立选择安装产物。
`timectld` 是指向 `timectl` 的链接，同一二进制按入口选择守护或 CLI 功能；只允许一个守护进程持锁。
命令通过 `/run/timectl/control.sock` 请求，CLI 总等待 15 秒，不自动重放请求。
`timectl status` 向存活进程查询，进程停止后返回错误，不将残留状态文件当作实时状态。

systemd 服务只排在板级 data 准备之后，不等待网络在线，也不依赖日志服务；没有可用网络时
保留未同步/保持状态并重试。停止 logctld 后校时和 Web 状态继续工作，停止 timectld 后日志采集
和 SOL 继续工作。服务声明与其他常见校时守护进程冲突；不要另行手动运行第二个设时程序。
日志程序不再链接校时实现，原 `logctl sync-time` 命令移除。

新状态文件不存在时，读取并校验旧 `/data/log/time-source.conf`，原子保存到
`/data/timectl/source.conf`；保留原文件供旧固件回退使用。新文件已经存在时不再导入旧副本。
持久化失败时不伪报成功，仍可使用读取到的有效源校时。data 准备和格式化不属于时间服务职责。

测试/独立运行可通过 `TIMECTL_CONFIG`、`TIMECTL_RUN_DIR`、`TIMECTL_STATE_FILE`
覆盖配置和状态路径；`TIMECTL_IMPORT_FILE` 指定迁移输入。日志目录不再用作运行状态目录。
本轮仅拆分职责，仍保留原有超过 100 ms 时步进设时的算法；频率驯服、渐进校正和多源选择
尚未实现，不能将独立守护进程视为完整 NTP 实现。RTC 跨复位限制也不会因拆分自动改变。

### 标准 NTP 采样边界

独立时间服务内的客户端沿用单播 SNTP，不启动 ntpd、chronyd 或 systemd-timesyncd。按 [RFC 5905](https://www.rfc-editor.org/rfc/rfc5905.html) 检查服务端模式、版本、leap、stratum、随机请求时间戳回显及接收/发送时间；拒绝未同步、KoD、截断、错配和异常时间响应。处理 NTP 2036 年 era 回绕，仅接受 2020–2099 年。三次采样取最短往返延迟，并扣除服务器处理时间。每次 UDP 收包等待最多 2 秒，可取消；域名解析使用系统解析器的超时策略。本功能用于联网后日历时间校准，不是完整 NTP 驯服算法或高精度 PTP 服务。

接口：`GET /api/clock` 查询时间状态；`POST /api/clock/config/<device>` 保存配置；设备 `GET/POST /api/clock/device/<device>` 拉取/确认。配置与报告是有界文本协议，不经 SOL 发送命令或执行 shell。沿用现有调试免凭据策略；普通模式复用设备/查看者凭据验证。

## 归档日志名

Web 的启动归档列表和下载文件使用服务器数据库中不可变的首次接收时间，按北京时间（+08:00）显示，示例：

```text
2026-11-04_08-36-15+0800_startup_df6e92a1.log
2026-11-04_08-36-15+0800_startup_df6e92a1_kernel.log
```

短 ID 区分同秒到达的记录，阶段后缀区分筛选导出。已有归档立即适用，不改变内部 capture ID 或 OTA 关联。实时控制台导出使用导出时刻。缺少可靠接收时间时明确显示 time-unknown，不伪造 1970 年日期。

首次接收时间不代表异常发生时间；校时不会回填过去的异常启动时间，也不会改写原始日志正文。设备上 `/data/log/boots/<boot-id>/` 继续按内部启动 ID 组织。

## 验证

`python3 time/tests/test_time_sources.py` 覆盖 NTP 包、外部源及配置持久化；`python3 ota/tests/test-clock-control.py` 检查请求与回报。`python3 time/tests/test_time_sync.py` 用进程内拦截的时钟和 RTC 测试客户端，始终不修改测试主机时间。`python3 ota/tests/test-manager-time.py` 检查真实 HTTP 服务；`node --test ota/tests/test-sol-names.cjs` 检查固定接收时间、同秒记录和时区。实机验收单独记录在 workspace 的 `docs/am62x/`。

`python3 time/tests/test_service.py` 检查单实例、CLI 有界请求、配置迁移、RAM 持久化拒绝、无 SOL 依赖以及日志程序不含设时入口。
