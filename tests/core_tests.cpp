#include "controllers/MonitoringController.h"
#include "services/AlarmService.h"
#include "services/OperatingPointService.h"
#include "services/DataLoggingService.h"
#include "utils/FlowDeviationCalculator.h"
#include "services/mfc/MfcProtocol.h"
#include "services/mfc/MfcManager.h"
#include "services/mfc/Cs200CommExperiment.h"
#include "services/mfc/SerialTransport.h"
#include "utils/ConfigManager.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QTemporaryDir>
#include <QFile>
#include <QDate>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <iostream>
#include <array>
#include <chrono>
#include <thread>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/select.h>
#include <stdlib.h>
#include <unistd.h>
#endif

static bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << "测试失败: " << message << '\n';
    return condition;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    QTemporaryDir configHome;
    ok &= require(configHome.isValid(), "应能创建配置持久化测试目录");
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    const QString selectedDataRoot = configHome.path() + QStringLiteral("/selected-data");
    {
        ConfigManager config;
        ok &= require(config.load(), "首次启动应能创建默认配置");
        ok &= require(config.mfcDevices().size() == 5, "默认配置应加载 5 台地址化 MFC");
        const auto fixedDevices = config.mfcDevices();
        ok &= require(fixedDevices[0].address == 32 && fixedDevices[0].gasType == QStringLiteral("空气")
                      && fixedDevices[0].fullScale == 1.0 && fixedDevices[0].unit == QStringLiteral("L/min")
                      && fixedDevices[1].address == 33 && fixedDevices[1].gasType == QStringLiteral("丙烷")
                      && fixedDevices[1].fullScale == 50.0 && fixedDevices[1].unit == QStringLiteral("ml/min")
                      && fixedDevices[2].address == 34 && fixedDevices[2].fullScale == 50.0 && fixedDevices[2].unit == QStringLiteral("ml/min")
                      && fixedDevices[3].address == 35 && fixedDevices[3].fullScale == 10.0 && fixedDevices[3].unit == QStringLiteral("L/min")
                      && fixedDevices[4].address == 36 && fixedDevices[4].fullScale == 4.0 && fixedDevices[4].unit == QStringLiteral("L/min"),
                      "miniCAST 固定五路 MFC 配置必须保留各自工程单位和满量程");
        ok &= require(fixedDevices[0].expectedGasCode == 8 && fixedDevices[1].expectedGasCode == 89
                      && fixedDevices[2].expectedGasCode == 13 && fixedDevices[3].expectedGasCode == 8
                      && fixedDevices[4].expectedGasCode == 13
                      && fixedDevices[0].expectedDeviceFullScaleSccm == 1000.0
                      && fixedDevices[1].expectedDeviceFullScaleSccm == 50.0
                      && fixedDevices[2].expectedDeviceFullScaleSccm == 50.0
                      && fixedDevices[3].expectedDeviceFullScaleSccm == 10000.0
                      && fixedDevices[4].expectedDeviceFullScaleSccm == 4000.0,
                      "集中式现场配置必须保存 Gas Code 与设备侧 SCCM 量程");
        ok &= require(config.setDataRoot(selectedDataRoot), "可写目录应能设为数据保存路径");
        ok &= require(config.setMfcAddressConfirmed(32, true), "操作员地址确认应能持久化");
        ok &= require(!config.setMfcAddressConfirmed(1, true), "不存在的协议地址不得被伪确认");
        ok &= require(config.dataRoot() == selectedDataRoot
                      && QDir(selectedDataRoot + "/logs").exists()
                      && QDir(selectedDataRoot + "/exports").exists(),
                      "设置数据路径时应创建日志和导出目录");
        // A persisted old mapping is not a compatibility fallback: the next
        // load must replace all five engineering definitions as a set.
        QJsonArray staleDevices;
        staleDevices.append(QJsonObject{{"address", 32}, {"unit", "ml/min"}, {"fullScale", 50.0},
                                        {"expectedDeviceFullScaleSccm", 50.0}});
        config.setValue("mfc", "devices", staleDevices);
        ok &= require(config.save(), "应能写入旧现场配置迁移夹具");
    }
    {
        ConfigManager reloadedConfig;
        ok &= require(reloadedConfig.load() && reloadedConfig.dataRoot() == selectedDataRoot
                      && reloadedConfig.mfcDevices().first().addressConfirmed
                      && reloadedConfig.mfcDevices()[0].fullScale == 1.0
                      && reloadedConfig.mfcDevices()[0].unit == QStringLiteral("L/min")
                      && reloadedConfig.mfcDevices()[2].fullScale == 50.0
                      && reloadedConfig.mfcDevices()[3].fullScale == 10.0
                      && reloadedConfig.mfcDevices()[4].fullScale == 4.0,
                      "用户配置重启后必须保留数据路径并强制替换旧 MFC 映射");
    }
    const auto fixedDevices = ConfigManager().mfcDevices();

    ok &= require(!MonitoringController::shouldArmMonitoringWatchdog(false, false)
                  && !MonitoringController::shouldArmMonitoringWatchdog(true, false)
                  && MonitoringController::shouldArmMonitoringWatchdog(true, true)
                  && !MonitoringController::shouldArmMonitoringWatchdog(true, true, true)
                  && MonitoringController::shouldArmMonitoringWatchdog(true, false, true),
                  "监控 watchdog 只能在 NORMAL_POLLING 且首个 READ_FLOW 成功后 arm");

    const QByteArray checksumInput = QByteArray::fromHex("200280036801B900");
    ok &= require(MfcProtocol::checksum(checksumInput) == 0xC7,
                  "Sevenstar 文档 Read Flow 校验和应为 C7");
    const QByteArray flowRequest = MfcProtocol::makeRequest(0x20, MfcProtocol::ReadService,
                                                            0x68, 0x01, 0xB9);
    ok &= require(flowRequest == QByteArray::fromHex("200280036801B900C7"),
                  "Read Flow 请求必须逐字节匹配协议 V2.3 示例");
    ok &= require(MfcProtocol::makeRequest(0x21, MfcProtocol::ReadService, 0x68, 0x01, 0xB9)
                      == QByteArray::fromHex("210280036801B900C8"),
                  "不同 MFC 地址必须进入请求帧并参与 checksum");
    const QByteArray gasCodeRequest = MfcProtocol::makeRequest(0x20, MfcProtocol::ReadService, 0x66, 0x01, 0x02);
    const QByteArray fullScaleRequest = MfcProtocol::makeRequest(0x20, MfcProtocol::ReadService, 0x66, 0x01, 0x03);
    const QByteArray calibrationGasCodeRequest = MfcProtocol::makeRequest(0x20, MfcProtocol::ReadService, 0x66, 0x01, 0x07);
    const QByteArray calibrationFullScaleRequest = MfcProtocol::makeRequest(0x20, MfcProtocol::ReadService, 0x66, 0x01, 0x08);
    const QByteArray rs485AddressRequest = MfcProtocol::makeRequest(0x20, MfcProtocol::ReadService, 0x03, 0x01, 0x01);
    ok &= require(gasCodeRequest == QByteArray::fromHex("20028003660102000E")
                  && fullScaleRequest == QByteArray::fromHex("20028003660103000F")
                  && calibrationGasCodeRequest == QByteArray::fromHex("200280036601070013")
                  && calibrationFullScaleRequest == QByteArray::fromHex("200280036601080014"),
                  "Target / Calibration Gas Code 与 Full Scale 必须严格使用 0x66/0x01 的只读属性");
    ok &= require(rs485AddressRequest == QByteArray::fromHex("2002800303010100AA"),
                  "RS485 MAC 地址必须使用协议定义的只读 0x03/0x01 请求");
    const auto metadataPacket = [](quint8 attribute, const QByteArray &payload) {
        QByteArray packet = QByteArray::fromHex("000280");
        packet.append(char(3 + payload.size())); packet.append(char(0x66)); packet.append(char(0x01));
        packet.append(char(attribute)); packet.append(payload); packet.append(char(0x00));
        packet.append(char(MfcProtocol::checksum(packet)));
        return packet;
    };
    const auto gasCodeResponse = MfcProtocol::parseResponse(metadataPacket(0x02, QByteArray::fromHex("0800")), 0x66, 0x01, 0x02);
    const auto fullScaleResponse = MfcProtocol::parseResponse(metadataPacket(0x03, QByteArray::fromHex("A00F")), 0x66, 0x01, 0x03);
    const auto calibrationGasCodeResponse = MfcProtocol::parseResponse(metadataPacket(0x07, QByteArray::fromHex("0D00")), 0x66, 0x01, 0x07);
    const auto calibrationFullScaleResponse = MfcProtocol::parseResponse(metadataPacket(0x08, QByteArray::fromHex("1027")), 0x66, 0x01, 0x08);
    const auto emptyGasCodeResponse = MfcProtocol::parseResponse(metadataPacket(0x02, {}), 0x66, 0x01, 0x02);
    const auto rs485AddressResponse = MfcProtocol::parseResponse(
        [] { QByteArray p = QByteArray::fromHex("00028004"); p.append(char(0x03)); p.append(char(0x01));
             p.append(char(0x01)); p.append(char(0x20)); p.append(char(0x00));
             p.append(char(MfcProtocol::checksum(p))); return p; }(), 0x03, 0x01, 0x01);
    ok &= require(MfcProtocol::readUInt16Le(gasCodeResponse.data) == 8
                  && MfcProtocol::readUInt16Le(fullScaleResponse.data) == 4000
                  && MfcProtocol::readUInt16Le(calibrationGasCodeResponse.data) == 13
                  && MfcProtocol::readUInt16Le(calibrationFullScaleResponse.data) == 10000
                  && emptyGasCodeResponse.data.isEmpty(),
                  "Target/Calibration UINT16 metadata 必须小端解析，LEN=3 必须保留为空 payload 而非伪造值");
    ok &= require(MfcProtocol::readUInt8(rs485AddressResponse.data) == 32,
                  "协议 V2.3 的 RS485 MAC 地址响应是一个 UINT8，地址 32 必须可合法解析");
    bool shortAddressRejected = false;
    try { (void)MfcProtocol::readUInt8({}); } catch (const MfcProtocol::Error &) { shortAddressRejected = true; }
    ok &= require(shortAddressRejected, "空的 RS485 地址响应不得用于地址确认");
    MfcDeviceInfo wrongAddressInfo;
    wrongAddressInfo.address = 33;
    wrongAddressInfo.targetGasName = fixedDevices[0].gasType;
    wrongAddressInfo.targetGasCode = fixedDevices[0].expectedGasCode;
    wrongAddressInfo.fullScale = fixedDevices[0].expectedDeviceFullScaleSccm;
    ok &= require(MfcManager::metadataMismatchFields(fixedDevices[0], wrongAddressInfo)
                      == QStringList{QStringLiteral("RS485 Address")},
                  "地址寄存器返回 33 而期望 32 时必须明确报地址不匹配");
    MfcDeviceInfo propaneFormulaInfo;
    propaneFormulaInfo.address = fixedDevices[1].address;
    propaneFormulaInfo.targetGasName = QStringLiteral("C3H8");
    propaneFormulaInfo.targetGasCode = fixedDevices[1].expectedGasCode;
    propaneFormulaInfo.fullScale = fixedDevices[1].expectedDeviceFullScaleSccm;
    ok &= require(MfcManager::metadataMismatchFields(fixedDevices[1], propaneFormulaInfo).isEmpty(),
                  "设备返回 C3H8 时必须与现场配置的丙烷视为同一种气体");
    ok &= require(MfcProtocol::encodeUfrac16(0.0) == 0x4000
                  && MfcProtocol::encodeUfrac16(0.5) == 0x8000
                  && MfcProtocol::encodeUfrac16(1.0) == 0xC000,
                  "Digital Setpoint 的 0/50/100% UFRAC16 必须精确");
    const QMap<int, double> halfScaleTargets{{32, 0.5}, {33, 25.0}, {34, 25.0}, {35, 5.0}, {36, 2.0}};
    for (const auto &device : fixedDevices) {
        const double target = halfScaleTargets.value(device.address);
        const double ratio = target / device.fullScale;
        MfcDeviceState halfScaleState;
        halfScaleState.config = device;
        halfScaleState.reading.communicationOk = true;
        halfScaleState.reading.flowPercent = 50.0;
        halfScaleState.reading.flowValue = target;
        const QVariantMap flowMap = halfScaleState.toVariantMap();
        ok &= require(std::abs(ratio - 0.5) < 1e-12
                      && MfcProtocol::encodeUfrac16(ratio) == 0x8000
                      && std::abs(flowMap.value("currentFlow").toDouble() - target) < 1e-12
                      && std::abs(flowMap.value("actualFlow").toDouble() - target) < 1e-12,
                      "每台 MFC 的 50%FS READ_FLOW 工程量与 Setpoint 必须使用当前固定满量程");
    }
    const QByteArray setpointFrame = MfcProtocol::makeRequest(0x20, MfcProtocol::WriteService,
        0x69, 0x01, 0xA4, MfcProtocol::uint16Le(0x8000));
    ok &= require(setpointFrame == QByteArray::fromHex("200281056901A400800036"),
                  "50%FS Digital Setpoint 必须小端编码并使用算法 checksum");
    const QByteArray stopSetpointFrame = MfcProtocol::makeRequest(0x20, MfcProtocol::WriteService,
        0x69, 0x01, 0xA4, MfcProtocol::uint16Le(0x4000));
    ok &= require(MfcProtocol::uint16Le(0x4000) == QByteArray::fromHex("0040")
                  && stopSetpointFrame.mid(7, 2) == QByteArray::fromHex("0040"),
                  "停止控制必须以小端 00 40 写入 0%FS Digital Setpoint");
    ok &= require(MfcProtocol::isAllowedWrite(0x69, 0x01, 0x03, QByteArray(1, char(1)))
                  && MfcProtocol::isAllowedWrite(0x69, 0x01, 0x05, QByteArray(1, char(0)))
                  && MfcProtocol::isAllowedWrite(0x69, 0x01, 0xA4, MfcProtocol::uint16Le(0x8000)),
                  "生产写白名单只含 Current CM、Hold/Follow、Digital Setpoint");
    bool defaultCmRejected = false, eepromRejected = false, valveRejected = false, malformedSetpointRejected = false;
    try { (void)MfcProtocol::makeRequest(0x20, MfcProtocol::WriteService, 0x69, 0x01, 0x04, QByteArray(1, char(1))); }
    catch (const MfcProtocol::Error &) { defaultCmRejected = true; }
    try { (void)MfcProtocol::makeRequest(0x20, MfcProtocol::WriteService, 0x69, 0x01, 0x06, QByteArray(1, char(1))); }
    catch (const MfcProtocol::Error &) { eepromRejected = true; }
    try { (void)MfcProtocol::makeRequest(0x20, MfcProtocol::WriteService, 0x68, 0x01, 0xA0, QByteArray(1, char(1))); }
    catch (const MfcProtocol::Error &) { valveRejected = true; }
    try { (void)MfcProtocol::makeRequest(0x20, MfcProtocol::WriteService, 0x69, 0x01, 0xA4, QByteArray(1, char(0))); }
    catch (const MfcProtocol::Error &) { malformedSetpointRejected = true; }
    ok &= require(defaultCmRejected && eepromRejected && valveRejected && malformedSetpointRejected,
                  "Default CM、EEPROM、Valve Command 和错误长度写帧必须在构帧前拒绝");
    const QByteArray flowPacket = QByteArray::fromHex("000280056801B93D4F0035");
    const auto flowResponse = MfcProtocol::parseResponse(flowPacket, 0x68, 0x01, 0xB9);
    const quint16 flowRaw = MfcProtocol::readUInt16Le(flowResponse.data);
    const double flowPercent = MfcProtocol::decodeUfrac16(flowRaw) * 100.0;
    ok &= require(flowRaw == 0x4F3D && std::abs(flowPercent - 11.9049) < 0.01,
                  "协议示例 3D 4F 应按小端解析为约 11.9%FS");
    ok &= require(MfcProtocol::decodeUfrac16(0x4000) == 0.0
                  && MfcProtocol::decodeUfrac16(0xC000) == 1.0
                  && MfcProtocol::decodeUfrac16(0x0000) == -0.5
                  && MfcProtocol::decodeUfrac16(0xE000) == 1.25
                  && MfcProtocol::encodeUfrac16(0.0) == 0x4000
                  && MfcProtocol::encodeUfrac16(1.0) == 0xC000,
                  "UFRAC16 端点转换必须精确");
    bool analogCmRejected = false, invalidHoldRejected = false;
    try { (void)MfcProtocol::makeRequest(0x20, MfcProtocol::WriteService, 0x69, 0x01, 0x03, QByteArray(1, char(2))); }
    catch (const MfcProtocol::Error &) { analogCmRejected = true; }
    try { (void)MfcProtocol::makeRequest(0x20, MfcProtocol::WriteService, 0x69, 0x01, 0x05, QByteArray(1, char(2))); }
    catch (const MfcProtocol::Error &) { invalidHoldRejected = true; }
    ok &= require(analogCmRejected && invalidHoldRejected,
                  "写白名单必须拒绝 Analog CM 和非 Hold/Follow 协议值");
    bool checksumRejected = false;
    try {
        QByteArray damaged = flowPacket;
        damaged[damaged.size() - 1] = char(0x34);
        (void)MfcProtocol::parseResponse(damaged, 0x68, 0x01, 0xB9);
    } catch (const MfcProtocol::Error &) { checksumRejected = true; }
    ok &= require(checksumRejected, "错误 checksum 必须抛出协议异常");
    bool lengthRejected = false;
    try {
        QByteArray damaged = flowPacket;
        damaged[3] = char(0x04);
        damaged[damaged.size() - 1] = char(MfcProtocol::checksum(damaged.first(damaged.size() - 1)));
        (void)MfcProtocol::parseResponse(damaged, 0x68, 0x01, 0xB9);
    } catch (const MfcProtocol::Error &) { lengthRejected = true; }
    ok &= require(lengthRejected, "响应 Data Length 与实际帧长不符时必须拒绝");
    bool responseStartRejected = false;
    try {
        QByteArray damaged = flowPacket;
        damaged[0] = char(0x20);
        damaged[damaged.size() - 1] = char(MfcProtocol::checksum(damaged.first(damaged.size() - 1)));
        (void)MfcProtocol::parseResponse(damaged, 0x68, 0x01, 0xB9);
    } catch (const MfcProtocol::Error &) { responseStartRejected = true; }
    ok &= require(responseStartRejected, "响应起始字节不是协议规定的 0x00 时必须拒绝");
    auto countParserFrames = [](MfcProtocol::ResponseStreamParser &parser,
                                const QByteArray &bytes, int *errors = nullptr) {
        int frames = 0;
        for (const auto &event : parser.feed(bytes)) {
            if (event.type == MfcProtocol::ParseEvent::Type::Frame) ++frames;
            else if (errors) ++*errors;
        }
        return frames;
    };
    {
        MfcProtocol::ResponseStreamParser parser;
        ok &= require(countParserFrames(parser, flowPacket) == 1,
                      "完整 READ_FLOW 帧一次 feed 应产生一帧");
    }
    {
        MfcProtocol::ResponseStreamParser parser;
        int frames = 0;
        for (const char byte : flowPacket)
            frames += countParserFrames(parser, QByteArray(1, byte));
        ok &= require(frames == 1, "READ_FLOW 逐字节 feed 应产生一帧");
    }
    for (const QList<int> chunks : {QList<int>{4, 7}, QList<int>{2, 3, 6}}) {
        MfcProtocol::ResponseStreamParser parser;
        int frames = 0;
        int offset = 0;
        for (const int size : chunks) {
            frames += countParserFrames(parser, flowPacket.mid(offset, size));
            offset += size;
        }
        ok &= require(frames == 1, "两段/三段拆包必须正确恢复完整帧");
    }
    {
        MfcProtocol::ResponseStreamParser parser;
        QRandomGenerator random(0xC5200u);
        int frames = 0;
        int offset = 0;
        while (offset < flowPacket.size()) {
            const int size = qMin(flowPacket.size() - offset,
                                  1 + static_cast<int>(random.bounded(5u)));
            frames += countParserFrames(parser, flowPacket.mid(offset, size));
            offset += size;
        }
        ok &= require(frames == 1, "任意随机 chunk 拆包必须正确恢复完整帧");
    }
    for (const int expectedFrames : {2, 3}) {
        MfcProtocol::ResponseStreamParser parser;
        ok &= require(countParserFrames(parser, flowPacket.repeated(expectedFrames)) == expectedFrames,
                      "一次输入两帧/三帧必须全部解析");
    }
    {
        MfcProtocol::ResponseStreamParser parser;
        int errors = 0;
        ok &= require(countParserFrames(parser, QByteArray::fromHex("FF3399") + flowPacket,
                                        &errors) == 1,
                      "垃圾字节之后必须恢复合法帧");
        ok &= require(countParserFrames(parser, flowPacket + QByteArray::fromHex("ABCD")
                                        + flowPacket, &errors) == 2,
                      "frame + garbage + frame 必须解析两帧");
    }
    auto requireRecovery = [&](QByteArray damaged, const char *message) {
        MfcProtocol::ResponseStreamParser parser;
        int errors = 0;
        const int frames = countParserFrames(parser, damaged + flowPacket, &errors);
        ok &= require(errors >= 1 && frames == 1, message);
    };
    {
        QByteArray damaged = flowPacket;
        damaged[damaged.size() - 1] ^= char(0x01);
        requireRecovery(damaged, "checksum bad 后 parser 必须恢复下一合法帧");
    }
    {
        QByteArray damaged = flowPacket;
        damaged[2] = char(0x81);
        requireRecovery(damaged, "invalid service 后 parser 必须恢复下一合法帧");
    }
    requireRecovery(QByteArray::fromHex("0003"),
                    "invalid 0x02 后 parser 必须恢复下一合法帧");
    requireRecovery(QByteArray::fromHex("00028002"),
                    "invalid LEN 后 parser 必须恢复下一合法帧");
    requireRecovery(QByteArray::fromHex("000280056802"),
                    "invalid instance 后 parser 必须恢复下一合法帧");
    {
        QByteArray damaged = flowPacket;
        damaged[damaged.size() - 2] = char(0x7E);
        requireRecovery(damaged, "invalid trailing zero 后 parser 必须恢复下一合法帧");
    }
    {
        MfcProtocol::ResponseStreamParser parser;
        (void)parser.feed(flowPacket.first(5));
        parser.reset(); // transaction timeout boundary
        ok &= require(countParserFrames(parser, flowPacket) == 1,
                      "半帧 timeout 后下一 request 必须恢复");
    }
    {
        MfcProtocol::ResponseStreamParser parser;
        int frames = 0;
        for (int i = 0; i < 10000; ++i)
            frames += countParserFrames(parser, flowPacket);
        ok &= require(frames == 10000
                      && parser.state() == MfcProtocol::ParserState::WaitStart00,
                      "连续 10000 合法帧不得丢帧或残留状态");
    }
    {
        MfcProtocol::ResponseStreamParser parser;
        int frames = 0;
        int errors = 0;
        for (int i = 0; i < 10000; ++i) {
            if (i % 17 == 0)
                frames += countParserFrames(parser, QByteArray::fromHex("FF330002817F"), &errors);
            frames += countParserFrames(parser, flowPacket, &errors);
        }
        ok &= require(frames == 10000 && errors > 0,
                      "持续垃圾注入时 parser 必须始终恢复合法帧");
    }
    bool commandRejected = false;
    try { (void)MfcProtocol::parseResponse(flowPacket, 0x69, 0x01, 0xB9); }
    catch (const MfcProtocol::Error &) { commandRejected = true; }
    ok &= require(commandRejected, "错误命令类必须被拒绝");
    bool attributeRejected = false;
    try { (void)MfcProtocol::parseResponse(flowPacket, 0x68, 0x01, 0xA5); }
    catch (const MfcProtocol::Error &) { attributeRejected = true; }
    ok &= require(attributeRejected, "错误属性必须被拒绝");
    bool addressRejected = false;
    try { (void)MfcProtocol::makeRequest(1, MfcProtocol::ReadService, 0x68, 1, 0xB9); }
    catch (const MfcProtocol::Error &) { addressRejected = true; }
    ok &= require(addressRejected, "地址 1 不在厂家 0x20-0x5F 范围内，必须拒绝");
    ok &= require(MfcProtocol::Nak == 0x15, "NAK 必须按标准单字节 0x15 识别");

    MfcDeviceState readableUnconfirmed;
    readableUnconfirmed.config.logicalChannel = 1;
    readableUnconfirmed.config.address = 0x20;
    readableUnconfirmed.config.displayName = QStringLiteral("MFC 1");
    readableUnconfirmed.communicationOnline = true;
    readableUnconfirmed.addressDetected = true;
    readableUnconfirmed.reading.communicationOk = true;
    readableUnconfirmed.refreshCapabilities();
    const auto readableMap = readableUnconfirmed.toVariantMap();
    ok &= require(readableMap.value("communicationOnline").toBool()
                  && readableMap.value("addressDetected").toBool()
                  && readableMap.value("readOnly").toBool(),
                  "地址已响应但未经配置确认的 MFC 应为可读，而不是通信错误");
    readableUnconfirmed.config.addressConfirmed = true;
    readableUnconfirmed.config.gasType = QStringLiteral("N2");
    readableUnconfirmed.config.function = QStringLiteral("稀释气");
    readableUnconfirmed.config.unit = QStringLiteral("sccm");
    readableUnconfirmed.config.fullScale = 1000.0;
    readableUnconfirmed.refreshCapabilities();
    ok &= require(readableUnconfirmed.engineeringConfigured,
                  "只读监测使用已确认工程配置换算实际流量，不读取额外 MFC 寄存器");
    readableUnconfirmed.refreshCapabilities();
    const auto deviceScaleMap = readableUnconfirmed.toVariantMap();
    ok &= require(deviceScaleMap.value("fullScaleValue").toDouble() == 1000.0
                  && deviceScaleMap.value("fullScaleSource").toString()
                      == QStringLiteral("工程配置（监测不读取量程寄存器）"),
                  "工程量程应仅来自已确认配置，生产监测不得读取额外寄存器");
    GasChannel displayChannel;
    displayChannel.flowUnit = QStringLiteral("SLM");
    displayChannel.fullScaleValue = 5.0;
    displayChannel.currentFlow = 2.013;
    displayChannel.actualFlow = 2.013;
    displayChannel.lastValidActualFlow = 2.013;
    displayChannel.lastValidFlowTimestamp = QDateTime::currentDateTime();
    displayChannel.targetFlow = 2.000;
    displayChannel.percentFullScale = 40.26;
    displayChannel.currentFlowAvailable = true;
    const auto displayMap = displayChannel.toVariantMap();
    ok &= require(displayMap.value("currentFlow").toDouble() == 2.013
                  && displayMap.value("actualFlow").toDouble() == 2.013
                  && displayMap.value("lastValidActualFlow").toDouble() == 2.013
                  && displayMap.value("targetFlow").toDouble() == 2.0
                  && displayMap.value("fullScaleValue").toDouble() == 5.0
                  && displayMap.value("flowUnit").toString() == QStringLiteral("SLM")
                  && displayMap.value("percentFullScale").toDouble() == 40.26,
                  "监测模型必须明确区分实际流量、目标流量、满量程、单位和 %FS");
    displayChannel.actualFlow = 0.0;
    displayChannel.hasValidActualFlow = true;
    displayChannel.actualFlowFresh = true;
    displayChannel.waitingForActualFlow = false;
    const auto zeroFlowMap = displayChannel.toVariantMap();
    ok &= require(zeroFlowMap.value("actualFlow").toDouble() == 0.0
                  && zeroFlowMap.value("hasValidActualFlow").toBool()
                  && zeroFlowMap.value("actualFlowFresh").toBool()
                  && !zeroFlowMap.value("waitingForActualFlow").toBool(),
                  "合法 READ_FLOW=0.0 必须是新鲜、有效的实际流量，而非等待状态");
    displayChannel.monitoringActive = true;
    displayChannel.communicationStateCode = 1;
    displayChannel.online = true;
    displayChannel.targetFlowAvailable = true;
    displayChannel.deviationAvailable = false;
    displayChannel.targetConfirmed = false;
    displayChannel.controlState = QStringLiteral("监测模式");
    ok &= require(displayChannel.toVariantMap().value("comparisonStatusText").toString()
                      == QStringLiteral("尚未应用设定"),
                  "实际流量已合法读取时，缺少偏差计算不得误报“正在获取实际流量”");
    displayChannel.targetConfirmed = true;
    ok &= require(displayChannel.toVariantMap().value("comparisonStatusText").toString()
                      == QStringLiteral("有效设定已同步"),
                  "确认 Active Setpoint 后应显示有效设定已同步");
    QFile monitorColumn(QStringLiteral(SOURCE_DIR "/qml/components/MfcMonitorColumn.qml"));
    ok &= require(monitorColumn.open(QIODevice::ReadOnly), "应能读取 MFC QML 列定义");
    const QString monitorQml = QString::fromUtf8(monitorColumn.readAll());
    ok &= require(monitorQml.contains(QStringLiteral("Number(channel.actualFlow || 0) / fullScale"))
                  && !monitorQml.contains(QStringLiteral("Number(channel.currentFlow || 0) / fullScale")),
                  "Actual 文本与柱状图必须统一绑定 actualFlow，不能再以 currentFlow 计算柱高");
    QFile monitorPage(QStringLiteral(SOURCE_DIR "/qml/pages/MonitorPage.qml"));
    ok &= require(monitorPage.open(QIODevice::ReadOnly), "应能读取监测详情 QML");
    const QString monitorPageQml = QString::fromUtf8(monitorPage.readAll());
    ok &= require(monitorPageQml.contains(QStringLiteral("selectedChannel.actualFlowFresh"))
                  && !monitorPageQml.contains(QStringLiteral("selectedChannel.currentFlowAvailable")),
                  "Actual 详情的有效性也必须来自 actualFlow freshness，而不是 currentFlow 兼容字段");
    MfcDeviceConfig metadataConfig;
    metadataConfig.address = 32;
    metadataConfig.gasType = QStringLiteral("空气");
    metadataConfig.expectedGasCode = 8;
    metadataConfig.expectedDeviceFullScaleSccm = 1000.0;
    MfcDeviceInfo metadataInfo;
    metadataInfo.address = 32;
    metadataInfo.targetGasName = QStringLiteral("Air");
    metadataInfo.targetGasCode = 8;
    metadataInfo.fullScale = 1000.0;
    ok &= require(MfcManager::metadataMismatchFields(metadataConfig, metadataInfo).isEmpty(),
                  "MFC1 Gas Code UINT16=8 必须与现场配置 8 匹配，不受 008 显示格式影响");
    metadataConfig.address = 34;
    metadataConfig.gasType = QStringLiteral("氮气");
    metadataConfig.expectedGasCode = 13;
    metadataConfig.expectedDeviceFullScaleSccm = 50.0;
    metadataInfo.address = 34;
    metadataInfo.targetGasName = QStringLiteral("N2");
    metadataInfo.targetGasCode = 13;
    metadataInfo.fullScale = 50.0;
    ok &= require(MfcManager::metadataMismatchFields(metadataConfig, metadataInfo).isEmpty(),
                  "MFC3 UI 50 ml/min 必须按设备侧 50 SCCM 进行 Full Scale 核验");
    metadataInfo.targetGasCode = 89;
    const QStringList mismatchFields = MfcManager::metadataMismatchFields(metadataConfig, metadataInfo);
    ok &= require(mismatchFields == QStringList{QStringLiteral("Target Gas Code")},
                  "metadata mismatch 必须精确指出字段，不能由实际流量或格式化字符串触发");
    // Stop Control is permitted to change only the command target.  This
    // model-level regression captures the UI/data contract before any next
    // legal READ_FLOW response arrives.
    displayChannel.targetFlow = 0.0;
    const auto stoppedMap = displayChannel.toVariantMap();
    ok &= require(stoppedMap.value("targetFlow").toDouble() == 0.0
                  && stoppedMap.value("actualFlow").toDouble() == 0.0
                  && stoppedMap.value("lastValidActualFlow").toDouble() == 2.013,
                  "停止控制归零目标时绝不能人为清零实际流量或最后有效流量");
    try {
        throw SerialTransport::Timeout("test timeout");
    } catch (const SerialTransport::Error &) {
        ok &= require(true, "timeout 应属于可捕获的串口事务异常");
    }
    {
        Cs200CommExperiment experiment;
        experiment.start(200, 300, 34);
        QList<int> scheduledAddresses;
        for (int i = 0; i < 6; ++i) {
            scheduledAddresses.append(experiment.currentAddress());
            experiment.advanceAddress();
        }
        const QVariantMap checksumAttempt{{"attempt", 1}, {"errorType", "checksum_error"},
            {"errorMessage", "response checksum mismatch"}, {"txRaw", "22 02 80"},
            {"currentFrame", "00 02 80 05 68 01 B9 3D 4F 00 34"},
            {"rxHistoryBeforeError", "06"}, {"rxAfterError", "06 00 02 80 05 68 01 B9 3D 4F 00 35"}};
        experiment.recordTransaction({{"address", 34}, {"result", "SUCCESS"},
            {"elapsedMs", 15.0}, {"attemptCount", 2}, {"previousFinishToNextTxMs", 200.2},
            {"attemptErrors", QVariantList{checksumAttempt}}}, true);
        const QVariantMap timeoutAttempt1{{"attempt", 1}, {"errorType", "timeout"},
            {"errorMessage", "serial response timeout"}, {"txRaw", "22 02 80"}, {"allRxRaw", "06"}};
        const QVariantMap timeoutAttempt2{{"attempt", 2}, {"errorType", "timeout"},
            {"errorMessage", "serial response timeout"}, {"txRaw", "22 02 80"}, {"allRxRaw", "06 06"}};
        experiment.recordTransaction({{"address", 34}, {"result", "TIMEOUT"},
            {"detail", "serial response timeout"}, {"attemptCount", 2},
            {"previousFinishToNextTxMs", 199.8},
            {"attemptErrors", QVariantList{timeoutAttempt1, timeoutAttempt2}}}, false,
            QStringLiteral("serial response timeout"));
        experiment.recordGuiHeartbeatStall(650);
        experiment.recordWorkerWatchdogTrigger();
        experiment.finish(QStringLiteral("unit_test"));
        const auto report = experiment.toVariantMap();
        const auto stats = report.value("addressStats").toList();
        const auto totals = report.value("global").toMap();
        ok &= require(scheduledAddresses == QList<int>({34, 34, 34, 34, 34, 34}),
                      "单设备实验选择地址 34 后调度器必须只产生 34");
        ok &= require(stats.size() == 1
                      && stats[0].toMap().value("requestCount").toInt() == 2
                      && stats[0].toMap().value("requestSuccess").toInt() == 1
                      && stats[0].toMap().value("requestFinalFailure").toInt() == 1
                      && stats[0].toMap().value("attemptCount").toInt() == 4
                      && stats[0].toMap().value("attemptChecksumError").toInt() == 1
                      && stats[0].toMap().value("attemptTimeout").toInt() == 2
                      && stats[0].toMap().value("retrySuccess").toInt() == 1,
                      "通信实验必须分开 request、attempt error、final failure 与 retry success");
        ok &= require(std::abs(totals.value("actualAverageTransactionGapMs").toDouble() - 200.0) < 0.01
                      && totals.value("guiHeartbeatStalls").toInt() == 1
                      && totals.value("workerWatchdogTriggers").toInt() == 1
                      && !experiment.active() && experiment.remainingSeconds() == 0,
                      "通信实验应汇总实测间隔、GUI heartbeat 与 worker watchdog");
        QTemporaryDir reportDirectory;
        QString jsonPath, textPath, reportError;
        ok &= require(experiment.writeReports(reportDirectory.path(), &jsonPath, &textPath, &reportError)
                      && QFileInfo::exists(jsonPath) && QFileInfo::exists(textPath)
                      && jsonPath.contains(QStringLiteral("single_addr34_200ms_5min"))
                      && textPath.endsWith(QStringLiteral(".md")),
                      "单设备实验结束应生成带模式、地址、间隔和时长的 JSON/Markdown 报告");
    }
