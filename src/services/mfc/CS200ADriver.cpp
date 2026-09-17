#include "CS200ADriver.h"
#include "MfcProtocol.h"
#include <QThread>
#include <array>
#include <cmath>

CS200ADriver::CS200ADriver(TraceSink trace)
    : m_ownedTransport(std::make_unique<SerialTransport>(trace)),
      m_transport(m_ownedTransport.get()), m_trace(std::move(trace)) {}

CS200ADriver::CS200ADriver(SerialTransport &sharedTransport, quint8 address, TraceSink trace)
    : m_transport(&sharedTransport), m_trace(std::move(trace))
{
    if (address < 0x20 || address > 0x5f) throw MfcProtocol::Error("invalid MFC address");
    m_info.address = address;
}

SerialTransport &CS200ADriver::transport() { return *m_transport; }

bool CS200ADriver::discoverAndConnect(quint8 preferredAddress, qint32 preferredBaud)
{
    disconnect();
    const QList<SerialPortDescriptor> ports = DeviceDiscovery::candidates();
    QList<qint32> baudRates{preferredBaud};
    for (const qint32 baud : {19200, 9600, 4800, 2400, 1200})
        if (!baudRates.contains(baud)) baudRates.append(baud);
    QList<quint8> addresses{preferredAddress};
    for (int address = 0x20; address <= 0x5f; ++address)
        if (address != preferredAddress) addresses.append(static_cast<quint8>(address));

    for (const auto &port : ports) {
        for (qsizetype baudIndex = 0; baudIndex < baudRates.size(); ++baudIndex) {
            try {
                transport().open(port.device, baudRates[baudIndex]);
            } catch (const std::exception &error) {
                if (m_trace) m_trace(QStringLiteral("OPEN ERROR %1 %2").arg(port.device, QString::fromUtf8(error.what())));
                break;
            }
            // A full address sweep is performed at the configured/default baud. At
            // fallback baud rates probe the preferred address to bound scan time.
            const qsizetype addressCount = baudIndex == 0 ? addresses.size() : 1;
            for (qsizetype addressIndex = 0; addressIndex < addressCount; ++addressIndex) {
                m_info.address = addresses[addressIndex];
                try {
                    const auto response = readAttribute(0x68, 0xB9);
                    (void)MfcProtocol::readUInt16Le(response.data);
                    m_info.port = port.device;
                    m_info.portDescription = port.description;
                    m_info.portManufacturer = port.manufacturer;
                    m_info.usbSerialNumber = port.serialNumber;
                    m_info.vendorId = port.vendorId;
                    m_info.productId = port.productId;
                    m_info.baudRate = baudRates[baudIndex];
                    if (m_trace) m_trace(QStringLiteral("DISCOVERED port=%1 address=0x%2 baud=%3")
                        .arg(port.device).arg(m_info.address, 2, 16, QLatin1Char('0')).arg(m_info.baudRate));
                    return true;
                } catch (const std::exception &) {
                    // A device is accepted only after a valid ACK and protocol frame.
                }
            }
            transport().close();
        }
    }
    return false;
}

void CS200ADriver::disconnect() { if (m_ownedTransport) m_transport->close(); }
bool CS200ADriver::isConnected() const { return m_transport && m_transport->isOpen(); }

MfcProtocol::Response CS200ADriver::readAttribute(quint8 commandClass, quint8 attribute)
{
    const auto request = MfcProtocol::makeRequest(m_info.address, MfcProtocol::ReadService,
                                                   commandClass, 0x01, attribute);
    return MfcProtocol::parseResponse(transport().transaction(request, m_ackTimeoutMs, m_responseTimeoutMs, m_retries), commandClass, 0x01, attribute);
}

