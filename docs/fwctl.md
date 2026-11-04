# fwctl

`fwctl` is the command-line adapter for the Device Manager FWMD firmware engine. The
FWMD RAW A/B implementation is linked directly into the standalone executable; this tool parses
arguments, formats status and progress output, and maps failures to exit codes.

## Commands

```sh
fwctl [--config path] init a|b
fwctl [--config path] status
fwctl [--config path] version
fwctl [--config path] update [all|bootloader|kernel|rootfs] [release.itb|URL]
fwctl [--config path] mark-good [deployment]
fwctl [--config path] mark-bad <deployment>
fwctl [--config path] rollback
fwctl [--config path] health-check
```

`health-check` runs the registered boot, marker, service, PDM/CAN device, and
firmware metadata checks. It also queries `system_manager` over the OSA local
IPC socket for required service readiness and heartbeat state. It exits zero
only when every check passes and is used by the AM62x boot confirmation flow.
Exit 75 means required services or device nodes are not ready yet; exit 1 means
a definite failure. The AM625 worker requires six consecutive passing checks,
with at most 24 observations separated by five seconds. Readiness resets the
consecutive-pass count; a definite failure ends observation immediately. A
failed pending candidate is marked bad and rebooted to the confirmed deployment.
An unhealthy already-confirmed system stays available for diagnostics.

`version` reports the deployment actually booted, identified by
`fw.deployment` in `/proc/cmdline`, followed by its release, kernel, and rootfs
versions. It does not substitute the pending, selected, or last-good deployment,
so staging an update does not change the reported running version. Unrecorded
versions are printed as `unknown` rather than `0.0.0`.

When factory metadata has no release version, a valid `FIT_VERSION` from
`/etc/ota-release` is printed separately as the running **rootfs image** version.
If the boot deployment is unavailable (for example network boot), only that
rootfs identity can be reported; it does not identify a TFTP-loaded kernel.
The command does not write metadata or confirm a deployment.

The AM62x BSP invokes `fwctl version` from `S99fw-version` during startup.
The initial status may be `boot-testing` / `successful=0` until the existing
health-confirmation worker completes. A version-printing failure is diagnostic
and does not itself block boot confirmation.

`status` reports release and component versions in `major.minor.patch` form
for encoded versions. Each component also reports the build time recorded in
its FIT image as `YYYY-MM-DD HH:MM +08:00`; older metadata or FIT images
without this field are shown as `unknown`.

`update kernel` (or `rootfs`/`bootloader`) uses the configured update server and
only stages that component from the release manifest. `update` with no
component stages all components. A local file or complete URL may still be
provided as the final argument for one-off updates. The command downloads or opens the release FIT, matches the complete device HWID against its `fw,hwids`
allowlist, verifies the manifest and component hashes, checks version and
rollback policy, and writes each component to the inactive physical slot.
The target deployment is persisted as non-bootable `writing` before any
component write. It becomes `ready` and `pending` only after every write and
readback succeeds. The command does not reboot the system.

For URL releases, component paths are resolved relative to the release URL.
HTTP, HTTPS, and TFTP are accepted. An HTTPS request may redirect only to
another HTTPS URL, while an HTTP request may redirect to HTTP or HTTPS. TFTP
does not follow redirects and should be used only on a trusted network. Enable
FIT signature verification when the transport itself does not authenticate
the firmware publisher.

The update settings in `/etc/device-manager/firmware.conf` are used when no local file or URL is
provided. `update-protocol` accepts `tftp`, `http`, or `https`; `update-server`,
`update-path`, and `update-release` form the release URL. For example,
`fwctl update kernel` resolves to
`tftp://192.168.18.245/release.itb` and stages only the kernel
component listed by that manifest.

## Configuration

The library requires a complete product layout; it has no built-in board map.
The default configuration path is `/etc/device-manager/firmware.conf`:

```ini
device=/dev/mmcblk1
storage-type=block
hwid-config=/etc/fw_env.config
boot-attempts=3
work-dir=/tmp
update-protocol=tftp
update-server=192.168.18.245
update-path=.
update-release=release.itb
allow-bootloader-update=false
require-signature=false
signature-key=

meta0-offset=0x00500000
meta0-size=0x00010000
meta1-offset=0x00510000
meta1-size=0x00010000
# Remaining component region offset/size pairs are required.
```

`storage-type=block` selects eMMC/SD or a regular file. `storage-type=mtd-nor`
selects an SPI-NOR MTD character device and applies its runtime write and erase
block sizes. The MTD device must report non-zero capacity, write size, and erase
size. All configured regions must satisfy the selected medium's alignment and
capacity requirements and must be mutually non-overlapping. Metadata regions
must be large enough for one metadata record and one erase block on MTD.
Product code owns this layout file; command-line options cannot override the
device or metadata offsets.

