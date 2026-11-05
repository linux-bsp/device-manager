# 设备固件管理

`fwctl/` 统一管理设备固件操作，构建一个 `bin/fwctl` 可执行文件，安装到
`/usr/sbin/fwctl`。设备 rootfs 与 initramfs 使用同一程序；initramfs 的 `/init`
脚本通过 `exec /usr/sbin/fwctl init` 进入早期启动流程。

## 内部职责

| 目录 | 职责 |
| --- | --- |
| `cli/` | 命令解析、HWID 命令、状态与版本输出，将命令参数转换为操作请求 |
| `core/` | FIT/布局解析、固件身份、A/B 元数据、升级事务、回滚和恢复写入策略 |
| `storage/` | 块设备与 MTD 访问、锁定、读写、擦除、同步与分区占用检查 |
| `recovery/` | PID 1 启动编排、rootfs 完整性校验、网络恢复、看门狗接力及 switch_root |
| `health/` | 启动健康检查、systemd 状态查询、外设监督服务 IPC 适配 |
| `include/` | 固件操作 API 和命令层声明；内部结构留在对应实现目录 |
| `tests/` | 元数据、事务、恢复、实际存储读取、命令和健康检查测试 |

`core/dm_fw_recovery.c` 实现受事务和存储约束保护的恢复操作；
`recovery/fw-init.c` 决定何时校验、回退、请求网络修复和切换根文件系统。
两者使用相同的版本、完整 SHA256 与分区安全规则。

CLI 和健康适配通过 `include/dm/firmware.h` 使用引擎。存储适配不调用 CLI，
不决定是否确认一次升级。下载统一使用 `../transfer/`；硬件身份使用 `../identity/`。
`../ota/agent/` 中的 fwctld 负责服务器轮询、持久下载和结果上报，通过 fwctl 执行安装。
固件引擎不依赖 EDK 库，健康检查使用独立 IPC 适配器。

## 构建与验证

在 device-manager 根目录运行：

```sh
make fwctl
make -C fwctl test
bin/fwctl help
```

也可以直接运行 `make -C fwctl`。两种入口都生成仓库根目录的 `bin/fwctl`，
Buildroot 的安装路径和选项保持一致。模块 Makefile 管理源文件和头文件依赖，
支持增量编译；切换编译器或编译选项前清理本模块及共享传输库的构建目录。
交叉编译使用 `CC`、`AR`、`CPPFLAGS`、`CFLAGS`、`LDFLAGS`，目标程序须在板上或
匹配的模拟器中执行。依赖 libcurl、OpenSSL、libfdt、libubootenv 和 pthread。

`make -C fwctl test` 运行八组原生固件测试与 systemd 健康适配测试，
通过临时文件模拟存储，不写真实设备。BSP 中的 `support/fw-recovery/tests/`
继续验证板级 FIT、根文件系统、initramfs 依赖和实际 ARM64 动态加载。

## 执行边界

- 普通更新保留已有 A/B 元数据、断电恢复与健康确认协议。
- `fwctl init` 只允许在 initramfs 中作为 PID 1 运行；不依赖主 rootfs 的 systemd。
- 恢复只写经过版本、完整哈希和未挂载检查的目标，不自动格式化 data。
- U-Boot/SPL 实现仍在 BSP 对应源码树；本目录重组不改变它们或设备分区布局。
- 时间、日志和 OTA 代理的运行职责不因目录整理而改变。
