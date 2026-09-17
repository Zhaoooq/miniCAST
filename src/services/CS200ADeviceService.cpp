#include "CS200ADeviceService.h"
#include "utils/FlowDeviationCalculator.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>

CS200ADeviceService::CS200ADeviceService(int interval, double warning, double critical,
                                         double zeroTolerance, QList<MfcDeviceConfig> devices,
                                         MfcManager::Settings settings, int reconnectIntervalMs,
                                         int settlingTimeMs, QString logDirectory, QObject *parent)
    : IDeviceService(parent), m_configs(std::move(devices)), m_settings(std::move(settings)),
      m_log(std::move(logDirectory)), m_sampleIntervalMs(qBound(100, interval, 5000)),
      m_warningPercent(warning), m_criticalPercent(critical), m_zeroTolerance(zeroTolerance),
      m_settlingTimeMs(qMax(0, settlingTimeMs))
{
    rebuildChannels();
    m_pollTimer = new QTimer(this);
    const int enabled = qMax(1, static_cast<int>(std::count_if(m_configs.cbegin(), m_configs.cend(),
        [](const MfcDeviceConfig &device) { return device.enabled; })));
    m_pollTimer->setInterval(qMax(m_settings.interRequestDelayMs,
                                  m_sampleIntervalMs / enabled));
    m_pollTimer->setTimerType(Qt::CoarseTimer);
    connect(m_pollTimer, &QTimer::timeout, this, &CS200ADeviceService::poll);
    // This timer only schedules addresses whose individual communication
    // state is non-normal. MfcManager enforces each address's one-second
    // deadline and serial transport still permits one pending request only.
    m_recoveryTimer = new QTimer(this);
    m_recoveryTimer->setInterval(100);
    m_recoveryTimer->setTimerType(Qt::CoarseTimer);
    connect(m_recoveryTimer, &QTimer::timeout, this, &CS200ADeviceService::retryUnhealthyAddress);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(qBound(500, reconnectIntervalMs, 60000));
    m_reconnectTimer->setTimerType(Qt::VeryCoarseTimer);
    connect(m_reconnectTimer, &QTimer::timeout, this, &CS200ADeviceService::reconnectIfNeeded);
    m_experimentTimer = new QTimer(this);
    m_experimentTimer->setSingleShot(true);
    m_experimentTimer->setTimerType(Qt::PreciseTimer);
    connect(m_experimentTimer, &QTimer::timeout, this, &CS200ADeviceService::pollExperiment);
}

CS200ADeviceService::~CS200ADeviceService()
{
    requestTransactionCancellation(SerialTransport::CancelReason::ApplicationShutdown);
    disconnectDevice();
    const std::lock_guard<std::mutex> lock(m_managerLifetimeMutex);
    m_manager.reset();
}

void CS200ADeviceService::requestTransactionCancellation(SerialTransport::CancelReason reason) noexcept
{
    if (reason == SerialTransport::CancelReason::MonitoringStop
        || reason == SerialTransport::CancelReason::DeviceDisconnect
        || reason == SerialTransport::CancelReason::ApplicationShutdown
        || reason == SerialTransport::CancelReason::ExperimentStop)
        m_lifecycleCancellationRequested.store(true);
    const std::lock_guard<std::mutex> lock(m_managerLifetimeMutex);
    if (m_manager) m_manager->cancelCurrentTransaction(reason);
}

void CS200ADeviceService::reserveDeviceInfoScan() noexcept
{
    // This is safe from the GUI thread.  The actual timer changes and all
    // serial work remain on the service/serial thread.
    m_deviceInfoScanReserved.store(true);
}

void CS200ADeviceService::requestExperimentStop() noexcept
{
    m_experimentStopRequested.store(true);
    requestTransactionCancellation(SerialTransport::CancelReason::ExperimentStop);
}

void CS200ADeviceService::recoverCommunicationFromWatchdog()
{
    if ((!m_monitoring && !m_experiment.active()) || !m_manager) return;
    m_manager->setMonitoringActive(true);
    m_log.write(QStringLiteral("[Startup] WORKER_RESET context=WATCHDOG parser_reset=true READ_ONLY polling continues"));
}

void CS200ADeviceService::setOperationState(const QString &state)
{
    if (m_operationState == state) return;
    m_operationState = state;
    emit deviceInfoChanged(deviceInfo());
}

void CS200ADeviceService::beginDeviceInfoScan(const QString &origin)
{
    m_deviceInfoScanReserved.store(true);
    m_deviceInfoScanActive.store(true);
    // QTimer callbacks and the scan are serialized by this worker thread, so
    // stopping both schedulers here lets any already-running READ_FLOW finish
    // normally before this queued mode switch begins.  No transaction is
    // cancelled to enter DeviceInfo mode.
    m_resumePollingAfterDeviceInfo = m_monitoring && m_pollTimer->isActive();
    m_resumeRecoveryAfterDeviceInfo = m_recoveryTimer->isActive();
    m_pollTimer->stop();
    m_recoveryTimer->stop();
    m_waitingForPostDeviceInfoReadFlow.store(m_resumePollingAfterDeviceInfo);
    m_log.write(QStringLiteral("[Startup/DeviceInfo] DEVICE_INFO_BEGIN origin=%1").arg(origin));
    m_log.write(QStringLiteral("[Polling] PAUSE reason=DEVICE_INFO_BEGIN"));
    m_log.write(QStringLiteral("[Watchdog] DISARM reason=DEVICE_INFO_BEGIN"));
}

