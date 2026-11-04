# BSP OTA：自动升级、结果上报与内核恢复

服务端和设备端均为 C 可执行程序，代码位于 Device Manager；板级配置由 `bsp-base-external` 提供。

## 下载故障分类

网络超时、连接中断、临时 HTTP 错误上报 `transport-error / download-interrupted`，
保留已同步的部分文件，下次继续 HTTP Range 下载；此类事件保留在升级历史中，不计升级失败或
设备异常，不隔离固件、不停止分发。界面显示“网络传输重试中”。
缓存打开、写入、读取、同步失败上报 `device-error / cache-storage-failed`；内容长度越界或
SHA256 不匹配上报 `verify-failed / download-sha256-mismatch`，后者删除损坏文件。
真实设备异常和完整性错误继续遵守既有的停发、隔离和健康版本回退策略。
主动停止 daemon 记录 cancelled，不作为固件故障隔离。升级前先部署支持上述事件的主机服务，再发布设备固件。

- `device-server`：提供固件上传与发布管理网页，以及设备查询/下载接口。
- `ota-cli`：C 命令行客户端，通过 HTTP 查看状态、上传固件包和发布草稿，适合 SSH/无图形界面环境。
- `fwctld`：定期检查发布，下载校验后调用 `fwctl` 安装、重启，并在新系统健康确认后上报结果。

完整流程：**上传 → 校验并保存草稿 → 发布 → 设备下载校验 → 写入备用槽 → 重启 → 健康确认 → 上报成功**。只有启用自动安装的新版 fwctld 执行完整流程；旧版设备仍只下载。

## 启动管理服务

从 BSP workspace 根目录执行。构建需要 C 编译器、make、od、awk、zlib 和 SQLite3 开发文件；服务运行需要 `sha256sum`、`fdtget` 及 zlib 运行库。网页编入可执行程序，无需单独部署前端。`fdtget` 可使用已有 Buildroot host 工具：

```sh
make -C bsp-base-external/device-manager/ota
export PATH="$PWD/output/am625/host/bin:$PATH"
mkdir -p output/ota-feed
device-manager/ota/device-server \
    --root "$PWD/output/ota-feed" --bind 0.0.0.0 --port 8090
```

浏览器访问 `http://192.168.10.246:8090/`（主机 IP 按实际情况调整）。默认端口是 8090，避免占用本机 GitLab 的 8080；默认监听地址为 `127.0.0.1`。后台运行时也需要在服务环境的 PATH 中提供 `fdtget`。

## 上传与发布

1. 网页选择 `.tar.gz` 固件包，点击“上传并校验”。
2. 服务器校验文件格式、release FIT 的产品/布局、组件文件名及文件 SHA256，读取 `fw,version`。成功后显示在“待发布固件”中。
3. 确认版本后点击“发布到设备”。服务器自动分配递增发布编号，并原子切换 `latest`。
4. “发布记录”显示当前版本和历史发布。设备在下一次轮询时获取新版本。

固件包必须直接包含三个普通文件，可另带 `SHA256SUMS`：

```text
release.itb
kernel.itb
rootfs.itb
SHA256SUMS     # 可选；组件校验依据 release FIT 中的哈希
```

已有构建产物可这样打包：

```sh
tar -C output/am625/images/release -czf output/ota-package.tar.gz \
    release.itb kernel.itb rootfs.itb
```

不要上传整个 `sdcard.img.gz`，也不要只上传引用其他文件的 `release.itb`。现有测试包 `output/ota-validation/1.0.5/ota-1.0.5.tar.gz` 可以直接在网页上传。

压缩包及解压后内容总量均限制为 512 MiB，最多保留 256 个上传草稿。支持普通 ustar/GNU tar 头和只含时间/所有者等元数据的 PAX 扩展；拒绝重复文件、链接、目录、路径覆盖及未知文件。上传中断或验证失败会清理临时文件，保留原发布。

一个服务目录对应一种产品和布局；网页发布拒绝产品/布局不匹配。显式发布可以选择更低的展示版本，设备仍执行组件 rollback index 检查。重复点击当前已发布草稿返回原发布编号；重新发布历史草稿会生成新编号。重复上传同一个固件会形成新草稿，再次明确发布可生成新的发布编号。

**固件版本与发布编号不同。** 网页中的 1.0.1 来自 FIT 的 `fw,version`，发布编号是 #1、#2 等服务器序号。服务器不会修改 FIT 或绕过 `fwctl` 的安装策略。默认板级 ITS 仍使用固定版本；发布正式新固件前需要在构建阶段设置相应 FIT 版本。完整 FIT/签名/安装条件仍由 `fwctl` 检查。

## 无图形界面的命令行发布

`ota-cli` 是独立的 C 可执行程序，使用 PATH 中的 `curl` 访问现有服务器，不需要
浏览器、Python、jq、libcurl 开发包或访问服务器文件目录。编译仅需 C 编译器和 make：

```sh
cd ~/workspaces/am62x/bsp-base-external/device-manager/ota
make ota-cli
export OTA_SERVER=http://192.168.10.246:8090

# 查看草稿、发布编号、当前发布和设备升级历史
./ota-cli list

# 上传并校验，返回 {"id":"draft-...","status":"draft"}，不会立即发布
./ota-cli upload ~/workspaces/am62x/output/ota-validation/1.0.8/ota-1.0.8.tar.gz

# 将下面的 draft-... 替换为 upload/list 返回的实际 ID
./ota-cli publish draft-...
```

