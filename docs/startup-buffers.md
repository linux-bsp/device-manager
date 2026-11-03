# 双缓冲启动日志

AM625 使用保留 DDR 保存当前和上次未归档的启动日志。它不是 ROM，也不能保证断电后保留；目标是 watchdog、软件重启等保留 DDR 的复位。SPL 和内核必须使用相同的 v2 布局。

## 缓存布局

设备树保留 `0x9c900000..0x9c9fffff`，共 1 MiB；不占用 `0x9ca00000` 开始的 ramoops。两组各 512 KiB，每组包含：

| 阶段 | 分配大小 | 说明 |
| --- | --- | --- |
| R5 SPL | 32 KiB | DDR 就绪前使用原有 2 KiB SRAM，随后迁入 DDR |
| A53 SPL | 32 KiB | 追加本次启动记录 |
| U-Boot | 64 KiB | 保留原始输出字节 |
| Kernel | 320 KiB | printk console 回放已有消息并采集至健康确认 |

每段含 32 字节头部；容量不含头部时相应减少 32 字节。前缀写满后记录 dropped 字节数，不伪装为完整日志。追加保留上一份已提交前缀的长度和 CRC；复位打断最后一次追加时，可恢复经过校验的旧前缀，并标为 `recovered-prefix`。CRC 用于识别 RAM 日志损坏，不替代固件 SHA256。

R5 每次在另一个 bank 开始，保留上次记录；第三次连续未归档启动会覆盖最旧的一组。编号 1/2 是日志缓存编号，与固件 A/B 槽无关。

## 上传、确认和清理

1. `S99fw-mark-good` 对本次启动完成六次健康检查、确认部署后，写 `/run/fwctl-boot-confirmed`；已确认过的固件在再次启动时也必须重新通过健康检查。
2. `logctld` 上传子进程冻结两组启动快照，后台按偏移重传；失败不会阻塞本地采集，也不会清空缓存。
3. 服务器按设备和 capture ID 保存四段原始字节，并检查整段长度与 CRC，SQLite FULL 事务提交后才确认整个快照。归档独立于实时 SOL 的四次启动/2048 块轮转。
4. 两组都得到服务器确认后，内核核对两个 capture ID，先使两个头部失效，再清空 DDR。下次 R5 从第一组开始。重复确认、确认丢失后重试、恢复启动上下文变化均允许；已提交内容禁止修改。

清除对象是两组已归档的启动缓存。服务器历史和现有本地普通日志不删除，实时 kmsg、服务日志及 pstore 继续工作。冻结后新的运行期 printk 不再写启动缓存。

## 查看与协议

`logctl status` 显示 `startup-state=collecting/sealed/acknowledged`、当前组和下次组；`logctl verify-boot` 能识别已归档清理状态。SOL 的启动列表提供正常启动和上次未确认启动归档，支持阶段选择、搜索和原始串口格式导出。capture ID 是缓存记录标识，不冒充异常启动的 Linux boot ID；`recovery-boot` 记录实际完成归档的 Linux 启动 ID。未确认记录不直接推断为 panic。

内核接口位于 `/sys/firmware/bootlog/`：`banks`、`state`、`bank{0,1}-{r5-spl,a53-spl,uboot,kernel}`、`seal` 和 `ack`。控制文件仅 root 可写。`POST /api/sol/startup/<device>/<capture>/<stage>/<offset>` 接收最多 4096 字节；`commit/0` 校验四段有限清单并持久确认。浏览器沿用 SOL 读取 API，原始归档标记 `raw=true`。

ROM、TIFS、ATF 直接输出仍未接入；R5 的采集初始化前输出、内核 console 注册前就发生的致命故障，以及 DDR 不保留的复位，不能由此机制保证恢复。ramoops 继续作为独立补充。异常启动早期若尚未生成 capture ID，由下一次成功运行的内核分配；不猜测其固件版本或实际 Linux boot ID。

## 验证

在 workspace 运行 `python3 device-manager/tests/test_bootlog_abi.py`；在组件中运行 `python3 ota/tests/test-startup.py`、`python3 ota/tests/test-boot-health.py` 和 `node --test ota/tests/test-sol-console.cjs`。主机 HTTP 测试只使用隔离临时数据库；实机结论单独记录在 workspace 的 `docs/am62x/`。
