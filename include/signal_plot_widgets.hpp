#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

#include <array>
#include <functional>

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
    enum class MarkerTrace {
        Current = 0,
        Average,
        MaxHold,
        MinHold
    };

    explicit SpectrumWidget(QWidget* parent = nullptr);
    void setReferenceLevel(float referenceLevelDb);
    void setCurrentTraceVisible(bool visible);
    void setActiveMarker(int marker);
    void setMarkerEnabled(int marker, bool enabled);
    void setMarkerFrequency(int marker, double frequencyHz);
    void setMarkerTracking(int marker, bool enabled);
    void setMarkerTrace(int marker, MarkerTrace trace);
    void peakSearch(int marker);
    void nextPeak(int marker);
    double markerFrequency(int marker) const;
    bool markerEnabled(int marker) const;
    bool markerTracking(int marker) const;
    MarkerTrace markerTrace(int marker) const;
    void setMarkerChangedCallback(std::function<void(int, double)> callback);
    void setSpectrum(const QVector<float>& currentDb,
                     const QVector<float>& averageDb,
                     const QVector<float>& maxHoldDb,
                     const QVector<float>& minHoldDb,
                     double sampleRate,
                     double centerFrequency);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QVector<float> currentDb_;
    QVector<float> averageDb_;
    QVector<float> maxHoldDb_;
    QVector<float> minHoldDb_;
    double sampleRate_ = 0.0;
    double centerFrequency_ = 0.0;
    float referenceLevelDb_ = 0.0f;
    bool currentTraceVisible_ = true;
    struct MarkerState {
        bool enabled = false;
        bool tracking = false;
        double frequencyHz = 0.0;
        int peakRank = 0;
        MarkerTrace trace = MarkerTrace::Current;
    };
    const QVector<float>& markerTraceValues(int marker) const;
    QVector<qsizetype> peakCandidates(int marker) const;
    qsizetype frequencyToIndex(double frequencyHz) const;
    double indexToFrequency(qsizetype index) const;
    void notifyMarkerChanged(int marker);
    std::array<MarkerState, 4> markers_{};
    int activeMarker_ = 0;
    std::function<void(int, double)> markerChangedCallback_;
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