void CS200ADeviceService::finishDeviceInfoScan(const QString &origin, bool success)
{
    m_deviceInfoScanActive.store(false);
    m_deviceInfoScanReserved.store(false);
    const int failedCount = m_manager ? m_manager->lastDeviceInfoFailedCount() : 0;
    const int retryRecoveredCount = m_manager ? m_manager->lastDeviceInfoRetryRecoveredCount() : 0;
    m_log.write(QStringLiteral("[Startup/DeviceInfo] DEVICE_INFO_ALL_COMPLETE origin=%1 result=%2 failed_count=%3 retry_recovered_count=%4")
        .arg(origin, success ? QStringLiteral("SUCCESS") : QStringLiteral("FAILED"))
        .arg(failedCount).arg(retryRecoveredCount));

    const bool resume = m_resumePollingAfterDeviceInfo && m_monitoring
        && !m_lifecycleCancellationRequested.load() && m_manager && m_manager->isConnected();
    if (resume) {
        m_log.write(QStringLiteral("[Polling] RESUME reason=DEVICE_INFO_COMPLETE"));
        m_log.write(QStringLiteral("[Watchdog] WAIT_FIRST_READ_FLOW"));
        if (!m_pollTimer->isActive()) m_pollTimer->start();
        if (m_resumeRecoveryAfterDeviceInfo && !m_recoveryTimer->isActive()) m_recoveryTimer->start();
    } else {
        // There will be no post-scan poll when stop/disconnect/shutdown won
        // the race.  Do not retain a stale watchdog suspension in that case.
        m_waitingForPostDeviceInfoReadFlow.store(false);
    }
    m_resumePollingAfterDeviceInfo = false;
    m_resumeRecoveryAfterDeviceInfo = false;
    emit deviceInfoScanFinished();
    if (resume) poll();
}

void CS200ADeviceService::setLogDirectory(const QString &directory)
{
    m_log.setDirectory(directory);
    emit deviceInfoChanged(deviceInfo());
}

