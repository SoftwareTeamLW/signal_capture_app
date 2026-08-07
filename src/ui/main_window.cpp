#include "ui/main_window.hpp"

#include "sdr_device.hpp"
#include "rx_receiver.hpp"
#include "rx_config.hpp"
#include "signal_plot_widgets.hpp"
#include "ui_main_window.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHeaderView>
#include <QPushButton>
#include <QPixmap>
#include <QLabel>
#include <QLibraryInfo>
#include <QList>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QSizePolicy>
#include <QSplitter>
#include <QStorageInfo>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QTranslator>
#include <QVariant>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

WindowFunction selectedWindow(int index)
{
    switch (index) {
    case 1: return WindowFunction::Hamming;
    case 2: return WindowFunction::BlackmanHarris;
    case 3: return WindowFunction::FlatTop;
    default: return WindowFunction::Hann;
    }
}

QString translatedSdrValue(const QString& source)
{
    if (source == QStringLiteral("UHD 设备"))
        return QCoreApplication::translate("SdrDevice", "UHD 设备");
    if (source == QStringLiteral("USB（速率未知）"))
        return QCoreApplication::translate("SdrDevice", "USB（速率未知）");
    if (source == QStringLiteral("未知"))
        return QCoreApplication::translate("SdrDevice", "未知");
    if (source == QStringLiteral("默认（未指定）"))
        return QCoreApplication::translate("SdrDevice", "默认（未指定）");
    return source;
}

QString translatedDeviceDisplayName(const QString& source)
{
    const QStringList knownPrefixes = {
        QStringLiteral("UHD 设备"),
        QStringLiteral("UHD Device"),
        QStringLiteral("Устройство UHD")
    };
    for (const QString& prefix : knownPrefixes) {
        if (source.startsWith(prefix)) {
            return QCoreApplication::translate("SdrDevice", "UHD 设备")
                + source.mid(prefix.size());
        }
    }
    return source;
}