执行 `publish` 会使该版本对设备可见；启用自动安装的设备将在轮询后下载、安装并重启。
无需额外确认输入，适合脚本调用。固件版本来自 FIT，发布编号由服务端分配，不需要手填。
重复发布当前草稿返回原编号；重新发布历史草稿生成新编号；重复上传会创建新草稿。

参数与返回约定：

- `--server URL` 覆盖 `OTA_SERVER`；都未设置时使用 `http://127.0.0.1:8090`。
- 默认以缩进 JSON 展示响应；`--json` 原样输出服务器 JSON，便于自动化处理，
  例如 `./ota-cli --server http://192.168.10.246:8090 --json list`。
- 成功退出码为 0；网络/HTTP/服务端校验失败为 1；参数或本地文件错误为 2。
  失败信息写 stderr，并保留服务器返回的原因。
- 上传只接受 1 字节至 512 MiB 的普通文件，采用流式发送，拒绝符号链接/FIFO/目录。
  文件名包含空格可正常使用；以 `-` 开头的相对文件名请加 `./`。
- 写请求不自动重试。网络断开时，先运行 `list` 确认服务器是否已接收或发布，避免重复上传。
- `list` 同时返回 `drafts`、`releases`、`devices` 与 `device_history`。
  查询响应上限为 16 MiB；超限会报错，不会输出看似成功的不完整列表。

客户端可复制到另一台兼容的 Linux 主机使用（需 curl）。设备端和 OTA 服务端无需更新。
`make test` 包含隔离 HTTP 服务上的 CLI 集成测试，不会发布到正在连接开发板的服务器。

## 设备端

下载协议兼容旧设备；自动安装需要新版 `fwctld` 和支持 `ota-status`、`ota-check` 的 `fwctl`。旧 1.0.0–1.0.2 测试固件需要先手动升级一次。AM62x defconfig 已启用 `BR2_PACKAGE_DEVICE_MANAGER_FWCTLD`，包含 curl 命令和 BusyBox SHA256。

```sh
make -C buildroot BR2_EXTERNAL=../bsp-base-external alientek_am62x_defconfig
make -C buildroot
```

既有输出目录首次启用该包时，使用 `menuconfig` 选择 `fwctld`，再执行 `device-manager-rebuild` 和完整构建。

下载缓存默认 `/data/fwctld`；data 未挂载、只读或空间不足时，启动脚本退回 `/tmp/fwctld`，此时未完成下载不能跨重启续传。小型事务日志独立保存到 `/data/fwctld-state`，不会随下载缓存降级到 RAM。只有本地事务能够同步持久化，自动安装才会开始；不自动格式化或调整分区。
```sh
mkdir -p /data/fwctld
fwctld --server http://192.168.10.246:8090 \
    --hwid "$(fwctl hwid value)" --state-dir /data/fwctld-state \
    --cache /data/fwctld --interval 60
```

加 `--once` 则只检查一次。尚未发布时正常等待下次检查。下载完整且校验通过后，生成 `/data/fwctld/<HWID>/<发布编号>/` 和同目录的 `current`；`current` 只表示下载完成，不代表已安装或启动成功。

重复检查不会重新下载同一发布，但会验证缓存。下载暂存目录为 `.download-<发布编号>`，每个文件按服务端 Range 响应续传；中断后保留已下载字节，完整下载后仍逐文件校验 SHA256。文件校验失败会丢弃该文件并重新下载，失败下载不会替换之前的 `current`。若正在下载时突然断电，最终 SHA256 校验仍会检测未落盘或损坏的数据。

## 自动安装与结果上报

常规镜像默认 `FWCTLD_ENABLED=0`，避免未配置服务器就升级。配置镜像中的 `/etc/default/fwctld`，或使用启动前已挂载的 `/data/fwctld.conf` 覆盖：

```sh
FWCTLD_ENABLED=1
FWCTLD_SERVER="http://192.168.10.246:8090"
FWCTLD_CACHE="/data/fwctld"
FWCTLD_STATE="/data/fwctld-state"
FWCTLD_INTERVAL=15
FWCTLD_AUTO_INSTALL=1
FWCTLD_DEVICE_ID=""  # 默认使用 eth0 的 MAC，将冒号替换为连字符
```

`FWCTLD_DEVICE_ID` 必须稳定且每台设备唯一。`S98fwctld` 自动启动，输出到 `/var/log/fwctld.log`。直接运行或主动检查一次：

```sh
# 先停止已有 fwctld，避免缓存锁冲突。
/etc/init.d/S98fwctld stop
mkdir -p /data/fwctld
fwctld --server http://192.168.10.246:8090 \
    --hwid "$(fwctl hwid value)" --state-dir /data/fwctld-state \
    --cache /data/fwctld --auto-install --device 1c-63-49-22-c4-f7 --once
```

`--once --auto-install` 也会实际安装并重启；不带 `--auto-install` 则只下载。正常后台运行去掉 `--once`，或启动 init 服务。

