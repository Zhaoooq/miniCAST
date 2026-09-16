#include "ConfigManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QList>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QSet>

namespace {
// These are installation facts, not operator-tunable defaults.  Keep the
// engineering scale in the unit used at the machine so that every conversion
// (READ_FLOW, setpoint and UI) has one authoritative source.
QJsonArray fixedMiniCastMfcDevices()
{
    return {
        QJsonObject{{"address", 32}, {"displayName", "MFC1"}, {"gasType", "空气"}, {"function", "发生器空气"}, {"unit", "L/min"}, {"fullScale", 1.0}, {"gasCode", 8}, {"expectedDeviceFullScaleSccm", 1000.0}, {"minSetpoint", 0.0}, {"maxSetpoint", 1.0}, {"addressConfirmed", true}, {"enabled", true}, {"required", true}},
        QJsonObject{{"address", 33}, {"displayName", "MFC2"}, {"gasType", "丙烷"}, {"function", "燃料气"}, {"unit", "ml/min"}, {"fullScale", 50.0}, {"gasCode", 89}, {"expectedDeviceFullScaleSccm", 50.0}, {"minSetpoint", 0.0}, {"maxSetpoint", 50.0}, {"addressConfirmed", true}, {"enabled", true}, {"required", true}},
        QJsonObject{{"address", 34}, {"displayName", "MFC3"}, {"gasType", "氮气"}, {"function", "稀释气"}, {"unit", "ml/min"}, {"fullScale", 50.0}, {"gasCode", 13}, {"expectedDeviceFullScaleSccm", 50.0}, {"minSetpoint", 0.0}, {"maxSetpoint", 50.0}, {"addressConfirmed", true}, {"enabled", true}, {"required", true}},
        QJsonObject{{"address", 35}, {"displayName", "MFC4"}, {"gasType", "空气"}, {"function", "发生器空气"}, {"unit", "L/min"}, {"fullScale", 10.0}, {"gasCode", 8}, {"expectedDeviceFullScaleSccm", 10000.0}, {"minSetpoint", 0.0}, {"maxSetpoint", 10.0}, {"addressConfirmed", true}, {"enabled", true}, {"required", true}},
        QJsonObject{{"address", 36}, {"displayName", "MFC5"}, {"gasType", "氮气"}, {"function", "稀释气"}, {"unit", "L/min"}, {"fullScale", 4.0}, {"gasCode", 13}, {"expectedDeviceFullScaleSccm", 4000.0}, {"minSetpoint", 0.0}, {"maxSetpoint", 4.0}, {"addressConfirmed", true}, {"enabled", true}, {"required", true}}
    };
}
}

ConfigManager::ConfigManager()
{
    m_defaultDataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_dataRoot = m_defaultDataRoot;
    QDir().mkpath(m_defaultDataRoot);
    QDir().mkpath(m_dataRoot + "/logs");
    QDir().mkpath(m_dataRoot + "/exports");
    // The configuration must stay at a stable location; otherwise the app
    // could not remember a user-selected data directory on the next launch.
    m_configPath = m_defaultDataRoot + "/config.json";
    m_config = defaults();
    load();
}

QJsonObject ConfigManager::defaults()
{
    return {{"application", QJsonObject{{"windowMode", 0}, {"language", "zh_CN"}}},
            {"monitoring", QJsonObject{{"sampleIntervalMs", 500}}},
            {"flowAlarm", QJsonObject{{"warningDeviationPercent", 5.0}, {"criticalDeviationPercent", 10.0}, {"zeroFlowTolerance", 0.02}}},
            {"storage", QJsonObject{{"dataRoot", ""}}},
            {"mfc", QJsonObject{
                {"serialPort", "auto"}, {"preferredBaud", 19200},
                {"ackTimeoutMs", 40}, {"responseTimeoutMs", 180}, {"retryCount", 1},
                {"interRequestDelayMs", 20}, {"metadataInterRequestDelayMs", 20}, {"freshnessTimeoutMs", 3000},
                {"offlineFailureThreshold", 5}, {"reconnectIntervalMs", 3000},
                {"settlingTimeMs", 3000},
                {"devices", fixedMiniCastMfcDevices()}}}};
}

