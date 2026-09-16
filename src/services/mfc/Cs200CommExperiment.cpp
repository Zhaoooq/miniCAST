#include "Cs200CommExperiment.h"

#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
double percentile(QVector<double> samples, double fraction)
{
    if (samples.isEmpty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const double index = fraction * (samples.size() - 1);
    const int lower = static_cast<int>(std::floor(index));
    const int upper = static_cast<int>(std::ceil(index));
    if (lower == upper) return samples[lower];
    return samples[lower] + (samples[upper] - samples[lower]) * (index - lower);
}
}

Cs200CommExperiment::Cs200CommExperiment()
{
    for (int address = 32; address <= 36; ++address) m_stats.insert(address, {});
}

void Cs200CommExperiment::start(int delayMs, int durationSeconds, int selectedAddress)
{
    m_selectedAddress = selectedAddress >= 32 && selectedAddress <= 36 ? selectedAddress : 0;
    m_currentAddress = m_selectedAddress == 0 ? 32 : m_selectedAddress;
    m_stats.clear();
    if (m_selectedAddress == 0) {
        for (int address = 32; address <= 36; ++address) m_stats.insert(address, {});
    } else {
        m_stats.insert(m_selectedAddress, {});
    }
    m_errorTimeline.clear();
    m_waitingForNextTxEvents.clear();
    m_serialConfiguration.clear();
    m_delayMs = qBound(100, delayMs, 300);
    m_durationSeconds = qBound(1, durationSeconds, 30 * 60);
    m_guiHeartbeatStalls = 0;
    m_maxGuiStallMs = 0;
    m_workerWatchdogTriggers = 0;
    m_gapCount = 0;
    m_gapTotalMs = 0.0;
    m_gapMinMs = -1.0;
    m_gapMaxMs = 0.0;
    m_startedAt = QDateTime::currentDateTime();
    m_finishedAt = {};
    m_finishedElapsedMs = 0;
    m_finishReason = QStringLiteral("running");
    m_active = true;
    m_elapsed.start();
}

void Cs200CommExperiment::advanceAddress()
{
    if (m_selectedAddress != 0) {
        m_currentAddress = m_selectedAddress;
        return;
    }
    m_currentAddress = m_currentAddress == 36 ? 32 : m_currentAddress + 1;
}

QString Cs200CommExperiment::classifyError(const QVariantMap &transaction,
                                            const QString &message)
{
    const QString explicitType = transaction.value("errorType").toString().toLower();
    if (!explicitType.isEmpty()) return explicitType;
    const QString result = transaction.value("result").toString();
    const QString detail = (transaction.value("detail").toString() + QLatin1Char(' ') + message).toLower();
    if (result == QStringLiteral("TIMEOUT") || detail.contains(QStringLiteral("timeout")))
        return QStringLiteral("timeout");
    if (detail.contains(QStringLiteral("checksum"))) return QStringLiteral("checksum_error");
    if (detail.contains(QStringLiteral("service"))) return QStringLiteral("invalid_service");
    if (detail.contains(QStringLiteral("class"))) return QStringLiteral("invalid_class");
    if (detail.contains(QStringLiteral("attribute"))) return QStringLiteral("invalid_attribute");
    if (detail.contains(QStringLiteral("instance"))) return QStringLiteral("invalid_instance");
    if (detail.contains(QStringLiteral("length")) || detail.contains(QStringLiteral("short")))
        return QStringLiteral("length_error");
    if (detail.contains(QStringLiteral("terminator")) || detail.contains(QStringLiteral("trailing")))
        return QStringLiteral("trailing_byte_error");
    if (detail.contains(QStringLiteral("unexpected")) || detail.contains(QStringLiteral("ack byte")))
        return QStringLiteral("unexpected_data");
    if (result == QStringLiteral("SERIAL_ERROR")) return QStringLiteral("serial_error");
    return QStringLiteral("other_protocol_error");
}

