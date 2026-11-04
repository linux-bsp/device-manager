# HWID 与 U-Boot 环境

HWID 使用固定 16 字符的小写十六进制字符串，格式为 `S-PP-JJJJ-MMMM-HH-RRR`。当前分配值为 `1010001000110000`，PCB 丝印版本 V1.0。HH 严格表示 PCB 版本。

## 身份来源和命令

持久化来源是 U-Boot 环境变量 `hwid`。Linux 使用 libubootenv 0.3.7 访问同一环境；普通 OTA 只声明支持的 HWID，不修改设备身份。

```sh
fwctl hwid show
fwctl hwid value
fwctl hwid set 1010001000110000
fwctl hwid --config /tmp/fw_env.config show
```

`set` 只更新 hwid，保留其他变量，写入后重新打开环境读回验证；同值设置不写盘。环境 CRC 都损坏时返回错误，不用默认值重建整份环境。允许维护人员改变有效 HWID，验证不同项目或型号匹配。

U-Boot 中使用 `hwid` 或 `printenv hwid` 查看，使用 `setenv hwid 1010001000110000` 和 `saveenv` 修改并保存。身份由人工配置，不会自动探测 PCB 丝印，须与实物相符。序列号另行管理。

## SD、eMMC、SPI NOR

标准 `/etc/fw_env.config` 必须与 U-Boot 的环境位置、大小及冗余设置一致。当前 SD 配置：

```text
/dev/mmcblk1 0x600000 0x10000
/dev/mmcblk1 0x620000 0x10000
```

- SD/eMMC user area 使用对应块设备，环境后端处理 CRC 和递增副本标志。
- eMMC boot0/boot1 使用 `/dev/mmcblkNboot0` 或 `boot1`，U-Boot 配置匹配的 `CONFIG_ENV_MMC_EMMC_HW_PARTITION`。不同 boot area 放置副本时，配置两条对应路径。工具不自动解除设备写保护。
- NOR 使用 Linux MTD 字符设备，偏移相对于该 MTD；U-Boot SPI 偏移相对于完整 Flash，必须正确换算。两个副本分别占用独立擦除块，参考 BSP 的 `fw_env.spi-nor.config.example`。
- Linux HWID 接口拒绝 NAND MTD。

跨介质使用相同 API，各板的环境位置和驱动配置由 BSP 指定。

## 启动与固件匹配

SPL 在 MMC/SPI 可用后加载环境，读取 hwid。R5 和 A53 环境大小、位置保持一致；MMC 冗余读取使用堆缓冲区，避免两个完整环境占用 SPL 栈。

完整 U-Boot 输出环境身份，并通过 `/chosen/dm,hwid` 传给 Linux。无 SPL 的设备直接使用完整 U-Boot 环境。initramfs 和 fwctl 从相同环境读取。

AM625 新版固件配置的 `hwid-config=/etc/fw_env.config` 要求 FIT 的 `fw,hwids` 列表包含设备完整 HWID。release、bootloader、kernel、rootfs 校验及恢复写入执行身份检查，不根据版本相近推断兼容。

修改后重启，使启动设备树与各阶段统一为新身份。`fwctl hwid show` 显示当前保存的值。工厂整卡镜像包含两份完整默认环境及分配的 HWID；普通 OTA 不覆盖环境区。恢复默认环境或重刷工厂环境后，需要核对人工配置的身份。

## 测试

`python3 tests/test_hwid.py` 使用真实 libubootenv 源码、块设备文件及 NOR 系统调用模拟，验证 CRC、副本选择、计数回绕、身份修改、其他变量保留、eMMC 区域路径、NOR 擦除和中断、NAND 拒绝及跨阶段编码一致性。`LIBUBOOTENV_SOURCE` 可指定 v0.3.7 源码目录。

模拟和交叉编译不代表真实 eMMC/NOR 硬件验证；实机结果另记在 workspace 验证文档。