void CS200ADeviceService::rebuildChannels()
{
    QMap<int, GasChannel> previous;
    for (const auto &channel : m_channels) previous[channel.address] = channel;
    m_channels.clear();
    const QList<MfcDeviceState> states = m_manager ? m_manager->devices() : QList<MfcDeviceState>{};
    for (const auto &config : m_configs) {
        if (!config.enabled) continue;
        GasChannel channel = previous.value(config.address);
        channel.id = config.stableId();
        channel.address = config.address;
        channel.nameChinese = config.displayName;
        channel.chemicalName = config.gasType == QStringLiteral("待确认") ? QString() : config.gasType;
        channel.gasType = config.gasType;
        channel.function = config.function;
        // Start unavailable.  Configuration values are not device telemetry
        // and must not be displayed as a fallback range while the real
        // full-scale register has not been read successfully.
        channel.flowUnit.clear();
        channel.unit = QStringLiteral("量程/单位待确认");
        channel.fullScaleValue = 0.0;
        channel.tolerancePercent = config.relativeTolerancePercent > 0.0
            ? config.relativeTolerancePercent : m_warningPercent;
        channel.minimum = config.minimumSetpoint;
        channel.maximum = 0.0;
        channel.adjustable = false;
        channel.online = false;
        channel.actualValueAvailable = false;
        channel.currentFlowAvailable = false;
        channel.hasValidActualFlow = false;
        channel.actualFlowFresh = false;
        channel.waitingForActualFlow = true;
        channel.targetFlowAvailable = m_targetFlows.contains(config.address);
        channel.targetFlow = m_targetFlows.value(config.address);
        channel.activeTargetFlow = channel.targetFlow;
        channel.configuredTargetFlowAvailable = m_configuredTargetFlows.contains(config.address);
        channel.configuredTargetFlow = m_configuredTargetFlows.value(config.address);
        channel.operatingPointId = m_activeOperatingPointId;
        // Legacy fields are retained for alarm/export compatibility and always
        // mirror the selected operating-point target, never device telemetry.
        channel.setValue = channel.targetFlow;
        channel.deviationAvailable = false;
        channel.monitoringActive = m_monitoring;
        channel.status = FlowStatus::Offline;
        for (const auto &state : states) {
            if (state.config.address != config.address) continue;
            const bool engineeringReady = state.engineeringConfigured;
            channel.online = state.communicationOnline;
            // A failed READ_FLOW never rewrites the last measured flow.  It
            // only makes the realtime value stale/unavailable.
            channel.actualValueAvailable = state.reading.communicationOk && state.communicationOnline;
            channel.addressDetected = state.addressDetected;
            channel.addressConfirmed = config.addressConfirmed;
            channel.engineeringConfigured = engineeringReady;
            channel.readOnly = !m_controlSession;
            channel.communicationState = state.toVariantMap().value("communicationState").toString();
            channel.communicationStateCode = static_cast<int>(state.communicationState);
            channel.configurationState = config.addressConfirmed && engineeringReady
                ? QStringLiteral("配置完整，只读监测")
                : config.addressConfirmed ? QStringLiteral("工程配置待确认") : QStringLiteral("地址映射待确认");
            channel.status = state.communicationState == MfcCommunicationState::Fault
                ? FlowStatus::CommunicationError
                : (state.communicationState == MfcCommunicationState::Degraded
                   || state.communicationState == MfcCommunicationState::Recovering)
                    ? FlowStatus::Warning
                : state.communicationOnline
                    ? (engineeringReady ? FlowStatus::Normal : FlowStatus::ConfigurationRequired)
                    : state.linkState == MfcLinkState::Offline ? FlowStatus::Offline : FlowStatus::CommunicationError;
            channel.responseTimeMs = state.responseTimeMs;
            const auto stateMap = state.toVariantMap();
            channel.controlState = stateMap.value("controlState").toString();
            channel.controlStateCode = stateMap.value("controlStateCode").toInt();
            channel.effectiveSetpointState = stateMap.value("effectiveSetpointState").toString();
            channel.targetConfirmed = state.targetConfirmed;
            channel.stopState = stateMap.value("stopState").toString();
            channel.stopFailure = stateMap.value("stopFailure").toString();
            // The device's read-only full-scale register is the only source
            // allowed for displayed engineering flow.  Do not resurrect a
            // legacy configured/default range while configuration is pending.
            const double fullScale = config.fullScale;
            const QString flowUnit = config.unit;
            const bool flowScaleAvailable = fullScale > 0.0 && !flowUnit.trimmed().isEmpty();
            channel.fullScaleValue = fullScale;
            channel.flowUnit = flowUnit;
            channel.currentFlowAvailable = state.reading.communicationOk && state.communicationOnline && flowScaleAvailable;
            // A zero-valued READ_FLOW response is a fully valid sample.  The
            // availability/freshness flags deliberately describe successful
            // protocol decoding, not whether the measured value is > 0.
            channel.hasValidActualFlow = state.reading.communicationOk
                && state.communicationOnline && flowScaleAvailable;
            channel.actualFlowFresh = channel.hasValidActualFlow;
            channel.waitingForActualFlow = !channel.hasValidActualFlow;
            channel.flowFresh = channel.actualFlowFresh;
            if (!flowUnit.trimmed().isEmpty()) channel.unit = flowUnit;
            if (state.reading.communicationOk) {
                channel.percentFullScale = state.reading.flowPercent;
                channel.maximum = flowScaleAvailable
                    ? (config.maximumSetpoint > 0.0 ? config.maximumSetpoint : fullScale) : 0.0;
                // The confirmed READ_FLOW payload is normalized to full scale.
                // Only convert it when the device/configuration provides both
                // a real full scale and an engineering unit.
                if (flowScaleAvailable) {
                    channel.currentFlow = state.reading.flowPercent / 100.0 * fullScale;
                    channel.actualFlow = channel.currentFlow;
                    channel.lastValidActualFlow = channel.currentFlow;
                    channel.lastValidFlowTimestamp = state.reading.timestamp;
                    channel.realValue = channel.currentFlow;
                    channel.unit = flowUnit;
                }
            }
            // A successful discovery/read outside monitoring is not a live
            // monitoring sample and must never produce a flow judgement.
            // Operating-point deviation alarms apply only while actively
            // controlling.  A stopped target of zero must not be judged
            // against the previous nonzero operating point.
            if (m_operationState == QStringLiteral("RUNNING") && engineeringReady && state.reading.communicationOk
                && state.communicationOnline) {
                channel.temperature = state.reading.temperature;
                if (channel.targetFlowAvailable) {
                    const auto deviation = FlowDeviationCalculator::calculate(channel.targetFlow, channel.realValue,
                        config.relativeTolerancePercent > 0.0 ? config.relativeTolerancePercent : m_warningPercent,
                        m_criticalPercent, config.absoluteTolerance > 0.0 ? config.absoluteTolerance : m_zeroTolerance,
                        true);
                    channel.deviation = channel.realValue - channel.targetFlow;
                    channel.deviationPercent = deviation.percent;
                    channel.deviationAvailable = deviation.available;
                    channel.status = !state.reading.alarms.isEmpty() ? FlowStatus::Critical
                        : !state.reading.warnings.isEmpty() ? FlowStatus::Warning : deviation.status;
                }
            }
            break;
        }
        m_channels.append(channel);
    }
}

void CS200ADeviceService::updateCommunicationNotice()
{
    const QString notice = m_manager ? m_manager->communicationNotice() : QString();
    if (notice == m_communicationNotice) return;
    m_communicationNotice = notice;
    emit communicationNoticeChanged(notice);
}

