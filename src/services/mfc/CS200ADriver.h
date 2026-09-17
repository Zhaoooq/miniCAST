#pragma once

#include "DeviceDiscovery.h"
#include "MfcProtocol.h"
#include "MfcTypes.h"
#include "SerialTransport.h"
#include <functional>
#include <memory>

class CS200ADriver
{
public:
    using TraceSink = SerialTransport::TraceSink;
    explicit CS200ADriver(TraceSink trace = {});
    CS200ADriver(SerialTransport &sharedTransport, quint8 address, TraceSink trace = {});

    bool discoverAndConnect(quint8 preferredAddress = 0x20, qint32 preferredBaud = 19200);
    void disconnect();
    bool isConnected() const;
    const MfcDeviceInfo &deviceInfo() const { return m_info; }
    MfcReading readFlow();
    MfcControlMode readCurrentControlMode();
    bool readHoldFollow();
    quint16 readDigitalSetpoint();
    quint16 readActiveSetpoint();
    void setCurrentControlMode(MfcControlMode mode);
    void setHoldFollow(bool follow);
    void setDigitalSetpoint(quint16 raw);
    QString readModelIdentifier();
    QString readSerialNumber();
    QString readTargetGasName();
    quint16 readTargetGasCode();
    quint16 readTargetGasFullScale();
    QString readCalibrationGasName();
    quint16 readCalibrationGasCode();
    quint16 readCalibrationGasFullScale();
    double readConversionFactor();
    quint8 readRs485MacAddress();
    quint16 readBaudRate();
    void setTransactionOptions(int ackTimeoutMs, int responseTimeoutMs, int retries);
    void setEngineeringScale(double fullScale, const QString &unit);

private:
    SerialTransport &transport();
    MfcProtocol::Response readAttribute(quint8 commandClass, quint8 attribute);
    quint16 readUInt16(quint8 commandClass, quint8 attribute);
    quint8 readUInt8(quint8 commandClass, quint8 attribute);
    void writeAttribute(quint8 commandClass, quint8 attribute, const QByteArray &data);
    std::unique_ptr<SerialTransport> m_ownedTransport;
    SerialTransport *m_transport{nullptr};
    MfcDeviceInfo m_info;
    TraceSink m_trace;
    int m_ackTimeoutMs{40};
    int m_responseTimeoutMs{180};
    int m_retries{1};
};
