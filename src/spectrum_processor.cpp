#include "spectrum_processor.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace {
constexpr float kPi = 3.14159265358979323846f;

void fft(QVector<std::complex<float>>& data)
{
    const qsizetype count = data.size();
    for (qsizetype i = 1, j = 0; i < count; ++i) {
        qsizetype bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (qsizetype length = 2; length <= count; length <<= 1) {
        const float angle = -2.0f * kPi / float(length);
        const std::complex<float> root(std::cos(angle), std::sin(angle));
        for (qsizetype begin = 0; begin < count; begin += length) {
            std::complex<float> factor(1.0f, 0.0f);
            for (qsizetype offset = 0; offset < length / 2; ++offset) {
                const auto even = data[begin + offset];
                const auto odd = data[begin + offset + length / 2] * factor;
                data[begin + offset] = even + odd;
                data[begin + offset + length / 2] = even - odd;
                factor *= root;
            }
        }
    }
}

float windowCoefficient(WindowFunction type, qsizetype index, qsizetype count)
{
    if (count < 2) return 1.0f;
    const float phase = 2.0f * kPi * float(index) / float(count - 1);
    switch (type) {
    case WindowFunction::Hamming:
        return 0.54f - 0.46f * std::cos(phase);
    case WindowFunction::BlackmanHarris:
        return 0.35875f - 0.48829f * std::cos(phase)
            + 0.14128f * std::cos(2.0f * phase)
            - 0.01168f * std::cos(3.0f * phase);
    case WindowFunction::FlatTop:
        return 0.21557895f - 0.41663158f * std::cos(phase)
            + 0.277263158f * std::cos(2.0f * phase)
            - 0.083578947f * std::cos(3.0f * phase)
            + 0.006947368f * std::cos(4.0f * phase);
    case WindowFunction::Hann:
    default:
        return 0.5f - 0.5f * std::cos(phase);
    }
}
}

SpectrumFrame SpectrumProcessor::process(const QVector<float>& iSamples,
                                         const QVector<float>& qSamples,
                                         int requestedFftSize,
                                         WindowFunction window,
                                         bool averageEnabled,
                                         int averageCount,
                                         bool maxHoldEnabled)
{
    const qsizetype available = std::min(iSamples.size(), qSamples.size());
    const qsizetype fftSize = std::min<qsizetype>(available, requestedFftSize);
    if (fftSize < 2 || (fftSize & (fftSize - 1)) != 0) return {};

    QVector<std::complex<float>> data(fftSize);
    for (qsizetype i = 0; i < fftSize; ++i) {
        const float coefficient = windowCoefficient(window, i, fftSize);
        data[i] = {iSamples[i] * coefficient, qSamples[i] * coefficient};
    }
    fft(data);

    if (averagePower_.size() != fftSize) {
        averagePower_.fill(0.0f, fftSize);
        maxHoldDb_.fill(-200.0f, fftSize);
    }

    SpectrumFrame frame;
    frame.currentDb.resize(fftSize);
    if (averageEnabled) frame.averageDb.resize(fftSize);
    if (maxHoldEnabled) frame.maxHoldDb.resize(fftSize);
    // 把界面中的“平均次数”换成指数平均系数。N 越大，曲线越稳定。
    const float alpha = 2.0f / float(std::max(1, averageCount) + 1);
    for (qsizetype i = 0; i < fftSize; ++i) {
        const qsizetype shifted = (i + fftSize / 2) % fftSize;
        const float magnitude = std::abs(data[shifted]) / float(fftSize);
        const float power = std::max(magnitude * magnitude, 1.0e-20f);
        const float currentDb = 10.0f * std::log10(power);
        if (averageEnabled) {
            averagePower_[i] = averagePower_[i] == 0.0f
                ? power : alpha * power + (1.0f - alpha) * averagePower_[i];
        } else {
            averagePower_[i] = power;
        }
        maxHoldDb_[i] = maxHoldEnabled
            ? std::max(maxHoldDb_[i], currentDb) : currentDb;
        frame.currentDb[i] = currentDb;
        if (averageEnabled)
            frame.averageDb[i] = 10.0f * std::log10(averagePower_[i]);
        if (maxHoldEnabled)
            frame.maxHoldDb[i] = maxHoldDb_[i];
    }
    return frame;
}

void SpectrumProcessor::reset()
{
    averagePower_.clear();
    maxHoldDb_.clear();
}
