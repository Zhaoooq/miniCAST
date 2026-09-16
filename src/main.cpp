#include "controllers/AppController.h"
#include "models/GasChannel.h"
#include "services/IDeviceService.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("科研仪器"));
    app.setApplicationName(QStringLiteral("高浓度碳烟发生器监测系统"));
    app.setApplicationDisplayName(QStringLiteral("高浓度碳烟发生器监测系统"));

    qRegisterMetaType<QList<GasChannel>>();
    qRegisterMetaType<OperatingPoint>();
    qRegisterMetaType<OperatingPointType>();
    qRegisterMetaType<DeviceStatus>();
    qRegisterMetaType<FlameStatus>();

    AppController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("MiniCastMonitor", "Main");
    const QString screenshotPath = qEnvironmentVariable("MINICAST_SCREENSHOT_PATH");
    if (!screenshotPath.isEmpty() && !engine.rootObjects().isEmpty()) {
        bool pageOk = false;
        const int page = qEnvironmentVariableIntValue("MINICAST_SCREENSHOT_PAGE", &pageOk);
        if (pageOk) engine.rootObjects().constFirst()->setProperty("pageIndex", page);
        engine.rootObjects().constFirst()->setProperty("captureMode", true);
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst())) {
            QTimer::singleShot(1200, &app, [window, screenshotPath, &app] {
                window->grabWindow().save(screenshotPath);
                app.quit();
            });
        }
    }
    return app.exec();
}
