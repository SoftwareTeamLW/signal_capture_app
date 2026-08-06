#pragma once

#include "sdr_device.hpp"
#include "rx_config.hpp"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QThread>
#include <QElapsedTimer>
#include <QTimer>

#include <memory>

class QString;
class RxWorker;
class SpectrumWidget;
class WaterfallWidget;
class WaveformWidget;
class QLabel;
class QTableWidget;

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

private:
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
    void setupMetadataView();
    void updateDiskCapacity();
    QString suggestedIqFileName(const RxConfig& config) const;
    RxConfig currentRxConfig() const;
    void applyRuntimeConfig();

    Ui::MainWindow* ui;
    std::unique_ptr<SdrDevice> sdrDevice_;
    QFutureWatcher<DeviceDiscoveryResult> discoveryWatcher_;
    QFutureWatcher<DeviceConnectionResult> connectionWatcher_;
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
    QElapsedTimer performanceClock_;
    quint64 previousProcessTime100ns_ = 0;
    quint64 displayFrameCount_ = 0;
    quint64 previousDisplayFrameCount_ = 0;
    QString diskStatusPath_;
    bool stopRequestedByUser_ = false;
    bool activeAcquisitionTimed_ = false;
    bool acquisitionFailed_ = false;
};
