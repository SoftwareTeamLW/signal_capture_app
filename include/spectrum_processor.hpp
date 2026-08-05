#pragma once

#include <QVector>
#include "rx_config.hpp"

struct SpectrumFrame
{
    QVector<float> currentDb;
    QVector<float> averageDb;
    QVector<float> maxHoldDb;
};

// 在接收线程中完成窗函数、FFT、平均和 Max Hold，GUI 只负责绘制。
class SpectrumProcessor
{
public:
    SpectrumFrame process(const QVector<float>& iSamples,
                          const QVector<float>& qSamples,
                          int fftSize,
                          WindowFunction window,
                          bool averageEnabled,
                          int averageCount,
                          bool maxHoldEnabled);
    void reset();

private:
    QVector<float> averagePower_;
    QVector<float> maxHoldDb_;
};
