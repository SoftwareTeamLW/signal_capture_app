#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include <memory>

struct DeviceConnectionResult;
struct DeviceDiscoveryResult;
struct RxConfig;
class SdrDevice;
class RxWorker;
class SpectrumWidget;
class WaterfallWidget;
class WaveformWidget;
class QLabel;
class QTableWidget;
class QActionGroup;
class QEvent;
class QThread;
class QTranslator;
template<typename T> class QFutureWatcher;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void changeEvent(QEvent* event) override;

private:
    enum class SystemUiState {
        Normal,
        SearchingDevices,
        DiscoveryFailed,
        NoDevices,
        DevicesFound,
        DeviceConnected,
        ConfiguringReception,
        Receiving,
        StoppingReception,
        ReceptionFailed,
        AcquisitionFailed,
        TimedAcquisitionCompleted,
        ReceptionStopped,
        DeviceDisconnected
    };

    enum class ConnectionUiState {
        Disconnected,
        Connecting,
        ConnectionFailed,
        Connected
    };

    enum class DeviceListUiState {
        Initial,
        Searching,
        DiscoveryFailed,
        Empty,
        Ready
    };

    enum class IqSaveUiState {
        NotSaving,
        Writing,
        Completed
    };

    void setupLanguageMenu();
    void switchLanguage(const QString& languageCode);
    void retranslateDynamicUi();
    void applyLanguageLayout();
    void applyMainPanelGeometry();
    void retranslateMetadataView();
    void updateIqSaveStatus();
    void updateDeviceListPlaceholder();
    void setSystemUiState(SystemUiState state, qulonglong deviceCount = 0);
    void setConnectionUiState(ConnectionUiState state);
    void findDevices();
    void connectDevice();
    void disconnectDevice();
    void setDeviceOperationBusy(bool busy);
    void updateDeviceControls();
    void setAcquisitionRunning(bool running);
    void startReceiving();
    void stopReceiving();
    void finishReceiving();
    void saveScreenshot();
    void appendLog(const QString& category, const QString& message);
    void updatePerformanceStatus();
    void updatePerformanceLabel();
    void setupMetadataView();
    void updateDiskCapacity();
    QString suggestedIqFileName(const RxConfig& config) const;
    RxConfig currentRxConfig() const;
    void applyRuntimeConfig();

    Ui::MainWindow* ui;
    std::unique_ptr<SdrDevice> sdrDevice_;
    std::unique_ptr<QFutureWatcher<DeviceDiscoveryResult>> discoveryWatcher_;
    std::unique_ptr<QFutureWatcher<DeviceConnectionResult>> connectionWatcher_;
    QThread* rxThread_ = nullptr;
    RxWorker* rxWorker_ = nullptr;
    SpectrumWidget* spectrumWidget_ = nullptr;
    WaterfallWidget* waterfallWidget_ = nullptr;
    WaveformWidget* waveformWidget_ = nullptr;
    bool deviceOperationBusy_ = false;
    bool acquisitionRunning_ = false;
    QLabel* performanceLabel_ = nullptr;
    QLabel* diskCapacityLabel_ = nullptr;
    QTimer performanceTimer_;
    std::unique_ptr<QTranslator> appTranslator_;
    std::unique_ptr<QTranslator> qtTranslator_;
    QActionGroup* languageActionGroup_ = nullptr;
    QString currentLanguageCode_;
    QString connectedInterfaceTypeSource_;
    SystemUiState systemUiState_ = SystemUiState::Normal;
    ConnectionUiState connectionUiState_ = ConnectionUiState::Disconnected;
    DeviceListUiState deviceListUiState_ = DeviceListUiState::Initial;
    IqSaveUiState iqSaveUiState_ = IqSaveUiState::NotSaving;
    QString iqSaveFileName_;
    double iqSaveCompletedMiB_ = 0.0;
    qulonglong discoveredDeviceCount_ = 0;
    QElapsedTimer performanceClock_;
    quint64 previousProcessTime100ns_ = 0;
    quint64 displayFrameCount_ = 0;
    quint64 previousDisplayFrameCount_ = 0;
    double lastCpuUsage_ = -1.0;
    quint64 lastDisplayFps_ = 0;
    QString diskStatusPath_;
    bool stopRequestedByUser_ = false;
    bool activeAcquisitionTimed_ = false;
    bool acquisitionFailed_ = false;
};