#ifdef Q_OS_LINUX
    const int ptyMaster = posix_openpt(O_RDWR | O_NOCTTY);
    const bool ptyReady = ptyMaster >= 0 && grantpt(ptyMaster) == 0 && unlockpt(ptyMaster) == 0;
    ok &= require(ptyReady,
                  "应能创建伪串口测试 ACK timeout 和设备拔出");
    if (ptyReady) {
        SerialTransport fakeTransport;
        bool ackTimedOut = false;
        bool nakCaught = false;
        bool unplugCaught = false;
        bool singleTransactionSucceeded = false;
        bool retrySucceeded = false;
        bool requestsStayedSequential = false;
        bool configuredGapsMeasured = false;
        bool responseTimeoutAdvanced = false;
        bool retryLimitObserved = false;
        bool checksumRecovered = false;
        bool serviceRecovered = false;
        bool instanceRecovered = false;
        bool commandMismatchRecovered = false;
        bool attributeMismatchRecovered = false;
        bool queueAdvancedAfterThirdFailure = false;
        bool cancellationCaught = false;
        bool preCreateCancellationSnapshotClean = false;
        bool tenThousandTransactionsFinished = false;
        bool reconnectSucceeded = false;
        bool errorRawEvidenceCaptured = false;
        bool staleBoundaryCaptured = false;
        bool ackBitCorruptionRecovered = false;
        bool preTxGarbageRecovered = false;
        bool multiDeviceIsolationRecovered = false;
        bool backlogGuardRecovered = false;
        bool continuousGarbageDegraded = false;
        try {
            fakeTransport.open(QString::fromLocal8Bit(ptsname(ptyMaster)), 19200);
            std::thread successPeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), 4);
                usleep(2000);
                (void)::write(ptyMaster, flowPacket.constData() + 4, flowPacket.size() - 4);
            });
            try {
                singleTransactionSucceeded = fakeTransport.transaction(flowRequest, 100, 100, 0) == flowPacket;
            } catch (...) {}
            successPeer.join();

            std::thread retryPeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                // Longer than the first 10 ms ACK deadline, but still inside
                // the second attempt's ACK window.
                usleep(12000);
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
            });
            try { retrySucceeded = fakeTransport.transaction(flowRequest, 10, 100, 1) == flowPacket; }
            catch (...) {}
            retryPeer.join();

            const QByteArray secondRequest = MfcProtocol::makeRequest(
                0x21, MfcProtocol::ReadService, 0x68, 0x01, 0xB9);
            bool sequenceObserved = false;
            std::thread sequentialPeer([ptyMaster, flowPacket, &sequenceObserved] {
                char requestBytes[64];
                const ssize_t firstSize = ::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const bool firstAddress = firstSize > 0 && static_cast<unsigned char>(requestBytes[0]) == 0x20;
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
                const ssize_t secondSize = ::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const bool secondAddress = secondSize > 0 && static_cast<unsigned char>(requestBytes[0]) == 0x21;
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
                sequenceObserved = firstAddress && secondAddress;
            });
            try {
                (void)fakeTransport.transaction(flowRequest, 100, 100, 0);
                (void)fakeTransport.transaction(secondRequest, 100, 100, 0);
            } catch (...) {}
            sequentialPeer.join();
            requestsStayedSequential = sequenceObserved;

            std::thread gapPeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                const char ack = char(MfcProtocol::Ack);
                for (int i = 0; i < 3; ++i) {
                    (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                    (void)::write(ptyMaster, &ack, 1);
                    (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
                }
            });
            double measured100 = 0.0;
            double measured200 = 0.0;
            try {
                fakeTransport.resetTransactionSpacing();
                (void)fakeTransport.transaction(flowRequest, 100, 100, 0);
                QThread::msleep(100);
                (void)fakeTransport.transaction(secondRequest, 100, 100, 0);
                measured100 = fakeTransport.diagnostics().value("lastTransaction").toMap()
                    .value("previousFinishToNextTxMs").toDouble();
                QThread::msleep(200);
                (void)fakeTransport.transaction(flowRequest, 100, 100, 0);
                measured200 = fakeTransport.diagnostics().value("lastTransaction").toMap()
                    .value("previousFinishToNextTxMs").toDouble();
            } catch (...) {}
            gapPeer.join();
            configuredGapsMeasured = measured100 >= 95.0 && measured100 < 140.0
                && measured200 >= 195.0 && measured200 < 240.0;

            std::thread timeoutThenNextPeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
            });
            bool firstResponseTimedOut = false;
            bool nextAfterTimeoutSucceeded = false;
            try { (void)fakeTransport.transaction(flowRequest, 100, 20, 0); }
            catch (const SerialTransport::Timeout &) { firstResponseTimedOut = true; }
            try { nextAfterTimeoutSucceeded = fakeTransport.transaction(secondRequest, 100, 100, 0) == flowPacket; }
            catch (...) {}
            timeoutThenNextPeer.join();
            responseTimeoutAdvanced = firstResponseTimedOut && nextAfterTimeoutSucceeded;

            int attemptsSeen = 0;
            std::thread retryLimitPeer([ptyMaster, &attemptsSeen] {
                char requestBytes[64];
                for (int i = 0; i < 2; ++i) {
                    if (::read(ptyMaster, requestBytes, sizeof(requestBytes)) > 0) ++attemptsSeen;
                }
            });
            bool retryLimitTimedOut = false;
            try { (void)fakeTransport.transaction(flowRequest, 10, 20, 1); }
            catch (const SerialTransport::Timeout &) { retryLimitTimedOut = true; }
            retryLimitPeer.join();
            retryLimitObserved = retryLimitTimedOut && attemptsSeen == 2
                && !fakeTransport.transactionPending();

            auto transactionRecovers = [&](QByteArray badFrame) {
                std::thread peer([ptyMaster, flowPacket, badFrame] {
                    char requestBytes[64];
                    (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                    const char ack = char(MfcProtocol::Ack);
                    (void)::write(ptyMaster, &ack, 1);
                    (void)::write(ptyMaster, badFrame.constData(), badFrame.size());
                    // The second request may only be sent after the transport
                    // has drained the corrupt response and observed RX quiet.
                    (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                    (void)::write(ptyMaster, &ack, 1);
                    (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
                });
                bool success = false;
                try { success = fakeTransport.transaction(flowRequest, 100, 100, 1) == flowPacket; }
                catch (...) {}
                peer.join();
                return success;
            };
            QByteArray checksumBad = flowPacket;
            checksumBad[checksumBad.size() - 1] ^= char(0x01);
            fakeTransport.setTransactionOrigin(QStringLiteral("CONTROL_PREFLIGHT"));
            checksumRecovered = transactionRecovers(checksumBad);
            {
                const QVariantMap transaction = fakeTransport.diagnostics().value("lastTransaction").toMap();
                const QVariantList errors = transaction.value("attemptErrors").toList();
                const QVariantMap firstError = errors.isEmpty() ? QVariantMap{} : errors.first().toMap();
                errorRawEvidenceCaptured = checksumRecovered
                    && transaction.value("origin").toString() == QStringLiteral("CONTROL_PREFLIGHT")
                    && transaction.value("result").toString() == QStringLiteral("SUCCESS")
                    && transaction.value("retryCount").toInt() == 1
                    && firstError.value("errorType").toString() == QStringLiteral("checksum_error")
                    && !firstError.value("currentFrame").toString().isEmpty()
                    && !firstError.value("allRxRaw").toString().isEmpty()
                    && firstError.value("rxAfterError").toString().contains(QStringLiteral("00 02 80 05"))
                    && !transaction.value("txTimestamp").toString().isEmpty()
                    && !transaction.value("firstRxTimestamp").toString().isEmpty()
                    && !transaction.value("frameCompleteTimestamp").toString().isEmpty()
                    && !transaction.value("rxBatches").toList().isEmpty()
                    && transaction.value("attempts").toList().first().toMap().value("ackRaw").toString() == QStringLiteral("06")
                    && transaction.value("serialConfiguration").toMap().value("baudRate").toInt() == 19200
                    && transaction.value("serialConfiguration").toMap().value("flowControl").toString() == QStringLiteral("NONE");
            }
            fakeTransport.setTransactionOrigin({});
            QByteArray serviceBad = flowPacket;
            serviceBad[2] = char(0x81);
            serviceRecovered = transactionRecovers(serviceBad);
            QByteArray instanceBad = flowPacket;
            instanceBad[5] = char(0x00);
            instanceBad[instanceBad.size() - 1] = char(MfcProtocol::checksum(
                instanceBad.first(instanceBad.size() - 1)));
            instanceRecovered = transactionRecovers(instanceBad);
            QByteArray commandBad = flowPacket;
            commandBad[4] = char(0x69);
            commandBad[commandBad.size() - 1] = char(MfcProtocol::checksum(
                commandBad.first(commandBad.size() - 1)));
            commandMismatchRecovered = transactionRecovers(commandBad);
            QByteArray attributeBad = flowPacket;
            attributeBad[6] = char(0xA5);
            attributeBad[attributeBad.size() - 1] = char(MfcProtocol::checksum(
                attributeBad.first(attributeBad.size() - 1)));
            attributeMismatchRecovered = transactionRecovers(attributeBad);

            // Critical regression: a damaged ACK is followed by the rest of
            // that response in separate UART batches.  The retry must start
            // only after the tail has been drained and RX is quiet.
            std::thread ackCorruptionPeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char badAck = char(0x04);
                (void)::write(ptyMaster, &badAck, 1);
                usleep(3000);
                (void)::write(ptyMaster, flowPacket.constData(), 4);
                usleep(6000);
                (void)::write(ptyMaster, flowPacket.constData() + 4, flowPacket.size() - 4);
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
            });
            try { ackBitCorruptionRecovered = fakeTransport.transaction(flowRequest, 100, 100, 1) == flowPacket; }
            catch (...) {}
            ackCorruptionPeer.join();
            {
                const QVariantMap transaction = fakeTransport.diagnostics().value("lastTransaction").toMap();
                const QVariantList recovery = transaction.value("recoveryEvents").toList();
                const QVariantMap first = recovery.isEmpty() ? QVariantMap{} : recovery.first().toMap();
                ackBitCorruptionRecovered = ackBitCorruptionRecovered
                    && transaction.value("attemptCount").toInt() == 2
                    && first.value("reason").toString() == QStringLiteral("INVALID_ACK")
                    && first.value("discardedBytes").toInt() >= flowPacket.size()
                    && first.value("rxPendingAtEnd").toInt() == 0;
            }

            // Garbage present before a transaction is a hard send gate, not a
            // diagnostic-only observation.
            const QByteArray preTxGarbage = QByteArray::fromHex("0002800568");
            (void)::write(ptyMaster, preTxGarbage.constData(), preTxGarbage.size());
            usleep(2000);
            std::thread preTxPeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
            });
            bool preTxSuccess = false;
            try { preTxSuccess = fakeTransport.transaction(flowRequest, 100, 100, 0) == flowPacket; } catch (...) {}
            preTxPeer.join();
            {
                const QVariantMap transaction = fakeTransport.diagnostics().value("lastTransaction").toMap();
                const QVariantList recovery = transaction.value("recoveryEvents").toList();
                preTxGarbageRecovered = preTxSuccess && transaction.value("rxBytesAvailableBeforeTx").toInt() > 0
                    && !recovery.isEmpty() && recovery.first().toMap().value("reason").toString()
                        == QStringLiteral("RX_PENDING_BEFORE_TX");
            }

            // Addr 33's corrupt ACK and valid-looking residual must be fully
            // contained before the shared stream is used for Addr 34.
            bool addressesIsolated = true;
            std::thread isolationPeer([ptyMaster, flowPacket, &addressesIsolated] {
                char requestBytes[64];
                const ssize_t first = ::read(ptyMaster, requestBytes, sizeof(requestBytes));
                addressesIsolated &= first > 0 && static_cast<unsigned char>(requestBytes[0]) == 0x21;
                const char badAck = char(0x04);
                (void)::write(ptyMaster, &badAck, 1);
                usleep(3000);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
                const ssize_t second = ::read(ptyMaster, requestBytes, sizeof(requestBytes));
                addressesIsolated &= second > 0 && static_cast<unsigned char>(requestBytes[0]) == 0x22;
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
            });
            const QByteArray addr33Request = MfcProtocol::makeRequest(0x21, MfcProtocol::ReadService, 0x68, 0x01, 0xB9);
            const QByteArray addr34Request = MfcProtocol::makeRequest(0x22, MfcProtocol::ReadService, 0x68, 0x01, 0xB9);
            bool addr33Failed = false, addr34Succeeded = false;
            try { (void)fakeTransport.transaction(addr33Request, 100, 100, 0); }
            catch (const SerialTransport::ProtocolFailure &) { addr33Failed = true; }
            const QVariantMap addr33Transaction = fakeTransport.diagnostics().value("lastTransaction").toMap();
            try { addr34Succeeded = fakeTransport.transaction(addr34Request, 100, 100, 0) == flowPacket; } catch (...) {}
            isolationPeer.join();
            multiDeviceIsolationRecovered = addressesIsolated && addr33Failed && addr34Succeeded
                && !addr33Transaction.value("recoveryEvents").toList().isEmpty()
                && fakeTransport.diagnostics().value("lastTransaction").toMap()
                    .value("rxBytesAvailableBeforeTx").toInt() == 0;

            QByteArray backlog(160, char(0x55));
            (void)::write(ptyMaster, backlog.constData(), backlog.size());
            usleep(2000);
            std::thread backlogPeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
            });
            bool backlogSuccess = false;
            try { backlogSuccess = fakeTransport.transaction(flowRequest, 100, 100, 0) == flowPacket; } catch (...) {}
            backlogPeer.join();
            backlogGuardRecovered = backlogSuccess && fakeTransport.diagnostics().value("hardRecoveryCount").toULongLong() > 0;

            // Continuous input must be bounded: no retry storm, no TX while
            // recovery cannot establish quiet, and an explicit degraded state.
            std::thread noisyPeer([ptyMaster] {
                const char garbage = char(0x55);
                for (int i = 0; i < 350; ++i) {
                    (void)::write(ptyMaster, &garbage, 1);
                    usleep(2000);
                }
            });
            bool noisyFailed = false;
            try { (void)fakeTransport.transaction(flowRequest, 100, 100, 1); }
            catch (const SerialTransport::ProtocolFailure &) { noisyFailed = true; }
            noisyPeer.join();
            {
                const QVariantMap transaction = fakeTransport.diagnostics().value("lastTransaction").toMap();
                continuousGarbageDegraded = noisyFailed && transaction.value("serialDegraded").toBool()
                    && transaction.value("attemptCount").toInt() == 1
                    && transaction.value("hardRecoveryCount").toULongLong() > 0;
            }
            fakeTransport.flush();

            fakeTransport.setExperimentDiagnostics(true);
            std::thread stalePeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                const char ack = char(MfcProtocol::Ack);
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
                usleep(10000);
                const char stale = char(0x55);
                (void)::write(ptyMaster, &stale, 1);
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
            });
            try { (void)fakeTransport.transaction(flowRequest, 100, 100, 0); } catch (...) {}
            QThread::msleep(20);
            try { (void)fakeTransport.transaction(flowRequest, 100, 100, 0); } catch (...) {}
            stalePeer.join();
            {
                const QVariantMap transaction = fakeTransport.diagnostics().value("lastTransaction").toMap();
                staleBoundaryCaptured = transaction.value("rxBytesAvailableBeforeTx").toInt() > 0
                    && transaction.value("lateRxAfterFinishCount").toInt() == 1
                    && transaction.value("preTxRxPeek").toString().contains(QStringLiteral("55"))
                    && transaction.value("parserStateBeforeTx").toString() == QStringLiteral("WAIT_START_00");
            }
            fakeTransport.setExperimentDiagnostics(false);
            fakeTransport.flush();

            std::array<QByteArray, 5> fiveRequests;
            for (int i = 0; i < 5; ++i)
                fiveRequests[i] = MfcProtocol::makeRequest(static_cast<quint8>(0x20 + i),
                    MfcProtocol::ReadService, 0x68, 0x01, 0xB9);
            bool fiveAddressesObserved = true;
            std::thread fivePeer([ptyMaster, flowPacket, checksumBad, &fiveAddressesObserved] {
                char requestBytes[64];
                for (int i = 0; i < 5; ++i) {
                    const ssize_t received = ::read(ptyMaster, requestBytes, sizeof(requestBytes));
                    fiveAddressesObserved &= received > 0
                        && static_cast<unsigned char>(requestBytes[0]) == 0x20 + i;
                    const char ack = char(MfcProtocol::Ack);
                    (void)::write(ptyMaster, &ack, 1);
                    const QByteArray &response = i == 2 ? checksumBad : flowPacket;
                    (void)::write(ptyMaster, response.constData(), response.size());
                }
            });
            int fiveFinished = 0;
            bool thirdFailed = false;
            for (int i = 0; i < 5; ++i) {
                try {
                    (void)fakeTransport.transaction(fiveRequests[i], 100, 30, 0);
                    ++fiveFinished;
                } catch (const SerialTransport::ProtocolFailure &) {
                    thirdFailed = i == 2;
                    ++fiveFinished;
                } catch (...) {}
            }
            fivePeer.join();
            queueAdvancedAfterThirdFailure = fiveAddressesObserved && thirdFailed && fiveFinished == 5;

            std::thread cancelPeer([ptyMaster] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                usleep(20000);
            });
            std::thread canceller([&fakeTransport] {
                usleep(5000);
                fakeTransport.requestCancel();
            });
            try { (void)fakeTransport.transaction(flowRequest, 100, 100, 0); }
            catch (const SerialTransport::Cancelled &) { cancellationCaught = true; }
            canceller.join();
            cancelPeer.join();
            fakeTransport.clearCancellation();

            // A cancellation before a PendingRequest is created must replace
            // (not inherit) the last successful transaction's raw evidence.
            fakeTransport.requestCancel(SerialTransport::CancelReason::WorkerWatchdog);
            try { (void)fakeTransport.transaction(secondRequest, 100, 100, 0); }
            catch (const SerialTransport::Cancelled &) {
                const QVariantMap cancelled = fakeTransport.diagnostics().value("lastTransaction").toMap();
                preCreateCancellationSnapshotClean = !cancelled.value("transactionCreated", true).toBool()
                    && cancelled.value("requestId").toULongLong() == 0
                    && cancelled.value("result").toString() == QStringLiteral("CANCELLED")
                    && cancelled.value("cancellationReason").toString() == QStringLiteral("WORKER_WATCHDOG")
                    && cancelled.value("txRaw").toString().isEmpty()
                    && cancelled.value("ackRaw").toString().isEmpty()
                    && cancelled.value("rxRaw").toString().isEmpty()
                    && cancelled.value("responseFrameRaw").toString().isEmpty()
                    && cancelled.value("payloadRaw").toString().isEmpty();
            }
            fakeTransport.clearCancellation();

            constexpr int stressTransactions = 10000;
            bool stressAddressesObserved = true;
            std::thread stressPeer([ptyMaster, flowPacket, &stressAddressesObserved] {
                char requestBytes[64];
                for (int i = 0; i < stressTransactions; ++i) {
                    const ssize_t received = ::read(ptyMaster, requestBytes, sizeof(requestBytes));
                    stressAddressesObserved &= received > 0
                        && static_cast<unsigned char>(requestBytes[0]) == 0x20 + (i % 5);
                    const char ack = char(MfcProtocol::Ack);
                    (void)::write(ptyMaster, &ack, 1);
                    (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
                }
            });
            int stressFinished = 0;
            for (int i = 0; i < stressTransactions; ++i) {
                try {
                    (void)fakeTransport.transaction(fiveRequests[i % 5], 100, 100, 0);
                    ++stressFinished;
                } catch (...) { break; }
            }
            stressPeer.join();
            tenThousandTransactionsFinished = stressAddressesObserved
                && stressFinished == stressTransactions && !fakeTransport.transactionPending();

            fakeTransport.close();
            fakeTransport.open(QString::fromLocal8Bit(ptsname(ptyMaster)), 19200);
            std::thread reconnectPeer([ptyMaster, flowPacket] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char ack = char(MfcProtocol::Ack);
                (void)::write(ptyMaster, &ack, 1);
                (void)::write(ptyMaster, flowPacket.constData(), flowPacket.size());
            });
            try { reconnectSucceeded = fakeTransport.transaction(flowRequest, 100, 100, 0) == flowPacket; }
            catch (...) {}
            reconnectPeer.join();

            std::thread nakPeer([ptyMaster] {
                char requestBytes[64];
                (void)::read(ptyMaster, requestBytes, sizeof(requestBytes));
                const char nak = char(MfcProtocol::Nak);
                (void)::write(ptyMaster, &nak, 1);
            });
            try { (void)fakeTransport.transaction(flowRequest, 100, 10, 0); }
            catch (const SerialTransport::NegativeAcknowledge &) { nakCaught = true; }
            nakPeer.join();
            try { (void)fakeTransport.transaction(flowRequest, 10, 10, 0); }
            catch (const SerialTransport::Timeout &) { ackTimedOut = true; }
            close(ptyMaster);
            fakeTransport.close();
            try { (void)fakeTransport.transaction(flowRequest, 10, 10, 0); }
            catch (const SerialTransport::Error &) { unplugCaught = true; }
            fakeTransport.close();
        } catch (const SerialTransport::Error &) {
            close(ptyMaster);
        }
        ok &= require(singleTransactionSucceeded,
                      "伪串口单请求应支持 ACK 后分段响应的完整收发");
        ok &= require(retrySucceeded, "首次 ACK timeout 后应有限重试并可恢复成功");
        ok &= require(requestsStayedSequential, "同一物理串口上的多个请求必须保持发送顺序");
        ok &= require(configuredGapsMeasured,
                      "串口时间戳必须验证约 100 ms 与 200 ms 的 FINISH 到下一 TX 间隔");
        ok &= require(responseTimeoutAdvanced, "完整无响应 timeout 后下一请求必须继续执行");
        ok &= require(retryLimitObserved, "重试必须严格受上限约束并最终清理 pending");
        ok &= require(checksumRecovered, "事务接收器必须从 checksum bad 恢复到随后合法帧");
        ok &= require(serviceRecovered, "事务接收器必须从 invalid service 恢复到随后合法帧");
        ok &= require(instanceRecovered, "事务接收器必须从 invalid instance 恢复、drain 后重试成功");
        ok &= require(commandMismatchRecovered, "事务接收器必须拒绝错命令并接受随后匹配帧");
        ok &= require(attributeMismatchRecovered, "事务接收器必须拒绝错属性并接受随后匹配帧");
        ok &= require(ackBitCorruptionRecovered,
                      "ACK bit 错误必须 drain 分批残帧、quiet 后 retry，且不得污染下一 attempt");
        ok &= require(preTxGarbageRecovered,
                      "pre-TX RX garbage 必须先 recovery；不得绕过 clean gate 发送业务请求");
        ok &= require(multiDeviceIsolationRecovered,
                      "Addr33 异常响应残留必须在 Addr34 TX 前清空，不能跨地址污染");
        ok &= require(backlogGuardRecovered,
                      "超过安全 RX backlog 时必须进入 hard recovery 后才能 TX");
        ok &= require(continuousGarbageDegraded,
                      "持续垃圾数据必须有界失败并进入 SERIAL_DEGRADED，而非无限 retry/TX");
        ok &= require(errorRawEvidenceCaptured,
                      "checksum attempt error 必须保存 TX、完整 RX、错误帧、RX 批次与关键时间戳");
        ok &= require(staleBoundaryCaptured,
                      "诊断模式必须在不预清输入的情况下记录 STALE_RX_AFTER_FINISH 与 PRE-TX RX");
        ok &= require(queueAdvancedAfterThirdFailure,
                      "五设备队列第 3 个失败后第 4、5 个仍必须执行");
        ok &= require(cancellationCaught && !fakeTransport.transactionPending(),
                      "pending request 必须可取消且统一清除 pending 状态");
        ok &= require(preCreateCancellationSnapshotClean,
                      "REQUEST_BEFORE_CREATE 取消不得复用上一 request 的 ID、TX、ACK、RX 或 payload");
        ok &= require(tenThousandTransactionsFinished,
                      "连续 10000 requests 后不得存在永久 pending transaction");
        ok &= require(reconnectSucceeded, "串口关闭并重连后事务必须恢复");
        ok &= require(nakCaught, "NAK 必须抛出独立的 NegativeAcknowledge 异常");
        ok &= require(ackTimedOut, "无 ACK 的串口事务必须有限超时");
        ok &= require(unplugCaught, "伪串口被拔出后必须转为可捕获异常而非退出进程");
    } else if (ptyMaster >= 0) close(ptyMaster);
