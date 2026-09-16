#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSerialPort>
#include <QString>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <optional>
#include <stdexcept>

#include "MfcProtocol.h"

class SerialTransport
{
public:
    // The state is deliberately transport-owned: callers never get to send a
    // command while this shared byte stream is being brought back to idle.
    enum class TransactionState { Idle, PreTxCheck, Tx, WaitAck, ReadFrame,
                                  Validate, Success, Recovery, Retry, SerialDegraded, Failed };
    class Error : public std::runtime_error { public: using std::runtime_error::runtime_error; };
    class Timeout : public Error { public: using Error::Error; };
    class NegativeAcknowledge : public Error { public: using Error::Error; };
    class ProtocolFailure : public Error { public: using Error::Error; };
    class Cancelled : public Error { public: using Error::Error; };

    using TraceSink = std::function<void(const QString &)>;
    explicit SerialTransport(TraceSink trace = {});
    ~SerialTransport();

    void open(const QString &portName, qint32 baudRate);
    void close();
    bool isOpen() const;
    QString portName() const;
    qint64 write(const QByteArray &bytes);
    QByteArray read(qint64 maximum, int timeoutMs);
    void flush();
    QByteArray transaction(const QByteArray &request, int ackTimeoutMs = 40,
                           int responseTimeoutMs = 180, int retries = 1);
    // Whitelisted CS200 writes acknowledge with ACK only; they do not get a
    // fabricated read response.  It shares the exact same pending gate.
    void transactionAck(const QByteArray &request, int ackTimeoutMs = 40, int retries = 1);
    void requestCancel() noexcept { m_cancelRequested.store(true); }
    void clearCancellation() noexcept { m_cancelRequested.store(false); }
    void setLogicalChannel(int channel) { m_logicalChannel = channel; }
    bool transactionPending() const { return m_pending.has_value(); }
    void setErrorRawOnly(bool enabled) { m_errorRawOnly = enabled; }
    void setExperimentDiagnostics(bool enabled) { m_experimentDiagnostics = enabled; }
    void resetTransactionSpacing() { m_lastFinishNs = -1; m_previousFinishTimestamp = {}; }
    bool physicalPortExists() const;
    QVariantMap diagnostics() const;

private:
    struct RecoveryCounters {
        quint64 busRecoveryCount{0};
        quint64 busRecoverySuccessCount{0};
        quint64 busRecoveryFailureCount{0};
        quint64 rxDrainCount{0};
        quint64 rxDiscardedBytes{0};
        quint64 rxPendingBeforeTxCount{0};
        quint64 rxPendingBeforeTxMax{0};
        quint64 rxQuietWaitCount{0};
        quint64 rxQuietWaitTotalMs{0};
        quint64 rxQuietWaitMaxMs{0};
        quint64 streamResyncCount{0};
        quint64 streamResyncDiscardedBytes{0};
        quint64 consecutiveProtocolErrorMax{0};
        quint64 consecutiveInvalidAckMax{0};
        quint64 hardRecoveryCount{0};
        quint64 serialReopenCount{0};
    };
    struct PendingRequest {
        quint64 requestId{0};
        quint8 protocolAddress{0};
        quint8 service{0};
        quint8 commandClass{0};
        quint8 instance{0};
        quint8 attribute{0};
        int logicalChannel{0};
        QByteArray tx;
    };
    QByteArray readExact(qint64 count, int timeoutMs);
    QByteArray readResponseFrame(int timeoutMs, const PendingRequest &pending,
                                 QByteArray *allReceived);
    bool verifyRxClean(const PendingRequest &pending, int attempt);
    bool recoverBus(const QString &reason, const PendingRequest &pending, int attempt,
                    bool hardRecovery = false);
    void recordProtocolFailure(const QString &reason);
    static QString stateName(TransactionState state);
    void noteRxBytes(const QByteArray &bytes);
    QVariantMap runtimeSerialConfiguration() const;
    void finishCurrentTransaction(const QString &result, const QString &detail,
                                  qint64 elapsedMs);
    QSerialPort m_port;
    TraceSink m_trace;
    MfcProtocol::ResponseStreamParser m_parser;
    std::optional<PendingRequest> m_pending;
    std::atomic_bool m_cancelRequested{false};
    quint64 m_previousRequestId{0};
    quint64 m_lastFinishedRequestId{0};
    QString m_lastResult;
    QString m_lastProtocolError;
    QByteArray m_lastRx;
    QByteArray m_lastAck;
    QByteArray m_lastResponseFrame;
    QVariantMap m_lastTransaction;
    QElapsedTimer m_monotonicClock;
    qint64 m_lastFinishNs{-1};
    qint64 m_currentTxNs{-1};
    QDateTime m_previousFinishTimestamp;
    QDateTime m_currentTxTimestamp;
    QString m_openedPortPath;
    QString m_lastParserState;
    int m_lastChecksumExpected{-1};
    int m_lastChecksumReceived{-1};
    int m_lastReceivedService{-1};
    int m_lastReceivedClass{-1};
    int m_lastReceivedInstance{-1};
    int m_lastReceivedAttribute{-1};
    int m_attemptCount{0};
    bool m_errorRawOnly{false};
    bool m_experimentDiagnostics{false};
    int m_logicalChannel{0};
    QByteArray m_rxHistory;
    quint64 m_rxSequence{0};
    QVariantList m_rxBatches;
    QVariantList m_attemptRecords;
    QVariantList m_attemptErrors;
    QVariantList m_preTxObservations;
    QDateTime m_firstRxTimestamp;
    QDateTime m_lastRxTimestamp;
    QDateTime m_frameCompleteTimestamp;
    QByteArray m_historyAtRequestStart;
    int m_lateRxAfterFinishCount{0};
    int m_lateRxAfterFinishBytes{0};
    int m_rxBytesAvailableBeforeTx{0};
    QString m_parserStateBeforeTx;
    QByteArray m_preTxRxPeek;
    QVariantList m_recoveryEvents;
    RecoveryCounters m_recoveryCounters;
    TransactionState m_state{TransactionState::Idle};
    int m_consecutiveProtocolErrors{0};
    int m_consecutiveInvalidAcks{0};
    int m_consecutiveRecoveryFailures{0};
    bool m_serialDegraded{false};
};
