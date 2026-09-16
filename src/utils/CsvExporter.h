#pragma once

#include "models/AlarmRecord.h"
#include <QString>
#include <QVariantList>

class CsvExporter
{
public:
    static bool exportAlarms(const QString &path, const QList<AlarmRecord> &records, QString *error);
private:
    static QString escape(const QString &value);
};