#endif

// Preflight must finish all read-only metadata checks before the first
// whitelisted write. A PTY peer returns an intentionally wrong Gas Code and
// records every request it receives.
#ifdef Q_OS_LINUX
    const int preflightPtyMaster = posix_openpt(O_RDWR | O_NOCTTY);
    const bool preflightPtyReady = preflightPtyMaster >= 0
        && grantpt(preflightPtyMaster) == 0 && unlockpt(preflightPtyMaster) == 0;
    ok &= require(preflightPtyReady, "应能创建 Preflight mismatch 伪串口");
    if (preflightPtyReady) {
        const QString preflightPort = QString::fromLocal8Bit(ptsname(preflightPtyMaster));
        MfcDeviceConfig preflightDevice;
        preflightDevice.logicalChannel = 1;
        preflightDevice.address = 32;
        preflightDevice.displayName = QStringLiteral("MFC1");
        preflightDevice.gasType = QStringLiteral("空气");
        preflightDevice.function = QStringLiteral("发生器空气");
        preflightDevice.unit = QStringLiteral("L/min");
        preflightDevice.fullScale = 1.0;
        preflightDevice.maximumSetpoint = 1.0;
        preflightDevice.expectedGasCode = 8;
        preflightDevice.expectedDeviceFullScaleSccm = 1000.0;
        preflightDevice.addressConfirmed = true;
        MfcManager::Settings preflightSettings;
        preflightSettings.serialPort = preflightPort;
        preflightSettings.ackTimeoutMs = 100;
        preflightSettings.responseTimeoutMs = 100;
        preflightSettings.retryCount = 0;
        int controlWrites = 0;
        std::thread preflightPeer([preflightPtyMaster, &controlWrites] {
            const auto response = [](quint8 commandClass, quint8 attribute, const QByteArray &data) {
                QByteArray packet;
                packet.append(char(0x00)); packet.append(char(0x02)); packet.append(char(0x80));
                packet.append(char(3 + data.size())); packet.append(char(commandClass));
                packet.append(char(0x01)); packet.append(char(attribute)); packet.append(data);
                packet.append(char(0x00)); packet.append(char(MfcProtocol::checksum(packet)));
                return packet;
            };
            // One READ_FLOW probe plus eight metadata reads. The final result
            // must be a preflight failure before a 0x81 request can arrive.
            for (int transaction = 0; transaction < 9; ++transaction) {
                char request[64]{};
                const ssize_t size = ::read(preflightPtyMaster, request, sizeof(request));
                if (size < 7) break;
                const quint8 service = static_cast<quint8>(request[2]);
                const quint8 commandClass = static_cast<quint8>(request[4]);
                const quint8 attribute = static_cast<quint8>(request[6]);
                if (service == MfcProtocol::WriteService) { ++controlWrites; continue; }
                QByteArray data;
                if (commandClass == 0x68 && attribute == 0xB9) data = QByteArray::fromHex("0040");
                else if (commandClass == 0x64 && attribute == 0x04) data = QByteArray("CS200-A");
                else if (commandClass == 0x64 && attribute == 0x07) data = QByteArray("TEST-32");
                else if (commandClass == 0x66 && attribute == 0x01) data = QByteArray("Air");
                else if (commandClass == 0x66 && attribute == 0x02) data = QByteArray::fromHex("5900"); // 89, not 8
                else if (commandClass == 0x66 && attribute == 0x03) data = QByteArray::fromHex("E803");
                else if (commandClass == 0x66 && attribute == 0x04) data = QByteArray::fromHex("00000100");
                else if (commandClass == 0x03 && attribute == 0x01) data = QByteArray::fromHex("20");
                else if (commandClass == 0x03 && attribute == 0x02) data = QByteArray::fromHex("004B");
                const char ack = char(MfcProtocol::Ack);
                (void)::write(preflightPtyMaster, &ack, 1);
                const QByteArray packet = response(commandClass, attribute, data);
                (void)::write(preflightPtyMaster, packet.constData(), packet.size());
            }
        });
        MfcManager preflightManager({preflightDevice}, preflightSettings);
        const bool preflightConnected = preflightManager.connectBus();
        bool writesStarted = true;
        QString preflightError;
        const bool preflightApplied = preflightConnected && preflightManager.applyOperatingPoint(
            {{32, 0.5}}, &preflightError, &writesStarted);
        preflightManager.disconnectBus();
        preflightPeer.join();
        ::close(preflightPtyMaster);
        ok &= require(preflightConnected && !preflightApplied && !writesStarted
                      && controlWrites == 0 && preflightError.contains(QStringLiteral("Gas Code")),
                      "Preflight Gas Code mismatch 必须阻止 Controlling，且不得发送任何 Digital Setpoint/CM/Hold/Follow 写命令");
    }
