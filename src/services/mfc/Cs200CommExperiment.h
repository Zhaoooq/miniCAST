#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QMap>
#include <QString>
#include <QVector>
#include <QVariantMap>

class Cs200CommExperiment
{
public:
    Cs200CommExperiment();
    struct AddressStats {
        int requestCount{0};
        int requestSuccess{0};
        int requestFinalFailure{0};
        int attemptCount{0};
        int attemptProtocolError{0};
        int attemptChecksumError{0};
        int attemptTimeout{0};
        int retrySuccess{0};
        int invalidService{0};
        int invalidClass{0};
        int invalidAttribute{0};
        int invalidInstance{0};
        int lengthError{0};
        int trailingByteError{0};
        int unexpectedDataError{0};
        int otherProtocolError{0};
        int serialError{0};
        int lateRxAfterFinishCount{0};
        int lateRxAfterFinishBytes{0};
        int preTxRxBytesCount{0};
        int preTxPartialStateCount{0};
        int ackObservedCount{0};
        int nonAckFirstByteCount{0};
        int busRecoveryCount{0};
        int busRecoverySuccessCount{0};
        int busRecoveryFailureCount{0};
        int rxDrainCount{0};
        qint64 rxDiscardedBytes{0};
        int rxPendingBeforeTxCount{0};
        int rxPendingBeforeTxMax{0};
        int rxQuietWaitCount{0};
        qint64 rxQuietWaitTotalMs{0};
        int rxQuietWaitMaxMs{0};
        int streamResyncCount{0};
        qint64 streamResyncDiscardedBytes{0};
        int hardRecoveryCount{0};
        int consecutiveProtocolErrorMax{0};
        int consecutiveInvalidAckMax{0};
        int serialReopenCount{0};
        double responseTotalMs{0.0};
        double minResponseMs{-1.0};
        double maxResponseMs{0.0};
        QVector<double> responseSamplesMs;
    };

    // selectedAddress == 0 means the five-address control test.
    void start(int delayMs, int durationSeconds, int selectedAddress = 0);
    void recordTransaction(const QVariantMap &transaction, bool success,
                           const QString &errorMessage = QString());
    void recordGuiHeartbeatStall(qint64 stallMs);
    void recordWorkerWatchdogTrigger();
    void finish(const QString &reason);
    bool active() const { return m_active; }
    bool expired() const;
    int remainingSeconds() const;
    int currentAddress() const { return m_currentAddress; }
    void advanceAddress();
    int selectedAddress() const { return m_selectedAddress; }
    QVariantMap toVariantMap() const;
    bool writeReports(const QString &directory, QString *jsonPath, QString *textPath,
                      QString *errorMessage = nullptr) const;

private:
    static QString classifyError(const QVariantMap &transaction, const QString &message);
    QVariantMap addressMap(int address, const AddressStats &stats) const;
    QMap<int, AddressStats> m_stats;
    QVariantList m_errorTimeline;
    QElapsedTimer m_elapsed;
    QDateTime m_startedAt;
    QDateTime m_finishedAt;
    qint64 m_finishedElapsedMs{0};
    bool m_active{false};
    int m_delayMs{100};
    int m_durationSeconds{300};
    int m_selectedAddress{0};
    int m_currentAddress{32};
    int m_guiHeartbeatStalls{0};
    qint64 m_maxGuiStallMs{0};
    int m_workerWatchdogTriggers{0};
    int m_gapCount{0};
    double m_gapTotalMs{0.0};
    double m_gapMinMs{-1.0};
    double m_gapMaxMs{0.0};
    QString m_finishReason{QStringLiteral("not_started")};
    QVariantMap m_serialConfiguration;
    QVector<int> m_waitingForNextTxEvents;
};