When `require-signature=true`, `signature-key` must name a trusted key DTB and
`/usr/bin/fit_check_sign` must be installed. The release FIT and every
component FIT are checked against that key. SHA-256 hashes detect corruption
but do not authenticate a publisher, so unsigned mode is intended only for
development.

## Release Layout

`release.itb` is a small FIT manifest. Component files such as `kernel.itb`
and `rootfs.itb` remain separate files in the same local directory or URL
directory. Each manifest entry contains the complete component file SHA-256;
each component FIT also contains hashes for its embedded images.

AM62x production OTA defaults to kernel and rootfs only. Remote bootloader
updates are disabled because the ROM always loads `tiboot3_a`, which prevents
a fully atomic first-stage A/B switch.

## Boot Confirmation

After U-Boot or SPL selects a pending deployment, Linux receives
`fw.deployment=<id>` on its kernel command line. `fwctl mark-good` reads that
value when no ID is supplied and confirms the deployment. Until confirmation,
the bootloader decrements the remaining attempts and eventually rolls back to
the last-good deployment.

### Health IPC diagnostics

`system_manager` receives one-shot READY/HEARTBEAT/STOP reports and closes each
health-query connection immediately after sending its snapshot. On a Unix
`SOCK_SEQPACKET` socket, a queued final packet and peer closure can produce
`POLLIN | POLLHUP` together. The OSA receiver must consume readable packets
before reporting a hangup; otherwise it can lose READY reports and return
`Connection reset by peer` for a valid health reply. This can cause repeated
restarts of a running `psatellited` and leave deployment confirmation pending.

The receiver retains frame validation and reports closure once the queue is
empty. `osa_ipc_test` covers queued packets after close, one-shot health events,
query/snapshot exchange, malformed frames and empty/disconnected sockets.
It is cross-compiled with the Buildroot test preset; execute it on the target
for runtime verification. `system_manager` line-buffers stdout so READY events
are immediately visible in `/var/log/system_manager.log`.

On the board, inspect:

```sh
cat /var/log/system_manager.log
cat /var/log/psatellited-service.log
fwctl health-check
```

The AM625 confirmation worker still requires all health checks to pass during
its stability window. IPC recovery does not bypass supervision or confirmation.
Failure to write the rejection metadata leaves an explicit diagnostic and never
silently confirms the candidate.

## Standby rootfs maintenance

The initramfs verifies the selected rootfs and hands off without checking or
downloading the standby rootfs. After this boot passes health confirmation, the
existing background health worker runs `fwctl recovery maintain`. It checks the
alternate rootfs against its recorded full identity and requests the exact
version, firmware ID and payload SHA256 if repair is needed. A manual retry is:

```sh
fwctl recovery maintain
```

Maintenance uses `/etc/device-manager/recovery.conf`, installed from the same
board configuration embedded in the initramfs. It does not reconfigure the
running network, open the watchdog, or delay startup. Downloads have a 60-second
limit; an unavailable server defers repair.

Standby repair rejects pending/writing transactions, a shared running rootfs
slot, stale identity and mismatched payload. On SD/eMMC, it holds an exclusive
kernel claim on the exact partition during write and readback; mounted
partitions and device-mapper holders are rejected. NOR background repair is not
enabled by this change. Repairing the selected rootfs still requires RAM root.
Repair does not confirm a deployment or change boot selection.

Block-device metadata, integrity checks and write readback use a separate
`O_DIRECT` descriptor. Reads are internally aligned to the logical sector size,
including short headers and unaligned FIT tails. This avoids whole-device versus
partition page-cache aliases and prevents readback from checking only the
writer's cached bytes. Writes are synchronized before readback. Regular-file
simulation uses ordinary reads; direct-I/O errors on real block devices are
reported rather than silently falling back to cached reads. Physical power-loss
behavior still requires validation with the actual SD/eMMC hardware.

Only a privileged service should have access to `fwctl` and the raw storage
device.

## OTA machine status

`fwctl ota-status` is a read-only, versioned interface used by the BSP OTA agent:

```text
FWOTA2 booted active pending version0 successful0 state0 firmware-id0 version1 successful1 state1 firmware-id1
```