QVariantMap CS200ADeviceService::deviceInfo() const
{
    QVariantMap map = m_manager ? m_manager->diagnostics() : QVariantMap{};
    map["mode"] = QStringLiteral("REAL");
    map["modeText"] = QStringLiteral("CS200 真机");
    map["operationState"] = m_operationState;
    map["operationStateText"] = m_operationState == QStringLiteral("IDLE") ? QStringLiteral("空闲")
        : m_operationState == QStringLiteral("READ_ONLY") ? QStringLiteral("监测模式")
        : m_operationState == QStringLiteral("REFERENCE_SELECTED") ? QStringLiteral("目标流量已选择")
        : m_operationState == QStringLiteral("RUNNING") ? QStringLiteral("控制运行")
        : m_operationState == QStringLiteral("STOPPING_CONTROL") ? QStringLiteral("正在停止控制…")
        : m_operationState == QStringLiteral("CONTROL_DEGRADED") ? QStringLiteral("控制停止状态未完全确认")
        : m_operationState == QStringLiteral("CS200_EXPERIMENT") ? QStringLiteral("CS200 通信实验（只读）")
        : m_operationState == QStringLiteral("STOPPING") ? QStringLiteral("正在停止采集") : QStringLiteral("异常");
    map["communicationLog"] = m_log.path();
    map["monitoringActive"] = m_monitoring;
    map["readOnlyMonitoring"] = !m_controlSession;
    map["controlSession"] = m_controlSession;
    map["controlStopping"] = m_operationState == QStringLiteral("STOPPING_CONTROL");
    map["operatingPointId"] = m_activeOperatingPointId;
    QVariantMap experiment = m_experiment.toVariantMap();
    experiment["jsonReportPath"] = m_experimentJsonReport;
    experiment["textReportPath"] = m_experimentTextReport;
    map["experiment"] = experiment;
    return map;
}

void CS200ADeviceService::setStatus(DeviceStatus status)
{
    if (m_status == status) return;
    m_status = status;
    emit deviceStatusChanged(status);
}

void CS200ADeviceService::connectDevice()
{
    if (m_manager && m_manager->isConnected()) return;
    setStatus(DeviceStatus::Scanning);
    m_log.write(QStringLiteral("[Startup] CONNECT_BEGIN configured_devices=%1 baud=%2")
                .arg(m_configs.size()).arg(m_settings.baudRate));
    {
        const std::lock_guard<std::mutex> lock(m_managerLifetimeMutex);
        m_manager = std::make_unique<MfcManager>(m_configs, m_settings,
            [this](const QString &line) { m_log.write(line); });
    }
    if (!m_manager->connectBus()) {
        rebuildChannels();
        setStatus(DeviceStatus::Disconnected);
        setOperationState(QStringLiteral("IDLE"));
        if (!m_reconnectTimer->isActive()) m_reconnectTimer->start();
        emit communicationError(QStringLiteral("未在配置地址上发现有效 CS MFC 响应"));
        emit deviceInfoChanged(deviceInfo());
        emit flowDataUpdated(m_channels);
        return;
    }
    // connectBus probes candidate ports to find the CS200-A.  Scan every
    // configured address again on the selected open port so initial state is
    // address-local and every unsuccessful address receives a retry deadline.
    m_manager->scanConfiguredAddresses();
    if (m_manager->hasOnlineDevice()) m_reconnectTimer->stop();
    else if (!m_reconnectTimer->isActive()) m_reconnectTimer->start();
    rebuildChannels();
    setStatus(DeviceStatus::Connected);
    setOperationState(QStringLiteral("IDLE"));
    emit deviceInfoChanged(deviceInfo());
    emit flowDataUpdated(m_channels);
    if (!m_recoveryTimer->isActive()) m_recoveryTimer->start();
    // A connected application is always in monitoring mode.  READ_FLOW and
    // logging therefore survive a later Stop Control transaction.
    startMonitoring();
}

void CS200ADeviceService::disconnectDevice()
{
    requestTransactionCancellation(SerialTransport::CancelReason::DeviceDisconnect);
    m_log.write(QStringLiteral("[transport] CANCELLATION_REQUESTED reason=DEVICE_DISCONNECT context=DISCONNECT_DEVICE"));
    m_pollTimer->stop();
    m_recoveryTimer->stop();
    m_reconnectTimer->stop();
    m_experimentTimer->stop();
    if (m_experiment.active()) finishCommunicationExperiment(QStringLiteral("device_disconnected"));
    m_monitoring = false;
    emit monitoringActiveChanged(false);
    if (m_manager) m_manager->disconnectBus();
    updateCommunicationNotice();
    rebuildChannels();
    setOperationState(QStringLiteral("IDLE"));
    setStatus(DeviceStatus::Disconnected);
    emit deviceInfoChanged(deviceInfo());
    emit flowDataUpdated(m_channels);
}

void CS200ADeviceService::rescan()
{
    if (m_experiment.active()) {
        emit communicationError(QStringLiteral("CS200 通信实验期间禁止扫描或重连"));
        return;
    }
    m_pollTimer->stop();
    m_monitoring = false;
    emit monitoringActiveChanged(false);
    if (m_manager) m_manager->disconnectBus();
    {
        const std::lock_guard<std::mutex> lock(m_managerLifetimeMutex);
        m_manager.reset();
    }
    connectDevice();
}

void CS200ADeviceService::startMonitoring()
{
    if (m_experiment.active()) {
        emit communicationError(QStringLiteral("CS200 通信实验正在运行，不能启动生产监测"));
        return;
    }
    if (m_monitoring) return;
    if (!m_manager || !m_manager->isConnected()) {
        setOperationState(QStringLiteral("ERROR"));
        emit communicationError(QStringLiteral("启动失败：串口未连接"));
        return;
    }
    if (!m_manager->hasOnlineDevice()) {
        setOperationState(QStringLiteral("ERROR"));
        emit communicationError(QStringLiteral("启动失败：串口已连接，但未发现可通信的 MFC"));
        return;
    }
    m_lifecycleCancellationRequested.store(false);
    m_monitoring = true;
    m_manager->setMonitoringActive(true);
    m_log.write(QStringLiteral("[Startup] READ_FLOW_START poll_interval_ms=%1").arg(m_pollTimer->interval()));
    setOperationState(QStringLiteral("READ_ONLY"));
    setStatus(DeviceStatus::Monitoring);
    emit monitoringActiveChanged(true);
    m_pollTimer->start();
    if (!m_recoveryTimer->isActive()) m_recoveryTimer->start();
    poll();
}

