# 高浓度碳烟发生器监测系统

高浓度碳烟发生器监测系统是面向多路质量流量控制器的 Linux 原生监测与受限数字控制软件。项目使用 C++20、Qt 6、Qt Quick/QML 和 CMake，第一目标平台为 Raspberry Pi 5（ARM64、Raspberry Pi OS 64-bit Bookworm），第一设计分辨率为 5 英寸横屏 800×480。

软件固定通过唯一 USB→RS485 总线与 N 台 Sevenstar CS200-A 真机通讯，当前参考配置为 5 台；每台通过厂家地址 `0x20`～`0x5F` 区分。

> 安全边界：软件默认以 Monitoring 启动，不会自动控制。只有用户明确选择 Operating Point 并点击“开始控制”后，才允许受限数字控制；它不是自动点火、硬件急停或安全联锁系统。“停止监测”只停止采集和记录。

生产写白名单仅允许：`69 / 01 / 03` Current Control Mode（仅 CM=1 Digital）、`69 / 01 / 05` Hold / Follow（仅 0/1）和 `69 / 01 / A4` Digital Setpoint。Default CM、EEPROM、Valve Command/Mode、Zero、Gas/Gas Code、Full Scale、Conversion Factor、MAC Address、Baud Rate、Reset 及任何永久设备配置写入均被禁止。

## 界面与操作

程序只有四个一级页面：监测、运行点、报警、设置，使用底部大按钮切换。

- 监测：逐路显示气体名称、目标流量、实际流量、偏差、偏差率以及“正常 / 偏高 / 偏低 / 通信异常”状态。点击某一路的目标流量可直接修改已选择的客户运行点；点击卡片其余区域可查看该 MFC 的详情。
- 报警：按提示、警告、严重过滤，异常恢复后保留记录。支持导出和带确认的清空。
- 设置：分为系统设置、运行点管理、设备设置、显示设置、数据管理和关于软件。可调整采样周期和报警阈值、选择数据目录、重新扫描 MFC，并在普通窗口、无边框窗口和全屏显示之间即时切换。
- 运行点：从监测页的“切换”进入。系统预设只读，可选择或复制；客户运行点可仅靠触摸保存、编辑、删除、选择覆盖和确认选择，名称自动采用“客户运行点 1～6”，不需要键盘。

800×480 下所有主页面、底部导航和关键操作均可直接显示，无水平滚动。关键按钮按触摸操作设计；更大分辨率会自动扩展内容区域。

## 项目结构

```text
MiniCastMonitor/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp
│   ├── models/          气体、运行点、报警模型
│   ├── services/        设备、运行点、报警、日志服务
│   ├── controllers/     QML 控制器与后台采集
│   ├── utils/           配置、偏差计算、CSV 导出
├── qml/
│   ├── Main.qml
│   ├── pages/           监测、运行点、报警、设置、模拟测试
│   ├── components/      顶栏、底部导航、表格和统一 HMI 控件
│   └── theme/           浅色工业主题
├── config/              参考配置和用户运行点示例
├── data/                开发期目录占位
├── scripts/             依赖、构建、自启动脚本
└── deployment/          Linux desktop 文件
```


## Raspberry Pi 5 安装依赖

```bash
cd MiniCastMonitor
chmod +x scripts/*.sh
./scripts/install_dependencies.sh
```

脚本安装编译工具、Qt 6 Base/Declarative/SerialPort、Qt Quick 运行模块、libusb 和 XKB 开发包。真实 CS200-A 通讯使用 Qt SerialPort。若 Debian 仓库的软件包名称不匹配，脚本会明确失败并提示检查。

## 编译和测试

```bash
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

也可执行 `./scripts/build.sh`。可执行文件为 `build/minicast-monitor`。

## 运行与全屏

```bash
./build/minicast-monitor
```

开发参考配置默认使用普通 800×480 窗口。显示设置可在普通窗口、无边框窗口和全屏显示之间即时切换，选择会自动保存并在下次启动时恢复。

程序退出时，如果正在监测，会要求确认。确认后关闭日志、串口和后台线程；不会改变 MFC 状态。

## Operating Point / 运行点

模型包含 ID、名称、类型、`mfcTargetFlows` 地址键值表、只读标记、说明、创建时间和更新时间。例如 `{"32": 1.2, "33": 0.4}` 始终按设备地址作为本地比较基准，不依赖 UI 排序。为历史兼容，文件中仍会保留同值的 `mfcSetpoints` 键；它可在用户明确点击“开始控制”后作为受限 Digital Setpoint 的来源。

- 系统模板（`FactoryDefault`）：只提供当前配置的地址结构，不提供未经确认的气路参数。
- 客户运行点（`Customer`）：保存在 `operating_points.json`，仅保存各路目标流量。
- “保存为客户运行点”复制当前已选择的目标流量；“选择”本身只切换监测比较基准，不访问设备控制路径。必须再由用户点击“开始控制”，并通过全部设备身份/气体/量程 Preflight 后，才会进入受限控制。6 个槽位已满时，用户必须选择目标并再次确认，不会自动覆盖。
- 选择客户运行点后，也可以在监测页点击对应通道的目标流量直接编辑。系统会按该 MFC 在 `config.json` 中配置的最小/最大设定值校验输入，先原子保存运行点；未控制时仅更新本地比较基准，不会写入设备。
- 控制进行中修改目标流量时，软件不会单独写入一个通道，而是将完整运行点按 `Digital → Hold → Load → Verify → Follow` 的顺序重新下发并核验。系统预设、未选择运行点、停止控制期间或正在下发上一次更新时，均禁止从监测页修改目标流量。

持久化使用 `QSaveFile` 原子替换，设备或系统断电时比直接截断写入更安全。预设参数只是设备手册演示数据，不代表适合任何具体实验。

## 报警与偏差

偏差为 `实际流量 - 目标流量`，偏差率为 `(实际流量 - 目标流量) / 目标流量 × 100%`。目标流量为零时不做除法，实际值超过 `zeroFlowTolerance` 会标记为“非预期流量”。默认阈值从配置读取：绝对偏差不超过 5% 为正常，5%～10% 为警告，超过 10% 为严重。

持续异常只产生一条活动报警；恢复后写入恢复时间，不删除历史。测试报警、通讯事件和流量报警都可在报警页查看或导出。

## 数据文件位置

程序使用 `QStandardPaths::AppDataLocation`，不写死 `/home/pi`。Linux 上通常位于：

```text
~/.local/share/科研仪器/MiniCastMonitor/
├── config.json
├── operating_points.json
├── logs/
│   ├── application.log
│   ├── YYYY-MM-DD_flow.csv
│   └── YYYY-MM-DD_alarm.csv
└── exports/
    ├── 报警_YYYYMMDD_HHMMSS.csv
    └── 趋势_YYYYMMDD_HHMMSS.csv
