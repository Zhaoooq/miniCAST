cmake_minimum_required(VERSION 3.21)

project(MiniCastMonitor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 SerialPort)

qt_standard_project_setup(REQUIRES 6.4)
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(minicast-monitor
    src/main.cpp
    src/models/GasChannel.h src/models/GasChannel.cpp
    src/models/OperatingPoint.h src/models/OperatingPoint.cpp
    src/models/AlarmRecord.h src/models/AlarmRecord.cpp
    src/services/IDeviceService.h
    src/services/MiniCastDeviceService.h src/services/MiniCastDeviceService.cpp
    src/services/CS200ADeviceService.h src/services/CS200ADeviceService.cpp
    src/services/mfc/MfcProtocol.h src/services/mfc/MfcProtocol.cpp
    src/services/mfc/MfcTypes.h src/services/mfc/MfcTypes.cpp
    src/services/mfc/MfcLog.h src/services/mfc/MfcLog.cpp
    src/services/mfc/Cs200CommExperiment.h src/services/mfc/Cs200CommExperiment.cpp
    src/services/mfc/SerialTransport.h src/services/mfc/SerialTransport.cpp
    src/services/mfc/DeviceDiscovery.h src/services/mfc/DeviceDiscovery.cpp
    src/services/mfc/CS200ADriver.h src/services/mfc/CS200ADriver.cpp
    src/services/mfc/MfcManager.h src/services/mfc/MfcManager.cpp
    src/services/AlarmService.h src/services/AlarmService.cpp
    src/services/OperatingPointService.h src/services/OperatingPointService.cpp
    src/services/DataLoggingService.h src/services/DataLoggingService.cpp
    src/controllers/AppController.h src/controllers/AppController.cpp
    src/controllers/DeviceController.h src/controllers/DeviceController.cpp
    src/controllers/MonitoringController.h src/controllers/MonitoringController.cpp
    src/utils/FlowDeviationCalculator.h src/utils/FlowDeviationCalculator.cpp
    src/utils/CsvExporter.h src/utils/CsvExporter.cpp
    src/utils/ConfigManager.h src/utils/ConfigManager.cpp
)

set_source_files_properties(qml/theme/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(minicast-monitor
    URI MiniCastMonitor
    VERSION 1.0
    NO_IMPORT_SCAN
    QML_FILES
        qml/Main.qml
        qml/theme/Theme.qml
        qml/pages/MonitorPage.qml
        qml/pages/OperatingPointPage.qml
        qml/pages/AlarmPage.qml
        qml/pages/SettingsPage.qml
        qml/components/BottomNavigation.qml
        qml/components/HmiButton.qml
        qml/components/HmiNavButton.qml
        qml/components/HmiComboBox.qml
        qml/components/HmiToggle.qml
        qml/components/HmiStatusDot.qml
        qml/components/HmiTableHeader.qml
        qml/components/HmiDialog.qml
        qml/components/TopStatusBar.qml
        qml/components/DeviceStatusBar.qml
        qml/components/StatusIndicator.qml
        qml/components/AlarmItem.qml
        qml/components/ConfirmDialog.qml
        qml/components/TouchNumericKeypadDialog.qml
        qml/components/SectionTitle.qml
        qml/components/MfcMonitorColumn.qml
)

target_include_directories(minicast-monitor PRIVATE src)
target_link_libraries(minicast-monitor PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::SerialPort
)

install(TARGETS minicast-monitor RUNTIME DESTINATION bin)

# Development/field diagnostic: deliberately a separate, read-only executable
# so Address 36 can be interrogated without the production READ_FLOW scheduler.
qt_add_executable(minicast-device-diag
    tools/mfc_device_diag.cpp
    src/services/mfc/MfcProtocol.h src/services/mfc/MfcProtocol.cpp
    src/services/mfc/SerialTransport.h src/services/mfc/SerialTransport.cpp
    src/services/mfc/DeviceDiscovery.h src/services/mfc/DeviceDiscovery.cpp
    src/services/mfc/CS200ADriver.h src/services/mfc/CS200ADriver.cpp
    src/services/mfc/MfcTypes.h src/services/mfc/MfcTypes.cpp
)
target_include_directories(minicast-device-diag PRIVATE src)
target_link_libraries(minicast-device-diag PRIVATE Qt6::Core Qt6::SerialPort)
install(TARGETS minicast-device-diag RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(minicast-core-tests
        tests/core_tests.cpp
        src/models/GasChannel.h src/models/GasChannel.cpp
        src/models/OperatingPoint.h src/models/OperatingPoint.cpp
        src/models/AlarmRecord.h src/models/AlarmRecord.cpp
        src/services/IDeviceService.h
        src/services/CS200ADeviceService.h src/services/CS200ADeviceService.cpp
        src/services/mfc/MfcProtocol.h src/services/mfc/MfcProtocol.cpp
        src/services/mfc/MfcTypes.h src/services/mfc/MfcTypes.cpp
        src/services/mfc/MfcLog.h src/services/mfc/MfcLog.cpp
        src/services/mfc/Cs200CommExperiment.h src/services/mfc/Cs200CommExperiment.cpp
        src/services/mfc/SerialTransport.h src/services/mfc/SerialTransport.cpp
        src/services/mfc/DeviceDiscovery.h src/services/mfc/DeviceDiscovery.cpp
        src/services/mfc/CS200ADriver.h src/services/mfc/CS200ADriver.cpp
        src/services/mfc/MfcManager.h src/services/mfc/MfcManager.cpp
        src/services/AlarmService.h src/services/AlarmService.cpp
        src/services/OperatingPointService.h src/services/OperatingPointService.cpp
        src/services/DataLoggingService.h src/services/DataLoggingService.cpp
        src/controllers/MonitoringController.h src/controllers/MonitoringController.cpp
        src/utils/FlowDeviationCalculator.h src/utils/FlowDeviationCalculator.cpp
        src/utils/ConfigManager.h src/utils/ConfigManager.cpp
    )
    target_include_directories(minicast-core-tests PRIVATE src)
    target_compile_definitions(minicast-core-tests PRIVATE SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(minicast-core-tests PRIVATE Qt6::Core Qt6::SerialPort)
    add_test(NAME minicast-core-tests COMMAND minicast-core-tests)
endif()