void CS200ADeviceService::stopMonitoring()
{
    if (m_controlSession) { stopControl(); return; }
    requestTransactionCancellation(SerialTransport::CancelReason::MonitoringStop);
    m_log.write(QStringLiteral("[transport] CANCELLATION_REQUESTED reason=MONITORING_STOP context=STOP_MONITORING"));
    setOperationState(QStringLiteral("STOPPING"));
    m_pollTimer->stop();
    // Keep address recovery alive while the port remains open.  Stopping
    // monitoring stops logging/normal polling, not automatic discovery.
    m_monitoring = false;
    if (m_manager) m_manager->setMonitoringActive(false);
    emit monitoringActiveChanged(false);
    setOperationState(QStringLiteral("IDLE"));
    setStatus(m_manager && m_manager->isConnected() ? DeviceStatus::Connected : DeviceStatus::Disconnected);
}

void CS200ADeviceService::selectOperatingPoint(const OperatingPoint &point)
{
    if (!point.hasValidFlowValues()) {
        emit communicationError(QStringLiteral("运行点目标流量数据无效"));
        return;
    }
    m_targetFlows = point.mfcSetpoints;
    m_configuredTargetFlows = point.mfcSetpoints;
    // Selection alone never takes control; writing requires Start Control.
    setOperationState(m_monitoring ? QStringLiteral("READ_ONLY") : QStringLiteral("REFERENCE_SELECTED"));
    rebuildChannels();
    emit deviceInfoChanged(deviceInfo());
    emit flowDataUpdated(m_channels);
}

void CS200ADeviceService::startControl(const OperatingPoint &point)
{
    if (!point.hasValidFlowValues()) { emit communicationError(QStringLiteral("运行点目标流量数据无效")); return; }
    if (!m_manager || !m_manager->isConnected()) {
        m_deviceInfoScanReserved.store(false);
        emit deviceInfoScanFinished();
        emit communicationError(QStringLiteral("开始控制失败：串口未连接")); return;
    }
    m_targetFlows = point.mfcSetpoints;
    m_configuredTargetFlows = point.mfcSetpoints;
    m_activeOperatingPointId = point.id;
    setOperationState(QStringLiteral("PREPARING_CONTROL"));
    beginDeviceInfoScan(QStringLiteral("CONTROL_PREFLIGHT"));
    QString error;
    bool writesStarted = false;
    const bool applied = m_manager->applyOperatingPoint(m_targetFlows, &error, &writesStarted);
    finishDeviceInfoScan(QStringLiteral("CONTROL_PREFLIGHT"), applied);
    if (!applied) {
        // A metadata preflight failure happens before Current CM/Hold/
        // Setpoint/Follow writes. It is not a control session and must not
        // present the UI as running or offer a misleading Stop Control.
        m_controlSession = writesStarted;
        setOperationState(writesStarted ? QStringLiteral("CONTROL_DEGRADED") : QStringLiteral("READ_ONLY")); rebuildChannels();
        emit communicationError(error); emit deviceInfoChanged(deviceInfo()); emit flowDataUpdated(m_channels); return;
    }
    m_controlSession = true;
    m_monitoring = true;
    m_manager->setMonitoringActive(true); setStatus(DeviceStatus::Monitoring); setOperationState(QStringLiteral("RUNNING"));
    emit monitoringActiveChanged(true); if (!m_pollTimer->isActive()) m_pollTimer->start();
    if (!m_recoveryTimer->isActive()) m_recoveryTimer->start();
    m_log.write(QStringLiteral("CONTROL_START operating_point=%1").arg(point.name));
    rebuildChannels(); emit deviceInfoChanged(deviceInfo()); emit flowDataUpdated(m_channels); poll();
}

void CS200ADeviceService::stopControl()
{
    if (!m_manager || !m_controlSession) return;
    setOperationState(QStringLiteral("STOPPING_CONTROL")); QString error;
    m_log.write(QStringLiteral("STOP_CONTROL_REQUESTED operating_point=%1").arg(m_activeOperatingPointId));
    // Publish target=0 before the first serial write.  actualFlow stays at
    // its last READ_FLOW value until another valid response arrives.
    for (auto it = m_targetFlows.begin(); it != m_targetFlows.end(); ++it) it.value() = 0.0;
    rebuildChannels(); emit deviceInfoChanged(deviceInfo()); emit flowDataUpdated(m_channels);
    const bool ok = m_manager->stopControl(&error, [this] {
        rebuildChannels();
        emit deviceInfoChanged(deviceInfo());
        emit flowDataUpdated(m_channels);
    });
    // Do not erase the last operating point.  On partial failure leave the
    // session actionable so the operator can retry Stop Control.
    m_controlSession = !ok;
    // Continue READ_FLOW after a verified zero target. Zero target is not an isolation valve.
    m_monitoring = true; m_manager->setMonitoringActive(true);
    setOperationState(ok ? QStringLiteral("READ_ONLY") : QStringLiteral("CONTROL_DEGRADED"));
    m_log.write(ok ? QStringLiteral("STOP_CONTROL_COMPLETED targets_zeroed; external isolation still required") : error);
    rebuildChannels(); emit deviceInfoChanged(deviceInfo()); emit flowDataUpdated(m_channels);
    if (!ok) emit communicationError(error);
}