Numeric fields are decimal integers; each deployment identity is its full 64-character
SHA-256 firmware ID, or `-` if metadata has no complete identity. `pending=255` means none. States follow
`edk_fw_deployment_state` (ready=2, boot-testing=3, confirmed=4, failed=5).
`booted` comes from the current kernel command line, never from mutable pending
or selected metadata. Missing boot identity (including network boot) returns an
error. Unknown metadata versions stay zero; rootfs markers are not substituted.
Automatic OTA requires a confirmed boot, writes the inactive deployment, and
reconciles the transaction using both the numeric version and full firmware ID.
It reports success only after rebooting into the exact published version/ID and
observing health confirmation. The agent does not invoke mark-good itself.

## initramfs 恢复接口

- `fwctl recovery status`：输出本次启动的 deployment、rootfs 槽和组件版本，格式为 `FWRECOVERY1 <deployment> <slot> <version>`。
- `fwctl recovery fallback`：仅在 RAM 根文件系统内运行；选择另一已确认且内核校验通过、包含恢复 ramdisk 的完整 deployment。保存后由调用者重启；没有候选时不改 metadata。
- `fwctl recovery restore ROOTFS_ITB PAYLOAD_BYTES PAYLOAD_SHA256`：仅供 initramfs 在 rootfs 未挂载时使用。验证包与内核绑定的准确内容，修复当前选定 rootfs 槽并读回核对，不更新 metadata 或其他分区。

恢复接口不执行 `mark-good`。新升级是否健康仍由正式系统确认。底层 API 的调用者必须保证待修复 rootfs 未挂载。

`fwctl update` 失败时保留 errno 作为进程退出码（1–125），stderr 同时打印错误名称和数字。
fwctld 据此将 I/O、空间不足、设备不可用等记录为设备异常；未知失败保留原始退出码，
不将其猜测为某一种硬件或内核根因。其他命令仍使用原有退出状态约定。


### 固件内容身份

`fwctl identity` 打印本次启动固件的 `tag`、SHA-256 内容 ID 的前 8 位和 deployment；
`fwctl version` 同时包含这行身份和组件状态。ID 来源于已安装 release 的最终
kernel FIT 与 rootfs 有效内容，不是 EDK 的源码 Git ID。

工具核对 U-Boot 在 `/chosen/fw,tag`、`fw,id`、`fw,deployment` 中传入的启动快照，
不会用等待下次启动的 pending deployment 代替运行版本。无身份的旧记录输出
`identity=unknown`；不一致返回错误。恢复环境使用 `fwctl identity initramfs`。

此格式要求支持 4096 B 元数据记录的 SPL/U-Boot，现有双 64 KiB 元数据分区不变。

### 启动和升级日志节点

固件日志使用 `[模块] [级别] 内容` 格式，如 `[fwctl] [INFO] running release=0.0.1` 和 `[fwctld] [ERROR] Cannot fetch ...`。不打印分隔线或重复阶段标题，fwctld 转发 fwctl 日志时保留原模块与级别，避免双重前缀。

- `Staged` / `Ready` 表示写入完成或部署已准备好，仍须重启和健康确认。
- `current deployment marked good` 表示本机健康确认；`success` 表示服务器已接受成功上报。
- `identity=unknown (legacy metadata)` 表示当前启动链没有可用的完整固件身份；新版内核/rootfs 配合旧 SPL/U-Boot 时可能出现。须更新启动链并重新安装完整 release 才能验证各阶段身份传递，不能根据发布记录伪造启动身份。
- initramfs 切换根文件系统时看门狗保持启用，随后由 system_manager 接管。`nowayout` 的关闭警告不等于升级失败，也不能通过关闭看门狗隐藏。
- 网络链路建立前的 HTTP 重试不代表固件异常；链路建立后持续失败则应检查服务器连通性。

`ota-status`、`ota-check`、`recovery status` 保持机器输出协议；`identity` 仍为单行身份日志。日志仅显示哈希前 8 位，协议和完整性校验仍使用完整 SHA256。

固件版本统一按 `(major << 16) | (minor << 8) | patch` 解码；数值 1 显示为 `0.0.1`，256 显示为 `0.1.0`。当前项目固定使用 tag `v0.0.1`，除非用户明确要求更改。内容变更以完整 SHA256 身份区分，相同 tag、不同哈希仍会升级。

`fwctl version` 在 release 及 kernel/rootfs 版本后打印 `hash=xxxxxxxx`。
这三行使用同一个完整固件内容 ID 的前 8 位，表示组件所属发布，不能当作各分区单独的校验值。
身份来自当前启动 deployment 与启动器传入的身份核对；旧元数据没有身份扩展时，从当前 deployment 对应的 kernel FIT 及其绑定的 rootfs 实际字节校验并重建身份；不从 OTA 最新发布推测，不修改元数据或确认健康。无法读取、验证或识别的旧固件仍不能提供可信哈希。