自动安装条件：当前 deployment 已确认且没有 pending 更新。设备先获取当前发布的身份信息，调用 `fwctl ota-check` 比较当前启动 deployment 的版本和实际存储内容；版本与 kernel/rootfs SHA256 都一致时跳过下载，否则安装当前发布。发布编号只用于历史记录，不作为“已处理就永远跳过”的依据。内核按 FIT 的有效总长度校验，rootfs 按 FIT 中实际写入的 payload 长度校验，不包含分区剩余空间。写入前再次检查当前发布和隔离策略，然后先持久保存本地 installing 事务。备用槽写入、同步和读回验证后，设备持久保存 rebooting 并重启，不以服务器 ACK 为门槛。开始写入前网络策略不可确认时等待重试；提交后的本地动作使用已保存的完整版本与内容 ID。

重启后通过 `fwctl ota-status` 核对实际启动的 deployment。`S99fw-mark-good` 仍负责健康检查，fwctld 不强行确认。网页“设备升级历史”保留每次升级，点击“刷新”获取，并可展开各阶段上报时间：

| 状态 | 含义 |
| --- | --- |
| `installing` | 已登记事务，正在调用 fwctl |
| `rebooting` | 备用槽已写入并核对，已持久记录重启意图；上报可能晚于重启 |
| `boot-testing` | 新版本已启动，健康检查尚未确认 |
| `success` | 目标 deployment、版本和健康确认均匹配 |
| `install-failed` | 安装或安装后状态核对失败 |
| `rolled-back` | 目标 deployment 已失败，设备回到旧版本 |
| `interrupted` | 事务中断，未找到可继续的 pending 更新 |
| `reboot-failed` | reboot 命令失败，需要排查 |
| `cancelled` | 主动停止 agent；保留记录，不隔离固件 |
| `up-to-date` | 早期设备上报的无需安装状态；新版通过实际内容校验跳过，不新增升级事务 |

同一个失败发布不会自动反复安装；排障后重新发布生成新编号。未确认的启动一直保留 `boot-testing`，由现有健康/看门狗策略处理，不误报成功。离线时服务器保留最后一次上报，不能把旧结果当作设备在线状态。设备恢复联网后继续核对和上报。

本地事务与待上报事件位于 `FWCTLD_STATE/<HWID>/<设备ID>/`。`transaction` 保存最后状态，按序号命名的 `*.event` 保存未 ACK 的真实阶段。事件先同步，再原子替换事务快照并同步目录；中断后可从已同步事件恢复。CRC32 保护记录完整性，固件匹配仍使用完整 SHA256。顺序补报后才删除对应事件；相同阶段不重复写入。最多保留 64 条待报事件，满额、损坏或同步失败时保留证据并暂停新的自动写入，不丢弃未确认事件。

缓存清空不影响本地事务。启动检查、健康结果和回退可在离线时记录；联网后按顺序补报。补报前核对服务端发布的 tag 和完整内容 ID，避免服务端目录恢复后编号碰巧相同。失败事件尚未补报时，不进入下一次安装。升级到本实现的首次启动可采用旧 agent 已保存在服务端的进行中事务；之后新发起的升级直接使用本地记录。

服务器 `devices/<设备ID>` 保存最近状态，`history/<设备ID>/<发布编号>-<来源boot-id>` 保存全部阶段及服务器接收时间。补报时间不冒充设备事件发生时间。重复的相同状态不增加历史记录，后续升级不覆盖旧历史。备份应包含整个 feed，含 devices、history、releases、审计和 SQLite 数据库。已隔离固件不会因重新上传或重启设备而被自动重试。调试模式仍无设备凭据认证。

手动模式仍可使用：

```sh
cache=/data/fwctld/$(fwctl hwid value)
read marker publication hwid < "$cache/current"
[ "$marker" = BSPOTA2 ] && fwctl update "$cache/$publication/release.itb"
# 确认命令成功后执行 reboot。
```

## U-Boot 主动请求内核恢复包

需要更新包含这些修改的 U-Boot；仅升级 kernel/rootfs 不会升级 U-Boot。AM625 A53 配置启用 HTTP/TCP，环境版本为 8，正常 eMMC 启动使用 `fwctl load-kernel` 读取并验证 FIT。出现读错误、FIT 格式错误、哈希或版本检查失败时，自动调用一次 `fwctl recover-kernel`。

U-Boot 中配置（地址按实际网络修改）：

```text
setenv ipaddr 192.168.10.211
setenv serverip 192.168.10.246
setenv netmask 255.255.255.0
setenv ota_serverip 192.168.10.246
setenv ota_serverport 8090
setenv ota_recovery 1
saveenv
```

`ota_serverip` 未设置时使用 `serverip`；`ota_recovery` 默认 1。调试用 TFTP/NFS 启动路径不触发自动修复。也可以主动检查及修复所选内核：

```text
fwctl select
fwctl recover-kernel
```

