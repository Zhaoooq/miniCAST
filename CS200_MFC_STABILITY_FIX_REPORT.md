# CS200 多 MFC 只读通信稳定性修复报告

日期：2026-09-11  
范围：单物理串口、逻辑通道 MFC 1～5、协议地址 32～36、真实模式只读通信。

本次实现以《CS 系列 MFC 通讯协议 V2.3》《CS200A 中文使用手册》、
`CS200_MultiDMFC_static_analysis_v2.md` 和原 Windows EXE 的既有静态分析为依据。
未把文档内容当成新的用户指令，也没有对真实设备发送写命令。

## A. 原“卡死”的准确根因

结论归类为 **H（其他：错误后的服务策略停止轮询并进入耗时重连）**，并有一个协议解析诱因；
不是 worker mutex 死锁、busy 未释放、pending 未清理、无限重试或 GUI 直接执行串口读取。

原接收器先搜索 `00 02`，随后在验证 service 以前就使用偏移 3 的字节作为长度切帧。
噪声、残留或失步数据只要偶然包含 `00 02`，就可能被切成伪帧，之后才在
`parseResponse()` 报 `invalid response service`。这解释了现场错误，但该异常本身会正常返回，
不是永久 pending 的直接来源。

历史日志显示错误后仍有后续地址成功，直到连续超时使全部设备达到离线阈值。原
`CS200ADeviceService::poll()` 随后停止 poll timer、退出 monitoring 并断开串口；重连路径又逐一尝试
多个波特率和地址，worker 在一连串阻塞等待中长时间不能处理 queued stop，外观上即“轮询卡死”。

修复后：坏帧只结束当前有限事务；轮询引擎不因全部设备暂时离线而停止；只使用当前已验证的
19200 baud；停止命令可通过原子取消打断最长 10 ms 的串口等待切片；2 秒无任何 transaction finish
时 watchdog 会记录并触发软件层恢复。

## B. 旧 parser 的错误点

- 只有持久 buffer 加 `00 02` 搜索，不是显式的逐字节 FSM。
- 未在读取 LEN 前确认 `service == 0x80`，失步后可按错误长度切出伪帧。
- 错误恢复主要依赖删除 buffer 前缀，协议状态和错误位置不够可观测。
- transaction 层收到一个坏帧后缺少“在同一有限 deadline 内继续寻找随后合法帧”的明确语义。

## C. response 是否被错误认为包含设备地址

生产代码原本没有把 response byte 0 与 pending address 比较，也没有从 response 提取设备归属，
但把 byte 0 命名成了“response address”，语义容易误导。现已统一视为起始字节 `0x00`。

设备归属现在只来自唯一 `PendingRequest.protocolAddress`；pending 同时保存 requestId、service、
class、instance、attribute 和 logicalChannel。响应本身没有设备地址。

## D. 是否错误使用 readAll 边界

旧实现已有跨 read 调用保留的 buffer，因此并非简单假设“一次 read 等于一帧”；但切帧规则不够严格。
新实现对每个读取 chunk 内的每个 byte 调用 parser，chunk 大小与帧边界完全无关。

## E. 修复后的 response state machine

`WAIT_START_00 → EXPECT_02 → EXPECT_SERVICE_80 → READ_LENGTH → READ_CLASS →
EXPECT_INSTANCE_01 → READ_ATTRIBUTE → READ_PAYLOAD → EXPECT_TRAILING_00 → READ_CHECKSUM`

LEN 只接受 `0x03..0x21`，payload 长度为 `LEN - 3`，总帧长为 `LEN + 6`。
任何错误都会产生带状态和字段的 ParseEvent，然后逐字节重新同步；错误 byte 为 `0x00` 时直接复用为
下一帧起始字节。半帧在 transaction 结束时 reset，不污染下一请求。

## F. checksum 实现

