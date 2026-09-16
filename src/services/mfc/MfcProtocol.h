#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>
#include <QtGlobal>
#include <stdexcept>

namespace MfcProtocol {

constexpr quint8 Stx = 0x02;
constexpr quint8 ReadService = 0x80;
constexpr quint8 WriteService = 0x81;
constexpr quint8 Ack = 0x06;
constexpr quint8 Nak = 0x15;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Response {
    quint8 service{0};
    quint8 commandClass{0};
    quint8 instance{0};
    quint8 attribute{0};
    QByteArray data;
};

enum class ParserState {
    WaitStart00,
    Expect02,
    ExpectService80,
    ReadLength,
    ReadClass,
    ExpectInstance01,
    ReadAttribute,
    ReadPayload,
    ExpectTrailing00,
    ReadChecksum
};
QString parserStateName(ParserState state);

struct ParseEvent {
    enum class Type { Frame, Error };
    Type type{Type::Error};
    QByteArray bytes;
    QString error;
    ParserState state{ParserState::WaitStart00};
    int length{-1};
    int checksumExpected{-1};
    int checksumReceived{-1};
    // Zero-based position of the event-ending byte within the latest feed()
    // chunk; diagnostic metadata only, not part of parsing decisions.
    int inputOffset{-1};
};

// Stateful response decoder. QSerialPort read boundaries have no protocol
// meaning: every received byte is fed through this finite-state machine.
class ResponseStreamParser
{
public:
    QList<ParseEvent> feed(const QByteArray &bytes);
    QList<ParseEvent> feed(quint8 byte);
    void reset();
    ParserState state() const { return m_state; }
    QString stateName() const;
    QByteArray partialFrame() const { return m_frame; }

private:
    ParseEvent errorEvent(const QString &message, quint8 received,
                          int checksumExpected = -1);
    void restartFrom(quint8 byte);
    ParserState m_state{ParserState::WaitStart00};
    QByteArray m_frame;
    int m_payloadExpected{0};
    int m_payloadReceived{0};
    int m_length{-1};
};

quint8 checksum(const QByteArray &bytes);
QByteArray makeRequest(quint8 address, quint8 service, quint8 commandClass,
                       quint8 instance, quint8 attribute,
                       const QByteArray &data = {});
Response parseResponse(const QByteArray &packet, quint8 expectedClass,
                       quint8 expectedInstance, quint8 expectedAttribute);
double decodeUfrac16(quint16 raw);
quint16 encodeUfrac16(double fraction);
quint16 readUInt16Le(const QByteArray &data);
qint32 readInt32Le(const QByteArray &data);
double decodeFixed16_16(const QByteArray &data);
QString decodeText(const QByteArray &data);
QByteArray uint16Le(quint16 value);
// The production control surface is intentionally tiny.  Keeping the policy
// beside frame construction means a prohibited command cannot reach the wire.
bool isAllowedWrite(quint8 commandClass, quint8 instance, quint8 attribute,
                    const QByteArray &data = {});

struct AlarmBits {
    quint16 raw{0};
    QStringList warnings;
    QStringList alarms;
};
AlarmBits decodeAlarms(quint16 raw);

}