QPixmap loadCompanyLogo()
{
    // Keep the executable-side file replaceable for deployments and customer
    // branding. The embedded image guarantees a visible logo in fresh builds.
    const QString externalPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/assets/company_logo.png");
    QPixmap logo(externalPath);
    if (logo.isNull())
        logo.load(QStringLiteral(":/branding/company_logo.png"));
    return logo;
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , sdrDevice_(std::make_unique<SdrDevice>())
    , discoveryWatcher_(
          std::make_unique<QFutureWatcher<DeviceDiscoveryResult>>())
    , connectionWatcher_(
          std::make_unique<QFutureWatcher<DeviceConnectionResult>>())
{
    ui->setupUi(this);
    setupLanguageMenu();

    setWindowTitle(tr("LuoWave Signal Capture"));
    resize(1450, 900);
    ui->mainSplitter->setChildrenCollapsible(false);
    ui->mainSplitter->setStretchFactor(0, 0);
    ui->mainSplitter->setStretchFactor(1, 1);
    ui->mainSplitter->setStretchFactor(2, 0);
    applyMainPanelGeometry();

    // 左侧组合框保持紧凑；弹出的设备列表单独加宽，以完整显示型号、
    // 序列号以及 X310 的地址或 PCIe resource。
    ui->deviceSelectComboBox->view()->setMinimumWidth(300);

    for (int handleIndex = 1;
         handleIndex < ui->mainSplitter->count();
         ++handleIndex) {
        ui->mainSplitter->handle(handleIndex)->setEnabled(false);
    }

    ui->displaySplitter->setChildrenCollapsible(false);
    // Keep the time-domain preview compact for now. Extra vertical space goes
    // primarily to the waterfall, leaving room for the later analysis panel
    // without sacrificing the spectrum's minimum readable height.
    ui->displaySplitter->setStretchFactor(0, 3);
    ui->displaySplitter->setStretchFactor(1, 4);
    ui->displaySplitter->setStretchFactor(2, 0);
    ui->displaySplitter->setSizes({290, 340, 105});

    const QPixmap companyLogo = loadCompanyLogo();
    if (!companyLogo.isNull()) {
        ui->companyLogoLabel->setText(QString());
        ui->companyLogoLabel->setStyleSheet(QStringLiteral(
            "background:#111722;border:1px solid #34445a;"
            "border-radius:7px;padding:3px;"));
        // Wait until the layout has assigned the label its final size. Scaling
        // before the first layout pass is why the logo could be tiny or absent.
        QTimer::singleShot(0, this, [this, companyLogo]() {
            const QSize target = ui->companyLogoLabel->contentsRect().size();
            if (!target.isEmpty()) {
                ui->companyLogoLabel->setPixmap(companyLogo.scaled(
                    target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        });
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

    // 右下角常驻显示磁盘、进程 CPU 和实际绘图帧率。
    performanceLabel_ = new QLabel(tr("CPU --  |  Display 0 FPS"), this);
    performanceLabel_->setMinimumWidth(190);
    performanceLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->statusBar->addPermanentWidget(performanceLabel_);
    diskCapacityLabel_ = new QLabel(tr("磁盘 --"), this);
    diskCapacityLabel_->setMinimumWidth(150);
    diskCapacityLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->statusBar->addPermanentWidget(diskCapacityLabel_);
    diskStatusPath_ = QDir::homePath();
    setupMetadataView();
    updateDiskCapacity();
    performanceClock_.start();
    previousProcessTime100ns_ = processCpuTime100ns();
    performanceTimer_.setInterval(1000);
    connect(&performanceTimer_, &QTimer::timeout,
            this, &MainWindow::updatePerformanceStatus);
    performanceTimer_.start();

    ui->durationSpinBox->setEnabled(ui->timedRadioButton->isChecked());
    connect(ui->timedRadioButton, &QRadioButton::toggled,
            ui->durationSpinBox, &QWidget::setEnabled);
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

    // 顶部参数按显示、Trace、Marker 三组排列。文字控件不能使用只适合
    // 中文的固定宽度，否则英文和俄文切换后会被截断。
    for (QLabel* label : {ui->labelFftSize, ui->labelWindowFunction,
                          ui->labelReferenceLevel, ui->labelInputCompensation,
                          ui->labelWaterfallRange, ui->labelMarker,
                          ui->markerTraceLabel}) {
        label->setMinimumWidth(0);
        label->setMaximumWidth(QWIDGETSIZE_MAX);
        label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
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
    ui->windowFunctionComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui->markerTraceComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui->markerTraceComboBox->setMaximumWidth(150);

    for (QAbstractButton* textControl : {
             static_cast<QAbstractButton*>(ui->maxHoldCheckBox),
             static_cast<QAbstractButton*>(ui->minHoldCheckBox),
             static_cast<QAbstractButton*>(ui->currentTraceCheckBox),
             static_cast<QAbstractButton*>(ui->averageCheckBox),
             static_cast<QAbstractButton*>(ui->markerEnableCheckBox),
             static_cast<QAbstractButton*>(ui->markerTrackCheckBox),
             static_cast<QAbstractButton*>(ui->markerPeakButton),
             static_cast<QAbstractButton*>(ui->markerNextPeakButton),
             static_cast<QAbstractButton*>(ui->saveScreenshotButton)}) {
        textControl->setMinimumWidth(0);
        textControl->setMaximumWidth(QWIDGETSIZE_MAX);
        textControl->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    }
    applyLanguageLayout();

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
    connect(ui->deviceSelectComboBox,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { updateDeviceControls(); });

    connect(discoveryWatcher_.get(),
            &QFutureWatcher<DeviceDiscoveryResult>::finished,
            this, [this]() {
        const DeviceDiscoveryResult result = discoveryWatcher_->result();
        setDeviceOperationBusy(false);
        ui->deviceSelectComboBox->clear();

        if (!result.errorMessage.empty()) {
            deviceListUiState_ = DeviceListUiState::DiscoveryFailed;
            updateDeviceListPlaceholder();
            appendLog(tr("错误"),
                      tr("UHD 设备查找失败：%1")
                          .arg(QString::fromStdString(result.errorMessage)));
            setSystemUiState(SystemUiState::DiscoveryFailed);
            updateDeviceControls();
            return;
        }

        if (result.devices.empty()) {
            deviceListUiState_ = DeviceListUiState::Empty;
            updateDeviceListPlaceholder();
            appendLog(tr("设备"), tr("未发现符合条件的 UHD 设备"));
            setSystemUiState(SystemUiState::NoDevices);
            updateDeviceControls();
            return;
        }

        const auto deviceCount = static_cast<qulonglong>(result.devices.size());
        deviceListUiState_ = DeviceListUiState::Ready;
        appendLog(tr("设备"), tr("发现 %1 台 UHD 设备").arg(deviceCount));
        for (std::size_t index = 0; index < result.devices.size(); ++index) {
            const auto& device = result.devices[index];
            const QString displayName = translatedDeviceDisplayName(
                QString::fromStdString(device.displayName));
            ui->deviceSelectComboBox->addItem(
                displayName,
                QString::fromStdString(device.connectionArgs));
            const int comboIndex = ui->deviceSelectComboBox->count() - 1;
            ui->deviceSelectComboBox->setItemData(
                comboIndex,
                displayName + QLatin1Char('\n')
                    + QString::fromStdString(device.connectionArgs),
                Qt::ToolTipRole);
            appendLog(tr("设备"),
                      tr("[%1] %2")
                          .arg(static_cast<qulonglong>(index + 1))
                          .arg(QString::fromStdString(device.connectionArgs)));
        }
        setSystemUiState(SystemUiState::DevicesFound, deviceCount);
        updateDeviceControls();
    });

    connect(connectionWatcher_.get(),
            &QFutureWatcher<DeviceConnectionResult>::finished,
            this, [this]() {
        const DeviceConnectionResult result = connectionWatcher_->result();
        setDeviceOperationBusy(false);

        if (!result.success) {
            setConnectionUiState(ConnectionUiState::ConnectionFailed);
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
        connectedInterfaceTypeSource_ = QString::fromStdString(result.interfaceType);
        ui->interfaceTypeValueLabel->setText(
            translatedSdrValue(connectedInterfaceTypeSource_));
        ui->fpgaVersionValueLabel->setText(tr("见设备日志"));
        setConnectionUiState(ConnectionUiState::Connected);
        ui->deviceConnectionStatusLabel->setStyleSheet(
            QStringLiteral("color: #30dd78;"));
        setSystemUiState(SystemUiState::DeviceConnected);

        appendLog(
            tr("设备"),
            tr("UHD 设备连接成功：型号 %1，接口 %2，连接参数 %3，设备参数 %4")
                .arg(QString::fromStdString(result.deviceModel),
                     translatedSdrValue(QString::fromStdString(result.interfaceType)),
                     translatedSdrValue(QString::fromStdString(result.connectionArgsUsed)),
                     translatedSdrValue(QString::fromStdString(result.deviceOptionsUsed))));
        appendLog(tr("设备"), QString::fromStdString(result.deviceInfo));
        updateDeviceControls();
    });

    setAcquisitionRunning(false);
    updateDeviceControls();
    switchLanguage(QStringLiteral("zh_CN"));

    // The splitter gets its usable width only after the first layout pass.
    // Reapply the panel geometry once so the center plot consumes every pixel
    // left between the two fixed side panels.
    QTimer::singleShot(0, this, [this]() { applyMainPanelGeometry(); });
}

MainWindow::~MainWindow()
{
    stopReceiving();
    if (rxThread_) {
        rxThread_->wait();
    }
    // 防止关闭窗口时后台任务仍访问已经析构的设备对象。
    discoveryWatcher_->waitForFinished();
    connectionWatcher_->waitForFinished();
    if (qtTranslator_)
        qApp->removeTranslator(qtTranslator_.get());
    if (appTranslator_)
        qApp->removeTranslator(appTranslator_.get());
    delete ui;
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange && ui) {
        const QList<QLabel*> dataLabels = {
            ui->deviceModelValueLabel,
            ui->connectionValueLabel,
            ui->interfaceTypeValueLabel,
            ui->fpgaVersionValueLabel,
            ui->actualCenterValueLabel,
            ui->actualRateValueLabel,
            ui->actualBandwidthValueLabel,
            ui->receiveRateValueLabel,
            ui->receivedDataValueLabel,
            ui->runningTimeValueLabel
        };
        QStringList dataLabelTexts;
        dataLabelTexts.reserve(dataLabels.size());
        for (const QLabel* label : dataLabels)
            dataLabelTexts.push_back(label->text());

        QStringList metadataValues;
        // 第 9 行是可翻译的 IQ 保存状态，不保存其显示文本；语言切换后
        // 根据语言无关的 iqSaveUiState_ 重新生成。其他行均为实时数据。
        for (int row = 0; row < ui->metadataTableWidget->rowCount() - 1; ++row) {
            const QTableWidgetItem* item = ui->metadataTableWidget->item(row, 1);
            metadataValues.push_back(item ? item->text() : QStringLiteral("--"));
        }

        struct DeviceItem {
            QString text;
            QVariant data;
        };
        QList<DeviceItem> deviceItems;
        const int selectedDevice = ui->deviceSelectComboBox->currentIndex();
        if (deviceListUiState_ == DeviceListUiState::Ready) {
            for (int index = 0; index < ui->deviceSelectComboBox->count(); ++index) {
                deviceItems.push_back({
                    ui->deviceSelectComboBox->itemText(index),
                    ui->deviceSelectComboBox->itemData(index)
                });
            }
        }

        ui->retranslateUi(this);

        for (int index = 0; index < dataLabels.size(); ++index) {
            const bool isDeviceField = index < 4;
            if (!isDeviceField || sdrDevice_->isConnected())
                dataLabels[index]->setText(dataLabelTexts[index]);
        }
        for (int row = 0;
             row < metadataValues.size() && row < ui->metadataTableWidget->rowCount();
             ++row) {
            if (QTableWidgetItem* item = ui->metadataTableWidget->item(row, 1))
                item->setText(metadataValues[row]);
        }
        if (!deviceItems.isEmpty()) {
            ui->deviceSelectComboBox->clear();
            for (const DeviceItem& item : deviceItems) {
                ui->deviceSelectComboBox->addItem(
                    translatedDeviceDisplayName(item.text), item.data);
                ui->deviceSelectComboBox->setItemData(
                    ui->deviceSelectComboBox->count() - 1,
                    translatedDeviceDisplayName(item.text) + QLatin1Char('\n')
                        + item.data.toString(),
                    Qt::ToolTipRole);
            }
            ui->deviceSelectComboBox->setCurrentIndex(selectedDevice);
        }

        retranslateDynamicUi();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::setupLanguageMenu()
{
    languageActionGroup_ = new QActionGroup(this);
    languageActionGroup_->setExclusive(true);
    languageActionGroup_->addAction(ui->actionLanguageChinese);
    languageActionGroup_->addAction(ui->actionLanguageEnglish);
    languageActionGroup_->addAction(ui->actionLanguageRussian);
    ui->actionLanguageChinese->setChecked(true);

    connect(ui->actionLanguageChinese, &QAction::triggered,
            this, [this]() { switchLanguage(QStringLiteral("zh_CN")); });
    connect(ui->actionLanguageEnglish, &QAction::triggered,
            this, [this]() { switchLanguage(QStringLiteral("en")); });
    connect(ui->actionLanguageRussian, &QAction::triggered,
            this, [this]() { switchLanguage(QStringLiteral("ru")); });
}

void MainWindow::switchLanguage(const QString& languageCode)
{
    if (currentLanguageCode_ == languageCode)
        return;

    const QString resourcePath = QStringLiteral(":/i18n/signal_capture_app_%1.qm")
        .arg(languageCode);
    auto nextTranslator = std::make_unique<QTranslator>();
    if (!nextTranslator->load(resourcePath))
        return;

    auto nextQtTranslator = std::make_unique<QTranslator>();
    const bool hasQtTranslation = languageCode != QStringLiteral("en")
        && nextQtTranslator->load(
            QStringLiteral("qtbase_%1").arg(languageCode),
            QLibraryInfo::path(QLibraryInfo::TranslationsPath));

    if (qtTranslator_)
        qApp->removeTranslator(qtTranslator_.get());
    if (appTranslator_)
        qApp->removeTranslator(appTranslator_.get());
    appTranslator_ = std::move(nextTranslator);
    if (hasQtTranslation)
        qtTranslator_ = std::move(nextQtTranslator);
    else
        qtTranslator_.reset();
    currentLanguageCode_ = languageCode;
    qApp->installTranslator(appTranslator_.get());
    if (qtTranslator_)
        qApp->installTranslator(qtTranslator_.get());
}

void MainWindow::retranslateDynamicUi()
{
    setWindowTitle(tr("LuoWave Signal Capture"));
    retranslateMetadataView();
    updateIqSaveStatus();
    updateDeviceListPlaceholder();
    setSystemUiState(systemUiState_, discoveredDeviceCount_);
    setConnectionUiState(connectionUiState_);
    if (sdrDevice_->isConnected()) {
        ui->interfaceTypeValueLabel->setText(
            translatedSdrValue(connectedInterfaceTypeSource_));
        ui->fpgaVersionValueLabel->setText(tr("见设备日志"));
    }
    updatePerformanceLabel();
    updateDiskCapacity();

    ui->actionLanguageChinese->setChecked(currentLanguageCode_ == QStringLiteral("zh_CN"));
    ui->actionLanguageEnglish->setChecked(currentLanguageCode_ == QStringLiteral("en"));
    ui->actionLanguageRussian->setChecked(currentLanguageCode_ == QStringLiteral("ru"));

    applyLanguageLayout();

    if (spectrumWidget_) spectrumWidget_->update();
    if (waterfallWidget_) waterfallWidget_->update();
    if (waveformWidget_) waveformWidget_->update();
}

void MainWindow::applyLanguageLayout()
{
    // 译文改变后让顶部文字控件重新提交 sizeHint，保持原有两行布局，
    // 但不再按中文版字符数裁切英文或俄文。
    for (QWidget* widget : {
             static_cast<QWidget*>(ui->labelFftSize),
             static_cast<QWidget*>(ui->labelWindowFunction),
             static_cast<QWidget*>(ui->labelReferenceLevel),
             static_cast<QWidget*>(ui->labelInputCompensation),
             static_cast<QWidget*>(ui->labelWaterfallRange),
             static_cast<QWidget*>(ui->labelMarker),
             static_cast<QWidget*>(ui->markerTraceLabel),
             static_cast<QWidget*>(ui->maxHoldCheckBox),
             static_cast<QWidget*>(ui->minHoldCheckBox),
             static_cast<QWidget*>(ui->currentTraceCheckBox),
             static_cast<QWidget*>(ui->averageCheckBox),
             static_cast<QWidget*>(ui->markerEnableCheckBox),
             static_cast<QWidget*>(ui->markerTrackCheckBox),
             static_cast<QWidget*>(ui->markerPeakButton),
             static_cast<QWidget*>(ui->markerNextPeakButton),
             static_cast<QWidget*>(ui->saveScreenshotButton),
             static_cast<QWidget*>(ui->windowFunctionComboBox),
             static_cast<QWidget*>(ui->markerTraceComboBox)}) {
        widget->updateGeometry();
    }

    // 右侧区域保持原宽度，由内部组件适应视口，而不是把滚动区域撑宽。
    for (QGroupBox* group : {ui->acquisitionInfoGroupBox,
                             ui->metadataGroupBox,
                             ui->logGroupBox}) {
        group->setMinimumWidth(0);
        group->setMaximumWidth(QWIDGETSIZE_MAX);
        group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    ui->rightScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    applyMainPanelGeometry();

    // 俄文“Устройство”比中文标签更宽；仅收紧组合框本体，弹出列表仍保持
    // 300 px，可完整显示型号、序列号和连接参数。
    const int deviceComboWidth =
        currentLanguageCode_ == QStringLiteral("ru") ? 135 : 150;
    ui->deviceSelectComboBox->setMinimumWidth(deviceComboWidth);
    ui->deviceSelectComboBox->setMaximumWidth(deviceComboWidth);
    ui->findDevicesButton->setToolTip(tr("查找并列出可用的 UHD 设备"));
    ui->connectDeviceButton->setToolTip(tr("连接当前选中的设备"));
    ui->disconnectDeviceButton->setToolTip(tr("断开当前已连接的设备"));

    // 元数据列宽由界面宽度控制。俄文项目名稍长，因此给第一列更多空间；
    // 第二列始终占用余下宽度，表格内部不出现横向或纵向滚动条。
    QTableWidget* table = ui->metadataTableWidget;
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setWordWrap(false);
    QHeaderView* header = table->horizontalHeader();
    header->setMinimumSectionSize(36);
    header->setSectionResizeMode(0, QHeaderView::Fixed);
    header->resizeSection(0,
        currentLanguageCode_ == QStringLiteral("ru") ? 118
        : currentLanguageCode_ == QStringLiteral("en") ? 108 : 86);
    header->setSectionResizeMode(1, QHeaderView::Stretch);

    // 日志行在固定右侧宽度内自动换行，不产生横向滚动条。
    ui->logPlainTextEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    ui->logPlainTextEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void MainWindow::applyMainPanelGeometry()
{
    constexpr int leftPanelWidth = 255;
    const int rightPanelWidth =
        currentLanguageCode_ == QStringLiteral("ru") ? 275
        : currentLanguageCode_ == QStringLiteral("en") ? 260 : 245;

    // Side panels use deliberate language-specific widths. The center panel
    // and its tab widget are the only horizontally expanding items, preventing
    // the unused strip that appeared beside the plots in Chinese mode.
    ui->leftScrollArea->setMinimumWidth(leftPanelWidth);
    ui->leftScrollArea->setMaximumWidth(leftPanelWidth);
    ui->leftScrollArea->setSizePolicy(
        QSizePolicy::Fixed, QSizePolicy::Expanding);

    ui->rightScrollArea->setMinimumWidth(rightPanelWidth);
    ui->rightScrollArea->setMaximumWidth(rightPanelWidth);
    ui->rightScrollArea->setSizePolicy(
        QSizePolicy::Fixed, QSizePolicy::Expanding);

    ui->centerPanel->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->displayTabWidget->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);

    const int centerWidth = std::max(
        ui->centerPanel->minimumWidth(),
        ui->mainSplitter->contentsRect().width()
            - leftPanelWidth - rightPanelWidth);
    ui->mainSplitter->setSizes(
        {leftPanelWidth, centerWidth, rightPanelWidth});
}

void MainWindow::updateDeviceListPlaceholder()
{
    QString text;
    switch (deviceListUiState_) {
    case DeviceListUiState::Initial:
        text = tr("请先点击查找设备");
        break;
    case DeviceListUiState::Searching:
        text = tr("正在查找设备...");
        break;
    case DeviceListUiState::DiscoveryFailed:
        text = tr("设备查找失败");
        break;
    case DeviceListUiState::Empty:
        text = tr("未发现设备");
        break;
    case DeviceListUiState::Ready:
        return;
    }
    ui->deviceSelectComboBox->clear();
    ui->deviceSelectComboBox->addItem(text);
}

void MainWindow::setSystemUiState(SystemUiState state, qulonglong deviceCount)
{
    systemUiState_ = state;
    if (state == SystemUiState::DevicesFound)
        discoveredDeviceCount_ = deviceCount;

    switch (state) {
    case SystemUiState::Normal:
        ui->systemStatusLabel->setText(tr("● 系统运行正常"));
        break;
    case SystemUiState::SearchingDevices:
        ui->systemStatusLabel->setText(tr("● 正在查找设备..."));
        break;
    case SystemUiState::DiscoveryFailed:
        ui->systemStatusLabel->setText(tr("● 设备查找失败"));
        break;
    case SystemUiState::NoDevices:
        ui->systemStatusLabel->setText(tr("● 未发现设备"));
        break;
    case SystemUiState::DevicesFound:
        ui->systemStatusLabel->setText(
            tr("● 已发现 %1 台设备").arg(discoveredDeviceCount_));
        break;
    case SystemUiState::DeviceConnected:
        ui->systemStatusLabel->setText(tr("● UHD 设备已连接"));
        break;
    case SystemUiState::ConfiguringReception:
        ui->systemStatusLabel->setText(tr("● 正在配置接收参数..."));
        break;
    case SystemUiState::Receiving:
        ui->systemStatusLabel->setText(tr("● 正在接收 IQ 数据"));
        break;
    case SystemUiState::StoppingReception:
        ui->systemStatusLabel->setText(tr("● 正在停止接收..."));
        break;
    case SystemUiState::ReceptionFailed:
        ui->systemStatusLabel->setText(tr("● 接收失败"));
        break;
    case SystemUiState::AcquisitionFailed:
        ui->systemStatusLabel->setText(tr("● 采集异常停止"));
        break;
    case SystemUiState::TimedAcquisitionCompleted:
        ui->systemStatusLabel->setText(tr("● 定长采集已完成"));
        break;
    case SystemUiState::ReceptionStopped:
        ui->systemStatusLabel->setText(tr("● 接收已经停止"));
        break;
    case SystemUiState::DeviceDisconnected:
        ui->systemStatusLabel->setText(tr("● UHD 设备已断开"));
        break;
    }
}

void MainWindow::setConnectionUiState(ConnectionUiState state)
{
    connectionUiState_ = state;
    switch (state) {
    case ConnectionUiState::Disconnected:
        ui->deviceConnectionStatusLabel->setText(tr("● 未连接"));
        break;
    case ConnectionUiState::Connecting:
        ui->deviceConnectionStatusLabel->setText(tr("● 正在连接"));
        break;
    case ConnectionUiState::ConnectionFailed:
        ui->deviceConnectionStatusLabel->setText(tr("● 连接失败"));
        break;
    case ConnectionUiState::Connected:
        ui->deviceConnectionStatusLabel->setText(tr("● 已连接"));
        break;
    }
}

void MainWindow::startReceiving()
{
    if (acquisitionRunning_ || deviceOperationBusy_ || !sdrDevice_->isConnected()) {
        return;
    }

    RxConfig config = currentRxConfig();
    spectrumWidget_->setReferenceLevel(
        static_cast<float>(ui->referenceLevelSpinBox->value()));

    if (config.sampleRate <= 0.0 || config.bandwidthHz <= 0.0 ||
        config.centerFrequencyHz <= 0.0) {
        QMessageBox::warning(this, tr("参数无效"),
                             tr("中心频率、采样率和带宽必须大于 0。"));
        return;
    }

    if (config.saveIq) {
        const QString initialPath = QDir(diskStatusPath_).filePath(
            suggestedIqFileName(config));
        QString filePath = QFileDialog::getSaveFileName(
            this, tr("选择 IQ 数据保存位置"), initialPath,
            tr("复数 IQ 裸二进制数据 (*.bin);;所有文件 (*.*)"));
        if (filePath.isEmpty()) return;
        const QFileInfo selectedFile(filePath);
        if (selectedFile.suffix().compare(
                QStringLiteral("bin"), Qt::CaseInsensitive) != 0) {
            filePath = QDir(selectedFile.absolutePath()).filePath(
                selectedFile.completeBaseName() + QStringLiteral(".bin"));
        }

        const QStorageInfo storage(QFileInfo(filePath).absolutePath());
        const quint64 expectedBytes = config.acquisitionMode == AcquisitionMode::Timed
            ? static_cast<quint64>(config.durationSeconds * config.sampleRate * 8.0)
            : 0;
        if (expectedBytes > 0 && storage.isValid()
            && expectedBytes > static_cast<quint64>(storage.bytesAvailable())) {
            QMessageBox::warning(
                this, tr("磁盘空间不足"),
                tr("定长采集预计需要 %1 GiB，但当前磁盘仅剩 %2 GiB。")
                    .arg(double(expectedBytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
                    .arg(double(storage.bytesAvailable()) /
                             (1024.0 * 1024.0 * 1024.0), 0, 'f', 2));
            return;
        }
        config.iqFilePath = filePath;
        diskStatusPath_ = QFileInfo(filePath).absolutePath();
        updateDiskCapacity();
    }

    stopRequestedByUser_ = false;
    acquisitionFailed_ = false;
    activeAcquisitionTimed_ =
        config.acquisitionMode == AcquisitionMode::Timed;

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
        acquisitionFailed_ = true;
        appendLog(tr("错误"), message);
        setSystemUiState(SystemUiState::ReceptionFailed);
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
        setSystemUiState(SystemUiState::Receiving);
        ui->systemStatusLabel->setStyleSheet(QStringLiteral("color:#30dd78;"));
        appendLog(tr("接收"),
                  activeAcquisitionTimed_ ? tr("定长接收已经启动")
                                          : tr("连续接收已经启动"));
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
    connect(rxWorker_, &RxWorker::metadataUpdated,
            this, [this](const QString& deviceTime, quint64 packets,
                         quint64 samples, quint64 overflows, quint64 timeouts,
                         const QString& status) {
        ui->metadataTableWidget->item(0, 1)->setText(
            QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
        ui->metadataTableWidget->item(1, 1)->setText(deviceTime);
        ui->metadataTableWidget->item(2, 1)->setText(
            QLocale().toString(static_cast<qulonglong>(packets)));
        ui->metadataTableWidget->item(3, 1)->setText(
            QLocale().toString(static_cast<qulonglong>(samples)));
        ui->metadataTableWidget->item(4, 1)->setText(status);
        ui->metadataTableWidget->item(5, 1)->setText(
            QLocale().toString(static_cast<qulonglong>(overflows)));
        ui->metadataTableWidget->item(6, 1)->setText(
            QLocale().toString(static_cast<qulonglong>(timeouts)));
    });
    connect(rxWorker_, &RxWorker::iqSaveCompleted,
            this, [this](const QString& path, quint64 samples, quint64 bytes) {
        iqSaveUiState_ = IqSaveUiState::Completed;
        iqSaveFileName_ = QFileInfo(path).fileName();
        iqSaveCompletedMiB_ = double(bytes) / (1024.0 * 1024.0);
        updateIqSaveStatus();
        appendLog(tr("保存"),
                  tr("IQ 文件写入完成：%1（%2 个 fc32 样本，%3 MiB）")
                      .arg(QDir::toNativeSeparators(path))
                      .arg(samples)
                      .arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 2));
        updateDiskCapacity();
    });
    // Capture a guarded worker pointer so the GUI can acknowledge the frame
    // without dereferencing a worker that has already been deleted.
    const QPointer<RxWorker> displayWorker(rxWorker_);
    connect(rxWorker_, &RxWorker::displayFrameReady,
            this, [this, displayWorker](const QVector<float>& i,
                                       const QVector<float>& current,
                                       const QVector<float>& average,
                                       const QVector<float>& maxHold,
                                       const QVector<float>& minHold,
                                       double rate, double frequency) {
        ++displayFrameCount_;
        waveformWidget_->setSamples(i, rate);
        spectrumWidget_->setSpectrum(current, average, maxHold, minHold,
                                     rate, frequency);
        waterfallWidget_->appendSpectrum(current, rate, frequency);
        if (displayWorker)
            displayWorker->acknowledgeDisplayFrame();
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
    ui->metadataTableWidget->item(0, 1)->setText(tr("等待首包"));
    ui->metadataTableWidget->item(1, 1)->setText(QStringLiteral("--"));
    for (int row = 2; row <= 6; ++row)
        ui->metadataTableWidget->item(row, 1)->setText(QStringLiteral("0"));
    ui->metadataTableWidget->item(7, 1)->setText(
        QString::number(static_cast<qulonglong>(config.channel)));
    ui->metadataTableWidget->item(8, 1)->setText(QStringLiteral("fc32 / sc16"));
    iqSaveUiState_ = config.saveIq
        ? IqSaveUiState::Writing
        : IqSaveUiState::NotSaving;
    iqSaveFileName_ = config.saveIq
        ? QFileInfo(config.iqFilePath).fileName()
        : QString();
    iqSaveCompletedMiB_ = 0.0;
    updateIqSaveStatus();
    setSystemUiState(SystemUiState::ConfiguringReception);
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
    appendLog(tr("采集"),
              activeAcquisitionTimed_
                  ? tr("模式：定长采集，时长 %1 s").arg(config.durationSeconds, 0, 'f', 3)
                  : tr("模式：连续采集"));
    if (config.saveIq)
        appendLog(tr("保存"), tr("IQ 数据将保存到：%1")
                  .arg(QDir::toNativeSeparators(config.iqFilePath)));
    rxThread_->start();
}

void MainWindow::stopReceiving()
{
    if (!acquisitionRunning_ || !rxWorker_) {
        return;
    }
    setSystemUiState(SystemUiState::StoppingReception);
    ui->systemStatusLabel->setStyleSheet(QStringLiteral("color:#ffba4a;"));
    rxWorker_->requestStop(); // 原子操作，可从 GUI 线程安全调用。
    stopRequestedByUser_ = true;
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
    const bool completed = activeAcquisitionTimed_ && !stopRequestedByUser_
        && !acquisitionFailed_;
    if (acquisitionFailed_) {
        setSystemUiState(SystemUiState::AcquisitionFailed);
        ui->systemStatusLabel->setStyleSheet(QStringLiteral("color:#ff5d68;"));
        appendLog(tr("接收"), tr("采集因错误停止，请检查上方错误日志"));
    } else {
        setSystemUiState(completed
            ? SystemUiState::TimedAcquisitionCompleted
            : SystemUiState::ReceptionStopped);
        ui->systemStatusLabel->setStyleSheet(
            completed ? QStringLiteral("color:#30dd78;")
                      : QStringLiteral("color:#ff5d68;"));
        appendLog(tr("接收"),
                  completed ? tr("定长采集已按设定长度完成")
                            : tr("接收已经安全停止"));
    }
    activeAcquisitionTimed_ = false;
    stopRequestedByUser_ = false;
    acquisitionFailed_ = false;
}

void MainWindow::findDevices()
{
    if (deviceOperationBusy_ || acquisitionRunning_) {
        return;
    }

    setDeviceOperationBusy(true);
    deviceListUiState_ = DeviceListUiState::Searching;
    updateDeviceListPlaceholder();
    setSystemUiState(SystemUiState::SearchingDevices);
    appendLog(tr("设备"), tr("开始查找 UHD 设备"));

    discoveryWatcher_->setFuture(QtConcurrent::run(
        [device = sdrDevice_.get()]() {
            return device->findDevices();
        }));
}

void MainWindow::connectDevice()
{
    const QString connectionArgs =
        ui->deviceSelectComboBox->currentData().toString().trimmed();
    const QString deviceOptions = ui->deviceArgsEdit->text().trimmed();

    if (deviceOperationBusy_ || acquisitionRunning_ || sdrDevice_->isConnected()) {
        return;
    }
    if (connectionArgs.isEmpty()) {
        QMessageBox::information(this, tr("尚未选择设备"),
                                 tr("请先点击“查找设备”，再从下拉框选择目标设备。"));
        return;
    }

    setDeviceOperationBusy(true);

    setConnectionUiState(ConnectionUiState::Connecting);
    ui->deviceConnectionStatusLabel->setStyleSheet(
        QStringLiteral("color: #ffba4a;"));

    appendLog(
        tr("设备"),
        connectionArgs.isEmpty()
            ? tr("开始连接 UHD 设备（未指定连接参数）")
            : tr("开始连接 UHD 设备，连接参数：%1").arg(connectionArgs));
    if (!deviceOptions.isEmpty())
        appendLog(tr("设备"), tr("UHD 初始化设备参数：%1").arg(deviceOptions));

    connectionWatcher_->setFuture(QtConcurrent::run(
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
    connectedInterfaceTypeSource_.clear();

    ui->deviceModelValueLabel->setText(tr("未知"));
    ui->connectionValueLabel->setText(tr("未连接"));
    ui->interfaceTypeValueLabel->setText(tr("--"));
    ui->fpgaVersionValueLabel->setText(tr("--"));

    setConnectionUiState(ConnectionUiState::Disconnected);
    ui->deviceConnectionStatusLabel->setStyleSheet(
        QStringLiteral("color: #ffba4a;"));

    setSystemUiState(SystemUiState::DeviceDisconnected);

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
    const bool deviceSelected =
        !ui->deviceSelectComboBox->currentData().toString().isEmpty();

    ui->findDevicesButton->setEnabled(idle && !connected);
    ui->connectDeviceButton->setEnabled(idle && !connected && deviceSelected);
    ui->disconnectDeviceButton->setEnabled(idle && connected);
    ui->actionFindDevices->setEnabled(idle && !connected);
    ui->actionConnectDevice->setEnabled(idle && !connected && deviceSelected);
    ui->actionDisconnectDevice->setEnabled(idle && connected);
    ui->deviceSelectComboBox->setEnabled(idle && !connected);
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
    const bool deterministicCapture =
        activeAcquisitionTimed_ || ui->saveIqCheckBox->isChecked();
    const QList<QWidget*> fixedDuringDeterministicCapture = {
        ui->centerFrequencySpinBox, ui->sampleRateSpinBox,
        ui->bandwidthSpinBox, ui->gainSpinBox, ui->autoBandwidthCheckBox
    };
    for (QWidget* widget : fixedDuringDeterministicCapture)
        widget->setEnabled(!(running && deterministicCapture));
    ui->continuousRadioButton->setEnabled(!running);
    ui->timedRadioButton->setEnabled(!running);
    ui->durationSpinBox->setEnabled(!running && ui->timedRadioButton->isChecked());
    ui->saveIqCheckBox->setEnabled(!running);
    ui->averageCountSpinBox->setEnabled(ui->averageCheckBox->isChecked());
    if (!(running && deterministicCapture))
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
    config.window = selectedWindow(ui->windowFunctionComboBox->currentIndex());
    config.averageEnabled = ui->averageCheckBox->isChecked();
    config.averageCount = ui->averageCountSpinBox->value();
    config.maxHoldEnabled = ui->maxHoldCheckBox->isChecked();
    config.minHoldEnabled = ui->minHoldCheckBox->isChecked();
    config.currentTraceVisible = ui->currentTraceCheckBox->isChecked();
    config.inputCompensationDb = ui->inputCompensationSpinBox->value();
    config.acquisitionMode = ui->timedRadioButton->isChecked()
        ? AcquisitionMode::Timed : AcquisitionMode::Continuous;
    config.durationSeconds = ui->durationSpinBox->value();
    config.saveIq = ui->saveIqCheckBox->isChecked();
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
    lastCpuUsage_ = cpu;
    lastDisplayFps_ = frames;
    updatePerformanceLabel();
    updateDiskCapacity();
}

void MainWindow::updatePerformanceLabel()
{
    performanceLabel_->setText(lastCpuUsage_ >= 0.0
        ? tr("CPU %1%  |  Display %2 FPS")
              .arg(lastCpuUsage_, 0, 'f', 1).arg(lastDisplayFps_)
        : tr("CPU N/A  |  Display %1 FPS").arg(lastDisplayFps_));
}

void MainWindow::setupMetadataView()
{
    QTableWidget* table = ui->metadataTableWidget;
    // 保留 .ui 创建的表头对象；语言切换时 retranslateUi() 仍会访问它们。
    table->clearContents();
    table->setRowCount(10);
    table->setColumnCount(2);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setDefaultSectionSize(27);
    table->setMinimumHeight(310);

    for (int row = 0; row < table->rowCount(); ++row) {
        auto* name = new QTableWidgetItem;
        name->setForeground(QColor(QStringLiteral("#91a4bf")));
        auto* value = new QTableWidgetItem(QStringLiteral("--"));
        value->setForeground(QColor(QStringLiteral("#e8eef7")));
        table->setItem(row, 0, name);
        table->setItem(row, 1, value);
    }
    retranslateMetadataView();
}

void MainWindow::retranslateMetadataView()
{
    QTableWidget* table = ui->metadataTableWidget;
    table->setHorizontalHeaderLabels({tr("项目"), tr("实时值")});
    const QStringList names = {
        tr("主机时间"), tr("设备时间"), tr("接收包数"), tr("样本总数"),
        tr("UHD 状态"), tr("溢出次数"), tr("超时次数"), tr("接收通道"),
        tr("数据格式"), tr("IQ 保存")
    };
    for (int row = 0; row < names.size() && row < table->rowCount(); ++row) {
        if (QTableWidgetItem* item = table->item(row, 0))
            item->setText(names[row]);
    }
}

void MainWindow::updateIqSaveStatus()
{
    QTableWidgetItem* item = ui->metadataTableWidget->item(9, 1);
    if (!item)
        return;

    switch (iqSaveUiState_) {
    case IqSaveUiState::NotSaving:
        item->setText(tr("未保存"));
        break;
    case IqSaveUiState::Writing:
        item->setText(tr("正在写入 %1").arg(iqSaveFileName_));
        break;
    case IqSaveUiState::Completed:
        item->setText(tr("完成 · %1 MiB").arg(iqSaveCompletedMiB_, 0, 'f', 2));
        break;
    }
}

void MainWindow::updateDiskCapacity()
{
    QStorageInfo storage(diskStatusPath_);
    storage.refresh();
    if (!storage.isValid() || !storage.isReady()) {
        diskCapacityLabel_->setText(tr("磁盘 --"));
        return;
    }
    const double bytes = static_cast<double>(storage.bytesAvailable());
    const QString available = bytes >= 1024.0 * 1024.0 * 1024.0
        ? tr("%1 GiB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1)
        : tr("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 0);
    diskCapacityLabel_->setText(tr("磁盘可用 %1").arg(available));
    diskCapacityLabel_->setToolTip(
        tr("当前保存位置：%1").arg(QDir::toNativeSeparators(diskStatusPath_)));
}

QString MainWindow::suggestedIqFileName(const RxConfig& config) const
{
    return QStringLiteral("IQ_F%1MHz_SR%2MSps_%3_fc32.bin")
        .arg(config.centerFrequencyHz / 1e6, 0, 'f', 6)
        .arg(config.sampleRate / 1e6, 0, 'f', 3)
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
}

void MainWindow::appendLog(const QString& category, const QString& message)
{
    const QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    ui->logPlainTextEdit->appendPlainText(
        QStringLiteral("[%1] [%2] %3").arg(timestamp, category, message));
}
