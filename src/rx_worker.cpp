#include "rx_receiver.hpp"

#include <uhd/stream.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/tune_request.hpp>

#include <algorithm>
#include <chrono>
#include <complex>
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

        uhd::stream_cmd_t startCommand(
            uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        startCommand.stream_now = true;
        streamer->issue_stream_cmd(startCommand);

        using Clock = std::chrono::steady_clock;
        const auto startTime = Clock::now();
        auto lastStatisticsTime = startTime;
        auto lastPreviewTime = startTime;
        quint64 totalSamples = 0;
        quint64 previousSamples = 0;
        bool overflowReported = false;
        unsigned appliedRevision = configRevision_.load();
        QVector<float> fftI;
        QVector<float> fftQ;
        fftI.reserve(config_.fftSize);
        fftQ.reserve(config_.fftSize);

        while (!stopRequested_.load()) {
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
            const std::size_t received =
                streamer->recv(buffer.data(), buffer.size(), metadata, 0.10, false);

            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                continue; // 短超时保证停止按钮能被及时响应。
            }
            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                if (!overflowReported) {
                    emit warningOccurred(tr("检测到接收溢出，部分 IQ 样本可能丢失"));
                    overflowReported = true;
                }
                continue;
            }
            if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
                throw std::runtime_error(metadata.strerror());
            }

            totalSamples += static_cast<quint64>(received);
            const auto now = Clock::now();

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
                previousSamples = totalSamples;
                lastStatisticsTime = now;
            }
        }

        uhd::stream_cmd_t stopCommand(
            uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        streamer->issue_stream_cmd(stopCommand);
        emit receptionStopped();
    } catch (const std::exception& exception) {
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
