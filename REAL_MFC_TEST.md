# 5-MFC 真机调试步骤

本流程仅适用于 Raspberry Pi 5 通过一个 USB→RS485 接口连接多台北京七星华创 CS 系列 MFC。软件实现的是厂家自定义二进制协议 V2.3，不是 Modbus RTU。

> 安全边界：本产品始终只做只读通信，不发送 setpoint、阀命令、调零、Purge、EEPROM 或地址/波特率修改命令。调试应由熟悉真实气路和独立安全联锁的人员执行。

## 1. 连接 USB 与总线

1. 断开不必要的工艺气源，确认独立截止和联锁可用。
2. 核对 RS485 A/B 极性、共地、终端电阻和 MFC 供电；不要由 Raspberry Pi USB 为 MFC 供电。
3. 将唯一 USB→RS485 转换器插入 Raspberry Pi。

## 2. 检查 Linux 串口

```bash
ls /dev/ttyUSB* 2>/dev/null
ls /dev/ttyACM* 2>/dev/null
ls -l /dev/serial/by-id/ 2>/dev/null
```

软件优先使用 `/dev/serial/by-id/`，并兼容 `ttyUSB*` / `ttyACM*`。若显示“串口没有访问权限”，由系统管理员检查当前用户的 `dialout` 组权限；程序不会自行执行 `sudo`。

## 3. 配置候选 MFC

编辑应用数据目录中的 `config.json`（Linux 通常为 `~/.local/share/科研仪器/MiniCastMonitor/config.json`）：

- `device.mode` 设为 `real`；
- `mfc.serialPort` 可为 `auto` 或明确的 `/dev/serial/by-id/...`；
- `mfc.preferredBaud` 默认 `19200`；
- `mfc.devices[]` 只填写现场需检查的合法地址（`32`～`95`，即 `0x20`～`0x5F`）。

参考配置里的 32～36 只是合法候选地址，`addressConfirmed` 默认为 `false`，不表示已经确认了真机地址或气路映射。

## 4. 启动程序

```bash
cd /path/to/MiniCastMonitor
./scripts/build.sh
ctest --test-dir build --output-on-failure
./build/minicast-monitor
```

设置页应显示 CS200 真机、串口路径、19200 baud、配置数量、在线数量和逐台通信统计。

## 5. 扫描/确认 MFC 地址

1. 点击“设置 → 设备设置 → 重新扫描”。
2. 扫描仅对 `mfc.devices[]` 中的候选地址发送 `Read Flow` (`68 01 B9`) 只读命令。
3. 记录能返回 ACK、合法长度和正确 checksum 的地址。
4. 扫描结果不会自动覆盖配置。由维护人员确认后再保存。

## 6. 观察 5 台 MFC 在线情况

设备诊断列表应逐台显示：名称、十进制地址、在线/超时/异常/离线、响应时间、成功数和失败数。一台超时时，其余地址应继续轮询。

## 7. 只读流量测试

可以点击“开始监测”：该按钮只启动只读采集，不会下发运行点。使用连接/扫描阶段及监测阶段的只读响应和通信日志确认：

```bash
tail -f ~/.local/share/科研仪器/MiniCastMonitor/logs/mfc-communication.log
```

典型地址 32 的 Read Flow 请求为 `20 02 80 03 68 01 B9 00 C7`；设备应先返回 `06` ACK，随后返回地址字节为 `00` 的数据帧。

默认日志只显示结构化结果。确需核对原始字节时，先退出程序，再临时使用
`MINICAST_MFC_RAW_LOG=1 ./build/minicast-monitor` 启动；完成诊断后恢复默认模式，避免持续写入 SD 卡。

## 8. 确认单位和量程

对每一台 MFC 分别确认：

- 铭牌/标定证书上的工程单位；
- 现场维护记录中的工程满量程；
- 可允许的目标流量范围（历史配置键仍为 `minSetpoint` / `maxSetpoint`）；
- 必要的绝对/相对容差。

确认后写入对应 `mfc.devices[]`。协议帧本身不携带 UI 所需的 SCCM/SLM 显示单位，所以软件不会为所有 MFC 猜同一单位；生产监测只发 `READ_FLOW`，不读取额外的设备元数据寄存器。

## 9. 逐台确认地址与真实气路用途

用现场标签、电缆跟踪和经批准的安全方法建立映射。确认后仅修改对应设备的 `displayName` / `gasType` / `function` 和 `addressConfirmed=true`；不要改变运行点的稳定地址键。

## 10. 运行点与目标流量

运行点中的数值是用户定义的目标流量/参考流量，仅用于和 `READ_FLOW` 返回的实际流量比较。选择运行点不会向串口发送任何命令；目标流量只能在“运行点”编辑页修改，监测页不可编辑。

点击“停止监测”只停止数据采集和记录。实际设备停机气路策略需另行确认。

## USB 拔插回归

1. 监测期间拔掉 USB→RS485。
2. 确认 GUI 不退出、不冻结；各 MFC 独立记录 timeout/error，总线无在线设备后显示“通讯中断”。
3. 重新插入后点击“重新扫描”。
4. 恢复后只回到空闲连接状态；不会自动重发非零运行点。

## CS200 通信实验模式

临时真机通信诊断入口位于“设置 → 维护 / 通信诊断”。开始前必须停止常规监测。

- “设备”可选择全部地址或仅 32/33/34/35/36 中的一台；单设备模式只会重复发送所选地址的 `READ_FLOW`（Service `0x80`、Class `0x68`、Instance `0x01`、Attribute `0xB9`），不会扫描或穿插其他读取。
- 本轮隔离实验的间隔固定为 200 ms。依次手工执行地址 32～36 各 5 分钟，每轮结束并保存报告后再切换；随后选择“全部设备”执行 10 分钟 control test。任何时刻只有一轮实验运行。
- 请求间隔是上一事务 `FINISH` 到下一事务实际 `TX` 的间隔，不经过 `sampleInterval / deviceCount`；日志和报告记录实测 `previousFinishToNextTxMs`。
- 单次超时、校验错误和协议字段错误只计入当前地址并继续下一地址，不扫描、不切换波特率、不重连。只有设备节点从 `/dev` 消失才进入串口断开状态。
- 统计明确区分 request 和实际 TX attempt：`requestFinalFailure` 是有限 retry 后仍失败，`attemptChecksumError` 等是内部 attempt 证据，`retrySuccess` 是重试后恢复成功。
- 正常事务不持续写 RAW；任一 attempt 出现协议错误时，报告保存 TX、全部 RX、错误帧、错误前 512-byte ring buffer、错误后数据、parser/LEN/payload、期望与实收字段、checksum、ACK、RX 批次和完整时序。checksum/字段错误的数据不会更新流量。
- 新 TX 前会短轮询并检查 unread RX 与 partial parser state；只要不干净便禁止 TX，执行 `BUS_RECOVERY → RX_DRAIN → quiet window → VERIFY_RX_CLEAN`。诊断模式仍记录 `STALE_RX_AFTER_FINISH`、原始残留与 recovery 事件，但不会为取证而让残留穿透到下一笔 TX。
- recovery quiet window 为 20 ms（19200/8N1 下约三倍于完整 READ_FLOW 响应的线缆时间），最大 recovery 时限为 250 ms；持续垃圾或超过 128-byte 安全 backlog 会升级 hard recovery / `SERIAL_DEGRADED`，不会继续正常轮询。
- 报告同时记录 QSerialPort 运行时的 baud/data bits/parity/stop bits/flow control，并在数据目录的 `exports` 子目录生成带模式、地址、200 ms、时长和时间戳的同名 JSON + Markdown。
