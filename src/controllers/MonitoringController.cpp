#include "MonitoringController.h"
#include "services/CS200ADeviceService.h"
#include <QMetaObject>

MonitoringController::MonitoringController(int interval, double warning, double critical,
                                           double zeroTolerance, QObject *parent,
                                           const QString &logDirectory,
                                           QList<MfcDeviceConfig> devices, const QString &serialPort,
                                           int preferredBaud, int ackTimeoutMs,
                                           int responseTimeoutMs, int retries, int offlineThreshold,
                                           int reconnectIntervalMs, int settlingTimeMs,
                                           int interRequestDelayMs, int freshnessTimeoutMs,
                                           int metadataInterRequestDelayMs)
    : QObject(parent)
{
    MfcManager::Settings settings;
        settings.serialPort = serialPort;
        settings.baudRate = preferredBaud;
        settings.ackTimeoutMs = ackTimeoutMs;
        settings.responseTimeoutMs = responseTimeoutMs;
        settings.retryCount = retries;
        settings.offlineFailureThreshold = offlineThreshold;
        settings.interRequestDelayMs = qBound(0, interRequestDelayMs, 1000);
        settings.metadataInterRequestDelayMs = qBound(20, metadataInterRequestDelayMs, 100);
        settings.freshnessTimeoutMs = qBound(500, freshnessTimeoutMs, 60000);
    m_service = new CS200ADeviceService(interval, warning, critical, zeroTolerance,
        devices, settings, reconnectIntervalMs, settlingTimeMs, logDirectory);
    m_deviceInfo = m_service->deviceInfo();
    m_service->moveToThread(&m_deviceThread);
    connect(&m_deviceThread, &QThread::finished, m_service, &QObject::deleteLater);
    connect(m_service, &IDeviceService::flowDataUpdated, this, &MonitoringController::onFlows);
    connect(m_service, &IDeviceService::deviceStatusChanged, this, &MonitoringController::onDeviceStatus);
    connect(m_service, &IDeviceService::flameStatusChanged, this, &MonitoringController::onFlameStatus);
    connect(m_service, &IDeviceService::communicationError, this, &MonitoringController::errorOccurred);
    connect(m_service, &IDeviceService::communicationNoticeChanged,
            this, &MonitoringController::communicationNoticeChanged);
    connect(m_service, &IDeviceService::deviceInfoChanged, this, &MonitoringController::onDeviceInfo);
    connect(m_service, &IDeviceService::monitoringActiveChanged,
            this, &MonitoringController::onMonitoringActive);
    connect(qobject_cast<CS200ADeviceService *>(m_service),
            &CS200ADeviceService::fullScaleDiagnosticsFinished,
            this, &MonitoringController::fullScaleDiagnosticsFinished);
    connect(qobject_cast<CS200ADeviceService *>(m_service),
            &CS200ADeviceService::readFlowSucceeded,
            this, &MonitoringController::onReadFlowSucceeded);
    connect(qobject_cast<CS200ADeviceService *>(m_service),
            &CS200ADeviceService::deviceInfoScanFinished,
            this, &MonitoringController::onDeviceInfoScanFinished);
    m_deviceThread.setObjectName(QStringLiteral("CS200-A 串口工作线程"));
        m_communicationWatchdog.setInterval(500);
        m_communicationWatchdog.setTimerType(Qt::CoarseTimer);
        connect(&m_communicationWatchdog, &QTimer::timeout, this, [this] {
            const bool experimentActive = m_deviceInfo.value("experiment").toMap().value("active").toBool();
            auto *service = qobject_cast<CS200ADeviceService *>(m_service);
            if (service && service->monitoringWatchdogSuspended()) {
                m_monitoringWatchdogArmed = false;
                m_waitingForPostDeviceInfoReadFlow = true;
                m_communicationWatchdog.stop();
                m_lastCommunicationProgress.invalidate();
                return;
            }
            if ((!m_monitoringWatchdogArmed && !experimentActive) || !m_lastCommunicationProgress.isValid()
                || m_lastCommunicationProgress.elapsed() < 2000) return;
            if (!service) return;
            service->requestTransactionCancellation(SerialTransport::CancelReason::WorkerWatchdog);
            if (experimentActive)
                QMetaObject::invokeMethod(service, &CS200ADeviceService::recordExperimentWatchdogTrigger,
                                          Qt::QueuedConnection);
            QMetaObject::invokeMethod(service, &CS200ADeviceService::recoverCommunicationFromWatchdog,
                                      Qt::QueuedConnection);
            m_lastCommunicationProgress.restart();
            emit errorOccurred(QStringLiteral("COMMUNICATION_WORKER_STALLED：已取消当前只读事务并重置通信状态"));
        });
        m_guiHeartbeat.setInterval(100);
        m_guiHeartbeat.setTimerType(Qt::PreciseTimer);
        connect(&m_guiHeartbeat, &QTimer::timeout, this, [this] {
            if (!m_lastGuiHeartbeat.isValid()) {
                m_lastGuiHeartbeat.start();
                return;
            }
            const qint64 elapsed = m_lastGuiHeartbeat.restart();
            if (elapsed <= 500) return;
            if (auto *service = qobject_cast<CS200ADeviceService *>(m_service))
                QMetaObject::invokeMethod(service, [service, elapsed] {
                    service->recordExperimentGuiStall(elapsed);
                }, Qt::QueuedConnection);
        });
    m_deviceThread.start();
    QMetaObject::invokeMethod(m_service, &IDeviceService::connectDevice, Qt::QueuedConnection);
}

