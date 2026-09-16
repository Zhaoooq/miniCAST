#include "AppController.h"
#include "utils/CsvExporter.h"
#include <QDateTime>
#include <QDir>
#include <QSet>
#include <cmath>

namespace {
bool pointMatches(const OperatingPoint &point, const OperatingPoint &values)
{
    constexpr double epsilon = 1e-6;
    if (point.mfcSetpoints.keys() != values.mfcSetpoints.keys()) return false;
    for (auto it = point.mfcSetpoints.cbegin(); it != point.mfcSetpoints.cend(); ++it)
        if (std::abs(it.value() - values.mfcSetpoints.value(it.key())) >= epsilon) return false;
    return true;
}

}

AppController::AppController(QObject *parent)
    : QObject(parent),
      m_points(m_config.dataRoot(), this, m_config.mfcDevices()),
      m_alarms(this), m_logging(m_config.dataRoot(), this),
      m_monitoring(m_config.sampleIntervalMs(), m_config.warningDeviationPercent(),
                   m_config.criticalDeviationPercent(), m_config.zeroFlowTolerance(), this,
                   m_config.dataRoot() + QStringLiteral("/logs"),
                   m_config.mfcDevices(), m_config.serialPort(), m_config.mfcPreferredBaud(),
                   m_config.ackTimeoutMs(),
                   m_config.responseTimeoutMs(), m_config.retryCount(),
                   m_config.offlineFailureThreshold(), m_config.reconnectIntervalMs(),
                   m_config.settlingTimeMs(), m_config.interRequestDelayMs(),
                   m_config.freshnessTimeoutMs(), m_config.metadataInterRequestDelayMs())
{
    if (!m_config.lastLoadSucceeded())
        m_logging.logApplication(QStringLiteral("配置读取失败，已使用安全默认值"));
    m_runtimeTimer.setInterval(1000);
    m_runtimeTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_runtimeTimer, &QTimer::timeout, this, &AppController::runningTimeChanged);
    connect(&m_monitoring, &MonitoringController::gasChannelsChanged, this, &AppController::gasChannelsChanged);
    connect(&m_monitoring, &MonitoringController::monitoringChanged, this, [this]{
        if (monitoring() && !m_runtime.isValid()) {
            if (!m_logging.start()) notify(QStringLiteral("数据日志启动失败：") + m_logging.lastError());
            m_runtime.start();
            m_runtimeTimer.start();
        } else if (!monitoring() && m_runtime.isValid()) {
            m_accumulatedMs += m_runtime.elapsed();
            m_runtime.invalidate();
            m_runtimeTimer.stop();
            m_logging.stop();
            emit runningTimeChanged();
        }
        emit monitoringChanged();
    });
    connect(&m_monitoring, &MonitoringController::deviceStatusChanged, this, [this]{
        emit deviceStatusChanged();
        m_logging.logApplication(QStringLiteral("设备状态：") + deviceStatusText());
    });
    connect(&m_monitoring, &MonitoringController::deviceInfoChanged, this, &AppController::deviceInfoChanged);
    connect(&m_monitoring, &MonitoringController::fullScaleDiagnosticsFinished, this, [this] {
        if (!m_fullScaleDiagnosticsRunning) return;
        m_fullScaleDiagnosticsRunning = false;
        emit fullScaleDiagnosticsRunningChanged();
    });
    connect(&m_monitoring, &MonitoringController::flameStatusChanged, this, [this]{
        emit flameStatusChanged();
        m_logging.logApplication(QStringLiteral("火焰状态：") + flameStatusText());
    });
    connect(&m_monitoring, &MonitoringController::errorOccurred, this, [this](const QString &e){ notify(e); m_logging.logApplication(e); });
    // Thresholded address-level communication states are handled by
    // AlarmService and rendered in the status alarm area. They are not modal
    // notifications and never expose parser-level errors to the operator.
    connect(&m_monitoring, &MonitoringController::flowSampleReceived, this, [this](const QList<GasChannel> &channels){
        m_alarms.processFlows(channels);
        if (monitoring()) {
            m_logging.logFlows(channels, m_monitoring.deviceStatus(), m_monitoring.flameStatus());
        }
    });
    connect(&m_points, &OperatingPointService::pointsChanged, this, &AppController::operatingPointsChanged);
    connect(&m_alarms, &AlarmService::alarmsChanged, this, &AppController::alarmsChanged);
    connect(&m_alarms, &AlarmService::alarmRaised, this, [this](const AlarmRecord &a){
        m_logging.logApplication(QStringLiteral("报警产生：") + a.channelName + QStringLiteral(" ") + a.message);
        m_logging.logAlarm(a, QStringLiteral("未恢复"));
    });
    connect(&m_alarms, &AlarmService::alarmRecovered, this, [this](const AlarmRecord &a){
        m_logging.logApplication(QStringLiteral("报警恢复：") + a.channelName);
        m_logging.logAlarm(a, QStringLiteral("已恢复"));
    });
    // A run point is an explicit local comparison choice.  Do not silently
    // turn a preset into the current run point at application startup.
}

