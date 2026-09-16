#pragma once

#include <QJsonObject>
#include <QString>
#include "services/mfc/MfcTypes.h"

class ConfigManager
{
public:
    ConfigManager();
    bool load();
    bool save() const;
    QJsonObject config() const { return m_config; }
    void setValue(const QString &section, const QString &key, const QJsonValue &value);
    int sampleIntervalMs() const;
    double warningDeviationPercent() const;
    double criticalDeviationPercent() const;
    double zeroFlowTolerance() const;
    int windowMode() const;
    int mfcPreferredBaud() const;
    QList<MfcDeviceConfig> mfcDevices() const;
    QString serialPort() const;
    int ackTimeoutMs() const;
    int responseTimeoutMs() const;
    int retryCount() const;
    int offlineFailureThreshold() const;
    int interRequestDelayMs() const;
    int metadataInterRequestDelayMs() const;
    int freshnessTimeoutMs() const;
    int reconnectIntervalMs() const;
    int settlingTimeMs() const;
    QString dataRoot() const { return m_dataRoot; }
    bool setDataRoot(const QString &path);
    bool setMfcAddressConfirmed(int address, bool confirmed);
    bool lastLoadSucceeded() const { return m_lastLoadSucceeded; }

private:
    static QJsonObject defaults();
    QJsonObject m_config;
    QString m_defaultDataRoot;
    QString m_dataRoot;
    QString m_configPath;
    bool m_lastLoadSucceeded{true};
};