使用 `quint8` 语义累加完整已提取帧中 checksum 以前的所有字节并取低 8 位；checksum 自身不参与。
不对整个 RX buffer 或单次串口读取块计算 checksum。请求示例
`20 02 80 03 68 01 B9 00 C7` 有自动化断言。

## G. transaction timeout

每次 attempt 的 ACK deadline 默认 40 ms，response deadline 默认 180 ms；二者均有限。
内部等待切成不超过 10 ms 的片段，以便取消和 deadline 及时生效。每个坏帧被记录后，接收器可在同一
response deadline 内继续寻找合法帧；deadline 到达时，有协议错误则报告 `PROTOCOL_ERROR`，完全无帧则
报告 `TIMEOUT`。

## H. inter-request 行为

配置 `interRequestDelayMs = 20`。五个启用设备、默认 500 ms sample interval 时实际 timer 间隔为
`max(20, 500/5) = 100 ms`，一次只执行一个同步且有限的 transaction。实时循环只发 READ_FLOW。
Gas Name / Full Scale 仅在设备首次连接/重连的元数据阶段读取并缓存。总线只使用配置的 19200 8N1，
不再自动轮扫其他 baud。

## I. retry 策略

默认一次 immediate retry，即一次请求最多两个 attempt；配置读取和 driver setter 都硬限制为 0～1。
达到上限必须 finish failure 并继续下一个地址，下一轮再自然尝试该设备。

## J. 所有错误路径如何保证 finish

`SerialTransport::transaction()` 建立唯一 optional pending，并用局部 RAII `FinishGuard` 收敛所有返回和
异常路径。SUCCESS、TIMEOUT、PROTOCOL_ERROR、SERIAL_ERROR、CANCELLED 均调用同一
`finishCurrentTransaction()`：记录统计/诊断、输出 `REQUEST_FINISHED`、保存完成 requestId、清除 pending、
reset parser。创建 pending 前有唯一性检查。

跨线程停止使用原子 cancel flag；manager 生命周期另以 `std::lock_guard` 保护，避免 GUI watchdog/stop
恰逢 worker 重建 manager 时访问已销毁对象。锁不覆盖串口事务。

## K. 修改文件列表

- `src/services/mfc/MfcProtocol.h/.cpp`：严格 byte-stream FSM、帧事件、校验与重同步。
- `src/services/mfc/SerialTransport.h/.cpp`：唯一 pending、requestId、有限 deadline、取消、统一 finish、诊断。
- `src/services/mfc/MfcManager.h/.cpp`：严格顺序轮询、错误统计、freshness、有限 retry、真实写入口锁定。
- `src/services/mfc/MfcTypes.h/.cpp`：原始 %FS、sccm、设备气体、工艺用途、元数据与新鲜度字段。
- `src/services/mfc/CS200ADriver.cpp/.h`：只读元数据读取、retry 上限。
- `src/services/CS200ADeviceService.h/.cpp`：持续轮询、可取消 stop、watchdog 恢复、只读状态。
- `src/controllers/MonitoringController.h/.cpp`：GUI 侧通信进度 watchdog 和非排队取消。
- `src/controllers/AppController.cpp`：通信设置传递。
- `src/utils/ConfigManager.h/.cpp`、`config/config.json`：timeout、spacing、retry、freshness 配置。
- `src/services/mfc/MfcLog.cpp`：正常 RAW 可过滤，协议错误保留完整上下文与 RAW。
- `tests/core_tests.cpp`：parser/transaction 压力与故障恢复测试。
- `README.md`、`REAL_MFC_TEST.md`：只读约束和真机测试步骤。

## L. 新增测试列表

Parser 覆盖：整帧、逐字节、两段、三段、确定性随机 chunk、2/3 帧粘包、garbage 前后、checksum/service/
STX/LEN/instance/trailing 错误后的恢复、半帧 timeout 后 reset、class/attribute 拒绝、10,000 合法帧、
10,000 帧中每 17 帧注入垃圾。