void Cs200CommExperiment::recordTransaction(const QVariantMap &transaction, bool success,
                                             const QString &errorMessage)
{
    for (const int index : std::as_const(m_waitingForNextTxEvents)) {
        if (index < 0 || index >= m_errorTimeline.size()) continue;
        QVariantMap priorError = m_errorTimeline[index].toMap();
        priorError["nextTxTimestamp"] = transaction.value("txTimestamp");
        priorError["finishToNextTxMs"] = transaction.value("previousFinishToNextTxMs");
        m_errorTimeline[index] = priorError;
    }
    m_waitingForNextTxEvents.clear();
    const int address = transaction.value("address").toInt();
    if (!m_stats.contains(address)) return;
    auto &stats = m_stats[address];
    ++stats.requestCount;
    const int attempts = qMax(1, transaction.value("attemptCount", 1).toInt());
    stats.attemptCount += attempts;
    if (m_serialConfiguration.isEmpty())
        m_serialConfiguration = transaction.value("serialConfiguration").toMap();
    for (const QVariant &value : transaction.value("attempts").toList()) {
        const QString ackRaw = value.toMap().value("ackRaw").toString().trimmed();
        if (ackRaw == QStringLiteral("06")) ++stats.ackObservedCount;
        else if (!ackRaw.isEmpty()) ++stats.nonAckFirstByteCount;
    }

    const double gap = transaction.value("previousFinishToNextTxMs", -1.0).toDouble();
    if (std::isfinite(gap) && gap >= 0.0) {
        ++m_gapCount;
        m_gapTotalMs += gap;
        m_gapMinMs = m_gapMinMs < 0.0 ? gap : qMin(m_gapMinMs, gap);
        m_gapMaxMs = qMax(m_gapMaxMs, gap);
    }
    stats.lateRxAfterFinishBytes += transaction.value("lateRxAfterFinishBytes").toInt();
    stats.lateRxAfterFinishCount += transaction.value("lateRxAfterFinishCount").toInt();
    if (transaction.value("rxBytesAvailableBeforeTx").toInt() > 0) ++stats.preTxRxBytesCount;
    const QString parserBefore = transaction.value("parserStateBeforeTx").toString();
    if (!parserBefore.isEmpty() && parserBefore != QStringLiteral("WAIT_START_00"))
        ++stats.preTxPartialStateCount;
    stats.consecutiveProtocolErrorMax = qMax(stats.consecutiveProtocolErrorMax,
        transaction.value("consecutiveProtocolErrorMax").toInt());
    stats.consecutiveInvalidAckMax = qMax(stats.consecutiveInvalidAckMax,
        transaction.value("consecutiveInvalidAckMax").toInt());
    stats.serialReopenCount = qMax(stats.serialReopenCount,
        transaction.value("serialReopenCount").toInt());
    for (const QVariant &value : transaction.value("recoveryEvents").toList()) {
        const QVariantMap recovery = value.toMap();
        ++stats.busRecoveryCount;
        if (recovery.value("result").toString() == QStringLiteral("SUCCESS"))
            ++stats.busRecoverySuccessCount;
        else
            ++stats.busRecoveryFailureCount;
        if (recovery.value("discardedBytes").toInt() > 0) {
            ++stats.rxDrainCount;
            stats.rxDiscardedBytes += recovery.value("discardedBytes").toLongLong();
            ++stats.streamResyncCount;
            stats.streamResyncDiscardedBytes += recovery.value("discardedBytes").toLongLong();
        }
        ++stats.rxQuietWaitCount;
        stats.rxQuietWaitTotalMs += recovery.value("quietWaitMs").toLongLong();
        stats.rxQuietWaitMaxMs = qMax(stats.rxQuietWaitMaxMs, recovery.value("quietWaitMs").toInt());
        if (recovery.value("rxPendingAtStart").toInt() > 0) {
            ++stats.rxPendingBeforeTxCount;
            stats.rxPendingBeforeTxMax = qMax(stats.rxPendingBeforeTxMax,
                                               recovery.value("rxPendingAtStart").toInt());
        }
        if (recovery.value("hardRecovery").toBool()) ++stats.hardRecoveryCount;
        QVariantMap event = recovery;
        event["operation"] = QStringLiteral("READ_FLOW");
        m_errorTimeline.append(event);
    }

    const int firstNewErrorIndex = m_errorTimeline.size();
    const QVariantList attemptErrors = transaction.value("attemptErrors").toList();
    QSet<QString> countedAttemptKinds;
    for (const QVariant &value : attemptErrors) {
        QVariantMap event = value.toMap();
        const QString type = classifyError(event, event.value("errorMessage").toString());
        const int attemptNumber = event.value("attempt", 1).toInt();
        const QString typeKey = QString::number(attemptNumber) + QLatin1Char(':') + type;
        const QString protocolKey = QString::number(attemptNumber) + QStringLiteral(":protocol");
        if (type == QStringLiteral("timeout")) {
            if (!countedAttemptKinds.contains(typeKey)) ++stats.attemptTimeout;
        } else if (type == QStringLiteral("serial_error")) {
            if (!countedAttemptKinds.contains(typeKey)) ++stats.serialError;
        }
        else {
            if (!countedAttemptKinds.contains(protocolKey)) ++stats.attemptProtocolError;
            if (!countedAttemptKinds.contains(typeKey)) {
                if (type == QStringLiteral("checksum_error")) ++stats.attemptChecksumError;
                else if (type == QStringLiteral("invalid_service")) ++stats.invalidService;
                else if (type == QStringLiteral("invalid_class")) ++stats.invalidClass;
                else if (type == QStringLiteral("invalid_attribute")) ++stats.invalidAttribute;
                else if (type == QStringLiteral("invalid_instance")) ++stats.invalidInstance;
                else if (type == QStringLiteral("length_error")) ++stats.lengthError;
                else if (type == QStringLiteral("trailing_byte_error")) ++stats.trailingByteError;
                else if (type == QStringLiteral("unexpected_data")) ++stats.unexpectedDataError;
                else ++stats.otherProtocolError;
            }
            countedAttemptKinds.insert(protocolKey);
        }
        countedAttemptKinds.insert(typeKey);
        event["event"] = QStringLiteral("attempt_error_raw");
        event["errorType"] = type;
        event["requestId"] = transaction.value("requestId");
        event["address"] = address;
        event["operation"] = QStringLiteral("READ_FLOW");
        event["requestFinalResult"] = transaction.value("result");
        event["previousRequestId"] = transaction.value("previousRequestId");
        event["previousRequestFinishTimestamp"] = transaction.value("previousFinishTimestamp");
        event["txTimestamp"] = transaction.value("txTimestamp");
        event["firstRxTimestamp"] = transaction.value("firstRxTimestamp");
        event["lastRxTimestamp"] = transaction.value("lastRxTimestamp");
        event["frameCompleteTimestamp"] = transaction.value("frameCompleteTimestamp");
        event["requestFinishTimestamp"] = transaction.value("finishTimestamp");
        event["previousFinishToNextTxMs"] = transaction.value("previousFinishToNextTxMs");
        event["rxBytesAvailableBeforeTx"] = transaction.value("rxBytesAvailableBeforeTx");
        event["parserStateBeforeTx"] = transaction.value("parserStateBeforeTx");
        event["preTxRxPeek"] = transaction.value("preTxRxPeek");
        event["rxBatches"] = transaction.value("rxBatches");
        event["attempts"] = transaction.value("attempts");
        event["serialConfiguration"] = transaction.value("serialConfiguration");
        m_errorTimeline.append(event);
    }

    if (success) {
        ++stats.requestSuccess;
        if (attempts > 1) ++stats.retrySuccess;
        const double response = transaction.value("elapsedMs").toDouble();
        stats.responseTotalMs += response;
        stats.responseSamplesMs.append(response);
        stats.minResponseMs = stats.minResponseMs < 0.0 ? response : qMin(stats.minResponseMs, response);
        stats.maxResponseMs = qMax(stats.maxResponseMs, response);
        for (int index = firstNewErrorIndex; index < m_errorTimeline.size(); ++index)
            m_waitingForNextTxEvents.append(index);
        return;
    }

    ++stats.requestFinalFailure;
    if (attemptErrors.isEmpty()) {
        const QString type = classifyError(transaction, errorMessage);
        if (type == QStringLiteral("timeout")) ++stats.attemptTimeout;
        else if (type == QStringLiteral("serial_error")) ++stats.serialError;
        else {
            ++stats.attemptProtocolError;
            if (type == QStringLiteral("checksum_error")) ++stats.attemptChecksumError;
            else ++stats.otherProtocolError;
        }
    }
    QVariantMap event = transaction;
    event["event"] = QStringLiteral("request_final_failure");
    event["errorType"] = classifyError(transaction, errorMessage);
    event["errorMessage"] = errorMessage;
    event["operation"] = QStringLiteral("READ_FLOW");
    m_errorTimeline.append(event);
    for (int index = firstNewErrorIndex; index < m_errorTimeline.size(); ++index)
        m_waitingForNextTxEvents.append(index);
}

