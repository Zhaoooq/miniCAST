#pragma once

#include "controllers/MonitoringController.h"
#include "services/AlarmService.h"
#include "services/DataLoggingService.h"
#include "services/OperatingPointService.h"
#include "utils/ConfigManager.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QUrl>

class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList gasChannels READ gasChannels NOTIFY gasChannelsChanged)
    Q_PROPERTY(QVariantList operatingPoints READ operatingPoints NOTIFY operatingPointsChanged)
    Q_PROPERTY(QVariantList alarms READ alarms NOTIFY alarmsChanged)
    Q_PROPERTY(QVariantList recentAlarms READ recentAlarms NOTIFY alarmsChanged)
    Q_PROPERTY(QVariantList activeAlarms READ activeAlarms NOTIFY alarmsChanged)
    Q_PROPERTY(int alarmTotalCount READ alarmTotalCount NOTIFY alarmsChanged)
    Q_PROPERTY(int activeAlarmCount READ activeAlarmCount NOTIFY alarmsChanged)
    Q_PROPERTY(int alarmCount24h READ alarmCount24h NOTIFY alarmsChanged)
    Q_PROPERTY(bool monitoring READ monitoring NOTIFY monitoringChanged)
    Q_PROPERTY(QString deviceStatusText READ deviceStatusText NOTIFY deviceStatusChanged)
    Q_PROPERTY(QString flameStatusText READ flameStatusText NOTIFY flameStatusChanged)
    Q_PROPERTY(int deviceStatusCode READ deviceStatusCode NOTIFY deviceStatusChanged)
    Q_PROPERTY(int flameStatusCode READ flameStatusCode NOTIFY flameStatusChanged)
    Q_PROPERTY(QVariantMap deviceInfo READ deviceInfo NOTIFY deviceInfoChanged)
    Q_PROPERTY(QVariantList mfcDevices READ mfcDevices CONSTANT)
    Q_PROPERTY(QString runningTimeText READ runningTimeText NOTIFY runningTimeChanged)
    Q_PROPERTY(QString currentPointName READ currentPointName NOTIFY currentPointChanged)
    Q_PROPERTY(QString currentPointId READ currentPointId NOTIFY currentPointChanged)
    Q_PROPERTY(bool customerPointFull READ customerPointFull NOTIFY operatingPointsChanged)
    Q_PROPERTY(QString nextCustomerPointName READ nextCustomerPointName NOTIFY operatingPointsChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY notificationChanged)
    Q_PROPERTY(int alarmFilter READ alarmFilter WRITE setAlarmFilter NOTIFY alarmsChanged)
    Q_PROPERTY(int sampleIntervalMs READ sampleIntervalMs WRITE setSampleIntervalMs NOTIFY settingsChanged)
    Q_PROPERTY(double warningThreshold READ warningThreshold WRITE setWarningThreshold NOTIFY settingsChanged)
    Q_PROPERTY(double criticalThreshold READ criticalThreshold WRITE setCriticalThreshold NOTIFY settingsChanged)
    Q_PROPERTY(int windowMode READ windowMode WRITE setWindowMode NOTIFY settingsChanged)
    Q_PROPERTY(QString dataRoot READ dataRoot NOTIFY settingsChanged)
    Q_PROPERTY(QUrl dataRootUrl READ dataRootUrl NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap communicationExperiment READ communicationExperiment NOTIFY deviceInfoChanged)
    Q_PROPERTY(bool controlling READ controlling NOTIFY deviceInfoChanged)
    Q_PROPERTY(bool controlStopping READ controlStopping NOTIFY deviceInfoChanged)
    Q_PROPERTY(bool fullScaleDiagnosticsRunning READ fullScaleDiagnosticsRunning NOTIFY fullScaleDiagnosticsRunningChanged)
