#include "CsvExporter.h"
#include <QDateTime>
#include <QFile>
#include <QTextStream>

QString CsvExporter::escape(const QString &value)
{
    QString copy = value;
    copy.replace('"', "\"\"");
    return '"' + copy + '"';
}

bool CsvExporter::exportAlarms(const QString &path, const QList<AlarmRecord> &records, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) *error = file.errorString(); return false;
    }
    file.write("\xEF\xBB\xBF");
    QTextStream out(&file);
    out << QStringLiteral("首次时间,最近时间,级别,设备/通道,参数,设定值,实际值,偏差(%),重复次数,持续时间(秒),报警信息,恢复时间\n");
    for (const auto &item : records)
        out << escape(item.timestamp.toString("yyyy-MM-dd HH:mm:ss")) << ','
            << escape((item.lastOccurrenceTime.isValid() ? item.lastOccurrenceTime : item.timestamp).toString("yyyy-MM-dd HH:mm:ss"))
            << ',' << escape(AlarmRecord::severityText(item.severity))
            << ',' << escape(item.channelName) << ',' << escape(item.parameter) << ',' << item.setValue << ',' << item.realValue
            << ',' << item.deviation << ',' << item.repeatCount << ','
            << item.timestamp.secsTo(item.recoveryTime.isValid() ? item.recoveryTime : QDateTime::currentDateTime())
            << ',' << escape(item.message) << ','
            << escape(item.recoveryTime.isValid() ? item.recoveryTime.toString("yyyy-MM-dd HH:mm:ss") : QStringLiteral("未恢复")) << '\n';
    return true;
}
