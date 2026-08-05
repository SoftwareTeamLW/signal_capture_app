#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

// 三个控件只负责显示已经处理好的数据。FFT 不在 GUI 线程执行。
class WaveformWidget final : public QWidget
{
public:
    explicit WaveformWidget(QWidget* parent = nullptr);
    void setSamples(const QVector<float>& samples, double sampleRate);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<float> samples_;
    double sampleRate_ = 0.0;
};

class SpectrumWidget final : public QWidget
{
public:
    explicit SpectrumWidget(QWidget* parent = nullptr);
    void setReferenceLevel(float referenceLevelDb);
    void setCurrentTraceVisible(bool visible);
    void setSpectrum(const QVector<float>& currentDb,
                     const QVector<float>& averageDb,
                     const QVector<float>& maxHoldDb,
                     double sampleRate,
                     double centerFrequency);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<float> currentDb_;
    QVector<float> averageDb_;
    QVector<float> maxHoldDb_;
    double sampleRate_ = 0.0;
    double centerFrequency_ = 0.0;
    float referenceLevelDb_ = 0.0f;
    bool currentTraceVisible_ = true;
};

class WaterfallWidget final : public QWidget
{
public:
    explicit WaterfallWidget(QWidget* parent = nullptr);
    void appendSpectrum(const QVector<float>& spectrumDb);
    void setColorRange(float minimumDb, float maximumDb);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void recreateImage();
    QImage image_;
    int writeRow_ = 0; // 环形行指针：避免每帧复制整张图片。
    float minimumDb_ = -125.0f;
    float maximumDb_ = -20.0f;
};