public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    QVariantList gasChannels() const { return m_monitoring.gasChannels(); }
    QVariantList operatingPoints() const { return m_points.pointMaps(); }
    QVariantList alarms() const { return m_alarms.alarmMaps(m_alarmFilter); }
    QVariantList recentAlarms() const;
    QVariantList activeAlarms() const { return m_alarms.activeAlarmMaps(); }
    int alarmTotalCount() const;
    int activeAlarmCount() const;
    int alarmCount24h() const;
    bool monitoring() const { return m_monitoring.monitoring(); }
    QString deviceStatusText() const { return m_monitoring.deviceStatusText(); }
    QString flameStatusText() const { return m_monitoring.flameStatusText(); }
    int deviceStatusCode() const { return m_monitoring.deviceStatusCode(); }
    int flameStatusCode() const { return m_monitoring.flameStatusCode(); }
    QVariantMap deviceInfo() const { return m_monitoring.deviceInfo(); }
    bool controlling() const { return deviceInfo().value("controlSession").toBool(); }
    bool controlStopping() const { return deviceInfo().value("controlStopping").toBool(); }
    bool fullScaleDiagnosticsRunning() const { return m_fullScaleDiagnosticsRunning; }
    QVariantList mfcDevices() const;
    QString runningTimeText() const;
    QString currentPointName() const { return m_currentPoint.name; }
    QString currentPointId() const { return m_currentPoint.id; }
    bool customerPointFull() const { return m_points.customerPointFull(); }
    QString nextCustomerPointName() const { return m_points.nextCustomerName(); }
    QString notification() const { return m_communicationNotice.isEmpty() ? m_notification : m_communicationNotice; }
    int alarmFilter() const { return m_alarmFilter; }
    void setAlarmFilter(int value);
    int sampleIntervalMs() const { return m_config.sampleIntervalMs(); }
    void setSampleIntervalMs(int value);
    double warningThreshold() const { return m_config.warningDeviationPercent(); }
    void setWarningThreshold(double value);
    double criticalThreshold() const { return m_config.criticalDeviationPercent(); }
    void setCriticalThreshold(double value);
    int windowMode() const { return m_config.windowMode(); }
    void setWindowMode(int value);
    QString dataRoot() const { return m_config.dataRoot(); }
    QUrl dataRootUrl() const { return QUrl::fromLocalFile(m_config.dataRoot()); }
    QVariantMap communicationExperiment() const { return deviceInfo().value("experiment").toMap(); }

    Q_INVOKABLE void startMonitoring();
    Q_INVOKABLE void stopMonitoring();
    Q_INVOKABLE void selectOperatingPoint(const QString &id);
    Q_INVOKABLE void saveOperatingPoint(const QVariantMap &data);
    Q_INVOKABLE void duplicateOperatingPoint(const QString &id);
    Q_INVOKABLE void deleteOperatingPoint(const QString &id);
    Q_INVOKABLE void exportAlarms();
    Q_INVOKABLE void clearAlarms();
    Q_INVOKABLE void clearNotification();
    Q_INVOKABLE QVariantMap operatingPoint(const QString &id) const;
    Q_INVOKABLE QVariantMap currentTargetValues() const;
    Q_INVOKABLE void rescanMfc();
    Q_INVOKABLE void confirmMfcAddress(int address);
    Q_INVOKABLE void setDataRoot(const QUrl &folderUrl);
    Q_INVOKABLE void startCommunicationExperiment(int delayMs, int durationSeconds, int selectedAddress);
    Q_INVOKABLE void stopCommunicationExperiment();
    Q_INVOKABLE void startControl();
    Q_INVOKABLE void stopControl();
    Q_INVOKABLE void verifyDeviceInformation();
    Q_INVOKABLE void runFullScaleDiagnostics();

signals:
    void gasChannelsChanged();
    void operatingPointsChanged();
    void alarmsChanged();
    void monitoringChanged();
    void deviceStatusChanged();
    void flameStatusChanged();
    void runningTimeChanged();
    void currentPointChanged();
    void notificationChanged();
    void settingsChanged();
    void deviceInfoChanged();
    void fullScaleDiagnosticsRunningChanged();

private:
    void notify(const QString &message);
    void setCommunicationNotice(const QString &message);
    OperatingPoint pointFromCurrentTargets() const;
    void refreshCurrentPointMatch();
    bool validateTargetFlows(const OperatingPoint &point, QString *errorMessage = nullptr) const;
    ConfigManager m_config;
    OperatingPointService m_points;
    AlarmService m_alarms;
    DataLoggingService m_logging;
    MonitoringController m_monitoring;
    OperatingPoint m_currentPoint;
    QElapsedTimer m_runtime;
    qint64 m_accumulatedMs{0};
    QTimer m_runtimeTimer;
    QString m_notification;
    QString m_communicationNotice;
    int m_alarmFilter{-1};
    bool m_fullScaleDiagnosticsRunning{false};
};
