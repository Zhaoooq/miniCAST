#include "MfcLog.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTextStream>

MfcLog::MfcLog(QString directory, bool rawFramesEnabled)
    : m_directory(std::move(directory)),
      m_rawFramesEnabled(rawFramesEnabled || qEnvironmentVariableIntValue("MINICAST_MFC_RAW_LOG") > 0)
{
    QDir().mkpath(m_directory);
}

QString MfcLog::path() const { return m_directory + QStringLiteral("/mfc-communication.log"); }

void MfcLog::setDirectory(const QString &directory)
{
    m_directory = directory;
    QDir().mkpath(m_directory);
}

void MfcLog::rotateIfNeeded()
{
    QFile current(path());
    if (!current.exists() || current.size() < 1024 * 1024) return;
    QFile::remove(path() + QStringLiteral(".5"));
    for (int i = 4; i >= 1; --i)
        if (QFile::exists(path() + QStringLiteral(".%1").arg(i)))
            QFile::rename(path() + QStringLiteral(".%1").arg(i), path() + QStringLiteral(".%1").arg(i + 1));
    QFile::rename(path(), path() + QStringLiteral(".1"));
}

void MfcLog::write(const QString &message)
{
    const bool rawFrame = message.startsWith(QStringLiteral("TX "))
        || message.startsWith(QStringLiteral("RX "))
        || message.contains(QStringLiteral(" TX attempt="))
        || message.contains(QStringLiteral(" RX_BYTES "))
        || message.contains(QStringLiteral(" RX_FRAME_COMPLETE "));
    // Transport lifecycle and per-request diagnostics are required in the
    // normal monitor log.  Keep the legacy raw-frame switch only for unrelated
    // old-format raw lines.
    if (rawFrame && !m_rawFramesEnabled && !message.startsWith(QStringLiteral("[transport]"))) return;
    rotateIfNeeded();
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ' ' << message << '\n';
    // The launcher captures Qt's message stream as minicast_startup.log.  Send
    // the exact transport timeline there as well as to mfc-communication.log.
    qInfo().noquote() << message;
}
