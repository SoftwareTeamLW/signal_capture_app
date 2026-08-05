#pragma once

#include <QString>
#include <cstddef>

// 一次接收任务使用的完整配置。GUI 在启动线程前生成它，Worker 只读取副本，
// 因而不会出现工作线程读取到一半已被界面修改的参数。
enum class WindowFunction { Hann, Hamming, BlackmanHarris, FlatTop };

struct RxConfig
{
    std::size_t channel = 0;
    QString antenna = QStringLiteral("RX2");
    double centerFrequencyHz = 100e6;
    double sampleRate = 5e6;
    double bandwidthHz = 5e6;
    double gainDb = 20.0;
    int fftSize = 1024;
    WindowFunction window = WindowFunction::Hann;
    bool averageEnabled = false;
    int averageCount = 10;
    bool maxHoldEnabled = false;
    bool minHoldEnabled = false;
    bool currentTraceVisible = true;
    double inputCompensationDb = 0.0;
};