#endif

// Regression for the field timeline: five sequential DeviceInfo reads take
// longer than the former 2 s watchdog threshold.  Bootstrap must finish all
// MFC5 attributes before the caller is allowed to begin READ_FLOW polling.
#ifdef Q_OS_LINUX
    const int bootstrapPtyMaster = posix_openpt(O_RDWR | O_NOCTTY);
    const bool bootstrapPtyReady = bootstrapPtyMaster >= 0
        && grantpt(bootstrapPtyMaster) == 0 && unlockpt(bootstrapPtyMaster) == 0;
    ok &= require(bootstrapPtyReady, "应能创建 DeviceInfo bootstrap 伪串口");
    if (bootstrapPtyReady) {
        const QString bootstrapPort = QString::fromLocal8Bit(ptsname(bootstrapPtyMaster));
        auto bootstrapDevices = fixedDevices;
        for (auto &device : bootstrapDevices) device.addressConfirmed = true;
        MfcManager::Settings bootstrapSettings;
        bootstrapSettings.serialPort = bootstrapPort;
        bootstrapSettings.ackTimeoutMs = 120;
        bootstrapSettings.responseTimeoutMs = 120;
        bootstrapSettings.retryCount = 0;
        bootstrapSettings.communicationRecoverySuccessThreshold = 1;
        bootstrapSettings.metadataInterRequestDelayMs = 20;
        QStringList bootstrapTrace;
        QList<QPair<int, QPair<int, int>>> bootstrapRequests;
        std::thread bootstrapPeer([bootstrapPtyMaster, bootstrapDevices, &bootstrapRequests] {
            const auto response = [](quint8 commandClass, quint8 attribute, const QByteArray &data) {
                QByteArray packet = QByteArray::fromHex("000280");
                packet.append(char(3 + data.size())); packet.append(char(commandClass)); packet.append(char(0x01));
                packet.append(char(attribute)); packet.append(data); packet.append(char(0x00));
                packet.append(char(MfcProtocol::checksum(packet))); return packet;
            };
            const auto u16le = [](quint16 value) {
                QByteArray bytes;
                bytes.append(char(value & 0xff));
                bytes.append(char((value >> 8) & 0xff));
                return bytes;
            };
            // Five connect probes + five complete (11-read) DeviceInfo
            // bootstraps + one post-bootstrap READ_FLOW poll.
            for (int transaction = 0; transaction < 61; ++transaction) {
                char request[64]{};
                const ssize_t size = ::read(bootstrapPtyMaster, request, sizeof(request));
                if (size < 7) break;
                const int address = static_cast<unsigned char>(request[0]);
                const quint8 commandClass = static_cast<quint8>(request[4]);
                const quint8 attribute = static_cast<quint8>(request[6]);
                bootstrapRequests.append({address, {commandClass, attribute}});
                const auto config = std::find_if(bootstrapDevices.cbegin(), bootstrapDevices.cend(),
                    [address](const MfcDeviceConfig &device) { return device.address == address; });
                if (config == bootstrapDevices.cend()) break;
                QByteArray data;
                if (commandClass == 0x68 && attribute == 0xB9) data = QByteArray::fromHex("0040");
                else if (commandClass == 0x64 && attribute == 0x04) data = QByteArray("70J4L");
                else if (commandClass == 0x64 && attribute == 0x07) data = QByteArray("0ZCSA038970");
                else if (commandClass == 0x66 && (attribute == 0x01 || attribute == 0x06)) {
                    data = config->address == 33 ? QByteArray("C3H8")
                         : config->expectedGasCode == 13 ? QByteArray("NITROGEN") : QByteArray("AIR");
                } else if (commandClass == 0x66 && (attribute == 0x02 || attribute == 0x07)) {
                    data = u16le(config->expectedGasCode);
                } else if (commandClass == 0x66 && (attribute == 0x03 || attribute == 0x08)) {
                    data = u16le(static_cast<quint16>(config->expectedDeviceFullScaleSccm));
                } else if (commandClass == 0x66 && attribute == 0x04) data = QByteArray::fromHex("00000100");
                else if (commandClass == 0x03 && attribute == 0x01) data = QByteArray(1, char(address));
                else if (commandClass == 0x03 && attribute == 0x02) data = QByteArray::fromHex("004B");
                usleep(40000); // 55 metadata commands alone exceed 2.5 s.
                const char ack = char(MfcProtocol::Ack);
                (void)::write(bootstrapPtyMaster, &ack, 1);
                const QByteArray packet = response(commandClass, attribute, data);
                (void)::write(bootstrapPtyMaster, packet.constData(), packet.size());
            }
        });
        MfcManager bootstrapManager(bootstrapDevices, bootstrapSettings,
            [&bootstrapTrace](const QString &line) { bootstrapTrace.append(line); });
        QElapsedTimer bootstrapElapsed;
        bootstrapElapsed.start();
        const bool bootstrapConnected = bootstrapManager.connectBus();
        QString bootstrapError;
        const bool bootstrapVerified = bootstrapConnected
            && bootstrapManager.verifyDeviceInformation(&bootstrapError);
        QString bootstrapPollError;
        const bool postBootstrapPoll = bootstrapVerified && bootstrapManager.pollNext(&bootstrapPollError);
        const qint64 bootstrapDurationMs = bootstrapElapsed.elapsed();
        const auto bootstrapStates = bootstrapManager.devices();
        bootstrapManager.disconnectBus(); bootstrapPeer.join(); ::close(bootstrapPtyMaster);
        const QList<QPair<int, int>> expectedMfc5Metadata{{0x64, 0x04}, {0x64, 0x07}, {0x66, 0x01},
            {0x66, 0x02}, {0x66, 0x03}, {0x66, 0x07}, {0x66, 0x08}, {0x66, 0x06},
            {0x66, 0x04}, {0x03, 0x01}, {0x03, 0x02}};
        QList<QPair<int, int>> mfc5Metadata;
        for (const auto &request : bootstrapRequests)
            if (request.first == 36 && request.second.first != 0x68) mfc5Metadata.append(request.second);
        const QString joinedBootstrapTrace = bootstrapTrace.join('\n');
        const bool allBootstrapMetadataComplete = std::all_of(bootstrapStates.cbegin(), bootstrapStates.cend(),
            [](const MfcDeviceState &state) { return state.metadataVerificationComplete; });
        ok &= require(bootstrapVerified && postBootstrapPoll && bootstrapDurationMs > 2500
                      && allBootstrapMetadataComplete && mfc5Metadata == expectedMfc5Metadata
                      && joinedBootstrapTrace.contains(QStringLiteral("DEVICE_INFO_ALL_COMPLETE result=SUCCESS")),
                      "超过 watchdog 阈值的五机 DeviceInfo bootstrap 必须完整完成，MFC5 不得被取消且随后才可 READ_FLOW");
    } else if (bootstrapPtyMaster >= 0) close(bootstrapPtyMaster);
