#include "MfcProtocol.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace MfcProtocol {

QString parserStateName(ParserState state)
{
    switch (state) {
    case ParserState::WaitStart00: return QStringLiteral("WAIT_START_00");
    case ParserState::Expect02: return QStringLiteral("EXPECT_02");
    case ParserState::ExpectService80: return QStringLiteral("EXPECT_SERVICE_80");
    case ParserState::ReadLength: return QStringLiteral("READ_LENGTH");
    case ParserState::ReadClass: return QStringLiteral("READ_CLASS");
    case ParserState::ExpectInstance01: return QStringLiteral("EXPECT_INSTANCE_01");
    case ParserState::ReadAttribute: return QStringLiteral("READ_ATTRIBUTE");
    case ParserState::ReadPayload: return QStringLiteral("READ_PAYLOAD");
    case ParserState::ExpectTrailing00: return QStringLiteral("EXPECT_TRAILING_00");
    case ParserState::ReadChecksum: return QStringLiteral("READ_CHECKSUM");
    }
    return QStringLiteral("UNKNOWN");
}

quint8 checksum(const QByteArray &bytes)
{
    quint32 sum = 0;
    for (const char byte : bytes) sum += static_cast<quint8>(byte);
    return static_cast<quint8>(sum & 0xff);
}

QByteArray makeRequest(quint8 address, quint8 service, quint8 commandClass,
                       quint8 instance, quint8 attribute, const QByteArray &data)
{
    if (address < 0x20 || address > 0x5f) throw Error("invalid MFC address");
    if (service != ReadService && service != WriteService) throw Error("unsupported service");
    if (service == WriteService && !isAllowedWrite(commandClass, instance, attribute, data))
        throw Error("MFC write is not on the production whitelist");
    if (data.size() > 252) throw Error("request data too long");
    QByteArray packet;
    packet.reserve(data.size() + 9);
    packet.append(char(address));
    packet.append(char(Stx));
    packet.append(char(service));
    packet.append(char(3 + data.size()));
    packet.append(char(commandClass));
    packet.append(char(instance));
    packet.append(char(attribute));
    packet.append(data);
    packet.append(char(0x00));
    packet.append(char(checksum(packet)));
    return packet;
}

bool isAllowedWrite(quint8 commandClass, quint8 instance, quint8 attribute,
                    const QByteArray &data)
{
    if (commandClass != 0x69 || instance != 0x01) return false;
    // This is intentionally a value whitelist as well as an attribute
    // whitelist.  Current CM must only ever be changed temporarily to
    // Digital (1); Hold/Follow only has the two protocol-defined values.
    if (attribute == 0x03)
        return data.size() == 1 && static_cast<quint8>(data[0]) == 1;
    if (attribute == 0x05)
        return data.size() == 1 && (static_cast<quint8>(data[0]) == 0
                                    || static_cast<quint8>(data[0]) == 1);
    if (attribute == 0xA4) return data.size() == 2;
    return false; // Default CM, EEPROM, valve commands and all configuration writes.
}

Response parseResponse(const QByteArray &packet, quint8 expectedClass,
                       quint8 expectedInstance, quint8 expectedAttribute)
{
    if (packet.size() < 9) throw Error("response too short");
    if (static_cast<quint8>(packet[0]) != 0x00) throw Error("invalid response start byte");
    if (static_cast<quint8>(packet[1]) != Stx) throw Error("invalid response STX");
    if (static_cast<quint8>(packet[2]) != ReadService) throw Error("invalid response service");
    const int dataLength = static_cast<quint8>(packet[3]);
    if (packet.size() != dataLength + 6) throw Error("response length mismatch");
    if (static_cast<quint8>(packet[packet.size() - 2]) != 0x00) throw Error("missing response terminator");
    if (checksum(packet.first(packet.size() - 1)) != static_cast<quint8>(packet.back()))
        throw Error("response checksum mismatch");
    const auto commandClass = static_cast<quint8>(packet[4]);
    const auto instance = static_cast<quint8>(packet[5]);
    const auto attribute = static_cast<quint8>(packet[6]);
    if (commandClass != expectedClass) throw Error("response class mismatch");
    if (instance != expectedInstance) throw Error("response instance mismatch");
    if (attribute != expectedAttribute) throw Error("response attribute mismatch");
    Response response;
    response.service = static_cast<quint8>(packet[2]);
    response.commandClass = commandClass;
    response.instance = instance;
    response.attribute = attribute;
    response.data = packet.mid(7, dataLength - 3);
    return response;
}

void ResponseStreamParser::reset()
{
    m_state = ParserState::WaitStart00;
    m_frame.clear();
    m_payloadExpected = 0;
    m_payloadReceived = 0;
    m_length = -1;
}

QString ResponseStreamParser::stateName() const
{
    return parserStateName(m_state);
}

void ResponseStreamParser::restartFrom(quint8 byte)
{
    reset();
    if (byte == 0x00) {
        m_frame.append(char(byte));
        m_state = ParserState::Expect02;
    }
}

ParseEvent ResponseStreamParser::errorEvent(const QString &message, quint8 received,
                                             int checksumExpected)
{
    ParseEvent event;
    event.type = ParseEvent::Type::Error;
    event.bytes = m_frame;
    event.bytes.append(char(received));
    event.error = message;
    event.state = m_state;
    event.length = m_length;
    event.checksumExpected = checksumExpected;
    event.checksumReceived = checksumExpected >= 0 ? received : -1;
    restartFrom(received);
    return event;
}

