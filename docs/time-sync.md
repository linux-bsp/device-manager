# 设备校时与归档时间

Device Manager 提供简易 HTTP 校时服务，复用现有管理端口（开发环境为 8090）。设备联网后由 logctld 的独立上传进程取时，不阻塞本地采集、启动健康检查或 OTA。标准 NTP 客户端不能直接使用这个 HTTP 接口。

## 设备行为

AM625 的 `/etc/device-manager/sol.conf` 默认包含：

```ini
server=http://192.168.10.246:8090
auth=none
interface=eth0
time-sync=1
```

网卡 link UP 且有 IP 后进行三次带随机 nonce 的请求，选择往返延迟最小的有效样本，以半程延迟估算返回时刻。服务器时间必须在 2020–2099 年，纳秒字段和 nonce 必须匹配；超时、无效内容和重定向不会被用于设时。

首次联网校时，随后每小时校正；检测到系统时间相对单调时间跳变超过两秒时提前重试，失败每 30 秒重试。偏差超过 100 ms 才调用 `clock_settime`。采样、重试和周期使用 CLOCK_BOOTTIME，不受日历时间调整影响；停止服务会取消正在进行的请求。

校准成功后尝试把 UTC 写入 `/dev/rtc0`；`rtc-state=written` 仅说明写操作成功。内核开启 `CONFIG_RTC_HCTOSYS=y` 读取 RTC，但本板 AM625 片内 RTC 在软件复位后实测回到初始值，因此可靠来源是每次联网重新校准，不能宣称跨重启保时。RTC 写失败不撤销已成功的网络校时，状态会单独标记。

TI 的 [AM625 硬件设计说明 SPRAD05E，第 7.11 节](https://www.ti.com/lit/ug/sprad05e/sprad05e.pdf) 说明片内 RTC 没有用于跨复位保时的独立供电，软件或外部冷/热复位可能使计时值复位。本轮不改变系统复位方式来绕过此硬件边界，也没有把复位后的 1970 年时间当作已校准时间。

```sh
logctl sync-time       # 立即手工校时，使用上述 server/interface
logctl status          # 查看校时状态、最近成功时间、延迟估计和 RTC 状态
date -u
cat /sys/class/rtc/rtc0/date
cat /sys/class/rtc/rtc0/time
```

`clock-offset-ms` 是最近一次校准前测得的修正量，首次从 1970 年校准时数值会很大，不是当前剩余误差。

`time-sync=0` 关闭自动校时，仍可显式执行手工命令。未配置该字段的旧配置默认关闭，不会使主机侧测试上传器自行调整主机时钟。

状态原子写入 `/run/logctl/clock-status`，包括 `clock-state`、`clock-source`、`clock-synced-at`、`clock-offset-ms`、`clock-uncertainty-ms`、`rtc-state`。网络不通时保留已有系统时间；从未成功时标为 waiting-network/unsynchronized，有既往样本时标为 holdover。精度取决于管理服务器时钟和网络延迟，往返延迟估计不是高精度时间保证。

## 服务接口

`GET /api/time/<32位小写十六进制nonce>` 返回：

```text
DMTIME1 <原nonce> <Unix秒> <纳秒>
```

接口只读，禁用缓存；支持 HEAD，其他方法返回 405。服务器自身时间超出合理范围时返回 503。设备使用配置的管理服务器，不跟随重定向到其他时间源；沿用当前调试网络配置。

## 归档日志名

Web 的启动归档列表和下载文件使用服务器数据库中不可变的首次接收时间，按北京时间（+08:00）显示，示例：

```text
2026-11-04_08-36-15+0800_startup_df6e92a1.log
2026-11-04_08-36-15+0800_startup_df6e92a1_kernel.log
```

短 ID 区分同秒到达的记录，阶段后缀区分筛选导出。已有归档立即适用，不改变内部 capture ID 或 OTA 关联。实时控制台导出使用导出时刻。缺少可靠接收时间时明确显示 time-unknown，不伪造 1970 年日期。

首次接收时间不代表异常发生时间；校时不会回填过去的异常启动时间，也不会改写原始日志正文。设备上 `/data/log/boots/<boot-id>/` 继续按内部启动 ID 组织。

## 验证

`python3 logging/tests/test_time_sync.py` 用进程内拦截的时钟和 RTC 测试客户端，始终不修改测试主机时间。`python3 ota/tests/test-manager-time.py` 检查真实 HTTP 服务；`node --test ota/tests/test-sol-names.cjs` 检查固定接收时间、同秒记录和时区。实机验收单独记录在 workspace 的 `docs/am62x/`。