bool ConfigManager::load()
{
    QFile file(m_configPath);
    if (!file.exists()) {
        m_lastLoadSucceeded = save();
        return m_lastLoadSucceeded;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_config = defaults();
        m_lastLoadSucceeded = false;
        return false;
    }
    QJsonParseError error;
    const auto payload = file.readAll();
    file.close();
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        m_config = defaults();
        m_lastLoadSucceeded = false;
        return false;
    }
    m_config = document.object();
    // The five-channel miniCAST mapping has been re-verified on site.  It is
    // intentionally replaced on every load so an already-persisted old
    // installation cannot remain a fallback for control or READ_FLOW.
    auto mfc = m_config.value("mfc").toObject();
    const auto defaultMfc = defaults().value("mfc").toObject();
    const bool fixedDevicesChanged = mfc.value("devices").toArray() != fixedMiniCastMfcDevices();
    mfc["devices"] = fixedMiniCastMfcDevices();
    for (auto it = defaultMfc.begin(); it != defaultMfc.end(); ++it)
        if (!mfc.contains(it.key())) mfc[it.key()] = it.value();
    m_config["mfc"] = mfc;
    const QString configuredRoot = m_config["storage"].toObject()["dataRoot"].toString().trimmed();
    m_dataRoot = configuredRoot.isEmpty() ? m_defaultDataRoot
                                          : QDir::cleanPath(QFileInfo(configuredRoot).absoluteFilePath());
    if (!QDir().mkpath(m_dataRoot + "/logs") || !QDir().mkpath(m_dataRoot + "/exports")) {
        m_dataRoot = m_defaultDataRoot;
        setValue(QStringLiteral("storage"), QStringLiteral("dataRoot"), QString());
        m_lastLoadSucceeded = false;
        return false;
    }
    m_lastLoadSucceeded = true;
    // Persist the replacement now, rather than allowing it to be saved only
    // at a later clean shutdown.
    if (fixedDevicesChanged && !save()) {
        m_lastLoadSucceeded = false;
        return false;
    }
    return true;
}

bool ConfigManager::save() const
{
    QSaveFile file(m_configPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(QJsonDocument(m_config).toJson(QJsonDocument::Indented)) < 0)
        return false;
    return file.commit();
}

void ConfigManager::setValue(const QString &section, const QString &key, const QJsonValue &value)
{
    auto object = m_config.value(section).toObject();
    object[key] = value;
    m_config[section] = object;
}

bool ConfigManager::setDataRoot(const QString &path)
{
    const QString cleaned = QDir::cleanPath(QFileInfo(path.trimmed()).absoluteFilePath());
    if (path.trimmed().isEmpty() || !QDir().mkpath(cleaned)
        || !QDir().mkpath(cleaned + "/logs") || !QDir().mkpath(cleaned + "/exports"))
        return false;

    QTemporaryFile logWriteTest(cleaned + QStringLiteral("/logs/.minicast-write-test-XXXXXX"));
    QTemporaryFile exportWriteTest(cleaned + QStringLiteral("/exports/.minicast-write-test-XXXXXX"));
    if (!logWriteTest.open() || !exportWriteTest.open()) return false;
    logWriteTest.close();
    exportWriteTest.close();

    const QString previousRoot = m_dataRoot;
    const QJsonObject previousStorage = m_config.value("storage").toObject();
    m_dataRoot = cleaned;
    setValue(QStringLiteral("storage"), QStringLiteral("dataRoot"), cleaned);
    if (save()) return true;

    m_dataRoot = previousRoot;
    m_config["storage"] = previousStorage;
    return false;
}

int ConfigManager::sampleIntervalMs() const { return m_config["monitoring"].toObject()["sampleIntervalMs"].toInt(500); }
double ConfigManager::warningDeviationPercent() const { return m_config["flowAlarm"].toObject()["warningDeviationPercent"].toDouble(5.0); }
double ConfigManager::criticalDeviationPercent() const { return m_config["flowAlarm"].toObject()["criticalDeviationPercent"].toDouble(10.0); }
double ConfigManager::zeroFlowTolerance() const { return m_config["flowAlarm"].toObject()["zeroFlowTolerance"].toDouble(0.02); }
int ConfigManager::windowMode() const
{
    const auto application = m_config["application"].toObject();
    if (application.contains("windowMode")) {
        const int mode = application["windowMode"].toInt(0);
        return mode >= 0 && mode <= 2 ? mode : 0;
    }

    // Older releases exposed a single kiosk flag. Preserve that choice when
    // upgrading instead of unexpectedly returning a deployed unit to a window.
    return application["kioskMode"].toBool(false) ? 2 : 0;
}
int ConfigManager::mfcPreferredBaud() const
{
    const int value = m_config["mfc"].toObject()["preferredBaud"].toInt(19200);
    return QList<int>{1200, 2400, 4800, 9600, 19200}.contains(value) ? value : 19200;
}

