#include "DataLoggingService.h"
#include <QDate>
#include <QDir>
#include <QTextStream>

static QString deviceText(DeviceStatus status)
{
    switch (status) {
    case DeviceStatus::Disconnected: return QStringLiteral("未连接");
    case DeviceStatus::Scanning: return QStringLiteral("扫描中");
    case DeviceStatus::Connecting: return QStringLiteral("正在连接");
    case DeviceStatus::Connected: return QStringLiteral("已连接");
    case DeviceStatus::Monitoring: return QStringLiteral("正在监测");
    case DeviceStatus::Warning: return QStringLiteral("存在警告");
    case DeviceStatus::Alarm: return QStringLiteral("设备报警");
    case DeviceStatus::CommunicationError: return QStringLiteral("通讯中断");
    }
    return {};
}

static QString flameText(FlameStatus status)
{
    switch (status) {
    case FlameStatus::Unknown: return QStringLiteral("未知");
    case FlameStatus::Normal: return QStringLiteral("正常燃烧");
    case FlameStatus::Off: return QStringLiteral("火焰熄灭");
    case FlameStatus::Fault: return QStringLiteral("火焰异常");
    }
    return {};
}

DataLoggingService::DataLoggingService(const QString &root, QObject *parent)
    : QObject(parent), m_root(root)
{
    QDir().mkpath(m_root + "/logs");
    m_appFile.setFileName(m_root + "/logs/application.log");
    logApplication(QStringLiteral("程序启动"));
}

DataLoggingService::~DataLoggingService() { stop(); logApplication(QStringLiteral("程序退出")); }

bool DataLoggingService::setDataRoot(const QString &root)
{
    if (root == m_root) return true;
    const bool wasActive = m_active;
    stop();
    if (m_appFile.isOpen()) m_appFile.close();
    m_root = root;
    m_flowDate.clear();
    if (!QDir().mkpath(m_root + "/logs")) {
        m_lastError = QStringLiteral("无法创建日志目录");
        return false;
    }
    m_appFile.setFileName(m_root + "/logs/application.log");
    logApplication(QStringLiteral("数据保存目录已切换"));
    return !wasActive || start();
}

void DataLoggingService::logApplication(const QString &message)
{
    if (!m_appFile.isOpen() && !m_appFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_lastError = QStringLiteral("无法写入应用日志：") + m_appFile.errorString(); return;
    }
    QTextStream stream(&m_appFile);
    stream << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz") << "  " << message << '\n';
    stream.flush();
}

bool DataLoggingService::start()
{
    m_active = ensureFlowFile();
    if (m_active) logApplication(QStringLiteral("监测开始"));
    return m_active;
}

void DataLoggingService::stop()
{
    if (m_active) logApplication(QStringLiteral("监测停止"));
    m_active = false;
    if (m_flowFile.isOpen()) m_flowFile.close();
}

bool DataLoggingService::ensureFlowFile()
{
    const QString date = QDate::currentDate().toString("yyyy-MM-dd");
    if (m_flowFile.isOpen() && m_flowDate == date) return true;
    if (m_flowFile.isOpen()) m_flowFile.close();
    m_flowDate = date;
    m_flowFile.setFileName(m_root + "/logs/" + date + "_flow.csv");
    const bool fresh = !QFile::exists(m_flowFile.fileName());
    if (!m_flowFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_lastError = QStringLiteral("无法写入流量日志：") + m_flowFile.errorString(); return false;
    }
    if (fresh) {
        m_flowFile.write("\xEF\xBB\xBF");
        QTextStream stream(&m_flowFile);
        stream << QStringLiteral("时间,MFC地址,MFC标识,气体名称,运行点ID,目标流量,实际流量,偏差,偏差率(%),单位,流量%FS,通信状态,控制状态,状态,设备状态,火焰状态\n");
    }
    return true;
}

void DataLoggingService::logFlows(const QList<GasChannel> &channels, DeviceStatus device, FlameStatus flame)
{
    if (!m_active || channels.isEmpty() || !ensureFlowFile()) return;
    QTextStream stream(&m_flowFile);
    const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    for (const auto &channel : channels) {
        stream << timestamp << ',' << channel.address << ',' << channel.id << ',' << channel.nameChinese
               << ',' << channel.operatingPointId
               << ',' << QString::number(channel.targetFlow, 'f', 3)
               << ',' << QString::number(channel.realValue, 'f', 3)
               << ',' << QString::number(channel.deviation, 'f', 3)
               << ',' << (channel.deviationAvailable ? QString::number(channel.deviationPercent, 'f', 3) : QStringLiteral("—"))
               << ',' << channel.unit << ',' << QString::number(channel.percentFullScale, 'f', 3)
               << ',' << channel.communicationState << ',' << channel.controlState
               << ',' << channel.toVariantMap().value("comparisonStatusText").toString()
               << ',' << deviceText(device) << ',' << flameText(flame) << '\n';
    }
    stream.flush();
}

void DataLoggingService::logAlarm(const AlarmRecord &record, const QString &state)
{
    const QString path = m_root + "/logs/" + QDate::currentDate().toString("yyyy-MM-dd") + "_alarm.csv";
    const bool fresh = !QFile::exists(path);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_lastError = QStringLiteral("无法写入报警日志：") + file.errorString();
        return;
    }
    if (fresh) file.write("\xEF\xBB\xBF");
    QTextStream stream(&file);
    if (fresh) stream << QStringLiteral("首次时间,最近时间,级别,通道,参数,目标流量,实际流量,偏差率(%),重复次数,状态,恢复时间,信息\n");
    stream << record.timestamp.toString("yyyy-MM-dd HH:mm:ss") << ','
           << (record.lastOccurrenceTime.isValid() ? record.lastOccurrenceTime : record.timestamp).toString("yyyy-MM-dd HH:mm:ss") << ','
           << AlarmRecord::severityText(record.severity) << ',' << record.channelName << ','
           << record.parameter << ',' << record.setValue << ',' << record.realValue << ','
           << record.deviation << ',' << record.repeatCount << ',' << state << ','
           << (record.recoveryTime.isValid() ? record.recoveryTime.toString("yyyy-MM-dd HH:mm:ss") : QString())
           << ',' << record.message << '\n';
}