```

CSV 使用 UTF-8 BOM，便于 Windows Excel 正确识别中文。设置页会显示当前设备上的实际数据目录。

## 开机自动启动

```bash
./scripts/install_autostart.sh
```

若可执行文件不在默认 `build/` 中，可把绝对路径作为第一个参数。脚本使用当前用户的 XDG 配置目录，不假设用户名为 `pi`。取消自启动：

```bash
./scripts/remove_autostart.sh
```

## Sevenstar CS200-A 真实 MFC

真实 MFC 接入严格使用《CS 系列 MFC 通讯协议 V2.3》，不是 Modbus RTU。`DeviceDiscovery` 优先枚举 `/dev/serial/by-id/`，并兼容 `/dev/ttyUSB*` 和 `/dev/ttyACM*`；唯一 `SerialTransport` 负责 19200/8N1、ACK/NAK、逐字节响应状态机、超时、有限重试和唯一 pending transaction；`MfcManager` 按配置顺序轮询 N 个地址。Monitoring 持续只发送 `READ_FLOW`；仅在上述显式控制操作通过 Preflight 后，才发送白名单内的受限写命令。

MFC 通信日志默认只写结构化 INFO 事件，不把每次轮询的原始帧持续写入 SD 卡；文件达到 1 MiB 后轮转，最多保留 5 个历史文件。临时排障需要原始 TX/RX 时，以 `MINICAST_MFC_RAW_LOG=1` 启动程序即可启用 DEBUG 帧日志。

每台的显示名称、用途、单位、工程满量程和目标流量允许范围都位于 `config.json` 的 `mfc.devices[]`。工程换算参数由现场确认后维护；生产监测不会读取额外的设备元数据寄存器。

旧版 `fuelGas/mixingGas/...` 运行点会把原始数值保存在 `legacyUnmappedValues`，同时标记 `requiresAddressMapping=true`；软件不会猜测它们对应哪个 MFC，也不允许作为比较基准启用。

完整接线、Read Flow、Digital Setpoint 和 USB 拔插验证见 [REAL_MFC_TEST.md](REAL_MFC_TEST.md)。通讯日志位于应用数据目录的 `logs/mfc-communication.log`，单文件 1 MiB、保留 5 个轮转文件。

### DeviceInfo 与通讯恢复

“验证设备信息”和控制前的身份校验会在同一条串口总线上顺序执行，并在扫描期间暂时挂起常规 `READ_FLOW` 轮询。扫描完成后，只有下一次成功的 `READ_FLOW` 才会重新启动通讯看门狗，因此完整的多设备读取不会被误判为工作线程卡死；真正的轮询停滞仍会触发恢复。

每个 DeviceInfo 属性固定最多尝试两次，日志会记录重试恢复、取消来源、原始帧和解析结果。RS485 Address 按单字节解析；Target 与 Calibration 的满量程允许不同，二者差异本身不是报警。一次完整、匹配的身份校验结果仅在连接、配置和设备地址均未改变的情况下短暂用于紧接着的“开始控制”预检。

### 只读现场诊断工具

构建后另提供 `build/minicast-device-diag`，它使用与主程序相同的协议栈，但不启动 `READ_FLOW` 调度，也不会发送写命令。适合将单台设备（默认地址 36）与现场其余设备隔离比对：

```bash
# 自动寻找串口，读取地址 36 的 DeviceInfo
./build/minicast-device-diag

# 指定串口与地址
./build/minicast-device-diag --port /dev/ttyUSB0 --address 36

# 依次只读比较参考地址 32 至 36
./build/minicast-device-diag --port /dev/ttyUSB0 --all-configured
```

工具逐项输出目标/标定气体与量程、型号、序列号、波特率和 RS485 地址，以及 TX、ACK、RX、校验、重试次数和解析结论。请在确认串口归属且没有其他程序占用总线时运行；现场诊断输出应保留在本机，不应提交到仓库。

## 当前限制

- miniCAST 6204C / PDM-U 主机本身的协议仍未配置；本次真实接入范围是独立的 Sevenstar CS200-A MFC。
- 报警事件按日写入 CSV，但进程重启后报警页面不会重新载入历史文件。
- 修改采样周期和报警阈值后，新的采样线程参数在下次启动时生效。