void CS200ADeviceService::verifyDeviceInformation()
{
    if (!m_manager || !m_manager->isConnected()) {
        m_deviceInfoScanReserved.store(false);
        emit deviceInfoScanFinished();
        emit communicationError(QStringLiteral("设备信息核验失败：串口未连接"));
        return;
    }
    if (m_controlSession) {
        m_deviceInfoScanReserved.store(false);
        emit deviceInfoScanFinished();
        emit communicationError(QStringLiteral("控制会话期间不能执行设备信息核验；请先停止控制"));
        return;
    }
    beginDeviceInfoScan(QStringLiteral("MANUAL_VERIFY"));
    QString error;
    const bool ok = m_manager->verifyDeviceInformation(&error);
    finishDeviceInfoScan(QStringLiteral("MANUAL_VERIFY"), ok);
    rebuildChannels();
    emit deviceInfoChanged(deviceInfo());
    emit flowDataUpdated(m_channels);
    if (ok) m_log.write(QStringLiteral("DEVICE_INFORMATION_VERIFIED read_only=true"));
    else emit communicationError(error);
}

void CS200ADeviceService::runFullScaleDiagnostics()
{
    if (!m_manager || !m_manager->isConnected()) {
        m_deviceInfoScanReserved.store(false);
        emit deviceInfoScanFinished();
        emit communicationError(QStringLiteral("Full Scale 诊断失败：串口未连接"));
        emit fullScaleDiagnosticsFinished();
        return;
    }
    if (m_controlSession) {
        m_deviceInfoScanReserved.store(false);
        emit deviceInfoScanFinished();
        emit communicationError(QStringLiteral("控制会话期间不能执行 Full Scale 诊断；请先停止控制"));
        emit fullScaleDiagnosticsFinished();
        return;
    }
    beginDeviceInfoScan(QStringLiteral("FULL_SCALE_DIAGNOSTICS"));
    QString error;
    const bool ok = m_manager->runFullScaleDiagnostics(&error);
    finishDeviceInfoScan(QStringLiteral("FULL_SCALE_DIAGNOSTICS"), ok);
    rebuildChannels();
    emit deviceInfoChanged(deviceInfo());
    emit flowDataUpdated(m_channels);
    if (ok) m_log.write(QStringLiteral("FULL_SCALE_DIAGNOSTICS_COMPLETED read_only=true repeats=10 alternating=32,34"));
    else emit communicationError(error);
    emit fullScaleDiagnosticsFinished();
}

void CS200ADeviceService::poll()
{
    if (!m_monitoring || m_deviceInfoScanActive.load() || !m_manager || !m_manager->isConnected()) return;
    QString error;
    const bool ok = m_manager->pollNext(&error);
    rebuildChannels();
    updateCommunicationNotice();
    const auto diagnostics = m_manager->diagnostics();
    const int onlineCount = diagnostics.value("onlineCount").toInt();
    bool alarm = false;
    bool warning = false;
    for (const auto &channel : m_channels) {
        alarm |= channel.status == FlowStatus::Critical;
        warning |= channel.status == FlowStatus::Warning;
    }
    if (diagnostics.value("communicationFaultCount").toInt() > 0)
        setStatus(DeviceStatus::CommunicationError);
    else if (onlineCount > 0 && onlineCount < diagnostics.value("deviceCount").toInt())
        setStatus(DeviceStatus::Warning);
    else if (alarm) setStatus(DeviceStatus::Alarm);
    else if (warning) setStatus(DeviceStatus::Warning);
    else setStatus(DeviceStatus::Monitoring);
    emit deviceInfoChanged(deviceInfo());
    emit flowDataUpdated(m_channels);
    if (ok) {
        if (m_waitingForPostDeviceInfoReadFlow.exchange(false))
            m_log.write(QStringLiteral("[Polling] READ_FLOW_SUCCESS phase=POST_DEVICEINFO"));
        else
            m_log.write(QStringLiteral("[Polling] READ_FLOW_SUCCESS"));
        emit readFlowSucceeded();
    }
    Q_UNUSED(ok)
    // A single READ_FLOW error is retained in the address-specific debug log
    // and never becomes a user-facing notification.  updateCommunicationNotice()
    // publishes only thresholded, Chinese automatic-retry state.
    // A run of bad frames or timeouts must not terminate the poll engine.
    // Keep issuing finite READ_FLOW transactions so any device can recover on
    // the next round. A physical serial error is surfaced in diagnostics and
    // each subsequent request still has a bounded deadline.
    if (m_manager->allEnabledDevicesOffline())
        setStatus(DeviceStatus::CommunicationError);
}