void Cs200CommExperiment::recordGuiHeartbeatStall(qint64 stallMs)
{
    if (!m_active) return;
    ++m_guiHeartbeatStalls;
    m_maxGuiStallMs = qMax(m_maxGuiStallMs, stallMs);
    m_errorTimeline.append(QVariantMap{{"event", "gui_heartbeat_stall"},
        {"timestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}, {"stallMs", stallMs}});
}

void Cs200CommExperiment::recordWorkerWatchdogTrigger()
{
    if (!m_active) return;
    ++m_workerWatchdogTriggers;
    m_errorTimeline.append(QVariantMap{{"event", "worker_watchdog_trigger"},
        {"timestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
}

void Cs200CommExperiment::finish(const QString &reason)
{
    if (!m_active) return;
    m_finishedElapsedMs = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;
    m_finishedAt = QDateTime::currentDateTime();
    m_finishReason = reason;
    m_active = false;
}

bool Cs200CommExperiment::expired() const
{
    return m_active && m_elapsed.isValid() && m_elapsed.elapsed() >= m_durationSeconds * 1000LL;
}

int Cs200CommExperiment::remainingSeconds() const
{
    if (!m_active || !m_elapsed.isValid()) return 0;
    return qMax(0, static_cast<int>((m_durationSeconds * 1000LL - m_elapsed.elapsed() + 999) / 1000));
}

QVariantMap Cs200CommExperiment::addressMap(int address, const AddressStats &s) const
{
    const double average = s.requestSuccess > 0 ? s.responseTotalMs / s.requestSuccess : 0.0;
    return {{"address", address}, {"requestCount", s.requestCount}, {"requestSuccess", s.requestSuccess},
        {"requestFinalFailure", s.requestFinalFailure}, {"attemptCount", s.attemptCount},
        {"attemptProtocolError", s.attemptProtocolError}, {"attemptChecksumError", s.attemptChecksumError},
        {"attemptTimeout", s.attemptTimeout}, {"retrySuccess", s.retrySuccess},
        {"invalidService", s.invalidService}, {"invalidClass", s.invalidClass},
        {"invalidAttribute", s.invalidAttribute}, {"invalidInstance", s.invalidInstance},
        {"lengthError", s.lengthError}, {"trailingByteError", s.trailingByteError},
        {"unexpectedDataError", s.unexpectedDataError}, {"otherProtocolError", s.otherProtocolError},
        {"serialError", s.serialError}, {"lateRxAfterFinishCount", s.lateRxAfterFinishCount},
        {"lateRxAfterFinishBytes", s.lateRxAfterFinishBytes}, {"preTxRxBytesCount", s.preTxRxBytesCount},
        {"preTxPartialStateCount", s.preTxPartialStateCount}, {"avgResponseMs", average},
        {"ackObservedCount", s.ackObservedCount}, {"nonAckFirstByteCount", s.nonAckFirstByteCount},
        {"busRecoveryCount", s.busRecoveryCount}, {"busRecoverySuccessCount", s.busRecoverySuccessCount},
        {"busRecoveryFailureCount", s.busRecoveryFailureCount}, {"rxDrainCount", s.rxDrainCount},
        {"rxDiscardedBytes", s.rxDiscardedBytes}, {"rxPendingBeforeTxCount", s.rxPendingBeforeTxCount},
        {"rxPendingBeforeTxMax", s.rxPendingBeforeTxMax}, {"rxQuietWaitCount", s.rxQuietWaitCount},
        {"rxQuietWaitTotalMs", s.rxQuietWaitTotalMs}, {"rxQuietWaitMaxMs", s.rxQuietWaitMaxMs},
        {"streamResyncCount", s.streamResyncCount}, {"streamResyncDiscardedBytes", s.streamResyncDiscardedBytes},
        {"hardRecoveryCount", s.hardRecoveryCount},
        {"consecutiveProtocolErrorMax", s.consecutiveProtocolErrorMax},
        {"consecutiveInvalidAckMax", s.consecutiveInvalidAckMax}, {"serialReopenCount", s.serialReopenCount},
        {"minResponseMs", s.minResponseMs < 0.0 ? 0.0 : s.minResponseMs},
        {"maxResponseMs", s.maxResponseMs}, {"p95ResponseMs", percentile(s.responseSamplesMs, 0.95)},
        {"p99ResponseMs", percentile(s.responseSamplesMs, 0.99)},
        {"txCount", s.requestCount}, {"success", s.requestSuccess}, {"errors", s.requestFinalFailure},
        {"timeout", s.attemptTimeout}, {"checksumError", s.attemptChecksumError},
        {"retryCount", qMax(0, s.attemptCount - s.requestCount)}};
}

QVariantMap Cs200CommExperiment::toVariantMap() const
{
    QVariantList addresses;
    int requests = 0, successes = 0, finalFailures = 0, attempts = 0;
    int protocolErrors = 0, checksumErrors = 0, timeouts = 0, retrySuccess = 0;
    int lateCount = 0, lateBytes = 0, preTxCount = 0, partialCount = 0;
    int ackCount = 0, nonAckCount = 0, recoveries = 0, recoverySuccesses = 0;
    int recoveryFailures = 0, drains = 0, hardRecoveries = 0, quietCount = 0;
    qint64 discarded = 0, quietTotalMs = 0;
    int pendingMax = 0, quietMaxMs = 0, resyncs = 0, consecutiveProtocolMax = 0;
    int consecutiveInvalidAckMax = 0, serialReopens = 0;
    for (auto it = m_stats.cbegin(); it != m_stats.cend(); ++it) {
        addresses.append(addressMap(it.key(), it.value()));
        requests += it->requestCount; successes += it->requestSuccess;
        finalFailures += it->requestFinalFailure; attempts += it->attemptCount;
        protocolErrors += it->attemptProtocolError; checksumErrors += it->attemptChecksumError;
        timeouts += it->attemptTimeout; retrySuccess += it->retrySuccess;
        lateCount += it->lateRxAfterFinishCount; lateBytes += it->lateRxAfterFinishBytes;
        preTxCount += it->preTxRxBytesCount; partialCount += it->preTxPartialStateCount;
        ackCount += it->ackObservedCount; nonAckCount += it->nonAckFirstByteCount;
        recoveries += it->busRecoveryCount; recoverySuccesses += it->busRecoverySuccessCount;
        recoveryFailures += it->busRecoveryFailureCount; drains += it->rxDrainCount;
        discarded += it->rxDiscardedBytes; hardRecoveries += it->hardRecoveryCount;
        quietCount += it->rxQuietWaitCount; quietTotalMs += it->rxQuietWaitTotalMs;
        pendingMax = qMax(pendingMax, it->rxPendingBeforeTxMax);
        quietMaxMs = qMax(quietMaxMs, it->rxQuietWaitMaxMs);
        resyncs += it->streamResyncCount;
        consecutiveProtocolMax = qMax(consecutiveProtocolMax, it->consecutiveProtocolErrorMax);
        consecutiveInvalidAckMax = qMax(consecutiveInvalidAckMax, it->consecutiveInvalidAckMax);
        serialReopens = qMax(serialReopens, it->serialReopenCount);
    }
    const qint64 elapsedMs = m_elapsed.isValid() ? (m_active ? m_elapsed.elapsed() : m_finishedElapsedMs) : 0;
    QVariantMap global{{"experimentDurationMs", elapsedMs}, {"configuredDurationSeconds", m_durationSeconds},
        {"configuredDelayMs", m_delayMs}, {"actualAverageTransactionGapMs", m_gapCount ? m_gapTotalMs / m_gapCount : 0.0},
        {"actualMinimumTransactionGapMs", m_gapMinMs < 0.0 ? 0.0 : m_gapMinMs},
        {"actualMaximumTransactionGapMs", m_gapMaxMs}, {"measuredGapCount", m_gapCount},
        {"requestCount", requests}, {"requestSuccess", successes}, {"requestFinalFailure", finalFailures},
        {"attemptCount", attempts}, {"attemptProtocolError", protocolErrors},
        {"attemptChecksumError", checksumErrors}, {"attemptTimeout", timeouts}, {"retrySuccess", retrySuccess},
        {"finalFailureRate", requests ? 100.0 * finalFailures / requests : 0.0},
        {"lateRxAfterFinishCount", lateCount}, {"lateRxAfterFinishBytes", lateBytes},
        {"preTxRxBytesCount", preTxCount}, {"preTxPartialStateCount", partialCount},
        {"ackObservedCount", ackCount}, {"nonAckFirstByteCount", nonAckCount},
        {"busRecoveryCount", recoveries}, {"busRecoverySuccessCount", recoverySuccesses},
        {"busRecoveryFailureCount", recoveryFailures}, {"rxDrainCount", drains},
        {"rxDiscardedBytes", discarded}, {"rxPendingBeforeTxMax", pendingMax},
        {"rxQuietWaitCount", quietCount}, {"rxQuietWaitTotalMs", quietTotalMs},
        {"rxQuietWaitMaxMs", quietMaxMs}, {"streamResyncCount", resyncs},
        {"hardRecoveryCount", hardRecoveries}, {"consecutiveProtocolErrorMax", consecutiveProtocolMax},
        {"consecutiveInvalidAckMax", consecutiveInvalidAckMax}, {"serialReopenCount", serialReopens},
        {"guiHeartbeatStalls", m_guiHeartbeatStalls}, {"maxGuiHeartbeatStallMs", m_maxGuiStallMs},
        {"guiHeartbeatStallThresholdMs", 500}, {"workerWatchdogTriggers", m_workerWatchdogTriggers},
        {"workerWatchdogThresholdMs", 2000}, {"totalRequests", requests}, {"totalSuccesses", successes},
        {"totalErrors", finalFailures}, {"errorRate", requests ? 100.0 * finalFailures / requests : 0.0}};
    const QVariantList devices = m_selectedAddress == 0
        ? QVariantList{32, 33, 34, 35, 36} : QVariantList{m_selectedAddress};
    return {{"active", m_active}, {"readOnly", true},
        {"mode", m_selectedAddress == 0 ? "MULTI_DEVICE" : "SINGLE_DEVICE"},
        {"operation", "READ_FLOW_ONLY"}, {"selectedAddress", m_selectedAddress},
        {"serialConfiguration", m_serialConfiguration},
        {"service", 0x80}, {"commandClass", 0x68}, {"instance", 0x01}, {"attribute", 0xB9},
        {"devices", devices}, {"delayMs", m_delayMs}, {"durationSeconds", m_durationSeconds},
        {"remainingSeconds", remainingSeconds()}, {"startedAt", m_startedAt.toString(Qt::ISODateWithMs)},
        {"finishedAt", m_finishedAt.toString(Qt::ISODateWithMs)}, {"finishReason", m_finishReason},
        {"addressStats", addresses}, {"global", global}, {"timeline", m_errorTimeline}};
}

bool Cs200CommExperiment::writeReports(const QString &directory, QString *jsonPath,
                                        QString *textPath, QString *errorMessage) const
{
    if (!QDir().mkpath(directory)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建实验报告目录：") + directory;
        return false;
    }
    const QString stamp = m_startedAt.toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString target = m_selectedAddress == 0 ? QStringLiteral("multi_addr32-36")
                                                   : QStringLiteral("single_addr%1").arg(m_selectedAddress);
    const QString base = QStringLiteral("cs200_%1_%2ms_%3min_%4")
        .arg(target).arg(m_delayMs).arg(qMax(1, m_durationSeconds / 60)).arg(stamp);
    const QString json = QDir(directory).filePath(base + QStringLiteral(".json"));
    const QString markdown = QDir(directory).filePath(base + QStringLiteral(".md"));
    QSaveFile jsonFile(json);
    if (!jsonFile.open(QIODevice::WriteOnly)
        || jsonFile.write(QJsonDocument::fromVariant(toVariantMap()).toJson(QJsonDocument::Indented)) < 0
        || !jsonFile.commit()) {
        if (errorMessage) *errorMessage = QStringLiteral("JSON 实验报告写入失败：") + json;
        return false;
    }
    QSaveFile textFile(markdown);
    if (!textFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage) *errorMessage = QStringLiteral("Markdown 实验报告写入失败：") + markdown;
        return false;
    }
    const QVariantMap report = toVariantMap();
    const QVariantMap global = report.value("global").toMap();
    QTextStream out(&textFile);
    out << "# CS200 Communication Experiment\n\n"
        << "- MODE: " << report.value("mode").toString() << "\n"
        << "- ADDRESS: " << (m_selectedAddress == 0 ? QStringLiteral("32/33/34/35/36") : QString::number(m_selectedAddress)) << "\n"
        << "- OPERATION: READ_FLOW ONLY (0x80 / 0x68 / 0x01 / 0xB9)\n"
        << "- INTER_REQUEST: " << m_delayMs << " ms\n"
        << "- DURATION: " << m_durationSeconds << " s\n"
        << "- REQUESTS: " << global.value("requestCount").toInt() << "\n"
        << "- SUCCESS: " << global.value("requestSuccess").toInt() << "\n"
        << "- FINAL_FAILURE: " << global.value("requestFinalFailure").toInt() << "\n"
        << "- ATTEMPTS: " << global.value("attemptCount").toInt() << "\n"
        << "- CHECKSUM_ATTEMPT_ERROR: " << global.value("attemptChecksumError").toInt() << "\n"
        << "- PROTOCOL_ATTEMPT_ERROR: " << global.value("attemptProtocolError").toInt() << "\n"
        << "- RETRY_SUCCESS: " << global.value("retrySuccess").toInt() << "\n"
        << "- LATE_RX_AFTER_FINISH: " << global.value("lateRxAfterFinishCount").toInt()
        << " events / " << global.value("lateRxAfterFinishBytes").toInt() << " bytes\n"
        << "- PRE_TX_RX_BYTES: " << global.value("preTxRxBytesCount").toInt() << "\n"
        << "- PRE_TX_PARTIAL_STATE: " << global.value("preTxPartialStateCount").toInt() << "\n\n"
        << "- BUS_RECOVERY: " << global.value("busRecoveryCount").toInt()
        << " (success=" << global.value("busRecoverySuccessCount").toInt()
        << ", failure=" << global.value("busRecoveryFailureCount").toInt() << ")\n"
        << "- RX_DRAIN: " << global.value("rxDrainCount").toInt()
        << " events / " << global.value("rxDiscardedBytes").toLongLong() << " bytes\n"
        << "- RX_QUIET: " << global.value("rxQuietWaitCount").toInt()
        << " waits / " << global.value("rxQuietWaitTotalMs").toLongLong() << " ms total"
        << " / " << global.value("rxQuietWaitMaxMs").toInt() << " ms max\n"
        << "- RX_PENDING_BEFORE_TX_MAX: " << global.value("rxPendingBeforeTxMax").toInt()
        << ", STREAM_RESYNC: " << global.value("streamResyncCount").toInt()
        << ", HARD_RECOVERY: " << global.value("hardRecoveryCount").toInt()
        << ", SERIAL_REOPEN: " << global.value("serialReopenCount").toInt() << "\n"
        << "- CONSECUTIVE_PROTOCOL_ERROR_MAX: " << global.value("consecutiveProtocolErrorMax").toInt()
        << ", CONSECUTIVE_INVALID_ACK_MAX: " << global.value("consecutiveInvalidAckMax").toInt() << "\n\n"
        << "- ACK_06_OBSERVED: " << global.value("ackObservedCount").toInt() << "\n"
        << "- NON_ACK_FIRST_BYTE: " << global.value("nonAckFirstByteCount").toInt() << "\n"
        << "- SERIAL: " << m_serialConfiguration.value("port").toString()
        << " @ " << m_serialConfiguration.value("baudRate").toInt() << " "
        << m_serialConfiguration.value("dataBits").toString() << m_serialConfiguration.value("parity").toString()
        << m_serialConfiguration.value("stopBits").toString() << " flow="
        << m_serialConfiguration.value("flowControl").toString() << "\n\n"
        << "| Address | Requests | Success | Final Fail | Attempts | Checksum | Protocol | Timeout | Retry Success | Avg | Min | Max | P95 | P99 |\n"
        << "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (auto it = m_stats.cbegin(); it != m_stats.cend(); ++it) {
        const QVariantMap row = addressMap(it.key(), it.value());
        out << '|' << it.key() << '|' << row.value("requestCount").toInt()
            << '|' << row.value("requestSuccess").toInt() << '|' << row.value("requestFinalFailure").toInt()
            << '|' << row.value("attemptCount").toInt() << '|' << row.value("attemptChecksumError").toInt()
            << '|' << row.value("attemptProtocolError").toInt() << '|' << row.value("attemptTimeout").toInt()
            << '|' << row.value("retrySuccess").toInt() << '|' << row.value("avgResponseMs").toDouble()
            << '|' << row.value("minResponseMs").toDouble() << '|' << row.value("maxResponseMs").toDouble()
            << '|' << row.value("p95ResponseMs").toDouble() << '|' << row.value("p99ResponseMs").toDouble() << "|\n";
    }
    out << "\n## ERROR RAW\n\n";
    for (const QVariant &value : m_errorTimeline)
        out << "```json\n" << QJsonDocument::fromVariant(value).toJson(QJsonDocument::Indented) << "```\n";
    if (!textFile.commit()) {
        if (errorMessage) *errorMessage = QStringLiteral("Markdown 实验报告提交失败：") + markdown;
        return false;
    }
    if (jsonPath) *jsonPath = json;
    if (textPath) *textPath = markdown;
    return true;
}