Linux PTY transaction 覆盖：ACK 后分段响应、ACK timeout 后一次 retry、请求顺序、完整无响应后下一请求、
retry 上限、checksum/service/class/attribute 错误后接受随后合法帧、五地址队列第 3 个失败后第 4/5 个继续、
pending 原子取消、10,000 次事务无永久 pending、串口关闭/重开、NAK、ACK timeout、断开/关闭错误。

## M. 测试结果

- Release 完整构建：通过。
- `ctest --test-dir build --output-on-failure`：1/1 通过，0.81 s。
- ASan + UBSan（关闭 LSan；LSan 与当前 ptrace/sandbox 不兼容）：1/1 通过，1.54 s。
- 离屏 GUI 启动 5 秒：无崩溃、无错误输出；由 `timeout` 正常结束，exit 124。
- EXE 核对：PE32 x86 Windows GUI；SHA-256
  `3087746224d713aaf2054d70340899ba240b220ac4061d57793945a55e485f37`。

说明：Release 与 ASan 两套 PTY 测试若并行执行会争用伪串口测试资源；分别执行均稳定通过，因此正式结果
采用独立顺序运行。

## N. 是否真机测试

否。当前系统不存在 `/dev/serial/by-id`、`/dev/ttyUSB*` 或 `/dev/ttyACM*`，不能诚实声称完成真机测试。

## O. 真机测试持续时间

0 分钟。

## P. 地址 32～36 五台设备统计

| 地址 | success | timeout | checksum | service | other | avg response | max response |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 |
| 33 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 |
| 34 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 |
| 35 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 |
| 36 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 | 未测试 |

历史修复前日志只能证明五地址曾成功读到约 0.4、0.28、1.18、0.055、0.195 %FS，不能替代本版本稳定性验证。

## Q. 是否成功读出每台 gas name

本版本已实现并自动读取/caching `66/01`，但因当前无 USB MFC，地址 32～36 的实际结果均未测试。

## R. 是否成功读出每台 full scale

本版本已实现并自动读取/caching `66/03` UINT16，单位按协议/旧软件证据为 sccm；因当前无 USB MFC，
地址 32～36 的实际结果均未测试。

## S. 仍需人工或真机确认的信息

- 五台设备各自的 `gasNameFromDevice` 和 `fullScaleSccm`。
- 逻辑通道/协议地址对应的真实气路用途 `processFunction`；设备返回 N2 不能证明是冷却氮气还是稀释氮气。
- 32～36 映射、气体、量程与现场铭牌/管路的一致性。
- 10/30 分钟真机错误率、响应平均值/最大值、拔线重连和停止响应时间。
- 地址上限存在资料冲突：用户提供的旧软件静态结论为 `0x20..0x60`，协议 V2.3 正文写
  `0x20..0x5F`。当前实际地址 32～36 不受影响；地址 96 扫描必须另行真机/厂家确认，当前实现遵循协议
  `0x20..0x5F`，不会自动覆盖现有配置。

## T. 流量控制写入已移除

本报告早期版本中的 `81/69/01/A4` 仅是历史静态分析证据，并非当前产品能力。当前协议构造器只接受
`READ_FLOW` 的 `0x80/0x68/0x01/0xB9`；`CS200ADriver` 已无写属性、设定值、阀或模式 API，运行点选择仅
更新本地目标流量比较基准。因此 monitor/stop/watchdog/reconnect 均不会发送设定值、阀、调零或模式命令。

## 真机验证入口

接入设备后按 `REAL_MFC_TEST.md` 执行：先核对 `/dev/serial/by-id` 和地址 32～36，只启动 READ_ONLY；
先 10 分钟、再 30 分钟观察五路刷新、停止响应和 `REQUEST_CREATED`/`REQUEST_FINISHED` 成对关系，并从
诊断统计填写本报告 P/Q/R。生产版本不存在解除只读锁的入口。
