#include "rx_receiver.hpp"
#include "iq_file_writer.hpp"

#include <uhd/stream.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/tune_request.hpp>

#include <algorithm>
#include <chrono>
#include <complex>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

RxWorker::RxWorker(uhd::usrp::multi_usrp::sptr usrp, RxConfig config,
                   QObject* parent)
    : QObject(parent), usrp_(std::move(usrp)), config_(std::move(config)),
      pendingConfig_(config_)
{
}

void RxWorker::startReceiving()
{
    stopRequested_.store(false);
    spectrumProcessor_.reset();
    if (!usrp_) {
        emit errorOccurred(tr("没有可用的 UHD 设备对象"));
        emit receptionStopped();
        return;
    }

    std::unique_ptr<IqFileWriter> iqWriter;
    const auto finishIqWriter = [this, &iqWriter]() {
        if (!iqWriter) return;
        QString error;
        const bool success = iqWriter->close(error);
        const quint64 samples = iqWriter->samplesWritten();
        const quint64 bytes = iqWriter->bytesWritten();
        if (success) {
            emit iqSaveCompleted(config_.iqFilePath, samples, bytes);
        } else if (!error.isEmpty()) {
            emit errorOccurred(error);
        }
        iqWriter.reset();
    };

    try {
        const std::size_t channel = config_.channel;

        usrp_->set_rx_rate(config_.sampleRate, channel);
        usrp_->set_rx_freq(uhd::tune_request_t(config_.centerFrequencyHz), channel);
        usrp_->set_rx_bandwidth(config_.bandwidthHz, channel);
        usrp_->set_rx_gain(config_.gainDb, channel);

        const auto antennas = usrp_->get_rx_antennas(channel);
        const std::string requestedAntenna = config_.antenna.toStdString();
        if (std::find(antennas.begin(), antennas.end(), requestedAntenna) != antennas.end()) {
            usrp_->set_rx_antenna(requestedAntenna, channel);
        }

        double actualRate = usrp_->get_rx_rate(channel);
        double actualFrequency = usrp_->get_rx_freq(channel);
        double actualBandwidth = usrp_->get_rx_bandwidth(channel);
        if (config_.saveIq) {
            iqWriter = std::make_unique<IqFileWriter>();
            QString error;
            if (!iqWriter->open(config_.iqFilePath, error))
                throw std::runtime_error(error.toUtf8().constData());
        }
        emit configurationCompleted(
            tr("参数配置成功：通道 %1，采样率 %2 MS/s，中心频率 %3 MHz，"
               "带宽 %4 MHz，增益 %5 dB，天线 %6，FFT %7")
                .arg(channel)
                .arg(actualRate / 1e6, 0, 'f', 3)
                .arg(actualFrequency / 1e6, 0, 'f', 6)
                .arg(actualBandwidth / 1e6, 0, 'f', 3)
                .arg(usrp_->get_rx_gain(channel), 0, 'f', 1)
                .arg(QString::fromStdString(usrp_->get_rx_antenna(channel)))
                .arg(config_.fftSize));
        emit actualParameters(actualFrequency, actualRate, actualBandwidth);
        emit receptionStarted();

        // fc32 是主机内存格式；sc16 是设备传输格式。
        uhd::stream_args_t streamArgs("fc32", "sc16");
        streamArgs.channels = {channel};
        const auto streamer = usrp_->get_rx_stream(streamArgs);
        const std::size_t bufferSize =
            std::max<std::size_t>(4096, streamer->get_max_num_samps());
        std::vector<std::complex<float>> buffer(bufferSize);
        uhd::rx_metadata_t metadata;

        const bool timed = config_.acquisitionMode == AcquisitionMode::Timed;
        const quint64 targetSamples = timed
            ? std::max<quint64>(
                  1, static_cast<quint64>(
                         std::llround(config_.durationSeconds * actualRate)))
            : 0;
        uhd::stream_cmd_t startCommand(timed
            ? uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE
            : uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        if (timed) {
            startCommand.num_samps = static_cast<std::size_t>(std::min<quint64>(
                targetSamples, std::numeric_limits<std::size_t>::max()));
        }
        startCommand.stream_now = true;
        streamer->issue_stream_cmd(startCommand);

        using Clock = std::chrono::steady_clock;
        const auto startTime = Clock::now();
        auto lastStatisticsTime = startTime;
        auto lastPreviewTime = startTime;
        quint64 totalSamples = 0;
        quint64 previousSamples = 0;
        quint64 packetCount = 0;
        quint64 overflowCount = 0;
        quint64 timeoutCount = 0;
        bool overflowReported = false;
        unsigned appliedRevision = configRevision_.load();
        QVector<float> fftI;
        QVector<float> fftQ;
        fftI.reserve(config_.fftSize);
        fftQ.reserve(config_.fftSize);

        while (!stopRequested_.load() && (!timed || totalSamples < targetSamples)) {
            // 所有 UHD 调用都留在接收线程。GUI 只提交配置副本，避免跨线程
            // 同时访问 multi_usrp。FFT/窗函数变化会清空历史迹线。
            const unsigned requestedRevision = configRevision_.load();
            if (requestedRevision != appliedRevision) {
                RxConfig next;
                {
                    std::lock_guard<std::mutex> lock(configMutex_);
                    next = pendingConfig_;
                }
                const bool fftChanged = next.fftSize != config_.fftSize ||
                    next.window != config_.window;
                const bool holdModeChanged =
                    next.averageEnabled != config_.averageEnabled ||
                    next.maxHoldEnabled != config_.maxHoldEnabled ||
                    next.minHoldEnabled != config_.minHoldEnabled ||
                    next.inputCompensationDb != config_.inputCompensationDb;
                if (next.sampleRate != config_.sampleRate) {
                    usrp_->set_rx_rate(next.sampleRate, channel);
                    actualRate = usrp_->get_rx_rate(channel);
                }
                if (next.centerFrequencyHz != config_.centerFrequencyHz) {
                    usrp_->set_rx_freq(
                        uhd::tune_request_t(next.centerFrequencyHz), channel);
                    actualFrequency = usrp_->get_rx_freq(channel);
                }
                if (next.bandwidthHz != config_.bandwidthHz) {
                    usrp_->set_rx_bandwidth(next.bandwidthHz, channel);
                    actualBandwidth = usrp_->get_rx_bandwidth(channel);
                }
                if (next.gainDb != config_.gainDb)
                    usrp_->set_rx_gain(next.gainDb, channel);
                // 任务属性启动后不可改变，运行时请求只更新射频/频谱参数。
                next.acquisitionMode = config_.acquisitionMode;
                next.durationSeconds = config_.durationSeconds;
                next.saveIq = config_.saveIq;
                next.iqFilePath = config_.iqFilePath;
                config_ = next;
                if (fftChanged || holdModeChanged) {
                    spectrumProcessor_.reset();
                    fftI.clear();
                    fftQ.clear();
                }
                fftI.reserve(config_.fftSize);
                fftQ.reserve(config_.fftSize);
                appliedRevision = requestedRevision;
                emit actualParameters(actualFrequency, actualRate, actualBandwidth);
            }
            const std::size_t requestedSamples = timed
                ? static_cast<std::size_t>(std::min<quint64>(
                      buffer.size(), targetSamples - totalSamples))
                : buffer.size();
            const std::size_t received = streamer->recv(
                buffer.data(), requestedSamples, metadata, 0.10, false);

            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                ++timeoutCount;
                continue; // 短超时保证停止按钮能被及时响应。
            }
            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                ++overflowCount;
                if (!overflowReported) {
                    emit warningOccurred(tr("检测到接收溢出，部分 IQ 样本可能丢失"));
                    overflowReported = true;
                }
                continue;
            }
            if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
                throw std::runtime_error(metadata.strerror());
            }

            ++packetCount;
            totalSamples += static_cast<quint64>(received);
            const auto now = Clock::now();

            if (iqWriter && received > 0) {
                QString error;
                if (!iqWriter->enqueue(buffer.data(), received, error))
                    throw std::runtime_error(error.toUtf8().constData());
            }

            // 目标约 50 FPS。绘图层还会按像素宽度压缩点数，避免大 FFT
            // 让 GUI 绘制数万条肉眼不可分辨的线段。
            if (received > 0 && now - lastPreviewTime >= std::chrono::milliseconds(20)) {
                // UHD 单次 recv() 可能少于 FFT 点数，因此跨接收包凑齐一帧。
                for (std::size_t index = 0;
                     index < received && fftI.size() < config_.fftSize; ++index) {
                    fftI.push_back(buffer[index].real());
                    fftQ.push_back(buffer[index].imag());
                }
                if (fftI.size() == config_.fftSize) {
                    const SpectrumFrame frame = spectrumProcessor_.process(
                        fftI, fftQ, config_.fftSize, config_.window,
                        config_.averageEnabled, config_.averageCount,
                        config_.maxHoldEnabled, config_.minHoldEnabled,
                        static_cast<float>(config_.inputCompensationDb));
                    emit displayFrameReady(fftI, frame.currentDb, frame.averageDb,
                                           frame.maxHoldDb, frame.minHoldDb,
                                           actualRate, actualFrequency);
                    fftI.clear();
                    fftQ.clear();
                    lastPreviewTime = now;
                }
            }

            if (now - lastStatisticsTime >= std::chrono::milliseconds(250)) {
                const double seconds =
                    std::chrono::duration<double>(now - lastStatisticsTime).count();
                const double rate =
                    static_cast<double>(totalSamples - previousSamples) / seconds;
                const qint64 elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - startTime).count();
                emit statisticsUpdated(totalSamples, rate, elapsed);
                const QString deviceTime = metadata.has_time_spec
                    ? tr("%1 s").arg(metadata.time_spec.get_real_secs(), 0, 'f', 6)
                    : QStringLiteral("--");
                const QString status = overflowCount > 0
                    ? tr("警告：发生溢出")
                    : (timeoutCount > 0 ? tr("运行中（有超时）") : tr("正常"));
                emit metadataUpdated(deviceTime, packetCount, totalSamples,
                                     overflowCount, timeoutCount, status);
                previousSamples = totalSamples;
                lastStatisticsTime = now;
            }
        }

        const auto finishTime = Clock::now();
        const qint64 finalElapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                finishTime - startTime).count();
        const double finalSeconds = std::max(
            1e-9, std::chrono::duration<double>(finishTime - startTime).count());
        emit statisticsUpdated(totalSamples,
                               static_cast<double>(totalSamples) / finalSeconds,
                               finalElapsed);
        const QString finalDeviceTime = totalSamples > 0 && metadata.has_time_spec
            ? tr("%1 s").arg(metadata.time_spec.get_real_secs(), 0, 'f', 6)
            : QStringLiteral("--");
        const QString finalStatus = overflowCount > 0
            ? tr("警告：发生溢出")
            : (timeoutCount > 0 ? tr("完成（有超时）") : tr("正常"));
        emit metadataUpdated(finalDeviceTime, packetCount, totalSamples,
                             overflowCount, timeoutCount, finalStatus);

        if (!timed || stopRequested_.load()) {
            uhd::stream_cmd_t stopCommand(
                uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
            streamer->issue_stream_cmd(stopCommand);
        }
        finishIqWriter();
        emit receptionStopped();
    } catch (const std::exception& exception) {
        finishIqWriter();
        emit errorOccurred(
            tr("接收失败：%1").arg(QString::fromUtf8(exception.what())));
        emit receptionStopped();
    }
}

void RxWorker::requestStop()
{
    stopRequested_.store(true);
}

void RxWorker::requestRuntimeConfig(const RxConfig& config)
{
    std::lock_guard<std::mutex> lock(configMutex_);
    pendingConfig_ = config;
    configRevision_.fetch_add(1);
}