AppController::~AppController()
{
    if (monitoring()) stopMonitoring();
    m_config.save();
}

QVariantList AppController::recentAlarms() const
{
    auto all = m_alarms.alarmMaps();
    while (all.size() > 5) all.removeLast();
    return all;
}

int AppController::alarmTotalCount() const
{
    return m_alarms.records().size();
}

int AppController::activeAlarmCount() const
{
    int count = 0;
    for (const auto &alarm : m_alarms.records())
        if (!alarm.recoveryTime.isValid()) ++count;
    return count;
}

int AppController::alarmCount24h() const
{
    int count = 0;
    const auto cutoff = QDateTime::currentDateTime().addSecs(-24 * 60 * 60);
    for (const auto &alarm : m_alarms.records())
        if (alarm.timestamp >= cutoff) ++count;
    return count;
}

QString AppController::runningTimeText() const
{
    qint64 total = m_accumulatedMs + (monitoring() && m_runtime.isValid() ? m_runtime.elapsed() : 0);
    const qint64 seconds = total / 1000;
    return QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

void AppController::startMonitoring()
{
    if (monitoring()) return;
    m_monitoring.startMonitoring();
    notify(QStringLiteral("正在启动监测模式；不会向 MFC 写入目标流量"));
}

void AppController::startControl()
{
    if (m_fullScaleDiagnosticsRunning) {
        notify(QStringLiteral("量程隔离诊断进行中，不能开始控制"));
        return;
    }
    if (m_currentPoint.id.isEmpty()) { notify(QStringLiteral("请先选择有效运行点")); return; }
    QString error; if (!validateTargetFlows(m_currentPoint, &error)) { notify(error); return; }
    m_monitoring.startControl(m_currentPoint);
    m_logging.logApplication(QStringLiteral("开始控制：") + m_currentPoint.name);
    notify(QStringLiteral("正在执行控制预检并下发运行点：") + m_currentPoint.name);
}

void AppController::stopControl()
{
    m_monitoring.stopControl();
    m_logging.logApplication(QStringLiteral("停止控制：请求将目标流量归零"));
    notify(QStringLiteral("正在将目标流量归零；这不等于气路安全隔离"));
}

void AppController::verifyDeviceInformation()
{
    if (m_fullScaleDiagnosticsRunning) {
        notify(QStringLiteral("量程隔离诊断进行中，不能执行设备信息核验"));
        return;
    }
    if (controlling()) { notify(QStringLiteral("请先停止控制，再执行只读设备信息核验")); return; }
    m_monitoring.verifyDeviceInformation();
    notify(QStringLiteral("正在读取设备信息；此操作不会向 MFC 写入任何参数"));
}

void AppController::runFullScaleDiagnostics()
{
    if (m_fullScaleDiagnosticsRunning) return;
    if (controlling()) { notify(QStringLiteral("请先停止控制，再执行只读 Full Scale 诊断")); return; }
    m_fullScaleDiagnosticsRunning = true;
    emit fullScaleDiagnosticsRunningChanged();
    m_monitoring.runFullScaleDiagnostics();
    notify(QStringLiteral("正在执行只读量程诊断：每台重复 10 次，并交错读取地址 32/34"));
}

void AppController::stopMonitoring()
{
    if (!monitoring()) return;
    m_monitoring.stopMonitoring();
    notify(QStringLiteral("正在停止数据采集；设备控制参数保持不变"));
}

void AppController::startCommunicationExperiment(int delayMs, int durationSeconds,
                                                  int selectedAddress)
{
    if (monitoring()) {
        notify(QStringLiteral("请先停止常规监测，再开始通信实验"));
        return;
    }
    if (delayMs != 200 || !QList<int>{300, 600}.contains(durationSeconds)
        || (selectedAddress != 0 && (selectedAddress < 32 || selectedAddress > 36))) {
        notify(QStringLiteral("实验参数无效"));
        return;
    }
    m_monitoring.startCommunicationExperiment(delayMs, durationSeconds, selectedAddress);
    notify(QStringLiteral("正在启动严格只读的 CS200 READ_FLOW 通信实验"));
}

void AppController::stopCommunicationExperiment()
{
    m_monitoring.stopCommunicationExperiment();
    notify(QStringLiteral("正在停止 CS200 通信实验并生成报告"));
}

void AppController::selectOperatingPoint(const QString &id)
{
    const auto point = m_points.point(id);
    if (point.id.isEmpty()) { notify(QStringLiteral("未找到运行点")); return; }
    QString validationError;
    if (!validateTargetFlows(point, &validationError)) { notify(validationError); return; }
    m_currentPoint = point;
    m_monitoring.selectOperatingPoint(point);
    emit currentPointChanged();
    m_logging.logApplication(QStringLiteral("选择运行点：") + point.name);
    notify(QStringLiteral("已选择运行点：") + point.name + QStringLiteral("；点击开始控制后才会写入 MFC"));
}

void AppController::saveOperatingPoint(const QVariantMap &d)
{
    OperatingPoint p;
    p.id = d.value("id").toString(); p.name = d.value("name").toString().trimmed();
    const QVariantMap targetFlows = d.value("mfcTargetFlows", d.value("mfcSetpoints")).toMap();
    for (auto it = targetFlows.cbegin(); it != targetFlows.cend(); ++it) {
        bool addressOk = false;
        const int address = it.key().toInt(&addressOk);
        bool valueOk = false;
        const double value = it.value().toDouble(&valueOk);
        if (addressOk && valueOk) p.mfcSetpoints[address] = value;
    }
    p.description = d.value("description").toString();
    p.type = OperatingPointType::Customer;
    p.readOnly = false;
    if (monitoring() && !p.id.isEmpty() && p.id == m_currentPoint.id) {
        notify(QStringLiteral("请先停止监测，再编辑当前运行点目标参数"));
        return;
    }
    if (p.name.isEmpty() && p.id.isEmpty()) p.name = m_points.nextCustomerName();
    QString validationError;
    if (!validateTargetFlows(p, &validationError)) { notify(validationError); return; }
    const bool wasNew = p.id.isEmpty();
    const bool matchesCurrentSettings = pointMatches(p, pointFromCurrentTargets());
    const bool ok = m_points.addOrUpdate(p);
    if (ok) m_logging.logApplication((p.id.isEmpty() ? QStringLiteral("新建用户运行点：") : QStringLiteral("修改用户运行点：")) + p.name);
    if (ok && matchesCurrentSettings) {
        if (!wasNew) {
            m_currentPoint = m_points.point(p.id);
        } else {
            const auto savedPoints = m_points.points();
            for (auto it = savedPoints.crbegin(); it != savedPoints.crend(); ++it) {
                if (!it->isFactoryDefault() && it->name == p.name && pointMatches(*it, p)) {
                    m_currentPoint = *it;
                    break;
                }
            }
        }
        emit currentPointChanged();
    } else if (ok) {
        refreshCurrentPointMatch();
    }
    notify(ok ? QStringLiteral("运行点已保存") : QStringLiteral("运行点保存失败或系统预设不可修改"));
}

OperatingPoint AppController::pointFromCurrentTargets() const
{
    return m_currentPoint;
}

QVariantMap AppController::currentTargetValues() const { return pointFromCurrentTargets().toVariantMap(); }


void AppController::refreshCurrentPointMatch()
{
    const auto values = pointFromCurrentTargets();
    for (const auto &point : m_points.points()) {
        if (pointMatches(point, values)) {
            if (m_currentPoint.id != point.id || m_currentPoint.name != point.name) {
                m_currentPoint = point;
                emit currentPointChanged();
            }
            return;
        }
    }
    if (!m_currentPoint.id.isEmpty() || m_currentPoint.name != QStringLiteral("自定义参数 *")) {
        m_currentPoint = values;
        m_currentPoint.name = QStringLiteral("自定义参数 *");
        emit currentPointChanged();
    }
}


void AppController::duplicateOperatingPoint(const QString &id)
{
    const auto copy = m_points.duplicate(id);
    if (!copy.id.isEmpty()) m_logging.logApplication(QStringLiteral("复制为用户运行点：") + copy.name);
    notify(copy.id.isEmpty() ? QStringLiteral("运行点复制失败") : QStringLiteral("已复制为用户运行点"));
}
void AppController::deleteOperatingPoint(const QString &id)
{
    const auto point = m_points.point(id);
    const bool ok = m_points.remove(id);
    if (ok) m_logging.logApplication(QStringLiteral("删除用户运行点：") + point.name);
    if (ok) refreshCurrentPointMatch();
    notify(ok ? QStringLiteral("运行点已删除") : QStringLiteral("系统预设不可删除"));
}

void AppController::exportAlarms()
{
    const QString path = m_config.dataRoot() + "/exports/报警_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".csv";
    QString error; notify(CsvExporter::exportAlarms(path, m_alarms.records(), &error) ? QStringLiteral("报警已导出：") + path : QStringLiteral("导出失败：") + error);
}


void AppController::clearAlarms() { m_alarms.clear(); m_logging.logApplication(QStringLiteral("清空报警页面记录")); notify(QStringLiteral("报警记录已清空")); }
void AppController::clearNotification()
{
    if (!m_notification.isEmpty()) {
        m_notification.clear();
        if (m_communicationNotice.isEmpty()) emit notificationChanged();
    }
}
QVariantMap AppController::operatingPoint(const QString &id) const { return m_points.point(id).toVariantMap(); }

void AppController::setAlarmFilter(int value) { if (m_alarmFilter != value) { m_alarmFilter = value; emit alarmsChanged(); } }
void AppController::setSampleIntervalMs(int value) { if (!QList<int>{200, 500, 1000, 2000}.contains(value)) return; m_config.setValue("monitoring", "sampleIntervalMs", value); m_config.save(); emit settingsChanged(); notify(QStringLiteral("轮询周期将在下次启动软件时生效")); }
void AppController::setWarningThreshold(double value) { if (value <= 0 || value >= criticalThreshold()) return; m_config.setValue("flowAlarm", "warningDeviationPercent", value); m_config.save(); emit settingsChanged(); notify(QStringLiteral("报警阈值将在下次启动软件时生效")); }
void AppController::setCriticalThreshold(double value) { if (value <= warningThreshold()) return; m_config.setValue("flowAlarm", "criticalDeviationPercent", value); m_config.save(); emit settingsChanged(); notify(QStringLiteral("报警阈值将在下次启动软件时生效")); }
void AppController::rescanMfc() { m_monitoring.rescan(); notify(QStringLiteral("正在后台重新扫描 USB/RS485 MFC…")); }

void AppController::confirmMfcAddress(int address)
{
    bool detected = false;
    const auto devices = m_monitoring.deviceInfo().value("devices").toList();
    for (const auto &value : devices) {
        const auto device = value.toMap();
        if (device.value("protocolAddress").toInt() == address
            && device.value("addressDetected").toBool()) {
            detected = true;
            break;
        }
    }
    if (!detected) {
        notify(QStringLiteral("该协议地址尚未取得有效只读响应，不能确认地址映射"));
        return;
    }
    if (!m_config.setMfcAddressConfirmed(address, true)) {
        notify(QStringLiteral("MFC 地址确认保存失败"));
        return;
    }
    m_monitoring.setAddressConfirmed(address, true);
    emit settingsChanged();
    notify(QStringLiteral("已确认协议地址 %1 与当前逻辑通道的真机映射；工程配置仍需独立校验")
           .arg(address));
}

void AppController::setDataRoot(const QUrl &folderUrl)
{
    const QString path = folderUrl.isLocalFile() ? folderUrl.toLocalFile() : folderUrl.toString();
    if (!m_config.setDataRoot(path)) {
        notify(QStringLiteral("数据保存路径不可写或保存失败"));
        return;
    }

    const QString root = m_config.dataRoot();
    const bool pointsOk = m_points.setDataRoot(root);
    const bool loggingOk = m_logging.setDataRoot(root);
    m_monitoring.setLogDirectory(root + QStringLiteral("/logs"));
    emit settingsChanged();
    notify(pointsOk && loggingOk ? QStringLiteral("数据保存路径已更新")
                                 : QStringLiteral("路径已保存，部分数据将在重启后切换"));
}
void AppController::setWindowMode(int value)
{
    if (value < 0 || value > 2 || value == windowMode()) return;
    m_config.setValue("application", "windowMode", value);
    if (!m_config.save()) {
        notify(QStringLiteral("窗口模式保存失败"));
        return;
    }
    emit settingsChanged();
    const QStringList modeNames{QStringLiteral("普通窗口"), QStringLiteral("无边框窗口"),
                                QStringLiteral("全屏显示")};
    notify(QStringLiteral("已切换为") + modeNames[value]);
}

void AppController::notify(const QString &message)
{
    m_notification = message;
    if (m_communicationNotice.isEmpty()) emit notificationChanged();
}

void AppController::setCommunicationNotice(const QString &message)
{
    if (m_communicationNotice == message) return;
    m_communicationNotice = message;
    // This message is already translated and thresholded by the service; it
    // intentionally contains no parser/checksum implementation detail.
    if (!message.isEmpty()) m_logging.logApplication(message);
    emit notificationChanged();
}

QVariantList AppController::mfcDevices() const
{
    QVariantList result;
    for (const auto &device : m_config.mfcDevices()) result.append(device.toVariantMap());
    return result;
}

bool AppController::validateTargetFlows(const OperatingPoint &point, QString *errorMessage) const
{
    if (!point.hasValidFlowValues()) {
        if (errorMessage) *errorMessage = point.requiresAddressMapping
            ? QStringLiteral("旧格式运行点无法安全推断地址，请重新确认")
            : QStringLiteral("运行点数据无效");
        return false;
    }
    const auto devices = m_config.mfcDevices();
    QSet<int> configuredAddresses;
    for (const auto &device : devices) configuredAddresses.insert(device.address);
    for (auto it = point.mfcSetpoints.cbegin(); it != point.mfcSetpoints.cend(); ++it) {
        if (!configuredAddresses.contains(it.key())) {
            if (errorMessage) *errorMessage = QStringLiteral("运行点包含未配置的 MFC 地址 %1").arg(it.key());
            return false;
        }
    }
    for (const auto &device : devices) {
        if (!device.enabled) continue;
        if (!point.mfcSetpoints.contains(device.address)) {
            if (device.required) {
                if (errorMessage) *errorMessage = QStringLiteral("运行点缺少 %1（地址 %2）").arg(device.displayName).arg(device.address);
                return false;
            }
            continue;
        }
        const double value = point.mfcSetpoints.value(device.address);
        const double maximum = device.maximumSetpoint > 0.0 ? device.maximumSetpoint
            : device.fullScale;
        if (maximum <= 0.0) {
            if (errorMessage) *errorMessage = QStringLiteral("%1 的量程尚未配置").arg(device.displayName);
            return false;
        }
        if (!std::isfinite(value) || value < device.minimumSetpoint || value > maximum) {
            if (errorMessage) *errorMessage = QStringLiteral("%1 目标流量超出允许范围").arg(device.displayName);
            return false;
        }
    }
    return true;
}