MonitoringController::~MonitoringController()
{
    if (m_deviceThread.isRunning()) {
        if (auto *service = qobject_cast<CS200ADeviceService *>(m_service))
            service->requestTransactionCancellation(SerialTransport::CancelReason::ApplicationShutdown);
        QMetaObject::invokeMethod(m_service, &IDeviceService::stopMonitoring, Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(m_service, &IDeviceService::disconnectDevice, Qt::BlockingQueuedConnection);
        m_deviceThread.quit();
        m_deviceThread.wait(3000);
    }
}

QVariantList MonitoringController::gasChannels() const
{
    QVariantList result;
    for (const auto &channel : m_channels) result.append(channel.toVariantMap());
    return result;
}

QString MonitoringController::deviceStatusText() const
{
    if (m_deviceStatus == DeviceStatus::Scanning) return QStringLiteral("正在扫描 MFC");
    if (!m_deviceInfo.value("serialConnected").toBool()) return QStringLiteral("串口未连接");
    const int online = m_deviceInfo.value("onlineCount").toInt();
    const int total = m_deviceInfo.value("deviceCount").toInt();
    if (online == 0) return QStringLiteral("未发现 MFC");
    if (online < total) return QStringLiteral("部分设备异常");
    return QStringLiteral("通信正常");
}

QString MonitoringController::flameStatusText() const
{
    switch (m_flameStatus) {
    case FlameStatus::Unknown: return QStringLiteral("未知");
    case FlameStatus::Normal: return QStringLiteral("正常燃烧");
    case FlameStatus::Off: return QStringLiteral("火焰熄灭");
    case FlameStatus::Fault: return QStringLiteral("火焰异常");
    }
    return QStringLiteral("未知");
}

void MonitoringController::startMonitoring()
{
    if (!m_monitoring)
        QMetaObject::invokeMethod(m_service, &IDeviceService::startMonitoring, Qt::QueuedConnection);
}

void MonitoringController::stopMonitoring()
{
    if (m_monitoring) { m_monitoring = false; emit monitoringChanged(); }
    // The service's false signal can be a duplicate after the optimistic UI
    // state update above, so disarm synchronously here as well.
    m_monitoringWatchdogArmed = false;
    m_communicationWatchdog.stop();
    m_lastCommunicationProgress.invalidate();
    if (auto *service = qobject_cast<CS200ADeviceService *>(m_service))
        service->requestTransactionCancellation(SerialTransport::CancelReason::MonitoringStop);
    QMetaObject::invokeMethod(m_service, &IDeviceService::stopMonitoring, Qt::QueuedConnection);
}

void MonitoringController::selectOperatingPoint(const OperatingPoint &point)
{
    if (!point.hasValidFlowValues()) {
        emit errorOccurred(QStringLiteral("运行点目标流量数据无效"));
        return;
    }
    QMetaObject::invokeMethod(m_service, [service=m_service, point]{ service->selectOperatingPoint(point); },
                              Qt::QueuedConnection);
}

void MonitoringController::setLogDirectory(const QString &directory)
{
    if (auto *service = qobject_cast<CS200ADeviceService *>(m_service))
        QMetaObject::invokeMethod(service, [service, directory]{ service->setLogDirectory(directory); },
                                  Qt::QueuedConnection);
}

void MonitoringController::setAddressConfirmed(int address, bool confirmed)
{
    if (auto *service = qobject_cast<CS200ADeviceService *>(m_service))
        QMetaObject::invokeMethod(service, [service, address, confirmed] {
            service->setAddressConfirmed(address, confirmed);
        }, Qt::QueuedConnection);
}

void MonitoringController::startCommunicationExperiment(int delayMs, int durationSeconds,
                                                          int selectedAddress)
{
    auto *service = qobject_cast<CS200ADeviceService *>(m_service);
    if (!service) {
        emit errorOccurred(QStringLiteral("CS200 通信实验服务不可用"));
        return;
    }
    if (m_monitoring) {
        emit errorOccurred(QStringLiteral("请先停止常规监测，再开始 CS200 通信实验"));
        return;
    }
    QMetaObject::invokeMethod(service, [service, delayMs, durationSeconds, selectedAddress] {
        service->startCommunicationExperiment(delayMs, durationSeconds, selectedAddress);
    }, Qt::QueuedConnection);
}

void MonitoringController::stopCommunicationExperiment()
{
    auto *service = qobject_cast<CS200ADeviceService *>(m_service);
    if (!service) return;
    service->requestExperimentStop();
    QMetaObject::invokeMethod(service, &CS200ADeviceService::stopCommunicationExperiment,
                              Qt::QueuedConnection);
}

void MonitoringController::startControl(const OperatingPoint &point)
{
    if (!point.hasValidFlowValues()) { emit errorOccurred(QStringLiteral("运行点目标流量数据无效")); return; }
    disarmMonitoringWatchdogForDeviceInfo();
    QMetaObject::invokeMethod(m_service, [service=m_service, point] {
        if (auto *cs = qobject_cast<CS200ADeviceService *>(service)) cs->startControl(point);
    }, Qt::QueuedConnection);
}

void MonitoringController::stopControl()
{
    QMetaObject::invokeMethod(m_service, [service=m_service] {
        if (auto *cs = qobject_cast<CS200ADeviceService *>(service)) cs->stopControl();
    }, Qt::QueuedConnection);
}

void MonitoringController::verifyDeviceInformation()
{
    disarmMonitoringWatchdogForDeviceInfo();
    QMetaObject::invokeMethod(m_service, [service=m_service] {
        if (auto *cs = qobject_cast<CS200ADeviceService *>(service)) cs->verifyDeviceInformation();
    }, Qt::QueuedConnection);
}

void MonitoringController::runFullScaleDiagnostics()
{
    disarmMonitoringWatchdogForDeviceInfo();
    QMetaObject::invokeMethod(m_service, [service=m_service] {
        if (auto *cs = qobject_cast<CS200ADeviceService *>(service)) cs->runFullScaleDiagnostics();
    }, Qt::QueuedConnection);
}

void MonitoringController::rescan() { QMetaObject::invokeMethod(m_service, &IDeviceService::rescan, Qt::QueuedConnection); }

void MonitoringController::onFlows(const QList<GasChannel> &channels)
{
    m_channels = channels;
    emit gasChannelsChanged();
    emit flowSampleReceived(channels);
}

void MonitoringController::onReadFlowSucceeded()
{
    auto *service = qobject_cast<CS200ADeviceService *>(m_service);
    const bool suspended = service && service->monitoringWatchdogSuspended();
    if (!shouldArmMonitoringWatchdog(m_monitoring, suspended, true)) return;
    m_waitingForPostDeviceInfoReadFlow = false;
    if (!m_monitoringWatchdogArmed) {
        m_monitoringWatchdogArmed = true;
        m_lastCommunicationProgress.start();
        m_communicationWatchdog.start();
    } else {
        // READ_FLOW success—not a metadata request—is normal polling
        // progress, so it alone advances the watchdog baseline.
        m_lastCommunicationProgress.restart();
    }
}

void MonitoringController::onDeviceInfoScanFinished()
{
    // A long legal scan invalidates the old READ_FLOW baseline.  The first
    // new successful poll is the sole event that can arm the timer again.
    m_monitoringWatchdogArmed = false;
    m_waitingForPostDeviceInfoReadFlow = m_monitoring;
    m_communicationWatchdog.stop();
    m_lastCommunicationProgress.invalidate();
}

void MonitoringController::disarmMonitoringWatchdogForDeviceInfo()
{
    if (auto *service = qobject_cast<CS200ADeviceService *>(m_service))
        service->reserveDeviceInfoScan();
    m_monitoringWatchdogArmed = false;
    m_waitingForPostDeviceInfoReadFlow = true;
    m_communicationWatchdog.stop();
    m_lastCommunicationProgress.invalidate();
}

void MonitoringController::onDeviceStatus(DeviceStatus status)
{
    if (m_deviceStatus == status) return;
    m_deviceStatus = status;
    emit deviceStatusChanged();
}

void MonitoringController::onMonitoringActive(bool active)
{
    if (m_monitoring == active) {
        if (!active) {
            m_monitoringWatchdogArmed = false;
            m_waitingForPostDeviceInfoReadFlow = false;
            m_communicationWatchdog.stop();
            m_lastCommunicationProgress.invalidate();
        }
        return;
    }
    m_monitoring = active;
    if (active) {
        m_lastFinishedRequestId = m_deviceInfo.value("transaction_lastFinishedRequestId").toULongLong();
        // The timer is armed by onFlows() after the first polling result,
        // never while startup DeviceInfo is still issuing metadata requests.
        m_monitoringWatchdogArmed = false;
        m_waitingForPostDeviceInfoReadFlow = false;
    } else {
        m_monitoringWatchdogArmed = false;
        m_waitingForPostDeviceInfoReadFlow = false;
        m_communicationWatchdog.stop();
        m_lastCommunicationProgress.invalidate();
    }
    emit monitoringChanged();
}

void MonitoringController::onFlameStatus(FlameStatus status)
{
    if (m_flameStatus == status) return;
    m_flameStatus = status;
    emit flameStatusChanged();
}

void MonitoringController::onDeviceInfo(const QVariantMap &info)
{
    const QString previousStatusText = deviceStatusText();
    const bool wasExperimentActive = m_deviceInfo.value("experiment").toMap().value("active").toBool();
    m_deviceInfo = info;
    const bool experimentActive = m_deviceInfo.value("experiment").toMap().value("active").toBool();
    const quint64 finished = info.value("transaction_lastFinishedRequestId").toULongLong();
    if (finished != 0 && finished != m_lastFinishedRequestId) {
        m_lastFinishedRequestId = finished;
        // Experiment diagnostics retain their independent transaction
        // heartbeat. Normal polling only progresses on READ_FLOW success.
        if (experimentActive) m_lastCommunicationProgress.restart();
    }
    if (experimentActive && !wasExperimentActive) {
        m_lastCommunicationProgress.start();
        m_lastGuiHeartbeat.start();
        m_guiHeartbeat.start();
        m_communicationWatchdog.start();
    } else if (!experimentActive && wasExperimentActive) {
        m_guiHeartbeat.stop();
        m_lastGuiHeartbeat.invalidate();
        if (!m_monitoring) {
            m_communicationWatchdog.stop();
            m_lastCommunicationProgress.invalidate();
        }
    }
    emit deviceInfoChanged();
    // Real-device status text is derived from independent communication fields
    // in deviceInfo, so refresh it even when the legacy enum did not change.
    if (deviceStatusText() != previousStatusText) emit deviceStatusChanged();
}
