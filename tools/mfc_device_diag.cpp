#include "services/mfc/CS200ADriver.h"
#include "services/mfc/DeviceDiscovery.h"
#include "services/mfc/SerialTransport.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>
#include <functional>

namespace {
QString hexByte(const QVariant &value)
{
    const int number = value.toInt();
    return number < 0 ? QStringLiteral("--")
                      : QStringLiteral("0x%1").arg(number, 2, 16, QLatin1Char('0'));
}

void printTransaction(QTextStream &out, const QVariantMap &tx, const QString &parse)
{
    out << "  destination=" << tx.value("address").toInt()
        << " service=" << hexByte(tx.value("service"))
        << " class=" << hexByte(tx.value("commandClass"))
        << " instance=" << tx.value("instance").toInt()
        << " attribute=" << hexByte(tx.value("attribute")) << '\n'
        << "  TX=" << tx.value("txRaw").toString()
        << " at=" << tx.value("txTimestamp").toString() << '\n'
        << "  ACK=" << (tx.value("ackRaw").toString().isEmpty() ? QStringLiteral("TIMEOUT")
                                                               : tx.value("ackRaw").toString()) << '\n'
        << "  RX=" << tx.value("responseFrameRaw").toString()
        << " response_address=" << hexByte(tx.value("responseAddress")) << '\n'
        << "  checksum=" << (tx.value("checksumValid").toBool() ? QStringLiteral("OK") : QStringLiteral("NOT_VALIDATED"))
        << " payload_length=" << qMax(0, tx.value("responseDataLength").toInt() - 3)
        << " payload=" << tx.value("payloadRaw").toString() << '\n'
        << "  expected=" << hexByte(tx.value("expectedService")) << "/"
        << hexByte(tx.value("expectedClass")) << "/" << tx.value("expectedInstance").toInt() << "/"
        << hexByte(tx.value("expectedAttribute"))
        << " received=" << hexByte(tx.value("receivedService")) << "/"
        << hexByte(tx.value("receivedClass")) << "/" << tx.value("receivedInstance").toInt() << "/"
        << hexByte(tx.value("receivedAttribute")) << '\n'
        << "  attempts=" << tx.value("attemptCount").toInt()
        << " retries=" << tx.value("retryCount").toInt()
        << " result=" << tx.value("result").toString()
        << " parser=" << tx.value("parserState").toString()
        << " parse=" << parse;
    if (!tx.value("detail").toString().isEmpty()) out << " detail=" << tx.value("detail").toString();
    out << "\n";
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("CS200/CS200-A 单设备只读信息诊断"));
    parser.addHelpOption();
    const QCommandLineOption addressOption({"a", "address"}, "MFC RS485 address (32..95).", "address", "36");
    const QCommandLineOption portOption({"p", "port"}, "Serial port; omit to use the first discovered candidate.", "path");
    const QCommandLineOption baudOption({"b", "baud"}, "Serial baud rate.", "baud", "19200");
    const QCommandLineOption allOption("all-configured",
        "Run the same read-only sequence on configured addresses 32..36 for comparison.");
    parser.addOption(addressOption); parser.addOption(portOption); parser.addOption(baudOption); parser.addOption(allOption);
    parser.process(app);

    bool addressOk = false, baudOk = false;
    const int address = parser.value(addressOption).toInt(&addressOk);
    const int baud = parser.value(baudOption).toInt(&baudOk);
    if (!addressOk || address < 32 || address > 95 || !baudOk || baud <= 0) {
        QTextStream(stderr) << "Invalid --address or --baud\n";
        return 2;
    }
    QString port = parser.value(portOption);
    if (port.isEmpty()) {
        const auto candidates = DeviceDiscovery::candidates();
        if (candidates.isEmpty()) {
            QTextStream(stderr) << "No serial candidate found; specify --port.\n";
            return 2;
        }
        port = candidates.first().device;
    }

    QTextStream out(stdout);
    out << "CS200 DeviceInfo diagnostic (single pending request; no READ_FLOW polling)\n"
        << "Address: " << address << "  Port: " << port << "  Baud: " << baud << "\n";
    SerialTransport transport([&out](const QString &line) { out << "[transport] " << line << '\n'; });
    try {
        transport.open(port, baud);
        const QList<int> addresses = parser.isSet(allOption)
            ? QList<int>{32, 33, 34, 35, 36} : QList<int>{address};
        for (const int selectedAddress : addresses) {
            CS200ADriver driver(transport, static_cast<quint8>(selectedAddress));
            driver.setTransactionOptions(40, 180, 1);
            const auto run = [&](const QString &name, const std::function<QString()> &reader) {
                out << "\n[DeviceInfo] addr=" << selectedAddress << " command=" << name << '\n';
                QString parse;
                try { parse = QStringLiteral("OK value=%1").arg(reader()); }
                catch (const std::exception &error) { parse = QStringLiteral("FAIL reason=%1").arg(QString::fromUtf8(error.what())); }
                printTransaction(out, transport.diagnostics().value("lastTransaction").toMap(), parse);
            };
            // This is the documented DeviceInfo order.  Without --all-configured
            // it is isolated from 32..35; with it, output supports A/B comparison.
            run("READ_GAS_NAME", [&] { return driver.readTargetGasName(); });
            run("READ_GAS_CODE", [&] { return QString::number(driver.readTargetGasCode()); });
            run("READ_FULL_SCALE", [&] { return QString::number(driver.readTargetGasFullScale()); });
            run("READ_CALIBRATION_GAS_NAME", [&] { return driver.readCalibrationGasName(); });
            run("READ_CALIBRATION_GAS_CODE", [&] { return QString::number(driver.readCalibrationGasCode()); });
            run("READ_CALIBRATION_FULL_SCALE", [&] { return QString::number(driver.readCalibrationGasFullScale()); });
            run("READ_MODEL", [&] { return driver.readModelIdentifier(); });
            run("READ_SERIAL", [&] { return driver.readSerialNumber(); });
            run("READ_BAUD", [&] { return QString::number(driver.readBaudRate()); });
            run("READ_RS485_ADDRESS", [&] { return QString::number(driver.readRs485MacAddress()); });
        }
        transport.close();
    } catch (const std::exception &error) {
        QTextStream(stderr) << "Diagnostic setup failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