quint16 CS200ADriver::readUInt16(quint8 commandClass, quint8 attribute)
{
    const QByteArray payload = readAttribute(commandClass, attribute).data;
    // A structurally valid class/instance/attribute response with LEN=3 is
    // evidence that this firmware returned no value.  Preserve that meaning
    // for metadata callers instead of turning it into a vague mismatch.
    if (payload.isEmpty())
        throw MfcProtocol::Error("EMPTY_PAYLOAD: UINT16 attribute returned no data");
    if (payload.size() < 2)
        throw MfcProtocol::Error("UINT16_PAYLOAD_TOO_SHORT");
    return MfcProtocol::readUInt16Le(payload);
}
quint8 CS200ADriver::readUInt8(quint8 commandClass, quint8 attribute)
{
    const QByteArray payload = readAttribute(commandClass, attribute).data;
    if (payload.isEmpty())
        throw MfcProtocol::Error("EMPTY_PAYLOAD: UINT8 attribute returned no data");
    if (payload.size() != 1)
        throw MfcProtocol::Error("UINT8_PAYLOAD_LENGTH_INVALID");
    return MfcProtocol::readUInt8(payload);
}
void CS200ADriver::writeAttribute(quint8 commandClass, quint8 attribute, const QByteArray &data)
{
    const auto request = MfcProtocol::makeRequest(m_info.address, MfcProtocol::WriteService,
        commandClass, 0x01, attribute, data);
    transport().transactionAck(request, m_ackTimeoutMs, m_retries);
}
MfcControlMode CS200ADriver::readCurrentControlMode()
{
    const auto data = readAttribute(0x69, 0x03).data;
    if (data.size() != 1) throw MfcProtocol::Error("Current CM response size invalid");
    const auto value = static_cast<quint8>(data[0]);
    return value <= 3 ? static_cast<MfcControlMode>(value) : MfcControlMode::Unknown;
}
bool CS200ADriver::readHoldFollow()
{
    const auto data = readAttribute(0x69, 0x05).data;
    if (data.size() != 1 || static_cast<quint8>(data[0]) > 1)
        throw MfcProtocol::Error("Hold/Follow response invalid");
    return static_cast<quint8>(data[0]) == 1;
}
quint16 CS200ADriver::readDigitalSetpoint() { return readUInt16(0x69, 0xA4); }
quint16 CS200ADriver::readActiveSetpoint() { return readUInt16(0x69, 0xA5); }
void CS200ADriver::setCurrentControlMode(MfcControlMode mode)
{
    if (mode != MfcControlMode::Digital) throw MfcProtocol::Error("only Digital CM is allowed");
    writeAttribute(0x69, 0x03, QByteArray(1, char(1)));
}
void CS200ADriver::setHoldFollow(bool follow) { writeAttribute(0x69, 0x05, QByteArray(1, char(follow ? 1 : 0))); }
void CS200ADriver::setDigitalSetpoint(quint16 raw)
{
    if (raw < 0x4000 || raw > 0xC000) throw MfcProtocol::Error("Digital Setpoint outside 0..100% FS");
    writeAttribute(0x69, 0xA4, MfcProtocol::uint16Le(raw));
}
QString CS200ADriver::readModelIdentifier() { return MfcProtocol::decodeText(readAttribute(0x64, 0x04).data); }
QString CS200ADriver::readSerialNumber() { return MfcProtocol::decodeText(readAttribute(0x64, 0x07).data); }
QString CS200ADriver::readTargetGasName() { return MfcProtocol::decodeText(readAttribute(0x66, 0x01).data); }
quint16 CS200ADriver::readTargetGasCode() { return readUInt16(0x66, 0x02); }
quint16 CS200ADriver::readTargetGasFullScale() { return readUInt16(0x66, 0x03); }
QString CS200ADriver::readCalibrationGasName() { return MfcProtocol::decodeText(readAttribute(0x66, 0x06).data); }
quint16 CS200ADriver::readCalibrationGasCode() { return readUInt16(0x66, 0x07); }
quint16 CS200ADriver::readCalibrationGasFullScale() { return readUInt16(0x66, 0x08); }
double CS200ADriver::readConversionFactor() { return MfcProtocol::decodeFixed16_16(readAttribute(0x66, 0x04).data); }
quint8 CS200ADriver::readRs485MacAddress() { return readUInt8(0x03, 0x01); }
quint16 CS200ADriver::readBaudRate() { return readUInt16(0x03, 0x02); }
MfcReading CS200ADriver::readFlow()
{
    MfcReading reading;
    reading.timestamp = QDateTime::currentDateTime();
    reading.flowRaw = readUInt16(0x68, 0xB9);
    const double fraction = MfcProtocol::decodeUfrac16(reading.flowRaw);
    reading.flowPercent = fraction * 100.0;
    reading.flowValue = fraction * m_info.fullScale;
    reading.communicationOk = true;
    return reading;
}

void CS200ADriver::setTransactionOptions(int ackTimeoutMs, int responseTimeoutMs, int retries)
{
    m_ackTimeoutMs = qBound(10, ackTimeoutMs, 1000);
    m_responseTimeoutMs = qBound(100, responseTimeoutMs, 5000);
    m_retries = qBound(0, retries, 1);
}

void CS200ADriver::setEngineeringScale(double fullScale, const QString &unit)
{
    m_info.fullScale = qMax(0.0, fullScale);
    m_info.fullScaleUnit = unit;
}