QList<MfcDeviceConfig> ConfigManager::mfcDevices() const
{
    QList<MfcDeviceConfig> result;
    const auto devices = m_config["mfc"].toObject()["devices"].toArray();
    QSet<int> addresses;
    for (const auto &value : devices) {
        const auto object = value.toObject();
        const int address = object["address"].toInt(-1);
        if (address < 0x20 || address > 0x5f || addresses.contains(address)) continue;
        addresses.insert(address);
        MfcDeviceConfig device;
        device.logicalChannel = object["logicalChannel"].toInt(result.size() + 1);
        device.address = static_cast<quint8>(address);
        device.displayName = object["displayName"].toString(QStringLiteral("MFC %1").arg(result.size() + 1));
        device.gasType = object["gasType"].toString(QStringLiteral("待确认"));
        device.function = object["function"].toString(QStringLiteral("待确认"));
        device.unit = object["unit"].toString().trimmed();
        device.fullScale = qMax(0.0, object["fullScale"].toDouble());
        const QJsonObject defaultDevice = defaults().value("mfc").toObject()
            .value("devices").toArray().at(result.size()).toObject();
        device.expectedGasCode = static_cast<quint16>(object["gasCode"].toInt(defaultDevice["gasCode"].toInt()));
        device.expectedDeviceFullScaleSccm = object["expectedDeviceFullScaleSccm"].toDouble(
            defaultDevice["expectedDeviceFullScaleSccm"].toDouble());
        device.minimumSetpoint = qMax(0.0, object["minSetpoint"].toDouble());
        device.maximumSetpoint = qMax(0.0, object["maxSetpoint"].toDouble(device.fullScale));
        device.absoluteTolerance = qMax(0.0, object["absoluteTolerance"].toDouble());
        device.relativeTolerancePercent = qMax(0.0, object["relativeTolerancePercent"].toDouble(5.0));
        device.enabled = object["enabled"].toBool(true);
        device.required = object["required"].toBool(true);
        device.addressConfirmed = object["addressConfirmed"].toBool(false);
        result.append(device);
    }
    return result;
}

bool ConfigManager::setMfcAddressConfirmed(int address, bool confirmed)
{
    auto root = m_config.value("mfc").toObject();
    auto devices = root.value("devices").toArray();
    bool found = false;
    for (qsizetype index = 0; index < devices.size(); ++index) {
        auto device = devices[index].toObject();
        if (device.value("address").toInt(-1) != address) continue;
        device["addressConfirmed"] = confirmed;
        devices[index] = device;
        found = true;
        break;
    }
    if (!found) return false;
    const QJsonObject previous = m_config;
    root["devices"] = devices;
    m_config["mfc"] = root;
    if (save()) return true;
    m_config = previous;
    return false;
}

QString ConfigManager::serialPort() const { return m_config["mfc"].toObject()["serialPort"].toString(QStringLiteral("auto")); }
int ConfigManager::ackTimeoutMs() const { return qBound(10, m_config["mfc"].toObject()["ackTimeoutMs"].toInt(40), 1000); }
int ConfigManager::responseTimeoutMs() const { return qBound(100, m_config["mfc"].toObject()["responseTimeoutMs"].toInt(180), 5000); }
int ConfigManager::retryCount() const { return qBound(0, m_config["mfc"].toObject()["retryCount"].toInt(1), 1); }
int ConfigManager::offlineFailureThreshold() const { return qBound(5, m_config["mfc"].toObject()["offlineFailureThreshold"].toInt(5), 20); }
int ConfigManager::interRequestDelayMs() const { return qBound(0, m_config["mfc"].toObject()["interRequestDelayMs"].toInt(20), 1000); }
int ConfigManager::metadataInterRequestDelayMs() const { return qBound(20, m_config["mfc"].toObject()["metadataInterRequestDelayMs"].toInt(20), 100); }
int ConfigManager::freshnessTimeoutMs() const { return qBound(500, m_config["mfc"].toObject()["freshnessTimeoutMs"].toInt(3000), 60000); }
int ConfigManager::reconnectIntervalMs() const { return qBound(500, m_config["mfc"].toObject()["reconnectIntervalMs"].toInt(3000), 60000); }
int ConfigManager::settlingTimeMs() const { return qBound(0, m_config["mfc"].toObject()["settlingTimeMs"].toInt(3000), 300000); }