#endif

// Regression for the running-mode field failure: after polling has already
// armed its watchdog, a five-device DeviceInfo scan may last well beyond two
// seconds (including a retry/recovery-sized jitter).  It must own the one
// serial bus exclusively and resume with a fresh READ_FLOW baseline.
#ifdef Q_OS_LINUX
    const int lifecyclePtyMaster = posix_openpt(O_RDWR | O_NOCTTY);
    const bool lifecyclePtyReady = lifecyclePtyMaster >= 0
        && grantpt(lifecyclePtyMaster) == 0 && unlockpt(lifecyclePtyMaster) == 0;
    ok &= require(lifecyclePtyReady, "应能创建运行中 DeviceInfo watchdog 回归伪串口");
    if (lifecyclePtyReady) {
        const QString lifecyclePort = QString::fromLocal8Bit(ptsname(lifecyclePtyMaster));
        auto lifecycleDevices = fixedDevices;
        for (auto &device : lifecycleDevices) device.addressConfirmed = true;
        std::atomic_bool lifecyclePeerStop{false};
        std::atomic_int metadataRequests{0};
        std::atomic_int readFlowDuringMetadata{0};
        std::atomic_int postScanReadFlows{0};
        std::atomic_bool stallAfterPostScan{false};
        std::thread lifecyclePeer([&] {
            const auto response = [](quint8 commandClass, quint8 attribute, const QByteArray &data) {
                QByteArray packet = QByteArray::fromHex("000280");
                packet.append(char(3 + data.size())); packet.append(char(commandClass)); packet.append(char(0x01));
                packet.append(char(attribute)); packet.append(data); packet.append(char(0x00));
                packet.append(char(MfcProtocol::checksum(packet))); return packet;
            };
            const auto u16le = [](quint16 value) {
                QByteArray bytes; bytes.append(char(value & 0xff)); bytes.append(char(value >> 8)); return bytes;
            };
            QByteArray buffered;
            while (!lifecyclePeerStop.load()) {
                fd_set readable; FD_ZERO(&readable); FD_SET(lifecyclePtyMaster, &readable);
                timeval timeout{0, 100000};
                if (::select(lifecyclePtyMaster + 1, &readable, nullptr, nullptr, &timeout) <= 0) continue;
                char incoming[128];
                const ssize_t count = ::read(lifecyclePtyMaster, incoming, sizeof(incoming));
                if (count <= 0) continue;
                buffered.append(incoming, static_cast<int>(count));
                while (buffered.size() >= 4) {
                    const int requestSize = static_cast<unsigned char>(buffered[3]) + 6;
                    if (buffered.size() < requestSize) break;
                    const QByteArray request = buffered.left(requestSize);
                    buffered.remove(0, requestSize);
                    const int address = static_cast<unsigned char>(request[0]);
                    const quint8 commandClass = static_cast<quint8>(request[4]);
                    const quint8 attribute = static_cast<quint8>(request[6]);
                    const bool readFlow = commandClass == 0x68 && attribute == 0xB9;
                    const int completedMetadata = metadataRequests.load();
                    if (readFlow && completedMetadata > 0 && completedMetadata < 55)
                        ++readFlowDuringMetadata;
                    if (!readFlow) ++metadataRequests;
                    const auto config = std::find_if(lifecycleDevices.cbegin(), lifecycleDevices.cend(),
                        [address](const MfcDeviceConfig &device) { return device.address == address; });
                    if (config == lifecycleDevices.cend()) continue;
                    QByteArray data;
                    if (commandClass == 0x68 && attribute == 0xB9) data = QByteArray::fromHex("0040");
                    else if (commandClass == 0x64 && attribute == 0x04) data = QByteArray("70J4L");
                    else if (commandClass == 0x64 && attribute == 0x07) data = QByteArray("0ZCSA038970");
                    else if (commandClass == 0x66 && (attribute == 0x01 || attribute == 0x06))
                        data = config->expectedGasCode == 13 ? QByteArray("NITROGEN") : QByteArray("AIR");
                    else if (commandClass == 0x66 && (attribute == 0x02 || attribute == 0x07)) data = u16le(config->expectedGasCode);
                    else if (commandClass == 0x66 && (attribute == 0x03 || attribute == 0x08)) data = u16le(static_cast<quint16>(config->expectedDeviceFullScaleSccm));
                    else if (commandClass == 0x66 && attribute == 0x04) data = QByteArray::fromHex("00000100");
                    else if (commandClass == 0x03 && attribute == 0x01) data = QByteArray(1, char(address));
                    else if (commandClass == 0x03 && attribute == 0x02) data = QByteArray::fromHex("004B");
                    // 50 ms per metadata command gives >2.7 s; a single
                    // extra 80 ms models checksum retry/BUS_RECOVERY jitter.
                    if (readFlow && completedMetadata >= 55 && stallAfterPostScan.load()) continue;
                    if (!readFlow) usleep(metadataRequests.load() == 20 ? 80000 : 50000);
                    const char ack = char(MfcProtocol::Ack); (void)::write(lifecyclePtyMaster, &ack, 1);
                    const QByteArray packet = response(commandClass, attribute, data);
                    (void)::write(lifecyclePtyMaster, packet.constData(), packet.size());
                    if (readFlow && completedMetadata >= 55) ++postScanReadFlows;
                }
            }
        });
        QTemporaryDir lifecycleLogDirectory;
        QStringList lifecycleErrors;
        bool lifecycleMetadataComplete = false;
        bool watchdogCancelledBeforeStall = false;
        bool watchdogTriggeredAfterStall = false;
        {
            MonitoringController lifecycleController(500, 10.0, 20.0, 0.01, nullptr,
                lifecycleLogDirectory.path(), lifecycleDevices, lifecyclePort, 19200, 120, 160, 0,
                3, 3000, 0, 20, 3000, 20);
            QObject::connect(&lifecycleController, &MonitoringController::errorOccurred,
                [&lifecycleErrors](const QString &message) { lifecycleErrors.append(message); });
            QElapsedTimer waitForPolling; waitForPolling.start();
            while (!lifecycleController.monitoring() && waitForPolling.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            lifecycleController.verifyDeviceInformation();
            QElapsedTimer waitForScan; waitForScan.start();
            while (waitForScan.elapsed() < 7000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
                const QVariantList states = lifecycleController.deviceInfo().value("devices").toList();
                lifecycleMetadataComplete = states.size() == 5 && std::all_of(states.cbegin(), states.cend(),
                    [](const QVariant &item) { return item.toMap().value("metadataVerificationComplete").toBool(); });
                if (lifecycleMetadataComplete && metadataRequests.load() >= 55 && postScanReadFlows.load() > 0) break;
            }
            QElapsedTimer armDelivery; armDelivery.start();
            while (armDelivery.elapsed() < 200)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            watchdogCancelledBeforeStall = std::any_of(lifecycleErrors.cbegin(), lifecycleErrors.cend(),
                [](const QString &message) { return message.contains(QStringLiteral("COMMUNICATION_WORKER_STALLED")); });
            stallAfterPostScan.store(true);
            QElapsedTimer waitForRealStall; waitForRealStall.start();
            while (waitForRealStall.elapsed() < 2800)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            watchdogTriggeredAfterStall = std::any_of(lifecycleErrors.cbegin(), lifecycleErrors.cend(),
                [](const QString &message) { return message.contains(QStringLiteral("COMMUNICATION_WORKER_STALLED")); });
            ok &= require(!watchdogCancelledBeforeStall && watchdogTriggeredAfterStall,
                          "post-DeviceInfo 首个 READ_FLOW 成功后，真实 polling stall 仍必须触发 watchdog");
        }
        lifecyclePeerStop.store(true); lifecyclePeer.join(); ::close(lifecyclePtyMaster);
        ok &= require(lifecycleMetadataComplete && metadataRequests.load() >= 55
                      && postScanReadFlows.load() > 0 && readFlowDuringMetadata.load() == 0
                      && !watchdogCancelledBeforeStall && watchdogTriggeredAfterStall,
                      "运行中超过 2 秒且含 BUS_RECOVERY 抖动的五机 DeviceInfo 必须完整、独占总线且不得触发 WORKER_WATCHDOG");
    } else if (lifecyclePtyMaster >= 0) close(lifecyclePtyMaster);
#endif

// CS200 replies use address 0x00.  Two devices deliberately return different
// Target/Calibration full scales; each result must remain attached to the
// address in the pending request, not to response.address or scan order.
#ifdef Q_OS_LINUX
    const int attributionPtyMaster = posix_openpt(O_RDWR | O_NOCTTY);
    const bool attributionPtyReady = attributionPtyMaster >= 0
        && grantpt(attributionPtyMaster) == 0 && unlockpt(attributionPtyMaster) == 0;
    ok &= require(attributionPtyReady, "应能创建 metadata 地址归属伪串口");
    if (attributionPtyReady) {
        const QString attributionPort = QString::fromLocal8Bit(ptsname(attributionPtyMaster));
        const auto makeDevice = [](quint8 address, int channel, const QString &gas, quint16 code, double scale) {
            MfcDeviceConfig device;
            device.logicalChannel = channel; device.address = address;
            device.displayName = QStringLiteral("MFC%1").arg(channel); device.gasType = gas;
            device.function = QStringLiteral("test"); device.unit = QStringLiteral("sccm");
            device.fullScale = scale; device.maximumSetpoint = scale;
            device.expectedGasCode = code; device.expectedDeviceFullScaleSccm = scale;
            device.addressConfirmed = true; return device;
        };
        const auto device32 = makeDevice(32, 1, QStringLiteral("空气"), 8, 1000.0);
        const auto device34 = makeDevice(34, 3, QStringLiteral("氮气"), 13, 50.0);
        MfcManager::Settings attributionSettings;
        attributionSettings.serialPort = attributionPort; attributionSettings.ackTimeoutMs = 100;
        attributionSettings.responseTimeoutMs = 100; attributionSettings.retryCount = 0;
        attributionSettings.metadataInterRequestDelayMs = 20;
        QStringList attributionTrace;
        std::thread attributionPeer([attributionPtyMaster] {
            const auto response = [](quint8 commandClass, quint8 attribute, const QByteArray &data) {
                QByteArray packet = QByteArray::fromHex("000280"); // response address is always 0x00
                packet.append(char(3 + data.size())); packet.append(char(commandClass)); packet.append(char(0x01));
                packet.append(char(attribute)); packet.append(data); packet.append(char(0x00));
                packet.append(char(MfcProtocol::checksum(packet))); return packet;
            };
            // Two connection probes and 11 metadata reads for each address.
            for (int transaction = 0; transaction < 24; ++transaction) {
                char request[64]{};
                const ssize_t size = ::read(attributionPtyMaster, request, sizeof(request));
                if (size < 7) break;
                const quint8 address = static_cast<quint8>(request[0]);
                const quint8 commandClass = static_cast<quint8>(request[4]);
                const quint8 attribute = static_cast<quint8>(request[6]);
                const bool is32 = address == 32;
                QByteArray data;
                if (commandClass == 0x68 && attribute == 0xB9) data = QByteArray::fromHex("0040");
                else if (commandClass == 0x64 && attribute == 0x04) data = QByteArray("CS200-A");
                else if (commandClass == 0x64 && attribute == 0x07) data = is32 ? QByteArray("TEST-32") : QByteArray("TEST-34");
                else if (commandClass == 0x66 && attribute == 0x01) data = is32 ? QByteArray("Air") : QByteArray("N2");
                else if (commandClass == 0x66 && attribute == 0x02) data = is32 ? QByteArray::fromHex("0800") : QByteArray::fromHex("0D00");
                else if (commandClass == 0x66 && attribute == 0x03) data = is32 ? QByteArray::fromHex("E803") : QByteArray::fromHex("3200");
                else if (commandClass == 0x66 && attribute == 0x06) data = is32 ? QByteArray("Air") : QByteArray("N2");
                else if (commandClass == 0x66 && attribute == 0x07) data = is32 ? QByteArray::fromHex("0800") : QByteArray::fromHex("0D00");
                else if (commandClass == 0x66 && attribute == 0x08) data = is32 ? QByteArray::fromHex("E803") : QByteArray::fromHex("3200");
                else if (commandClass == 0x66 && attribute == 0x04) data = QByteArray::fromHex("00000100");
                else if (commandClass == 0x03 && attribute == 0x01) data = is32 ? QByteArray::fromHex("20") : QByteArray::fromHex("22");
                else if (commandClass == 0x03 && attribute == 0x02) data = QByteArray::fromHex("004B");
                const char ack = char(MfcProtocol::Ack);
                (void)::write(attributionPtyMaster, &ack, 1);
                const QByteArray packet = response(commandClass, attribute, data);
                (void)::write(attributionPtyMaster, packet.constData(), packet.size());
            }
        });
        MfcManager attributionManager({device32, device34}, attributionSettings,
            [&attributionTrace](const QString &line) { attributionTrace.append(line); });
        const bool attributionConnected = attributionManager.connectBus();
        QString attributionError;
        const bool attributionVerified = attributionConnected
            && attributionManager.verifyDeviceInformation(&attributionError);
        const auto attributionStates = attributionManager.devices();
        attributionManager.disconnectBus(); attributionPeer.join(); ::close(attributionPtyMaster);
        const QVariantMap state32 = attributionStates[0].toVariantMap();
        const QVariantMap state34 = attributionStates[1].toVariantMap();
        const QVariantMap results32 = state32.value("metadataResults").toMap();
        const QVariantMap results34 = state34.value("metadataResults").toMap();
        const QString joinedAttributionTrace = attributionTrace.join('\n');
        ok &= require(attributionVerified
                      && results32.value("Target Full Scale").toMap().value("reported").toString() == QStringLiteral("1000")
                      && results34.value("Target Full Scale").toMap().value("reported").toString() == QStringLiteral("50")
                      && results32.value("Calibration Full Scale").toMap().value("reported").toString() == QStringLiteral("1000")
                      && results34.value("Calibration Full Scale").toMap().value("reported").toString() == QStringLiteral("50")
                      && results32.value("RS485 Address").toMap().value("status").toString() == QStringLiteral("MATCH")
                      && results34.value("RS485 Address").toMap().value("status").toString() == QStringLiteral("MATCH")
                      && joinedAttributionTrace.contains(QStringLiteral("REQUEST_ADDRESS=32 PENDING_ADDRESS=32 STATE_DESTINATION_ADDRESS=32 RESPONSE_ADDRESS=0x00"))
                      && joinedAttributionTrace.contains(QStringLiteral("REQUEST_ADDRESS=34 PENDING_ADDRESS=34 STATE_DESTINATION_ADDRESS=34 RESPONSE_ADDRESS=0x00")),
                      "response Address=0x00 时 metadata 只能更新 pending request 对应的设备状态");
    } else if (attributionPtyMaster >= 0) close(attributionPtyMaster);
#endif

// A legal LEN=3 metadata frame is an unsupported/empty response, not a
// configuration mismatch.  The PTY also verifies the metadata-only spacing
// and the raw evidence written by the manager.
#ifdef Q_OS_LINUX
    const int metadataPtyMaster = posix_openpt(O_RDWR | O_NOCTTY);
    const bool metadataPtyReady = metadataPtyMaster >= 0
        && grantpt(metadataPtyMaster) == 0 && unlockpt(metadataPtyMaster) == 0;
    ok &= require(metadataPtyReady, "应能创建 metadata 空载荷伪串口");
    if (metadataPtyReady) {
        const QString metadataPort = QString::fromLocal8Bit(ptsname(metadataPtyMaster));
        MfcDeviceConfig metadataDevice;
        metadataDevice.logicalChannel = 1; metadataDevice.address = 32; metadataDevice.displayName = QStringLiteral("MFC1");
        metadataDevice.gasType = QStringLiteral("空气"); metadataDevice.function = QStringLiteral("发生器空气");
        metadataDevice.unit = QStringLiteral("L/min"); metadataDevice.fullScale = 1.0; metadataDevice.maximumSetpoint = 1.0;
        metadataDevice.expectedGasCode = 8; metadataDevice.expectedDeviceFullScaleSccm = 1000.0; metadataDevice.addressConfirmed = true;
        MfcManager::Settings metadataSettings;
        metadataSettings.serialPort = metadataPort; metadataSettings.ackTimeoutMs = 100;
        metadataSettings.responseTimeoutMs = 100; metadataSettings.retryCount = 0;
        metadataSettings.metadataInterRequestDelayMs = 20;
        QStringList metadataTrace;
        QList<qint64> metadataRequestTimes;
        std::thread metadataPeer([metadataPtyMaster, &metadataRequestTimes] {
            const auto started = std::chrono::steady_clock::now();
            const auto response = [](quint8 commandClass, quint8 attribute, const QByteArray &data) {
                QByteArray packet = QByteArray::fromHex("000280");
                packet.append(char(3 + data.size())); packet.append(char(commandClass)); packet.append(char(0x01));
                packet.append(char(attribute)); packet.append(data); packet.append(char(0x00));
                packet.append(char(MfcProtocol::checksum(packet))); return packet;
            };
            // One connection READ_FLOW followed by eleven metadata fields,
            // including the three read-only Calibration Gas attributes.
            for (int transaction = 0; transaction < 12; ++transaction) {
                char request[64]{};
                const ssize_t size = ::read(metadataPtyMaster, request, sizeof(request));
                if (size < 7) break;
                const quint8 commandClass = static_cast<quint8>(request[4]);
                const quint8 attribute = static_cast<quint8>(request[6]);
                if (transaction > 0) metadataRequestTimes.append(static_cast<qint64>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
                QByteArray data;
                if (commandClass == 0x68 && attribute == 0xB9) data = QByteArray::fromHex("0040");
                else if (commandClass == 0x64 && attribute == 0x04) data = QByteArray("CS200-A");
                else if (commandClass == 0x64 && attribute == 0x07) data = QByteArray("TEST-32");
                else if (commandClass == 0x66 && attribute == 0x01) data = QByteArray("Air");
                else if (commandClass == 0x66 && attribute == 0x02) data.clear(); // LEN=3, no UINT16 payload
                else if (commandClass == 0x66 && attribute == 0x03) data = QByteArray::fromHex("E803");
                else if (commandClass == 0x66 && attribute == 0x06) data = QByteArray("Air");
                else if (commandClass == 0x66 && attribute == 0x07) data = QByteArray::fromHex("0800");
                else if (commandClass == 0x66 && attribute == 0x08) data = QByteArray::fromHex("E803");
                else if (commandClass == 0x66 && attribute == 0x04) data = QByteArray::fromHex("00000100");
                else if (commandClass == 0x03 && attribute == 0x01) data = QByteArray::fromHex("20");
                else if (commandClass == 0x03 && attribute == 0x02) data = QByteArray::fromHex("004B");
                const char ack = char(MfcProtocol::Ack);
                (void)::write(metadataPtyMaster, &ack, 1);
                const QByteArray packet = response(commandClass, attribute, data);
                (void)::write(metadataPtyMaster, packet.constData(), packet.size());
            }
        });
        MfcManager metadataManager({metadataDevice}, metadataSettings,
            [&metadataTrace](const QString &line) { metadataTrace.append(line); });
        const bool metadataConnected = metadataManager.connectBus();
        QString metadataError;
        const bool metadataVerified = metadataConnected && metadataManager.verifyDeviceInformation(&metadataError);
        const auto metadataState = metadataManager.devices().first().toVariantMap();
        metadataManager.disconnectBus(); metadataPeer.join(); ::close(metadataPtyMaster);
        const QVariantMap metadataResults = metadataState.value("metadataResults").toMap();
        const QVariantMap gasCodeResult = metadataResults.value("Target Gas Code").toMap();
        const QVariantMap scaleResult = metadataResults.value("Target Full Scale").toMap();
        const QVariantMap calibrationCodeResult = metadataResults.value("Calibration Gas Code").toMap();
        const QVariantMap calibrationScaleResult = metadataResults.value("Calibration Full Scale").toMap();
        bool spacingOk = metadataRequestTimes.size() == 11;
        for (int i = 1; i < metadataRequestTimes.size(); ++i)
            spacingOk &= metadataRequestTimes[i] - metadataRequestTimes[i - 1] >= 15;
        const QString joinedTrace = metadataTrace.join('\n');
        ok &= require(!metadataVerified && metadataError.contains(QStringLiteral("无法完成核验"))
                      && gasCodeResult.value("status").toString() == QStringLiteral("UNSUPPORTED_OR_EMPTY_RESPONSE")
                      && gasCodeResult.value("reported").toString() == QStringLiteral("--")
                      && scaleResult.value("status").toString() == QStringLiteral("MATCH")
                      && calibrationCodeResult.value("reported").toString() == QStringLiteral("8")
                      && calibrationScaleResult.value("reported").toString() == QStringLiteral("1000")
                      && !metadataState.value("verificationMatches").toBool(),
                      "空 Gas Code payload 必须显示为未返回数据，不能显示配置不一致或回填 Expected");
        ok &= require(spacingOk && joinedTrace.contains(QStringLiteral("[META][32][Target Gas Code]"))
                      && joinedTrace.contains(QStringLiteral("RX_DATA_LENGTH=3"))
                      && joinedTrace.contains(QStringLiteral("PAYLOAD="))
                      && joinedTrace.contains(QStringLiteral("REQUEST_ADDRESS=32 PENDING_ADDRESS=32 STATE_DESTINATION_ADDRESS=32"))
                      && joinedTrace.contains(QStringLiteral("RESPONSE_ADDRESS=0x00"))
                      && joinedTrace.contains(QStringLiteral("TX=20 02 80 03 66 01 02")),
                      "metadata 必须记录 pending 地址归属、0x00 响应地址、完整 TX/RX 证据，并在完整事务之后至少等待 20 ms");
    } else if (metadataPtyMaster >= 0) close(metadataPtyMaster);
#endif

    const auto normal = FlowDeviationCalculator::calculate(20.0, 20.8, 5.0, 10.0, 0.02);
    ok &= require(normal.status == FlowStatus::Normal && normal.available, "4% 偏差应为正常");
    const auto warning = FlowDeviationCalculator::calculate(20.0, 21.2, 5.0, 10.0, 0.02);
    ok &= require(warning.status == FlowStatus::Warning, "6% 偏差应为警告");
    const auto critical = FlowDeviationCalculator::calculate(20.0, 22.2, 5.0, 10.0, 0.02);
    ok &= require(critical.status == FlowStatus::Critical, "11% 偏差应为严重异常");
    const auto zero = FlowDeviationCalculator::calculate(0.0, 0.03, 5.0, 10.0, 0.02);
    ok &= require(zero.status == FlowStatus::Warning && !zero.available, "零设定值不得计算百分比且应识别非预期流量");
    const auto negativeSetpoint = FlowDeviationCalculator::calculate(-1.0, 0.03, 5.0, 10.0, 0.02);
    ok &= require(!negativeSetpoint.available,
                  "负设定值同样不得执行百分比偏差计算");

    AlarmService alarms;
    GasChannel channel;
    channel.id = "oxidation"; channel.nameChinese = QStringLiteral("氧化空气");
    channel.setValue = 0.6; channel.realValue = 0.52; channel.unit = "L/min";
    channel.targetFlow = channel.setValue;
    channel.targetFlowAvailable = true;
    channel.communicationStateCode = 1;
    channel.deviationPercent = -13.33; channel.deviationAvailable = true; channel.status = FlowStatus::Critical;
    alarms.processFlows({channel});
    ok &= require(alarms.records().size() == 1, "异常应生成一条报警");
    alarms.processFlows({channel});
    ok &= require(alarms.records().size() == 1, "持续异常不得重复生成报警");
    channel.status = FlowStatus::Normal; channel.realValue = 0.6; channel.deviationPercent = 0;
    alarms.processFlows({channel});
    ok &= require(alarms.records().first().recoveryTime.isValid(), "恢复正常后应记录恢复时间");

    AlarmService communicationAlarms;
    channel.id = "mfc_34"; channel.nameChinese = QStringLiteral("MFC 3"); channel.address = 34;
    channel.communicationStateCode = 2; channel.status = FlowStatus::Warning;
    communicationAlarms.processFlows({channel});
    communicationAlarms.processFlows({channel});
    ok &= require(communicationAlarms.records().size() == 1
                  && communicationAlarms.activeAlarmMaps().size() == 1
                  && communicationAlarms.records().first().repeatCount == 2,
                  "持续通信异常必须维护一条活动报警及重复次数");
    channel.communicationStateCode = 4;
    communicationAlarms.processFlows({channel});
    ok &= require(communicationAlarms.activeAlarmMaps().size() == 1,
                  "通信恢复确认期间不得过早清除活动报警");
    channel.communicationStateCode = 1; channel.status = FlowStatus::Normal;
    communicationAlarms.processFlows({channel});
    ok &= require(communicationAlarms.activeAlarmMaps().isEmpty()
                  && communicationAlarms.records().first().recoveryTime.isValid(),
                  "通信状态恢复正常后必须自动关闭活动报警并保留历史");

    AlarmService escalationAlarms;
    channel.status = FlowStatus::Warning; channel.deviationPercent = -7.5;
    escalationAlarms.processFlows({channel});
    channel.status = FlowStatus::Critical; channel.deviationPercent = -14.0;
    escalationAlarms.processFlows({channel});
    ok &= require(escalationAlarms.records().size() == 2, "警告升级为严重时应生成新的严重报警");
    ok &= require(escalationAlarms.records().first().recoveryTime.isValid()
                  && escalationAlarms.records().last().severity == AlarmSeverity::Critical,
                  "升级时旧警告应结束且严重报警应保持活动");

    QTemporaryDir pointDirectory;
    ok &= require(pointDirectory.isValid(), "应能创建运行点测试目录");
    QString duplicatedId;
    {
        OperatingPointService points(pointDirectory.path());
        const auto initial = points.points();
        ok &= require(initial.isEmpty(), "空配置不得伪造待配置运行点");
        OperatingPoint customer;
        customer.name = QStringLiteral("客户运行点 1");
        customer.type = OperatingPointType::Customer;
        customer.mfcSetpoints = {{32, 0.1}, {33, 0.2}, {34, 0.3}, {35, 0.4}, {36, 0.5}};
        ok &= require(points.addOrUpdate(customer), "应能创建真实客户运行点");
        const auto created = points.points().first();
        const auto duplicated = points.duplicate(created.id);
        duplicatedId = duplicated.id;
        ok &= require(!duplicated.id.isEmpty() && !duplicated.readOnly,
                      "客户运行点应可复制");
    }
    {
        OperatingPointService reloaded(pointDirectory.path());
        const auto duplicated = reloaded.point(duplicatedId);
        ok &= require(!duplicated.id.isEmpty(), "用户运行点重建服务后仍应存在");
        ok &= require(duplicated.hasValidFlowValues(), "用户运行点重启后应保留有效的 MFC 流量设定值");
    }

    QTemporaryDir capacityDirectory;
    {
        OperatingPointService points(capacityDirectory.path());
        for (int i = 1; i <= OperatingPointService::MaximumCustomerPoints; ++i) {
            OperatingPoint point;
            point.name = QStringLiteral("客户运行点 %1").arg(i);
            for (int address = 0x20; address <= 0x24; ++address) point.mfcSetpoints[address] = i;
            ok &= require(points.addOrUpdate(point), "未满 6 个槽位时应允许保存客户运行点");
        }
        ok &= require(points.customerPointFull(), "6 个客户运行点占满后必须进入选择覆盖流程");
        OperatingPoint overflow;
        overflow.name = QStringLiteral("不得自动覆盖");
        ok &= require(!points.addOrUpdate(overflow), "客户运行点已满时不得自动新增或覆盖");
    }

    OperatingPoint validPoint;
    validPoint.name = QStringLiteral("校验测试");
    for (int address = 0x20; address <= 0x24; ++address) validPoint.mfcSetpoints[address] = 1.0;
    ok &= require(validPoint.hasValidFlowValues(), "合法 MFC 目标流量应通过模型校验");
    validPoint.mfcSetpoints[0x22] = -1.0;
    ok &= require(!validPoint.hasValidFlowValues(), "负的 MFC 目标流量必须被模型拒绝");
    validPoint.mfcSetpoints[0x22] = 1.0;

    QTemporaryDir fixedPointDirectory;
    {
        OperatingPointService points(fixedPointDirectory.path(), nullptr, fixedDevices);
        OperatingPoint halfScalePoint;
        halfScalePoint.name = QStringLiteral("新量程 50%FS");
        halfScalePoint.mfcSetpoints = halfScaleTargets;
        ok &= require(points.addOrUpdate(halfScalePoint),
                      "Customer Operating Point 必须接受五台新量程的 50%FS 目标");
        OperatingPoint retiredRangePoint = halfScalePoint;
        retiredRangePoint.name = QStringLiteral("旧 MFC5 量程");
        retiredRangePoint.mfcSetpoints[36] = 10.0;
        ok &= require(!points.addOrUpdate(retiredRangePoint),
                      "Customer Operating Point 不得保存超出新 MFC5 4 L/min 量程的旧目标");
    }

    QTemporaryDir legacyDirectory;
    ok &= require(legacyDirectory.isValid(), "应能创建旧运行点迁移测试目录");
    {
        QJsonObject legacy{{"id", "legacy-customer"}, {"name", QStringLiteral("旧客户运行点")},
                           {"type", "Customer"}, {"fuelGas", 25.0}, {"mixingGas", 20.0},
                           {"oxidationAir", 0.5}, {"dilutionAir", 5.0}, {"quenchGas", 2.0}};
        QFile file(legacyDirectory.path() + "/operating_points.json");
        ok &= require(file.open(QIODevice::WriteOnly), "应能写入旧格式运行点测试数据");
        file.write(QJsonDocument(QJsonArray{legacy}).toJson());
    }
    {
        OperatingPointService migrated(legacyDirectory.path());
        const auto point = migrated.point(QStringLiteral("legacy-customer"));
        ok &= require(point.requiresAddressMapping && !point.hasValidFlowValues(),
                      "旧气体字段不得在未确认时自动猜测 MFC 地址映射");
        const auto json = point.toJson();
        const auto legacyValues = json.value("legacyUnmappedValues").toObject();
        ok &= require(legacyValues.value("fuelGas").toDouble() == 25.0
                      && legacyValues.value("quenchGas").toDouble() == 2.0,
                      "未确认地址映射时必须原样保留旧版气体设定值，不能丢失数据");
    }
    {
        OperatingPointService reloaded(legacyDirectory.path());
        const auto point = reloaded.point(QStringLiteral("legacy-customer"));
        ok &= require(point.requiresAddressMapping
                      && point.legacyUnmappedValues.value("mixingGas").toDouble() == 20.0,
                      "旧运行点迁移标记和原始设定值在再次启动后仍必须保留");
    }

    QTemporaryDir logDirectory;
    {
        DataLoggingService logging(logDirectory.path());
        QList<GasChannel> logChannels;
        for (int i = 0; i < 5; ++i) {
            GasChannel item;
            item.nameChinese = QStringLiteral("测试通道%1").arg(i + 1);
            item.setValue = i + 1;
            item.realValue = i + 1.01;
            item.deviationPercent = 1.0;
            item.status = FlowStatus::Normal;
            logChannels.append(item);
        }
        ok &= require(logging.start(), "流量日志应能在应用数据目录启动");
        logging.logFlows(logChannels, DeviceStatus::Monitoring, FlameStatus::Normal);
        logging.stop();
        AlarmRecord logAlarm;
        logAlarm.timestamp = QDateTime::currentDateTime();
        logAlarm.severity = AlarmSeverity::Warning;
        logAlarm.channelName = QStringLiteral("氧化空气");
        logAlarm.parameter = QStringLiteral("流量");
        logging.logAlarm(logAlarm, QStringLiteral("未恢复"));
    }
    QFile flowLog(logDirectory.path() + "/logs/" + QDate::currentDate().toString("yyyy-MM-dd") + "_flow.csv");
    ok &= require(flowLog.open(QIODevice::ReadOnly) && flowLog.readAll().contains(QStringLiteral("MFC地址").toUtf8()),
                  "每日流量 CSV 应使用支持 N 路的地址化长表格式");
    QFile alarmLog(logDirectory.path() + "/logs/" + QDate::currentDate().toString("yyyy-MM-dd") + "_alarm.csv");
    ok &= require(alarmLog.open(QIODevice::ReadOnly) && alarmLog.readAll().contains(QStringLiteral("氧化空气").toUtf8()),
                  "每日报警 CSV 应记录报警事件");

    if (ok) std::cout << "全部核心测试通过\n";
    return ok ? 0 : 1;
}