设备从可信 metadata 获取固件版本和完整内容 ID，向 `/recovery/<product>/<layout>/kernel/<版本整数>/<固件ID>/kernel.itb` 请求，例如版本 1.0.2 对应 65538。服务器只从已发布历史中返回相同产品、布局、版本和固件 ID 的包；找不到返回 404，绝不使用最新版或同版本的其他构建替代。U-Boot 还会将下载 FIT 的完整 SHA256 与 metadata 绑定值比较后才写分区。应长期保留设备可能运行的历史包。initramfs 在当前 rootfs 验证通过后也检查另一 rootfs 槽；若其 metadata 身份完整且内容损坏，会尝试通过对应版本+固件 ID+payload SHA256 恢复。修复后的备用 deployment 仍保持 failed，需经常规升级/健康确认后才可启动。

下载大小受分区大小限制；U-Boot 验证 FIT 哈希、既有签名策略、产品、布局、组件类型、版本、rollback index 和已记录构建时间，再修复**原本损坏的内核槽**并读回验证。保留另一槽、rootfs 和 A/B metadata。健康内核不重复刷写。修复成功后重新加载并启动；下载、校验或存储写入失败则不继续 bootm，保留现有启动失败/看门狗行为。

U-Boot 恢复入口覆盖内核读入/完整性故障。rootfs 损坏由 kernel FIT 内独立的 initramfs 恢复环境处理，见 [AM625 恢复说明](../../bsp-base-external/board/ti/am625/verified-boot.md)。物理存储永久故障不能靠重刷修复。metadata 丢失或工厂初始化后的内核版本为 0 时无法确定匹配包，会拒绝自动恢复。恢复结果显示在 U-Boot 串口；网页设备结果表针对 Linux fwctld 的 OTA 事务，不表示 U-Boot 恢复遥测。

