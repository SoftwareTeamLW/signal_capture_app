#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <mutex>
#include <uhd/usrp/multi_usrp.hpp>

#include "spectrum_processor.hpp"
#include "rx_config.hpp"

class RxWorker final : public QObject
{
    Q_OBJECT

public:
    explicit RxWorker(uhd::usrp::multi_usrp::sptr usrp,
                      RxConfig config,
                      QObject* parent = nullptr);
    void requestStop();
    // GUI 可直接调用：只在互斥锁保护下保存请求，UHD 参数仍由接收线程应用。
    void requestRuntimeConfig(const RxConfig& config);

public slots:
    void startReceiving();

signals:
    void receptionStarted();
    void receptionStopped();
    void errorOccurred(const QString& message);
    void warningOccurred(const QString& message);
    void configurationCompleted(const QString& message);
    void actualParameters(double centerFrequency,
                          double sampleRate,
                          double bandwidth);
    void statisticsUpdated(quint64 totalSamples,
                           double samplesPerSecond,
                           qint64 elapsedMilliseconds);
    void metadataUpdated(const QString& deviceTime,
                         quint64 packetCount,
                         quint64 totalSamples,
                         quint64 overflowCount,
                         quint64 timeoutCount,
                         const QString& status);
    void iqSaveCompleted(const QString& filePath,
                         quint64 samplesWritten,
                         quint64 bytesWritten);
    void displayFrameReady(const QVector<float>& iSamples,
                           const QVector<float>& currentDb,
                           const QVector<float>& averageDb,
                           const QVector<float>& maxHoldDb,
                           const QVector<float>& minHoldDb,
                           double sampleRate,
                           double centerFrequency);

private:
    std::atomic_bool stopRequested_{false};
    uhd::usrp::multi_usrp::sptr usrp_;
    RxConfig config_;
    RxConfig pendingConfig_;
    std::mutex configMutex_;
    std::atomic_uint configRevision_{0};
    SpectrumProcessor spectrumProcessor_;
};