void CS200ADeviceService::retryUnhealthyAddress()
{
    if (m_experiment.active() || !m_manager || !m_manager->isConnected()) return;
    QString error;
    // pollRecoveryDue is deliberately one synchronous READ_FLOW transaction
    // at most; the transport's pending-request gate remains the sole owner of
    // the serial bus.
    m_manager->pollRecoveryDue(&error);
    rebuildChannels();
    updateCommunicationNotice();
    emit deviceInfoChanged(deviceInfo());
    emit flowDataUpdated(m_channels);
}

void CS200ADeviceService::reconnectIfNeeded()
{
    if (m_experiment.active()) return;
    if (m_manager && m_manager->isConnected() && m_manager->hasOnlineDevice()) {
        m_reconnectTimer->stop();
        return;
    }
    if (m_manager) m_manager->disconnectBus();
    connectDevice();
}

void CS200ADeviceService::startCommunicationExperiment(int delayMs, int durationSeconds,
                                                        int selectedAddress)
{
    if (m_experiment.active()) return;
    if (m_monitoring) {
        emit communicationError(QStringLiteral("请先停止常规监测，再开始 CS200 通信实验"));
        return;
    }
    if (selectedAddress != 0 && (selectedAddress < 32 || selectedAddress > 36)) {
        emit communicationError(QStringLiteral("实验设备选择无效"));
        return;
    }
    const int firstRequired = selectedAddress == 0 ? 32 : selectedAddress;
    const int lastRequired = selectedAddress == 0 ? 36 : selectedAddress;
    for (int address = firstRequired; address <= lastRequired; ++address) {
        const bool configured = std::any_of(m_configs.cbegin(), m_configs.cend(), [address](const auto &config) {
            return config.enabled && config.address == address;
        });
        if (!configured) {
            emit communicationError(QStringLiteral("实验地址 %1 未在当前配置中启用").arg(address));
            return;
        }
    }
    m_reconnectTimer->stop();
    if (!m_manager) {
        const std::lock_guard<std::mutex> lock(m_managerLifetimeMutex);
        m_manager = std::make_unique<MfcManager>(m_configs, m_settings,
            [this](const QString &line) { m_log.write(line); });
    }
    if (!m_manager->isConnected() && !m_manager->connectBusForExperiment()) {
        setStatus(DeviceStatus::Disconnected);
        emit communicationError(QStringLiteral("无法打开当前配置串口；实验未启动（未执行地址扫描）"));
        emit deviceInfoChanged(deviceInfo());
        return;
    }

    m_experimentStopRequested.store(false);
    m_experimentJsonReport.clear();
    m_experimentTextReport.clear();
    m_manager->setMonitoringActive(true);
    m_manager->setExperimentLogging(true);
    m_manager->resetExperimentSpacing();
    m_experiment.start(delayMs, durationSeconds, selectedAddress);
    setOperationState(QStringLiteral("CS200_EXPERIMENT"));
    setStatus(DeviceStatus::Monitoring);
    const QVariantMap serial = m_manager->serialRuntimeConfiguration();
    const QString addresses = selectedAddress == 0 ? QStringLiteral("32,33,34,35,36")
                                                    : QString::number(selectedAddress);
    m_log.write(QStringLiteral("CS200_EXPERIMENT_START READ_ONLY=true experiment_mode=%1 mode=READ_FLOW_ONLY addresses=%2 "
                               "service=0x80 class=0x68 instance=0x01 attribute=0xB9 delay_ms=%3 duration_s=%4 "
                               "port=%5 baud=%6 data_bits=%7 parity=%8 stop_bits=%9 flow_control=%10")
                .arg(selectedAddress == 0 ? QStringLiteral("MULTI_DEVICE") : QStringLiteral("SINGLE_DEVICE"), addresses)
                .arg(m_experiment.toVariantMap().value("delayMs").toInt())
                .arg(m_experiment.toVariantMap().value("durationSeconds").toInt())
                .arg(serial.value("port").toString()).arg(serial.value("baudRate").toInt())
                .arg(serial.value("dataBits").toString(), serial.value("parity").toString(),
                     serial.value("stopBits").toString(), serial.value("flowControl").toString()));
    emit deviceInfoChanged(deviceInfo());
    m_experimentTimer->start(0);
}

void CS200ADeviceService::stopCommunicationExperiment()
{
    if (!m_experiment.active()) return;
    finishCommunicationExperiment(QStringLiteral("stopped_by_user"));
}

