#include "ui/main_window.hpp"

#include "sdr_device.hpp"
#include "rx_receiver.hpp"
#include "rx_config.hpp"
#include "signal_plot_widgets.hpp"
#include "ui_main_window.h"

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QPushButton>
#include <QPixmap>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QSplitter>
#include <QString>
#include <QTime>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
quint64 processCpuTime100ns()
{
#ifdef Q_OS_WIN
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
#else
    return 0; // 当前项目目标是 Windows；其他平台显示 N/A。
#endif
}

WindowFunction selectedWindow(const QString& text)
{
    if (text == QStringLiteral("Hamming")) return WindowFunction::Hamming;
    if (text == QStringLiteral("Blackman-Harris")) return WindowFunction::BlackmanHarris;
    if (text == QStringLiteral("Flat Top")) return WindowFunction::FlatTop;
    return WindowFunction::Hann;
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , sdrDevice_(std::make_unique<SdrDevice>())
{
    ui->setupUi(this);

    setWindowTitle(tr("LuoWave Signal Capture"));
    resize(1450, 900);
    ui->mainSplitter->setChildrenCollapsible(false);
    ui->mainSplitter->setStretchFactor(0, 0);
    ui->mainSplitter->setStretchFactor(1, 1);
    ui->mainSplitter->setStretchFactor(2, 0);
    ui->mainSplitter->setSizes({250, 940, 260});

    for (int handleIndex = 1;
         handleIndex < ui->mainSplitter->count();
         ++handleIndex) {
        ui->mainSplitter->handle(handleIndex)->setEnabled(false);
    }

    ui->displaySplitter->setChildrenCollapsible(false);
    ui->displaySplitter->setStretchFactor(0, 3);
    ui->displaySplitter->setStretchFactor(1, 2);
    ui->displaySplitter->setStretchFactor(2, 1);
    ui->displaySplitter->setSizes({320, 210, 140});

    // Logo 不编译进程序：客户只需替换可执行文件旁 assets/company_logo.png。
    const QString logoPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/assets/company_logo.png");
    const QPixmap companyLogo(logoPath);
    if (!companyLogo.isNull()) {
        ui->companyLogoLabel->setText(QString());
        ui->companyLogoLabel->setPixmap(companyLogo.scaled(
            std::max(160, ui->companyLogoLabel->width()), 72,
            Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    // 正式仪表控件采用 Qt 栅格绘制；无独显、远程桌面和软件渲染均可工作。
    spectrumWidget_ = new SpectrumWidget(ui->spectrumPlaceholder);
    waterfallWidget_ = new WaterfallWidget(ui->waterfallPlaceholder);
    waveformWidget_ = new WaveformWidget(ui->timeDomainPlaceholder);
    ui->spectrumPlaceholderLabel->hide();
    ui->waterfallPlaceholderLabel->hide();
    ui->timeDomainPlaceholderLabel->hide();
    ui->spectrumPlaceholderLayout->addWidget(spectrumWidget_);
    ui->waterfallPlaceholderLayout->addWidget(waterfallWidget_);
    ui->timeDomainPlaceholderLayout->addWidget(waveformWidget_);

    // 右下角常驻显示本进程 CPU 和实际绘图帧率，便于低配客户机测试。
    performanceLabel_ = new QLabel(tr("CPU --  |  Display 0 FPS"), this);
    performanceLabel_->setMinimumWidth(190);
    performanceLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->statusBar->addPermanentWidget(performanceLabel_);
    performanceClock_.start();
    previousProcessTime100ns_ = processCpuTime100ns();
    performanceTimer_.setInterval(1000);
    connect(&performanceTimer_, &QTimer::timeout,
            this, &MainWindow::updatePerformanceStatus);
    performanceTimer_.start();

    // 当前阶段明确只支持连续采集，避免用户误以为保存/定长已经生效。
    ui->timedRadioButton->setEnabled(false);
    ui->durationSpinBox->setEnabled(false);
    ui->saveIqCheckBox->setEnabled(false);
    ui->averageCountSpinBox->setEnabled(ui->averageCheckBox->isChecked());
    ui->bandwidthSpinBox->setEnabled(!ui->autoBandwidthCheckBox->isChecked());
    connect(ui->averageCheckBox, &QCheckBox::toggled,
            ui->averageCountSpinBox, &QWidget::setEnabled);
    connect(ui->autoBandwidthCheckBox, &QCheckBox::toggled,
            ui->bandwidthSpinBox, [this](bool checked) {
        ui->bandwidthSpinBox->setEnabled(!checked);
        applyRuntimeConfig();
    });

    // 显示控制立即生效；采集参数通过线程安全配置请求交给接收线程应用。
    const auto runtimeChanged = [this]() { applyRuntimeConfig(); };
    connect(ui->sampleRateSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, runtimeChanged);
    connect(ui->centerFrequencySpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, runtimeChanged);
    connect(ui->bandwidthSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, runtimeChanged);
    connect(ui->gainSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, runtimeChanged);
    connect(ui->fftSizeComboBox, &QComboBox::currentTextChanged,
            this, runtimeChanged);
    connect(ui->windowFunctionComboBox, &QComboBox::currentTextChanged,
            this, runtimeChanged);
    connect(ui->averageCheckBox, &QCheckBox::toggled, this, runtimeChanged);
    connect(ui->maxHoldCheckBox, &QCheckBox::toggled, this, runtimeChanged);
    connect(ui->minHoldCheckBox, &QCheckBox::toggled, this, runtimeChanged);
    connect(ui->inputCompensationSpinBox,
            qOverload<double>(&QDoubleSpinBox::valueChanged), this, runtimeChanged);
    connect(ui->averageCountSpinBox, qOverload<int>(&QSpinBox::valueChanged),
            this, runtimeChanged);
    connect(ui->referenceLevelSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
        spectrumWidget_->setReferenceLevel(static_cast<float>(value));
    });
    connect(ui->currentTraceCheckBox, &QCheckBox::toggled,
            spectrumWidget_, &SpectrumWidget::setCurrentTraceVisible);
    // Marker 是有状态的测量工具：默认固定频率，只有用户主动搜索或开启局部跟踪时移动。
    const auto loadSelectedMarker = [this]() {
        const int marker = ui->markerSelectComboBox->currentIndex();
        spectrumWidget_->setActiveMarker(marker);
        const QSignalBlocker blockEnable(ui->markerEnableCheckBox);
        const QSignalBlocker blockFrequency(ui->markerFrequencySpinBox);
        const QSignalBlocker blockTrack(ui->markerTrackCheckBox);
        const QSignalBlocker blockTrace(ui->markerTraceComboBox);
        ui->markerEnableCheckBox->setChecked(spectrumWidget_->markerEnabled(marker));
        ui->markerFrequencySpinBox->setValue(spectrumWidget_->markerFrequency(marker) / 1e6);
        ui->markerTrackCheckBox->setChecked(spectrumWidget_->markerTracking(marker));
        ui->markerTraceComboBox->setCurrentIndex(
            static_cast<int>(spectrumWidget_->markerTrace(marker)));
    };
    connect(ui->markerSelectComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [loadSelectedMarker](int) { loadSelectedMarker(); });
    connect(ui->markerEnableCheckBox, &QCheckBox::toggled, this, [this](bool enabled) {
        spectrumWidget_->setMarkerEnabled(ui->markerSelectComboBox->currentIndex(), enabled);
    });
    connect(ui->markerFrequencySpinBox,
            qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double mhz) {
        spectrumWidget_->setMarkerFrequency(ui->markerSelectComboBox->currentIndex(), mhz * 1e6);
    });
    connect(ui->markerTrackCheckBox, &QCheckBox::toggled, this, [this](bool enabled) {
        spectrumWidget_->setMarkerTracking(ui->markerSelectComboBox->currentIndex(), enabled);
    });
    connect(ui->markerTraceComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        spectrumWidget_->setMarkerTrace(
            ui->markerSelectComboBox->currentIndex(),
            static_cast<SpectrumWidget::MarkerTrace>(index));
    });
    connect(ui->markerPeakButton, &QPushButton::clicked, this, [this]() {
        const int marker = ui->markerSelectComboBox->currentIndex();
        spectrumWidget_->setMarkerEnabled(marker, true);
        ui->markerEnableCheckBox->setChecked(true);
        spectrumWidget_->peakSearch(marker);
    });
    connect(ui->markerNextPeakButton, &QPushButton::clicked, this, [this]() {
        const int marker = ui->markerSelectComboBox->currentIndex();
        const bool wasEnabled = spectrumWidget_->markerEnabled(marker);
        spectrumWidget_->setMarkerEnabled(marker, true);
        ui->markerEnableCheckBox->setChecked(true);
        if (wasEnabled) spectrumWidget_->nextPeak(marker);
    });
    spectrumWidget_->setMarkerChangedCallback([this](int marker, double frequencyHz) {
        if (marker != ui->markerSelectComboBox->currentIndex()) return;
        const QSignalBlocker blocker(ui->markerFrequencySpinBox);
        ui->markerFrequencySpinBox->setValue(frequencyHz / 1e6);
        const QSignalBlocker enableBlocker(ui->markerEnableCheckBox);
        ui->markerEnableCheckBox->setChecked(spectrumWidget_->markerEnabled(marker));
        const QSignalBlocker trackBlocker(ui->markerTrackCheckBox);
        ui->markerTrackCheckBox->setChecked(spectrumWidget_->markerTracking(marker));
        const QSignalBlocker traceBlocker(ui->markerTraceComboBox);
        ui->markerTraceComboBox->setCurrentIndex(
            static_cast<int>(spectrumWidget_->markerTrace(marker)));
    });
    loadSelectedMarker();
    const auto waterfallRangeChanged = [this]() {
        waterfallWidget_->setColorRange(
            static_cast<float>(ui->waterfallMinSpinBox->value()),
            static_cast<float>(ui->waterfallMaxSpinBox->value()));
    };
    connect(ui->waterfallMinSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, waterfallRangeChanged);
    connect(ui->waterfallMaxSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, waterfallRangeChanged);
    spectrumWidget_->setReferenceLevel(
        static_cast<float>(ui->referenceLevelSpinBox->value()));
    spectrumWidget_->setCurrentTraceVisible(ui->currentTraceCheckBox->isChecked());
    waterfallRangeChanged();

    // 顶部参数按显示、Trace、Marker 三组排列；紧凑尺寸避免分隔区缩小时重叠。
    for (QLabel* label : {ui->labelFftSize, ui->labelWindowFunction,
                          ui->labelReferenceLevel, ui->labelInputCompensation,
                          ui->labelWaterfallRange, ui->labelMarker,
                          ui->markerTraceLabel}) {
        label->setMinimumWidth(62);
        label->setMaximumWidth(76);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    const QList<QWidget*> alignedControls = {
        ui->fftSizeComboBox, ui->windowFunctionComboBox, ui->referenceLevelSpinBox,
        ui->inputCompensationSpinBox, ui->averageCountSpinBox,
        ui->waterfallMinSpinBox, ui->waterfallMaxSpinBox,
        ui->markerSelectComboBox, ui->markerFrequencySpinBox,
        ui->markerTraceComboBox
    };
    for (QWidget* control : alignedControls)
        control->setMinimumHeight(28);

    ui->fftSizeComboBox->setMaximumWidth(92);
    ui->windowFunctionComboBox->setMaximumWidth(150);
    ui->referenceLevelSpinBox->setMaximumWidth(108);
    ui->inputCompensationSpinBox->setMaximumWidth(96);
    ui->markerFrequencySpinBox->setMaximumWidth(158);
    ui->markerTraceComboBox->setMaximumWidth(92);
    ui->markerPeakButton->setMaximumWidth(62);
    ui->markerNextPeakButton->setMaximumWidth(72);

    // 所有可点击按钮使用手型光标，增强可操作性反馈。
    const auto buttons = findChildren<QAbstractButton*>();
    for (QAbstractButton* button : buttons) {
        button->setCursor(Qt::PointingHandCursor);
    }

    connect(ui->findDevicesButton, &QPushButton::clicked,
            this, &MainWindow::findDevices);
    connect(ui->actionFindDevices, &QAction::triggered,
            this, &MainWindow::findDevices);
    connect(ui->connectDeviceButton, &QPushButton::clicked,
            this, &MainWindow::connectDevice);
    connect(ui->actionConnectDevice, &QAction::triggered,
            this, &MainWindow::connectDevice);
    connect(ui->disconnectDeviceButton, &QPushButton::clicked,
            this, &MainWindow::disconnectDevice);
    connect(ui->actionDisconnectDevice, &QAction::triggered,
            this, &MainWindow::disconnectDevice);
    connect(ui->startReceiveButton, &QPushButton::clicked,
            this, &MainWindow::startReceiving);
    connect(ui->actionStartReceive, &QAction::triggered,
            this, &MainWindow::startReceiving);
    connect(ui->stopReceiveButton, &QPushButton::clicked,
            this, &MainWindow::stopReceiving);
    connect(ui->actionStopReceive, &QAction::triggered,
            this, &MainWindow::stopReceiving);
    connect(ui->clearLogButton, &QPushButton::clicked,
            ui->logPlainTextEdit, &QPlainTextEdit::clear);
    connect(ui->saveScreenshotButton, &QPushButton::clicked,
            this, &MainWindow::saveScreenshot);

    connect(&discoveryWatcher_, &QFutureWatcher<DeviceDiscoveryResult>::finished,
            this, [this]() {
        const DeviceDiscoveryResult result = discoveryWatcher_.result();
        setDeviceOperationBusy(false);

        if (!result.errorMessage.empty()) {
            appendLog(tr("错误"),
                      tr("UHD 设备查找失败：%1")
                          .arg(QString::fromStdString(result.errorMessage)));
            ui->systemStatusLabel->setText(tr("● 设备查找失败"));
            return;
        }

        if (result.devices.empty()) {
            appendLog(tr("设备"), tr("未发现符合条件的 UHD 设备"));
            ui->systemStatusLabel->setText(tr("● 未发现设备"));
            return;
        }

        const auto deviceCount = static_cast<qulonglong>(result.devices.size());
        appendLog(tr("设备"), tr("发现 %1 台 UHD 设备").arg(deviceCount));
        for (std::size_t index = 0; index < result.devices.size(); ++index) {
            appendLog(tr("设备"),
                      tr("[%1] %2")
                          .arg(static_cast<qulonglong>(index + 1))
                          .arg(QString::fromStdString(result.devices[index])));
        }
        ui->systemStatusLabel->setText(
            tr("● 已发现 %1 台设备").arg(deviceCount));
    });

    connect(&connectionWatcher_, &QFutureWatcher<DeviceConnectionResult>::finished,
            this, [this]() {
        const DeviceConnectionResult result = connectionWatcher_.result();
        setDeviceOperationBusy(false);

        if (!result.success) {
            ui->deviceConnectionStatusLabel->setText(tr("● 连接失败"));
            ui->deviceConnectionStatusLabel->setStyleSheet(
                QStringLiteral("color: #ff5d68;"));
            appendLog(tr("错误"),
                      tr("设备连接失败：%1")
                          .arg(QString::fromStdString(result.errorMessage)));
            return;
        }

        ui->deviceModelValueLabel->setText(
            QString::fromStdString(result.deviceModel));
        ui->connectionValueLabel->setText(tr("UHD"));
        ui->interfaceTypeValueLabel->setText(
            QString::fromStdString(result.interfaceType));
        ui->connectionArgsValueLabel->setText(
            QString::fromStdString(result.connectionArgsUsed));
        ui->fpgaVersionValueLabel->setText(tr("见设备日志"));
        ui->deviceConnectionStatusLabel->setText(tr("● 已连接"));
        ui->deviceConnectionStatusLabel->setStyleSheet(
            QStringLiteral("color: #30dd78;"));
        ui->systemStatusLabel->setText(tr("● UHD 设备已连接"));

        appendLog(
            tr("设备"),
            tr("UHD 设备连接成功：型号 %1，接口 %2，连接参数 %3，设备参数 %4")
                .arg(QString::fromStdString(result.deviceModel),
                     QString::fromStdString(result.interfaceType),
                     QString::fromStdString(result.connectionArgsUsed),
                     QString::fromStdString(result.deviceOptionsUsed)));
        appendLog(tr("设备"), QString::fromStdString(result.deviceInfo));
        updateDeviceControls();
    });

    setAcquisitionRunning(false);
    updateDeviceControls();
}

MainWindow::~MainWindow()
{
    stopReceiving();
    if (rxThread_) {
        rxThread_->quit();
        rxThread_->wait(2000);
    }
    // 防止关闭窗口时后台任务仍访问已经析构的设备对象。
    discoveryWatcher_.waitForFinished();
    connectionWatcher_.waitForFinished();
    delete ui;
}

void MainWindow::startReceiving()
{
    if (acquisitionRunning_ || deviceOperationBusy_ || !sdrDevice_->isConnected()) {
        return;
    }

    const RxConfig config = currentRxConfig();
    spectrumWidget_->setReferenceLevel(
        static_cast<float>(ui->referenceLevelSpinBox->value()));

    if (config.sampleRate <= 0.0 || config.bandwidthHz <= 0.0 ||
        config.centerFrequencyHz <= 0.0) {
        QMessageBox::warning(this, tr("参数无效"),
                             tr("中心频率、采样率和带宽必须大于 0。"));
        return;
    }

    rxThread_ = new QThread(this);
    rxWorker_ = new RxWorker(sdrDevice_->usrp(), config);
    rxWorker_->moveToThread(rxThread_);

    connect(rxThread_, &QThread::started,
            rxWorker_, &RxWorker::startReceiving);
    connect(rxWorker_, &RxWorker::configurationCompleted,
            this, [this](const QString& message) { appendLog(tr("接收"), message); });
    connect(rxWorker_, &RxWorker::warningOccurred,
            this, [this](const QString& message) { appendLog(tr("警告"), message); });
    connect(rxWorker_, &RxWorker::errorOccurred,
            this, [this](const QString& message) {
        appendLog(tr("错误"), message);
        ui->systemStatusLabel->setText(tr("● 接收失败"));
        ui->systemStatusLabel->setStyleSheet(QStringLiteral("color:#ff5d68;"));
    });
    connect(rxWorker_, &RxWorker::actualParameters,
            this, [this](double frequency, double rate, double bandwidth) {
        ui->actualCenterValueLabel->setText(
            tr("%1 MHz").arg(frequency / 1e6, 0, 'f', 6));
        ui->actualRateValueLabel->setText(
            tr("%1 MSps").arg(rate / 1e6, 0, 'f', 3));
        ui->actualBandwidthValueLabel->setText(
            tr("%1 MHz").arg(bandwidth / 1e6, 0, 'f', 3));
    });
    connect(rxWorker_, &RxWorker::receptionStarted,
            this, [this]() {
        ui->systemStatusLabel->setText(tr("● 正在接收 IQ 数据"));
        ui->systemStatusLabel->setStyleSheet(QStringLiteral("color:#30dd78;"));
        appendLog(tr("接收"), tr("连续接收已经启动"));
    });
    connect(rxWorker_, &RxWorker::statisticsUpdated,
            this, [this](quint64 samples, double samplesPerSecond, qint64 elapsedMs) {
        // fc32 每个复数样本为 I/Q 两个 float，共 8 字节。
        const double totalMiB = static_cast<double>(samples) * 8.0 / (1024.0 * 1024.0);
        ui->receivedDataValueLabel->setText(tr("%1 MiB").arg(totalMiB, 0, 'f', 2));
        ui->receiveRateValueLabel->setText(
            tr("%1 MiB/s").arg(samplesPerSecond * 8.0 / (1024.0 * 1024.0), 0, 'f', 2));
        ui->runningTimeValueLabel->setText(
            QTime(0, 0).addMSecs(static_cast<int>(elapsedMs % 86400000)).toString("HH:mm:ss"));
    });
    connect(rxWorker_, &RxWorker::displayFrameReady,
            this, [this](const QVector<float>& i,
                         const QVector<float>& current,
                         const QVector<float>& average,
                         const QVector<float>& maxHold,
                         const QVector<float>& minHold,
                         double rate, double frequency) {
        ++displayFrameCount_;
        waveformWidget_->setSamples(i, rate);
        spectrumWidget_->setSpectrum(current, average, maxHold, minHold,
                                     rate, frequency);
        waterfallWidget_->appendSpectrum(current);
    });
    connect(rxWorker_, &RxWorker::receptionStopped,
            rxThread_, &QThread::quit);
    connect(rxWorker_, &RxWorker::receptionStopped,
            rxWorker_, &QObject::deleteLater);
    connect(rxThread_, &QThread::finished,
            rxThread_, &QObject::deleteLater);
    connect(rxThread_, &QThread::finished,
            this, &MainWindow::finishReceiving);

    setAcquisitionRunning(true);
    ui->receivedDataValueLabel->setText(tr("0 B"));
    ui->runningTimeValueLabel->setText(QStringLiteral("00:00:00"));
    ui->systemStatusLabel->setText(tr("● 正在配置接收参数..."));
    ui->systemStatusLabel->setStyleSheet(QStringLiteral("color:#ffba4a;"));
    appendLog(tr("参数"),
              tr("Center=%1 MHz, Rate=%2 MSps, BW=%3 MHz, Gain=%4 dB, "
                 "FFT=%5, Window=%6, Average=%7, MaxHold=%8, MinHold=%9, Comp=%10 dB")
                  .arg(config.centerFrequencyHz / 1e6, 0, 'f', 6)
                  .arg(config.sampleRate / 1e6, 0, 'f', 3)
                  .arg(config.bandwidthHz / 1e6, 0, 'f', 3)
                  .arg(config.gainDb, 0, 'f', 1)
                  .arg(config.fftSize)
                  .arg(ui->windowFunctionComboBox->currentText())
                  .arg(config.averageEnabled ? tr("开") : tr("关"))
                  .arg(config.maxHoldEnabled ? tr("开") : tr("关"))
                  .arg(config.minHoldEnabled ? tr("开") : tr("关"))
                  .arg(config.inputCompensationDb, 0, 'f', 2));
    rxThread_->start();
}

void MainWindow::stopReceiving()
{
    if (!acquisitionRunning_ || !rxWorker_) {
        return;
    }
    ui->systemStatusLabel->setText(tr("● 正在停止接收..."));
    ui->systemStatusLabel->setStyleSheet(QStringLiteral("color:#ffba4a;"));
    rxWorker_->requestStop(); // 原子操作，可从 GUI 线程安全调用。
    ui->stopReceiveButton->setEnabled(false);
    ui->actionStopReceive->setEnabled(false);
}

void MainWindow::finishReceiving()
{
    if (!acquisitionRunning_) {
        return;
    }
    setAcquisitionRunning(false);
    rxWorker_ = nullptr;
    rxThread_ = nullptr;
    ui->systemStatusLabel->setText(tr("● 接收已经停止"));
    ui->systemStatusLabel->setStyleSheet(QStringLiteral("color:#ff5d68;"));
    appendLog(tr("接收"), tr("连续接收已经安全停止"));
}

void MainWindow::findDevices()
{
    const QString deviceArgs = ui->connectionArgsEdit->text().trimmed();

    if (deviceOperationBusy_ || acquisitionRunning_) {
        return;
    }

    setDeviceOperationBusy(true);
    ui->systemStatusLabel->setText(tr("● 正在查找设备..."));
    appendLog(tr("设备"),
              deviceArgs.isEmpty()
                  ? tr("开始查找 UHD 设备（无筛选条件）")
                  : tr("开始查找 UHD 设备，参数：%1").arg(deviceArgs));

    discoveryWatcher_.setFuture(QtConcurrent::run(
        [device = sdrDevice_.get(), args = deviceArgs.toStdString()]() {
            return device->findDevices(args);
        }));
}

void MainWindow::connectDevice()
{
    const QString connectionArgs = ui->connectionArgsEdit->text().trimmed();
    const QString deviceOptions = ui->deviceArgsEdit->text().trimmed();

    if (deviceOperationBusy_ || acquisitionRunning_ || sdrDevice_->isConnected()) {
        return;
    }

    setDeviceOperationBusy(true);

    ui->deviceConnectionStatusLabel->setText(tr("● 正在连接"));
    ui->deviceConnectionStatusLabel->setStyleSheet(
        QStringLiteral("color: #ffba4a;"));

    appendLog(
        tr("设备"),
        connectionArgs.isEmpty()
            ? tr("开始连接 UHD 设备（未指定连接参数）")
            : tr("开始连接 UHD 设备，连接参数：%1").arg(connectionArgs));
    if (!deviceOptions.isEmpty())
        appendLog(tr("设备"), tr("UHD 初始化设备参数：%1").arg(deviceOptions));

    connectionWatcher_.setFuture(QtConcurrent::run(
        [device = sdrDevice_.get(), connection = connectionArgs.toStdString(),
         options = deviceOptions.toStdString()]() {
            return device->connectDevice(connection, options);
        }));
}

void MainWindow::saveScreenshot()
{
    const QString suggested = QDir::homePath() + QLatin1Char('/')
        + QStringLiteral("signal_capture_%1.png")
              .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    const QString fileName = QFileDialog::getSaveFileName(
        this, tr("保存显示区域截图"), suggested, tr("PNG 图像 (*.png)"));
    if (fileName.isEmpty()) return;

    // 只截取三张仪表图及其控制栏，不把左/右参数区和日志写入图片。
    const QPixmap image = ui->spectrumTab->grab();
    if (image.save(fileName, "PNG")) {
        appendLog(tr("截图"), tr("显示区域已保存：%1").arg(QDir::toNativeSeparators(fileName)));
        ui->statusBar->showMessage(tr("截图保存成功"), 3000);
    } else {
        QMessageBox::warning(this, tr("截图保存失败"), tr("无法写入文件：%1").arg(fileName));
    }
}

void MainWindow::disconnectDevice()
{
    if (deviceOperationBusy_ || acquisitionRunning_ || !sdrDevice_->isConnected()) {
        return;
    }

    sdrDevice_->disconnectDevice();

    ui->deviceModelValueLabel->setText(tr("未知"));
    ui->connectionValueLabel->setText(tr("未连接"));
    ui->interfaceTypeValueLabel->setText(tr("--"));
    ui->connectionArgsValueLabel->setText(tr("--"));
    ui->fpgaVersionValueLabel->setText(tr("--"));
    ui->temperatureValueLabel->setText(tr("-- °C"));

    ui->deviceConnectionStatusLabel->setText(tr("● 未连接"));
    ui->deviceConnectionStatusLabel->setStyleSheet(
        QStringLiteral("color: #ffba4a;"));

    ui->systemStatusLabel->setText(tr("● UHD 设备已断开"));

    appendLog(tr("设备"), tr("UHD 设备已断开"));
    updateDeviceControls();
}

void MainWindow::setDeviceOperationBusy(bool busy)
{
    deviceOperationBusy_ = busy;
    updateDeviceControls();
}

void MainWindow::updateDeviceControls()
{
    const bool connected = sdrDevice_->isConnected();
    const bool idle = !deviceOperationBusy_ && !acquisitionRunning_;

    ui->findDevicesButton->setEnabled(idle && !connected);
    ui->connectDeviceButton->setEnabled(idle && !connected);
    ui->disconnectDeviceButton->setEnabled(idle && connected);
    ui->actionFindDevices->setEnabled(idle && !connected);
    ui->actionConnectDevice->setEnabled(idle && !connected);
    ui->actionDisconnectDevice->setEnabled(idle && connected);
    ui->connectionArgsEdit->setEnabled(idle && !connected);
    ui->deviceArgsEdit->setEnabled(idle && !connected);

    // 接收功能接通后，这两个状态仍由同一个函数统一维护。
    ui->startReceiveButton->setEnabled(idle && connected);
    ui->stopReceiveButton->setEnabled(acquisitionRunning_);
    ui->actionStartReceive->setEnabled(idle && connected);
    ui->actionStopReceive->setEnabled(acquisitionRunning_);
}

void MainWindow::setAcquisitionRunning(bool running)
{
    acquisitionRunning_ = running;
    // 通道和天线会改变流结构，采集中保持锁定。模拟带宽策略可在线更新。
    // 其余参数允许在线调整，并由 RxWorker 在接收线程应用。
    const QList<QWidget*> configurationWidgets = {
        ui->channelComboBox, ui->antennaComboBox
    };
    for (QWidget* widget : configurationWidgets) widget->setEnabled(!running);
    ui->averageCountSpinBox->setEnabled(ui->averageCheckBox->isChecked());
    ui->bandwidthSpinBox->setEnabled(!ui->autoBandwidthCheckBox->isChecked());
    updateDeviceControls();
}

RxConfig MainWindow::currentRxConfig() const
{
    RxConfig config;
    config.channel = static_cast<std::size_t>(ui->channelComboBox->currentIndex());
    config.antenna = ui->antennaComboBox->currentText();
    config.centerFrequencyHz = ui->centerFrequencySpinBox->value() * 1e6;
    config.sampleRate = ui->sampleRateSpinBox->value() * 1e6;
    config.bandwidthHz = ui->autoBandwidthCheckBox->isChecked()
        ? config.sampleRate : ui->bandwidthSpinBox->value() * 1e6;
    config.gainDb = ui->gainSpinBox->value();
    config.fftSize = ui->fftSizeComboBox->currentText().toInt();
    config.window = selectedWindow(ui->windowFunctionComboBox->currentText());
    config.averageEnabled = ui->averageCheckBox->isChecked();
    config.averageCount = ui->averageCountSpinBox->value();
    config.maxHoldEnabled = ui->maxHoldCheckBox->isChecked();
    config.minHoldEnabled = ui->minHoldCheckBox->isChecked();
    config.currentTraceVisible = ui->currentTraceCheckBox->isChecked();
    config.inputCompensationDb = ui->inputCompensationSpinBox->value();
    return config;
}

void MainWindow::applyRuntimeConfig()
{
    spectrumWidget_->setCurrentTraceVisible(ui->currentTraceCheckBox->isChecked());
    if (acquisitionRunning_ && rxWorker_)
        rxWorker_->requestRuntimeConfig(currentRxConfig());
}

void MainWindow::updatePerformanceStatus()
{
    const qint64 wallMs = performanceClock_.restart();
    const quint64 processNow = processCpuTime100ns();
    const quint64 processDelta = processNow - previousProcessTime100ns_;
    previousProcessTime100ns_ = processNow;
    const int cores = std::max(1, QThread::idealThreadCount());
    const double cpu = (wallMs > 0 && processNow > 0)
        ? 100.0 * double(processDelta) / (double(wallMs) * 10000.0 * cores)
        : -1.0;
    const quint64 frames = displayFrameCount_ - previousDisplayFrameCount_;
    previousDisplayFrameCount_ = displayFrameCount_;
    performanceLabel_->setText(cpu >= 0.0
        ? tr("CPU %1%  |  Display %2 FPS").arg(cpu, 0, 'f', 1).arg(frames)
        : tr("CPU N/A  |  Display %1 FPS").arg(frames));
}

void MainWindow::appendLog(const QString& category, const QString& message)
{
    const QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    ui->logPlainTextEdit->appendPlainText(
        QStringLiteral("[%1] [%2] %3").arg(timestamp, category, message));
}