HTTP 端口处理遵循当前 U-Boot legacy 网络栈的 `httpdstp`，恢复命令临时设置并恢复它，参见 [U-Boot wget 文档](https://docs.u-boot.org/en/stable/usage/cmd/wget.html)。

## 服务器本地发布与存储

维护服务器本地 feed 时，也可使用 `device-server publish` 从三份 ITB 直接发布；远程日常操作优先使用上述 `ota-cli`：

```sh
device-server publish --root output/ota-feed \
    --source output/am625/images/release --version 1
```

CLI 的 `--version` 是发布编号，必须高于已有编号。`drafts/` 保存网页上传结果；`releases/<编号>/` 保存固定发布快照；`latest` 是当前对设备可见的清单。不要覆盖已经发布的文件。

## 接口与范围

| 接口 | 用途 |
| --- | --- |
| `GET /` | 上传、待发布列表和发布记录 |
| `GET /api/state` | 管理页面的版本状态 JSON |
| `POST /api/upload` | 上传原始 `.tar.gz` 数据，保存草稿 |
| `POST /api/publish/<草稿ID>` | 将指定草稿发布给设备 |
| `GET /latest` | 兼容现有设备的 `BSPOTA1` 清单 |
| `GET /releases/<编号>/<文件名>` | 设备获取固定版本组件 |
| `GET /releases/<编号>/identity` | 实际 kernel FIT/rootfs payload 的长度与 SHA256，用于刷写内容比较 |
| `GET /releases/<编号>/info` | 固件版本整数，与旧下载清单兼容 |
| `GET/POST /api/devices/<设备ID>` | 读取/持久保存设备事务；POST 需要 `X-OTA-Request: 1` |
| `GET /recovery/<产品>/<布局>/rootfs/<版本>/<固件ID>/<payload-sha256>/rootfs.itb` | initramfs 按版本、固件 ID 和 rootfs 内容获取准确恢复包 |
| `GET /recovery/<产品>/<布局>/kernel/<版本>/<固件ID>/kernel.itb` | U-Boot 按版本和固件 ID 获取准确 FIT |
| `GET /healthz` | 存活检查 |

管理写请求需要 `X-OTA-Request: 1`，上传还需要准确的 Content-Length 和 `application/octet-stream` 类型；不接受 multipart/chunked。浏览器跨来源写请求会被拒绝。这是可信局域网开发服务，没有账号认证、TLS、设备分组或主动推送连接。设备使用轮询；服务串行处理请求，大包上传期间其他请求需要等待。

压缩数据由 [zlib](https://www.zlib.net/manual.html) 处理；tar 解包只接受固定文件集合。服务端无 Python 运行依赖，Python 仅用于主机测试。

## 验证

```sh
make -C bsp-base-external/device-manager/ota test
sh bsp-base-external/device-manager/ota/tests/test-real-release.sh \
    output/am625/images/release
```

测试覆盖上传不发布、显式发布、重复发布、坏包/缺文件/路径限制、型号和版本检查、HTTP 请求边界、原设备下载协议及缓存保护。测试不会写入开发板或重启设备。

可选真实浏览器测试需要 Node.js 22 和 Chromium。对**独立临时服务目录**运行以下命令；测试会真的上传并发布指定包：

```sh
CHROMIUM_BIN=/path/to/chrome node \
    bsp-base-external/device-manager/ota/tests/browser-smoke.mjs \
    http://127.0.0.1:测试端口 \
    output/ota-validation/1.0.5/ota-1.0.5.tar.gz \
    output/ota-admin-validation
```

已用现有 1.0.1 固件完成 Chromium 文件选择、上传、发布、桌面/移动页面和设备客户端下载验证。实机安装与回滚仍需在开发板验证。

自动流程测试使用真实 HTTP 服务和 fwctld，替身 fwctl/reboot 模拟安装、确认、回滚与中断，不操作板卡。U-Boot sandbox 测试验证内核格式/哈希检查；AM625 实机 HTTP 恢复、断电中断和 watchdog 时序仍需通过串口验证。

## 1.0.5 启动与回滚修正

实机 1.0.3 暴露了三个问题，1.0.4 也包含相同问题，未发布的这两版测试草稿已归档：

- BusyBox 未启用 `tr`，旧版启动脚本生成设备 ID 失败，rcS 记录服务失败，健康检查正确地拒绝确认。现在使用 ash 内建 `read`、模式匹配和 `printf`。
- lighttpd 1.4.82 会移除关闭的目录列表模块，残留 `dir-listing.activate` 产生警告。删除冗余配置，使用默认关闭行为；已实际验证目录访问返回 403。
- 回滚到 schema 2 的旧 U-Boot 后，旧环境迁移保留了新版 `fw_load_kernel`，其中新命令不被旧 U-Boot 识别。schema 5 保持该变量为兼容的 MMC 加载命令，新校验使用 `fw_load_checked_kernel`。

1.0.5 四个 ITB 已部署到 `/srv/tftp/am625/`。处于 U-Boot 命令行时执行 `run upa && run upb && reset`，先更新 kernel/rootfs，再更新引导程序。两步成功才重启。启动应显示 release 1.0.5，等待约 30 秒正常健康确认；不应手动绕过检查。随后可在管理页面发布 1.0.6 验证自动升级。

如果只需让旧 U-Boot 启动回滚后的系统，可恢复兼容命令：

```text
setenv fw_load_kernel 'mmc dev ${fw_mmcdev} && setexpr fw_kernel_lba ${fw_kernel_offset} / 0x200 && setexpr fw_kernel_blocks ${fw_kernel_size} / 0x200 && mmc read ${kernel_addr_r} ${fw_kernel_lba} ${fw_kernel_blocks}'
saveenv
run fw_boot
```

新增验证：`python3 tests/test-init.py` 在缺少 tr 的 PATH 下运行 BusyBox ash；
`python3 tests/test-lighttpd-config.py <lighttpd-1.4.82主机程序> <模块目录>` 验证真实 Web 行为；
U-Boot 的 `python3 test/py/tests/test_am62x_boot_env.py` 覆盖回滚到 schema 2/3 的启动路径。修正版仍待实机复测。


## 1.0.7 关机清理修正

1.0.6 关机日志中的 PDM busy 和 CAN `-100` 需要区分原因：`-100` 是
`ENETDOWN`，说明 CAN 接口关闭后接收线程仍在运行；模块 busy 的具体占用者需要
实机诊断。源码中已确认并修正以下问题：

- `rcK` 开始时创建 `/run/shutdown`。`system_manager` 在此期间禁止新的服务重启，
  也不执行已排队的重启；存活期间继续喂狗。避免服务主动报告 STOP 后被再次拉起。
- PDM 统一由 `S10pdm` 管理，移除 `modules-load.d/pdm.conf` 的重复加载/卸载。
  post-build 同时清理增量构建中遗留的该文件。
- 服务被 SIGKILL 后继续有界等待退出；未退出时保留 PID 文件并明确报告失败。
  PDM 卸载最多尝试三次，失败时打印引用计数及可发现的进程文件描述符。
  CAN 服务在 PDM 仍加载时拒绝关闭接口，不强制卸载使用中的驱动。
- BusyBox init 在终止剩余进程之前执行 shutdown 动作。保留仍在使用的 tmpfs、
  devtmpfs 等 RAM/虚拟文件系统；继续同步并卸载或只读重挂载 ext4、UBIFS、NFS
  等持久/网络文件系统。没有把所有卸载错误重定向隐藏。

验证命令：

```sh
# 在 device-manager/ota 下执行
make test
# 在 workspace 下执行
cmake -S edk/tests -B output/edk-shutdown-tests
cmake --build output/edk-shutdown-tests -j8
ctest --test-dir output/edk-shutdown-tests --output-on-failure
make -C buildroot O="$PWD/output/am625" edk-reconfigure \
    BR2_PACKAGE_EDK_DEFCONFIG=ti_am62x_defconfig BR2_PACKAGE_EDK_WEB=y
make -C buildroot O="$PWD/output/am625" all
```

本机测试与 AM625 交叉编译、镜像打包已通过；实机卸载和关机时序仍需验证。
本次测试包位于 `output/ota-validation/1.0.7/ota-1.0.7.tar.gz`，只上传为待发布草稿。
从 1.0.6 升级进入 1.0.7 的第一次关机仍使用旧脚本。进入 1.0.7 并通过健康确认后，
用 `fwctl version` 确认版本，再执行一次 `reboot`，通过串口观察 PDM 卸载后再停止 CAN。
如仍出现 busy，保留 `PDM reference count` 和 `PDM holder` 输出继续定位，不能据本机测试
认定实机所有引用均已释放。看门狗 nowayout 保持启用，关闭喂狗进程时的提示不等同于
升级失败。

OTA 管理页同时增加持久升级历史。已有的最后状态自动迁移；此前被覆盖的记录无法恢复。
此服务端功能无需升级设备固件即可生效。历史测试覆盖多次升级、状态去重、失败/回滚、
服务器重启、旧数据迁移、写入中断后的重试以及损坏记录拒绝确认；Chromium 验证历史表、
阶段展开和原有上传/发布流程。


## OTA 实时串口日志

`S98fwctld` 默认传入 `--console /dev/console`。发现发布、开始/完成组件下载、
SHA256 校验、写入备用 deployment、安装子进程输出及重启前提示都会立即写入
`/var/log/fwctld.log`，同时镜像到串口。`fwctl` 每次进度回调主动刷新 stdout，
fwctld 边读边转发 stdout/stderr，不再等安装结束后一次性打印。健康确认和回滚上报也会显示。

串口镜像采用非阻塞写入，控制台不可用时保留文件日志；串口拥塞时以文件日志为准。
普通重复缓存轮询只写文件，不反复刷串口。设备通过轮询发现发布，提示时间取决于
`FWCTLD_INTERVAL`（测试固件 15 秒），不是服务器主动推送。

可在 `/data/fwctld.conf` 中设置 `FWCTLD_CONSOLE=""` 关闭镜像；日志文件仍保留。
该功能需要新版 fwctld/init 脚本与 fwctl。升级进入新版时，下载和关机仍由旧版升级守护进程
执行；进入新版后下一次升级才能看到完整实时流程。主机测试在下载和模拟 flash 写入
中途阻塞操作，检查进程退出之前文件和控制台均已收到日志。


## 设备守护进程统一命名为 fwctld

工具职责：`device-server` 管理服务端，`ota-cli` 操作服务端，`fwctl` 执行设备本地固件操作，
`fwctld` 负责后台 OTA 流程。协议、发布编号、设备 ID 和服务器上的升级历史保持兼容。
设备 daemon 源码位于 `ota/agent/`，共享 OTA 协议位于 `ota/shared/`，服务端实现位于
`ota/server/`；构建产物仍是 `ota/fwctld`、`ota/device-server` 和 `ota/ota-cli`。

| 项目 | 新名称 |
| --- | --- |
| Buildroot 包/选择项 | `package/device-manager` / `BR2_PACKAGE_DEVICE_MANAGER_FWCTLD` |
| 程序 | `/usr/sbin/fwctld` |
| 启动脚本 | `/etc/init.d/S98fwctld` |
| 默认/持久配置 | `/etc/default/fwctld` / `/data/fwctld.conf` |
| PID / 日志 | `/var/run/fwctld.pid` / `/var/log/fwctld.log` |
| 新配置默认缓存 | `/data/fwctld` |

设备配置变量统一使用 `FWCTLD_*`，仅加载 `/etc/default/fwctld` 和
`/data/fwctld.conf`，后者覆盖默认配置。管理主机 `ota-cli` 的 `OTA_SERVER` 不变。
启动及安装脚本只处理当前配置和文件，不包含旧名称映射、迁移或清理逻辑。
当前 AM625 构建目录中的过时包和文件已一次性清理；新构建使用
`BR2_PACKAGE_DEVICE_MANAGER_FWCTLD`。

```sh
# workspace 根目录；已有 AM625 配置已启用 BR2_PACKAGE_DEVICE_MANAGER_FWCTLD
make -C buildroot O="$PWD/output/am625" device-manager
make -C buildroot O="$PWD/output/am625" all

# 设备端
/etc/init.d/S98fwctld restart
tail -f /var/log/fwctld.log
```


自动升级成功后，即使 RAM 缓存因重启丢失，只要版本和实际内容一致，就不会再次下载。
网络/HTTP 查询失败和 fwctl 本机状态错误分别打印原因，并提示当前失败阶段与重试间隔。

## 设备主动强制恢复

停止后台实例，使用同一个设备 ID 发起一次恢复；成功后自动重启，并在重启后健康确认才上报成功：

```sh
/etc/init.d/S98fwctld stop
fwctld --server http://192.168.10.246:8090 \
  --cache /tmp/fwctld --state-dir /data/fwctld-state --hwid "$(fwctl hwid value)" \
  --device 1c-63-49-22-c4-f7 --auto-install --once --force --console /dev/console
```

设备 ID 应替换为本设备实际 ID。`--force` 必须和 `--once --auto-install` 一起使用；
它重新下载并安装，即使版本和哈希一致，也不复用旧 payload 缓存。
它不会覆盖 pending deployment，也不会跳过包校验、rollback index、A/B 写入和健康确认。
命令失败且未重启时，可执行 `/etc/init.d/S98fwctld start` 恢复后台检查。
普通检查在本地存储读取失败时停止；显式强制恢复允许绕过这个相等性检查，仍需可读的 A/B metadata。
Linux 无法启动时不能运行此命令；U-Boot 内核恢复和根文件系统恢复是不同入口。

## 固件身份与阶段验证

详见 [AM625 分阶段完整性验证](../../bsp-base-external/board/ti/am625/verified-boot.md)。
当前已经实现实际 payload 校验、强制恢复及对外展示的固件内容 ID / 版本 tag。
`firmware_id` 是下面规范化清单的 SHA256（换行固定为 LF），不包含发布编号：

```text
BSPFWCONTENT1
kernel <实际写入字节数> <kernel FIT SHA256>
rootfs <实际写入字节数> <rootfs payload SHA256>
```

相同内容重新发布得到相同 ID；实际内容改变则 ID 改变。这是类似 commit ID 的固件身份，
不是源码 Git commit。`tag` 从 FIT 版本派生为 `v1.0.0` 形式；既有 FIT 数字字段和
`/releases/<发布编号>/` 存储路径保留。`GET /releases/<编号>/version` 返回
`BSPOTAVERSION1 <编号> <tag> <完整内容哈希>`，`/api/state` 同时提供 `firmware_id` 和 `tag`。
fwctld 发现、下载、校验、安装和结果日志优先显示 tag / 哈希前 8 位；发布编号仅用于追溯。

当前版本和实际内容一致时不下载、不安装、不重启。旧版 1.0.10 的判断发生在下载之后，
所以重新发布 1.0.10 后可能先下载，再打印 `up-to-date`；这是无需升级的结果，不是漏执行重启。

initramfs 恢复与普通 fwctld 升级事务不同：按内核绑定的 rootfs payload 哈希取包，当前恢复阶段打印串口日志，不新增独立的服务器恢复事件记录；正常 pending 升级仍由正式系统上报健康结果。

## 异常停发、回退发布与审计

设备上报 `rolled-back`、`install-failed`、`verify-failed` 时记为**升级失败**；
`device-error`、`interrupted`、`reboot-failed` 记为**设备异常**。两类分别统计，
但任一事件都会将关联固件的 kernel/rootfs 内容身份标记为全局异常：

1. 先持久保存设备历史和隔离标记，再确认上报。异常包下载返回 HTTP 410；
   已上传同内容草稿也不能重新发布。`--force` 不绕过服务器的全局隔离。
2. 如果当前发布仍是异常内容，立即撤下，选择该产品/布局在异常发布之前、
   有设备 `success` 健康确认且未被隔离的最近发布，复制为**新发布编号**。
   固件版本及二进制保持原正常版本。没有符合条件的版本时保持停发。
3. 延迟到达的旧版本异常不会替换已经发布的不同内容版本；重复上报不重复回退。
   服务启动时重放历史异常，补做尚未完成的隔离/回退。

`GET /api/state` 提供 `release_stats` 和 `audit_events`。每个发布编号按设备 ID
去重，显示 `success`、`upgrade_failed`、`device_errors`、`in_progress`、`up_to_date`（无需升级）；
同一设备的多次尝试仍完整保存在 `device_history`，统计采用最近事务结果。
“成功”只由重启后的健康确认产生，下载完成、安装完成均不计成功。

设备新上报格式为 `BSPOTASTATE3 DEVICE HWID PUBLICATION VERSION BOOT PHASE TARGET RUNNING REASON`，原因是机器可读标识，
例如 `boot-failed-rolled-back`、`download-sha256-mismatch`、`cache-storage-failed`、
`fwctl-exit-5`。`fwctl update` 失败返回 errno（1–125），I/O、空间不足、设备不存在、
内存不足等由守护进程归为设备异常。旧上报没有原因时显示 `not-reported`，不推测内核 panic
等根因。设备完全离线时服务器收不到事件；本规则从收到有效异常上报开始执行。

持久数据位于服务 `--root` 下：`history/` 保存升级过程，`quarantine/` 保存内容隔离标记，
`audit/` 保存仅新增的事件文件（设备/启动 ID、发布编号、版本、原因、服务端时间、
kernel/rootfs SHA256、回退源及新发布编号）。这些是本地持久审计记录，备份时须连同
`releases/` 一起保留。管理页面提供异常标记、三类数量和审计明细。

无活动发布时，新版 fwctld 正常等待，不再将 `/latest` 的 404/204 当成升级错误。
固件已经传输完毕或写入中的设备无法被服务器远程撤销；新下载请求会被拒绝，启动结果
仍由设备 A/B 回滚和健康检查负责。

验证：`make -C bsp-base-external/device-manager/ota test`，其中
`tests/test-quarantine.py` 覆盖全局停发、自动回退、重复事件、重启恢复、旧原因缺失和分类计数。

## 网口就绪与后台等待

`S40network` 先配置网络，`S98fwctld` 再启动守护进程；配置静态 IP 完成不代表 PHY 已建立链路。启动脚本默认传递 `--interface eth0`，fwctld 每秒检查该网卡是否 UP、RUNNING 且具有非空 IPv4 地址或非 link-local 的 IPv6 地址。条件满足前不向服务器查询发布或下载固件；本地事务仍继续对账并保存健康/回退结果，只在进入等待状态时打印一次提示。网络恢复后立即恢复检查；此机制不阻塞 rcS 和本机健康检查。运行中掉线也在下一次轮询时进入等待。

可在 `/data/fwctld.conf` 设置 `FWCTLD_INTERFACE="eth1"`；设为空可关闭本地网卡检查，适用于自定义路由或多网卡环境。直接执行 fwctld 时按需指定 `--interface`。`--once` 在网卡尚未就绪时立即返回非零，不无限等待。

本地就绪不保证路由、DNS 或 OTA 服务可达；此类 HTTP 错误仍打印原因并按配置的轮询间隔重试。不使用 ping 作为门槛，避免服务端禁用 ICMP 时误阻止升级。

## 固件数据库、详情与统计

服务数据目录中的 `firmware.db`（SQLite，WAL 模式）持久保存完整内容 SHA256、组件 SHA256、版本 tag、上传时间和下载事件。新上传及发布时写入，已有固件首次访问时补录；后续刷新和进程重启直接查询数据库。文件 inode、大小或修改时间发生变化时重新验证并更新目录。网页目录查询不替代发布、下载和恢复时的完整性校验。备份服务数据时需同时保存数据库及 WAL，或停止服务后备份整个数据目录。

点击固件版本可打开独立 URL 的详情页，查看上传时间、tag、完整哈希、下载次数、成功/失败次数、设备异常和原因。相同内容哈希跨发布编号汇总。下载次数以完整发送 `rootfs.itb` 响应计数，重试和恢复请求分别计次；带 `X-OTA-Device` 的设备请求与未识别请求分开统计。历史下载不推算，完整发送不代表设备校验或安装成功。旧记录上传时间由草稿 manifest 时间补录，缺少草稿的历史发布使用本地快照时间。

只有 `latest` 指向且未隔离的版本显示“已发布”，其余显示“未发布”；固件是否有效单独由异常隔离状态表示。再次发布一个非当前的有效草稿会生成新发布编号并切换当前版本；重复发布当前草稿保持幂等。历史发布和升级审计不删除。旧版本精确恢复地址按原策略保留。

如果使用 workspace 的主机依赖，可先执行 `make -C buildroot O="$PWD/output/am625" host-sqlite`，再为服务编译指定 `CPPFLAGS=-I.../output/am625/host/include` 和 `LDFLAGS="-L.../output/am625/host/lib -Wl,-rpath,.../output/am625/host/lib"`。SQLite 仅用于主机端服务，不加入设备固件。

管理页面采用顶层“上传与发布 / 升级历史 / 发布记录 / 异常审计”标签页；第一个标签页上方为整页宽度的紧凑上传横条，下方为发布固件列表；同产品、布局、版本号和完整固件 ID 的记录合并为一行，当前发布快照优先显示；后面分别查看各类记录。详情页提供“下载固件包”，导出包含三个 ITB 的可重新上传 tar.gz；主机需要 `tar`、`gzip`。手动导出先核对包内文件完整性，不计设备 OTA 下载次数。已隔离但文件完整的包可手工导出用于分析，设备自动下载与发布禁令不变。

启动确认阶段仅由 fwctl 打印 `current deployment marked good`。fwctld 仍向服务端保存 `boot-testing` 和 `success`，但不重复打印正常就绪、健康确认及无需升级消息；网络等待、下载、安装与异常仍可见。


### 标签页筛选与设备网络审计

- 标签页使用 `#manage`、`#history`、`#releases`、`#audit` 定位；刷新页面及浏览器前进/后退保留当前标签。每页都有刷新按钮，筛选值在当前浏览器会话中保留。
- 发布固件和发布记录列表列为产品、固件版本、固件 ID、大小、上传时间、操作、详情；上传时间使用数据库保存的原始上传时间，按浏览器本地时间显示，缺失时显示“未知”。下载入口仅在详情页。ID 显示前 8 位，悬停可查看完整 SHA256；搜索匹配完整 ID、版本、产品、状态等属性，空格分隔的条件须同时满足。布局及统计在详情页查看。
- 当前正在分发的固件显示灰色禁用按钮“当前发布版本”，其他可发布固件显示“发布到设备”。无效固件显示红色禁用按钮“无效固件，禁止发布”；服务端隔离检查仍阻止直接 API 发布。设备正常日志只展示版本与短 ID；发布序号仍用于服务端事务和审计关联。
- `fwctld` 每次 OTA 检查前读取所选网络接口的 IP/MAC 和系统 hostname，并随请求发送 `X-OTA-IP`、`X-OTA-MAC`、`X-OTA-Hostname`、`X-OTA-Boot`、`X-OTA-Device`。MAC 来自接口，不从设备 ID 推测；缺失信息标为 `unknown`。
- SQLite `device_requests` 保存版本请求及升级阶段的网络快照，包括设备 ID、发布序号、升级来源启动 ID、实际请求启动 ID、设备自报 IP/MAC/hostname、连接来源 IP、首次/最后请求时间及次数。相同上下文累计次数；网络信息改变时新建记录，保留旧值。
- 升级历史“查看过程与设备信息”展示与该事务关联的快照，可用 IP、MAC 或 hostname 筛选。历史未上报的数据明确显示缺失，不回填当前值。即使尚无可发布固件，版本查询也记录在数据库（publication 为 0），不会误计为一次升级。
- IP/MAC/hostname 是设备自报信息；连接来源 IP 由服务器 socket 获取，忽略客户端伪造的转发地址。字段用于溯源，不替代设备认证。
- 回归：`python3 tests/test-device-context.py` 验证上下文留存、事务隔离和非法字段拒绝；`tests/browser-smoke.mjs` 验证刷新保持、筛选、详情返回、红色禁用按钮及网络审计展示。