void CS200ADeviceService::pollExperiment()
{
    if (!m_experiment.active()) return;
    if (m_experimentStopRequested.load()) {
        finishCommunicationExperiment(QStringLiteral("stopped_by_user"));
        return;
    }
    if (m_experiment.expired()) {
        finishCommunicationExperiment(QStringLiteral("duration_complete"));
        return;
    }

    QString error;
    const int address = m_experiment.currentAddress();
    const bool success = m_manager && m_manager->pollExperimentFlow(address, &error);
    const QVariantMap transaction = m_manager ? m_manager->lastTransactionDiagnostics() : QVariantMap{};
    if (!(m_experimentStopRequested.load()
          && transaction.value("result").toString() == QStringLiteral("CANCELLED")))
        m_experiment.recordTransaction(transaction, success, error);

    const QString result = transaction.value("result").toString();
    // A protocol frame can be structurally valid yet fail READ_FLOW payload
    // decoding. The transport then reports SUCCESS, so capture its RAW here.
    if (!success && result == QStringLiteral("SUCCESS")) {
        m_log.write(QStringLiteral("EXPERIMENT_ERROR_RAW requestId=%1 address=%2 TX_RAW=%3 RX_RAW=%4 "
                                   "parser_state=%5 expected_service=%6 received_service=%7 "
                                   "expected_class=%8 received_class=%9 expected_attribute=%10 "
                                   "received_attribute=%11 expected_checksum=%12 received_checksum=%13 "
                                   "previous_requestId=%14 previous_finish_timestamp=%15 current_TX_timestamp=%16 error=%17")
            .arg(transaction.value("requestId").toULongLong()).arg(transaction.value("address").toInt())
            .arg(transaction.value("txRaw").toString(), transaction.value("rxRaw").toString())
            .arg(transaction.value("parserState").toString())
            .arg(transaction.value("expectedService").toInt()).arg(transaction.value("receivedService").toInt())
            .arg(transaction.value("expectedClass").toInt()).arg(transaction.value("receivedClass").toInt())
            .arg(transaction.value("expectedAttribute").toInt()).arg(transaction.value("receivedAttribute").toInt())
            .arg(transaction.value("expectedChecksum").toInt()).arg(transaction.value("receivedChecksum").toInt())
            .arg(transaction.value("previousRequestId").toULongLong())
            .arg(transaction.value("previousFinishTimestamp").toString(), transaction.value("currentTxTimestamp").toString())
            .arg(error));
    }
    if (!success && result == QStringLiteral("SERIAL_ERROR")
        && m_manager && !m_manager->physicalPortExists()) {
        m_manager->disconnectBus();
        rebuildChannels();
        setStatus(DeviceStatus::Disconnected);
        finishCommunicationExperiment(QStringLiteral("serial_disconnected"));
        emit communicationError(QStringLiteral("物理串口已消失，CS200 通信实验停止"));
        return;
    }

    m_experiment.advanceAddress();
    // Compensate for post-transaction bookkeeping: the requested interval is
    // measured from transport FINISH, not from the end of this slot.
    if (!m_experiment.expired() && !m_experimentStopRequested.load()) {
        const int configuredDelay = m_experiment.toVariantMap().value("delayMs").toInt();
        const QDateTime finishedAt = QDateTime::fromString(
            transaction.value("finishTimestamp").toString(), Qt::ISODateWithMs);
        const int bookkeepingMs = finishedAt.isValid()
            ? qMax(0, static_cast<int>(finishedAt.msecsTo(QDateTime::currentDateTime()))) : 0;
        m_experimentTimer->start(qMax(0, configuredDelay - bookkeepingMs));
    }
    emit deviceInfoChanged(deviceInfo());
    if (m_experiment.expired()) finishCommunicationExperiment(QStringLiteral("duration_complete"));
}

void CS200ADeviceService::finishCommunicationExperiment(const QString &reason)
{
    if (!m_experiment.active()) return;
    m_experimentTimer->stop();
    m_experiment.finish(reason);
    if (m_manager) {
        m_manager->setExperimentLogging(false);
        m_manager->setMonitoringActive(false);
    }
    const QString logDir = QFileInfo(m_log.path()).absolutePath();
    const QString reportDir = QDir(logDir).absoluteFilePath(QStringLiteral("../exports"));
    QString reportError;
    if (!m_experiment.writeReports(reportDir, &m_experimentJsonReport,
                                   &m_experimentTextReport, &reportError)) {
        m_log.write(QStringLiteral("CS200_EXPERIMENT_REPORT_ERROR %1").arg(reportError));
        emit communicationError(reportError);
    }
    const auto global = m_experiment.toVariantMap().value("global").toMap();
    m_log.write(QStringLiteral("CS200_EXPERIMENT_FINISH reason=%1 requests=%2 success=%3 errors=%4 "
                               "configured_delay_ms=%5 actual_average_gap_ms=%6 gui_stalls=%7 worker_watchdog=%8")
        .arg(reason).arg(global.value("totalRequests").toInt())
        .arg(global.value("totalSuccesses").toInt()).arg(global.value("totalErrors").toInt())
        .arg(global.value("configuredDelayMs").toInt())
        .arg(global.value("actualAverageTransactionGapMs").toDouble(), 0, 'f', 3)
        .arg(global.value("guiHeartbeatStalls").toInt())
        .arg(global.value("workerWatchdogTriggers").toInt()));
    setOperationState(QStringLiteral("IDLE"));
    setStatus(m_manager && m_manager->isConnected() ? DeviceStatus::Connected : DeviceStatus::Disconnected);
    emit deviceInfoChanged(deviceInfo());
}

void CS200ADeviceService::recordExperimentGuiStall(qint64 stallMs)
{
    m_experiment.recordGuiHeartbeatStall(stallMs);
}

void CS200ADeviceService::recordExperimentWatchdogTrigger()
{
    m_experiment.recordWorkerWatchdogTrigger();
}

void CS200ADeviceService::setAddressConfirmed(int address, bool confirmed)
{
    for (auto &config : m_configs)
        if (config.address == address) config.addressConfirmed = confirmed;
    if (m_manager) m_manager->setAddressConfirmed(address, confirmed);
    rebuildChannels();
    emit deviceInfoChanged(deviceInfo());
    emit flowDataUpdated(m_channels);
}
