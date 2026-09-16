#pragma once

#include <QString>

class MfcLog
{
public:
    explicit MfcLog(QString directory, bool rawFramesEnabled = false);
    void write(const QString &message);
    QString path() const;
    void setDirectory(const QString &directory);
    void setRawFramesEnabled(bool enabled) { m_rawFramesEnabled = enabled; }

private:
    void rotateIfNeeded();
    QString m_directory;
    bool m_rawFramesEnabled{false};
};
