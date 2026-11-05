# OTA 失败诊断与日志证据

固件详情的“失败与设备异常原因”按升级事务展示诊断。设备、发布记录及升级前的 source boot ID 共同标识事务；升级后的每次尝试使用实际请求头 `X-OTA-Boot` 单独关联，不能把 source boot 或回滚后的启动当作失败启动。

## 日志路径

复用 logctld/SOL 已采集、已上传的阶段日志。在安装和 boot-testing 上报时，服务器先为可确认的启动建立候选归档；发生失败后保留相应执行阶段或升级后启动的日志，后到片段可继续补入。成功事务删除候选归档；失败证据不随 SOL 实时窗口的有界实时启动/数据块裁剪删除。

日志和关联存储在 `sol.db` 的 `evidence_groups`、`evidence_boots`、`evidence_chunks`。每个事务最多 32 MiB、16384 块；超限明确设置 truncated，不把截断当成完整上传。失败片段不自动删除，后续全局容量策略仍待实现。

升级结果与证据写入解耦。日志归档失败不会丢弃已记录的升级结果或阻止设备恢复；设备后续查询终态事务时会重试证据关联。已有历史但缺少启动关联的记录显示不可用，不猜测失败启动。

## 仅原因记录

- Linux：在重启后的失败对账中检测到 ramoops panic/Oops 痕迹，报告 `linux-panic-observed` 或 `linux-oops-observed`。痕迹没有可靠 origin boot ID 时，界面明确标注“未确认对应哪次启动”，不将它断言为目标固件的根因。
- data：根据实际状态报告 `data-unavailable-observed`、`data-read-only-observed`、`data-low-space-observed` 或 `data-unreadable-observed`。不可用不等于已证明介质损坏。
- data 观察可在 boot-testing 时提前记录；回滚后的健康系统不会覆盖此前观察。接口的 observed_boot 保存该观察所属启动，不能将它误读为故障根因已经确认。
- 上述记录只展示原因，删除该事务的候选日志关联及副本，不提供查看/下载接口。普通 SOL 的独立保留规则不变。
- 没有到达可确认日志采集阶段的回滚，只保留已知失败/回滚原因，不用恢复后的日志冒充故障日志。

Linux 无法运行时不能主动上报；以上诊断在回滚或恢复后、fwctld 再次运行且网络可达时上报。该功能针对 OTA 事务失败，不包含独立于 OTA 的所有运行期崩溃告警。

## 界面与接口

展开原因记录可查看每个已关联 boot 的日志，并下载串口风格的 `.log`。使用现有 SolConsole 解码器，保留正文格式，不加入传输前缀。

```text
GET /api/sol/incidents/<device>/<publication>/<source_boot>
GET /api/sol/incidents/<device>/<publication>/<source_boot>/<attempt_boot>/<cursor>
```

接口沿用 SOL 查看权限；当前调试部署仍为免凭据模式。返回字节数表示已归档片段，不是设备产生的总量；`complete=false` 明确表示尚无完整性清单。原因记录的日志接口返回 404。

本地仍只有有限启动/轮转保留，长时间离线、采集器启动前失败或 data 不可读时，日志可能缺失。此版本不承诺完整物理 UART 采集、无限离线保留或真实掉电无损。

验证：`python3 ota/tests/test-incident-evidence.py`、`python3 ota/tests/test-diagnostics.py`；另运行 SOL、OTA 历史和网络身份回归。故障夹具只使用临时服务和临时目录，不向生产发布记录注入失败。