QList<ParseEvent> ResponseStreamParser::feed(const QByteArray &bytes)
{
    QList<ParseEvent> events;
    for (int index = 0; index < bytes.size(); ++index) {
        QList<ParseEvent> byteEvents = feed(static_cast<quint8>(bytes[index]));
        for (ParseEvent &event : byteEvents) event.inputOffset = index;
        events.append(byteEvents);
    }
    return events;
}

QList<ParseEvent> ResponseStreamParser::feed(quint8 byte)
{
    QList<ParseEvent> events;
    switch (m_state) {
    case ParserState::WaitStart00:
        if (byte == 0x00) {
            m_frame = QByteArray(1, char(byte));
            m_state = ParserState::Expect02;
        }
        break;
    case ParserState::Expect02:
        if (byte != Stx) {
            events.append(errorEvent(QStringLiteral("invalid response STX"), byte));
            break;
        }
        m_frame.append(char(byte));
        m_state = ParserState::ExpectService80;
        break;
    case ParserState::ExpectService80:
        if (byte != ReadService) {
            events.append(errorEvent(QStringLiteral("invalid response service"), byte));
            break;
        }
        m_frame.append(char(byte));
        m_state = ParserState::ReadLength;
        break;
    case ParserState::ReadLength:
        if (byte < 0x03 || byte > 0x21) {
            events.append(errorEvent(QStringLiteral("invalid response length"), byte));
            break;
        }
        m_length = byte;
        m_payloadExpected = m_length - 3;
        m_payloadReceived = 0;
        m_frame.append(char(byte));
        m_state = ParserState::ReadClass;
        break;
    case ParserState::ReadClass:
        m_frame.append(char(byte));
        m_state = ParserState::ExpectInstance01;
        break;
    case ParserState::ExpectInstance01:
        if (byte != 0x01) {
            events.append(errorEvent(QStringLiteral("invalid response instance"), byte));
            break;
        }
        m_frame.append(char(byte));
        m_state = ParserState::ReadAttribute;
        break;
    case ParserState::ReadAttribute:
        m_frame.append(char(byte));
        m_state = m_payloadExpected == 0 ? ParserState::ExpectTrailing00
                                         : ParserState::ReadPayload;
        break;
    case ParserState::ReadPayload:
        m_frame.append(char(byte));
        if (++m_payloadReceived == m_payloadExpected)
            m_state = ParserState::ExpectTrailing00;
        break;
    case ParserState::ExpectTrailing00:
        if (byte != 0x00) {
            events.append(errorEvent(QStringLiteral("missing response terminator"), byte));
            break;
        }
        m_frame.append(char(byte));
        m_state = ParserState::ReadChecksum;
        break;
    case ParserState::ReadChecksum: {
        const int expected = checksum(m_frame);
        if (byte != expected) {
            events.append(errorEvent(QStringLiteral("response checksum mismatch"), byte, expected));
            break;
        }
        m_frame.append(char(byte));
        ParseEvent event;
        event.type = ParseEvent::Type::Frame;
        event.bytes = m_frame;
        event.state = m_state;
        event.length = m_length;
        event.checksumExpected = expected;
        event.checksumReceived = byte;
        events.append(event);
        reset();
        break;
    }
    }
    return events;
}

double decodeUfrac16(quint16 raw)
{
    return (static_cast<double>(raw) - 0x4000) / static_cast<double>(0x8000);
}

quint16 encodeUfrac16(double fraction)
{
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0)
        throw Error("UFRAC16 fraction outside 0..1");
    return static_cast<quint16>(std::lround(0x4000 + fraction * 0x8000));
}

quint16 readUInt16Le(const QByteArray &data)
{
    if (data.size() < 2) throw Error("UINT16 response too short");
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data.constData()));
}

qint32 readInt32Le(const QByteArray &data)
{
    if (data.size() < 4) throw Error("INT32 response too short");
    return qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(data.constData()));
}

double decodeFixed16_16(const QByteArray &data)
{
    return static_cast<double>(readInt32Le(data)) / 65536.0;
}

QString decodeText(const QByteArray &data)
{
    const int nul = data.indexOf('\0');
    return QString::fromLatin1(nul >= 0 ? data.first(nul) : data).trimmed();
}

QByteArray uint16Le(quint16 value)
{
    QByteArray bytes(2, '\0');
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data()));
    return bytes;
}

AlarmBits decodeAlarms(quint16 raw)
{
    AlarmBits result;
    result.raw = raw;
    const struct Entry { int bit; const char *text; bool alarm; } entries[] = {
        {0, "环境温度过高", false}, {1, "环境温度过低", false},
        {4, "传感器负零漂", false}, {5, "传感器正零漂", false},
        {8, "环境温度过高", true}, {9, "环境温度过低", true},
        {10, "阀线圈短路", true}, {11, "阀线圈开路", true},
        {12, "传感器负零漂", true}, {13, "传感器正零漂", true},
        {14, "EEPROM 失效", true}
    };
    for (const auto &entry : entries) {
        if (!(raw & (quint16(1) << entry.bit))) continue;
        (entry.alarm ? result.alarms : result.warnings).append(QString::fromUtf8(entry.text));
    }
    return result;
}

}
