#include "signal_plot_widgets.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
const QColor kBackground(3, 3, 3);
const QColor kPlotBackground(0, 0, 0);
const QColor kMajorGrid(72, 72, 72, 180);
const QColor kMinorGrid(35, 35, 35, 150);
const QColor kText(205, 205, 205);
const QColor kCurrent(255, 222, 45);       // 主流频谱仪风格：实时迹线为黄色
const QColor kAverage(55, 205, 255, 210);  // 平均：青蓝色
const QColor kMaxHold(255, 72, 72, 220);   // Max Hold：红色
const QColor kMinHold(184, 105, 255, 220); // Min Hold：紫色
const QColor kMarker(255, 255, 255);
constexpr float kTopDb = 0.0f;
constexpr float kBottomDb = -140.0f;

QRect plotRect(const QWidget& widget)
{
    return widget.rect().adjusted(54, 30, -12, -28);
}

void drawInstrumentFrame(QPainter& painter, const QWidget& widget,
                         const QString& title)
{
    painter.fillRect(widget.rect(), kBackground);
    const QRect area = plotRect(widget);
    painter.fillRect(area, kPlotBackground);

    painter.setRenderHint(QPainter::Antialiasing, false);
    for (int i = 1; i < 20; ++i) {
        painter.setPen((i % 2 == 0) ? kMajorGrid : kMinorGrid);
        const int x = area.left() + i * area.width() / 20;
        painter.drawLine(x, area.top(), x, area.bottom());
    }
    for (int i = 1; i < 16; ++i) {
        painter.setPen((i % 2 == 0) ? kMajorGrid : kMinorGrid);
        const int y = area.top() + i * area.height() / 16;
        painter.drawLine(area.left(), y, area.right(), y);
    }
    painter.setPen(QPen(QColor(74, 101, 121), 1));
    painter.drawRect(area);
    painter.setPen(kText);
    painter.drawText(12, 20, title);
}

qreal dbToY(float value, const QRect& area, float topDb = kTopDb)
{
    const float bottomDb = topDb - (kTopDb - kBottomDb);
    const float normalized = std::clamp(
        (value - bottomDb) / (topDb - bottomDb), 0.0f, 1.0f);
    return area.bottom() - normalized * area.height();
}

QPainterPath tracePath(const QVector<float>& values, const QRect& area,
                       float topDb = kTopDb)
{
    QPainterPath path;
    if (values.size() < 2) return path;
    // 每个像素列保留最大值：降低绘制负担，同时避免漏掉窄带峰值。
    const int columns = std::max(2, std::min<int>(area.width(), values.size()));
    for (int column = 0; column < columns; ++column) {
        const qsizetype begin = qsizetype(column) * values.size() / columns;
        const qsizetype end = std::max<qsizetype>(
            begin + 1, qsizetype(column + 1) * values.size() / columns);
        float peak = values[begin];
        for (qsizetype i = begin + 1;
             i < std::min<qsizetype>(end, values.size()); ++i)
            peak = std::max(peak, values[i]);
        const qreal x = area.left() + column * area.width() / qreal(columns - 1);
        const qreal y = dbToY(peak, area, topDb);
        column == 0 ? path.moveTo(x, y) : path.lineTo(x, y);
    }
    return path;
}

QRgb waterfallColor(float db, float minimumDb, float maximumDb)
{
    const float span = std::max(1.0f, maximumDb - minimumDb);
    const float t = std::clamp((db - minimumDb) / span, 0.0f, 1.0f);
    // 深蓝 → 青 → 黄 → 红，低电平保持暗色，细节比 HSV 彩虹更清楚。
    QColor color;
    if (t < 0.33f) color = QColor::fromRgbF(0.02, 0.08 + t, 0.18 + 1.8 * t);
    else if (t < 0.68f) color = QColor::fromRgbF(0.02, 0.25 + t, 1.0 - t);
    else color = QColor::fromRgbF(
        1.0, 1.0 - 0.9 * (t - 0.68) / 0.32, 0.05);
    return color.rgb();
}
}

WaveformWidget::WaveformWidget(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void WaveformWidget::setSamples(const QVector<float>& samples, double sampleRate)
{
    samples_ = samples;
    sampleRate_ = sampleRate;
    update();
}

void WaveformWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    drawInstrumentFrame(painter, *this, tr("TIME DOMAIN  ·  I CHANNEL"));
    const QRect area = plotRect(*this);
    if (samples_.size() < 2 || area.isEmpty()) return;

    // 射频基带样本经常只有几 mV 对应的归一化幅度。自动量程避免按固定 ±1
    // 绘制时看起来像“没有波形”。量程下限防止纯噪声被无限放大。
    float peak = 0.0f;
    for (float sample : samples_) peak = std::max(peak, std::abs(sample));
    const float fullScale = std::max(peak * 1.10f, 1.0e-4f);
    QPainterPath path;
    for (qsizetype i = 0; i < samples_.size(); ++i) {
        const qreal x = area.left() + i * area.width() / qreal(samples_.size() - 1);
        const qreal y = area.center().y() -
            std::clamp(samples_[i] / fullScale, -1.0f, 1.0f) * area.height() * 0.46;
        i == 0 ? path.moveTo(x, y) : path.lineTo(x, y);
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(kCurrent, 0.8));
    painter.drawPath(path);
    painter.setPen(kText);
    const double durationUs = sampleRate_ > 0.0
        ? samples_.size() / sampleRate_ * 1e6 : 0.0;
    painter.drawText(area.left(), height() - 8,
                     tr("0 µs"));
    painter.drawText(area.right() - 90, height() - 8,
                     tr("%1 µs").arg(durationUs, 0, 'f', 1));
    painter.drawText(area.right() - 170, 20,
                     tr("AUTO ±%1").arg(fullScale, 0, 'g', 3));
}

SpectrumWidget::SpectrumWidget(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void SpectrumWidget::setReferenceLevel(float referenceLevelDb)
{
    referenceLevelDb_ = referenceLevelDb;
    update();
}

void SpectrumWidget::setCurrentTraceVisible(bool visible)
{
    currentTraceVisible_ = visible;
    update();
}

void SpectrumWidget::setActiveMarker(int marker)
{
    activeMarker_ = std::clamp(marker, 0, 3);
    update();
}

void SpectrumWidget::setMarkerEnabled(int marker, bool enabled)
{
    if (marker < 0 || marker >= int(markers_.size())) return;
    markers_[marker].enabled = enabled;
    if (enabled && markers_[marker].frequencyHz == 0.0) peakSearch(marker);
    update();
}

void SpectrumWidget::setMarkerFrequency(int marker, double frequencyHz)
{
    if (marker < 0 || marker >= int(markers_.size())) return;
    markers_[marker].frequencyHz = frequencyHz;
    markers_[marker].peakRank = 0;
    notifyMarkerChanged(marker);
    update();
}

void SpectrumWidget::setMarkerTracking(int marker, bool enabled)
{
    if (marker < 0 || marker >= int(markers_.size())) return;
    markers_[marker].tracking = enabled;
}

void SpectrumWidget::setMarkerTrace(int marker, MarkerTrace trace)
{
    if (marker < 0 || marker >= int(markers_.size())) return;
    markers_[marker].trace = trace;
    markers_[marker].peakRank = 0;
    update();
}

const QVector<float>& SpectrumWidget::markerTraceValues(int marker) const
{
    static const QVector<float> empty;
    if (marker < 0 || marker >= int(markers_.size())) return empty;
    switch (markers_[marker].trace) {
    case MarkerTrace::Current: return currentDb_;
    case MarkerTrace::Average: return averageDb_;
    case MarkerTrace::MaxHold: return maxHoldDb_;
    case MarkerTrace::MinHold: return minHoldDb_;
    }
    return empty;
}

QVector<qsizetype> SpectrumWidget::peakCandidates(int marker) const
{
    const QVector<float>& values = markerTraceValues(marker);
    QVector<qsizetype> result;
    for (qsizetype i = 1; i + 1 < values.size(); ++i)
        if (values[i] >= values[i - 1] && values[i] >= values[i + 1])
            result.push_back(i);
    std::sort(result.begin(), result.end(), [&values](qsizetype a, qsizetype b) {
        return values[a] > values[b];
    });
    QVector<qsizetype> separated;
    const qsizetype guard = std::max<qsizetype>(2, values.size() / 100);
    for (qsizetype candidate : result) {
        bool usable = true;
        for (qsizetype used : separated)
            if (std::abs(candidate - used) < guard) usable = false;
        if (usable) separated.push_back(candidate);
        if (separated.size() >= 32) break;
    }
    return separated;
}

qsizetype SpectrumWidget::frequencyToIndex(double frequencyHz) const
{
    if (currentDb_.size() < 2 || sampleRate_ <= 0.0) return -1;
    const double start = centerFrequency_ - sampleRate_ / 2.0;
    return std::clamp<qsizetype>(
        qsizetype(std::llround((frequencyHz - start) / sampleRate_ * (currentDb_.size() - 1))),
        0, currentDb_.size() - 1);
}

double SpectrumWidget::indexToFrequency(qsizetype index) const
{
    if (currentDb_.size() < 2) return centerFrequency_;
    return centerFrequency_ - sampleRate_ / 2.0 +
        index * sampleRate_ / double(currentDb_.size() - 1);
}

void SpectrumWidget::notifyMarkerChanged(int marker)
{
    if (markerChangedCallback_) markerChangedCallback_(marker, markers_[marker].frequencyHz);
}

void SpectrumWidget::setMarkerChangedCallback(std::function<void(int, double)> callback)
{
    markerChangedCallback_ = std::move(callback);
}

double SpectrumWidget::markerFrequency(int marker) const
{
    return marker >= 0 && marker < int(markers_.size()) ? markers_[marker].frequencyHz : 0.0;
}

bool SpectrumWidget::markerEnabled(int marker) const
{
    return marker >= 0 && marker < int(markers_.size()) && markers_[marker].enabled;
}

bool SpectrumWidget::markerTracking(int marker) const
{
    return marker >= 0 && marker < int(markers_.size()) && markers_[marker].tracking;
}

SpectrumWidget::MarkerTrace SpectrumWidget::markerTrace(int marker) const
{
    return marker >= 0 && marker < int(markers_.size())
        ? markers_[marker].trace : MarkerTrace::Current;
}

void SpectrumWidget::peakSearch(int marker)
{
    if (marker < 0 || marker >= int(markers_.size())) return;
    const auto candidates = peakCandidates(marker);
    if (candidates.isEmpty()) return;
    markers_[marker].peakRank = 0;
    markers_[marker].frequencyHz = indexToFrequency(candidates.front());
    notifyMarkerChanged(marker);
    update();
}

void SpectrumWidget::nextPeak(int marker)
{
    if (marker < 0 || marker >= int(markers_.size())) return;
    const auto candidates = peakCandidates(marker);
    if (candidates.isEmpty()) return;
    markers_[marker].peakRank = (markers_[marker].peakRank + 1) % candidates.size();
    markers_[marker].frequencyHz = indexToFrequency(candidates[markers_[marker].peakRank]);
    notifyMarkerChanged(marker);
    update();
}

void SpectrumWidget::setSpectrum(const QVector<float>& currentDb,
                                 const QVector<float>& averageDb,
                                 const QVector<float>& maxHoldDb,
                                 const QVector<float>& minHoldDb,
                                 double sampleRate, double centerFrequency)
{
    currentDb_ = currentDb;
    averageDb_ = averageDb;
    maxHoldDb_ = maxHoldDb;
    minHoldDb_ = minHoldDb;
    sampleRate_ = sampleRate;
    centerFrequency_ = centerFrequency;
    for (int marker = 0; currentDb_.size() >= 3 && marker < int(markers_.size()); ++marker)
        if (markers_[marker].enabled && markers_[marker].frequencyHz == 0.0)
            peakSearch(marker);
    // 跟踪只在 Marker 当前点附近约 2% FFT 范围内寻找最高点，避免跨频谱跳跃。
    for (int marker = 0; currentDb_.size() >= 3 && marker < int(markers_.size()); ++marker) {
        auto& state = markers_[marker];
        if (!state.enabled || !state.tracking || state.frequencyHz == 0.0) continue;
        const QVector<float>& values = markerTraceValues(marker);
        if (values.size() < 3) continue;
        const double start = centerFrequency_ - sampleRate_ / 2.0;
        const double stop = centerFrequency_ + sampleRate_ / 2.0;
        if (state.frequencyHz < start || state.frequencyHz > stop) continue;
        const qsizetype center = frequencyToIndex(state.frequencyHz);
        const qsizetype radius = std::max<qsizetype>(3, values.size() / 50);
        const qsizetype begin = std::max<qsizetype>(1, center - radius);
        const qsizetype end = std::min<qsizetype>(values.size() - 2, center + radius);
        qsizetype best = begin;
        for (qsizetype i = begin + 1; i <= end; ++i)
            if (values[i] > values[best]) best = i;
        const double trackedFrequency = indexToFrequency(best);
        if (trackedFrequency != state.frequencyHz) {
            state.frequencyHz = trackedFrequency;
            notifyMarkerChanged(marker);
        }
    }
    update();
}

void SpectrumWidget::mousePressEvent(QMouseEvent* event)
{
    const QRect area = plotRect(*this);
    if (!area.contains(event->position().toPoint()) || currentDb_.size() < 2) return;
    auto& state = markers_[activeMarker_];
    state.enabled = true;
    state.tracking = false;
    const double ratio = std::clamp((event->position().x() - area.left()) / area.width(), 0.0, 1.0);
    state.frequencyHz = centerFrequency_ - sampleRate_ / 2.0 + ratio * sampleRate_;
    state.peakRank = 0;
    notifyMarkerChanged(activeMarker_);
    update();
}

void SpectrumWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    drawInstrumentFrame(painter, *this, tr("SPECTRUM  ·  REAL-TIME"));
    const QRect area = plotRect(*this);
    if (currentDb_.size() < 2 || area.isEmpty()) return;

    painter.setPen(kText);
    const int top = static_cast<int>(std::round(referenceLevelDb_ / 20.0f)) * 20;
    for (int db = top; db >= top - 140; db -= 20) {
        painter.drawText(5, qRound(dbToY(float(db), area, float(top))) + 4,
                         QString::number(db));
    }

    painter.setRenderHint(QPainter::Antialiasing);
    if (!maxHoldDb_.isEmpty()) {
        painter.setPen(QPen(kMaxHold, 0.8));
        painter.drawPath(tracePath(maxHoldDb_, area, float(top)));
    }
    if (!minHoldDb_.isEmpty()) {
        painter.setPen(QPen(kMinHold, 0.8));
        painter.drawPath(tracePath(minHoldDb_, area, float(top)));
    }
    if (!averageDb_.isEmpty()) {
        painter.setPen(QPen(kAverage, 0.8));
        painter.drawPath(tracePath(averageDb_, area, float(top)));
    }

    const QPainterPath currentPath = tracePath(currentDb_, area, float(top));
    if (currentTraceVisible_) {
        painter.setPen(QPen(kCurrent, 0.85));
        painter.drawPath(currentPath);
    }

    painter.setPen(kText);
    painter.drawText(area.left(), height() - 8,
        tr("Center %1 MHz   Span %2 MHz   FFT %3")
            .arg(centerFrequency_ / 1e6, 0, 'f', 3)
            .arg(sampleRate_ / 1e6, 0, 'f', 3)
            .arg(currentDb_.size()));
    for (int marker = 0; marker < int(markers_.size()); ++marker) {
        const auto& state = markers_[marker];
        const double start = centerFrequency_ - sampleRate_ / 2.0;
        const double stop = centerFrequency_ + sampleRate_ / 2.0;
        if (!state.enabled || state.frequencyHz < start || state.frequencyHz > stop) continue;
        const QVector<float>& values = markerTraceValues(marker);
        if (values.size() != currentDb_.size()) continue;
        const qsizetype peakIndex = frequencyToIndex(state.frequencyHz);
        const float peakValue = values[peakIndex];
        const qreal peakX = area.left() + peakIndex * area.width() /
            qreal(currentDb_.size() - 1);
        const qreal peakY = dbToY(peakValue, area, float(top));
        const double peakHz = centerFrequency_ - sampleRate_ / 2.0 +
            peakIndex * sampleRate_ / double(currentDb_.size() - 1);
        painter.setBrush(marker == activeMarker_ ? kCurrent : kMarker);
        painter.setPen(marker == activeMarker_ ? kCurrent : kMarker);
        painter.drawLine(QPointF(peakX, area.top()), QPointF(peakX, area.bottom()));
        painter.drawEllipse(QPointF(peakX, peakY), 2.5, 2.5);
        painter.drawText(QPointF(peakX + 4, peakY - 4), tr("M%1").arg(marker + 1));
        painter.drawText(area.right() - 245, 20 + marker * 15,
            tr("M%1  %2 MHz  %3 dBFS")
                .arg(marker + 1)
                .arg(peakHz / 1e6, 0, 'f', 6)
                .arg(peakValue, 0, 'f', 1));
    }
}

WaterfallWidget::WaterfallWidget(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void WaterfallWidget::recreateImage()
{
    const QRect area = plotRect(*this);
    if (area.width() <= 0 || area.height() <= 0) return;
    image_ = QImage(area.size(), QImage::Format_RGB32);
    image_.fill(kPlotBackground);
    writeRow_ = 0;
}

void WaterfallWidget::resizeEvent(QResizeEvent*) { recreateImage(); }

void WaterfallWidget::appendSpectrum(const QVector<float>& values)
{
    if (values.isEmpty()) return;
    if (image_.isNull()) recreateImage();
    if (image_.isNull()) return;

    // 指针向上循环，保证从 writeRow_ 开始读时第一行永远是最新数据。
    writeRow_ = (writeRow_ - 1 + image_.height()) % image_.height();
    QRgb* line = reinterpret_cast<QRgb*>(image_.scanLine(writeRow_));
    for (int x = 0; x < image_.width(); ++x) {
        const qsizetype index = std::min<qsizetype>(values.size() - 1,
            qsizetype(double(x) * values.size() / image_.width()));
        line[x] = waterfallColor(values[index], minimumDb_, maximumDb_);
    }
    update();
}

void WaterfallWidget::setColorRange(float minimumDb, float maximumDb)
{
    if (minimumDb >= maximumDb) return;
    minimumDb_ = minimumDb;
    maximumDb_ = maximumDb;
}

void WaterfallWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    drawInstrumentFrame(painter, *this, tr("WATERFALL  ·  NEWEST AT TOP"));
    if (image_.isNull()) return;
    const QRect area = plotRect(*this);
    // 环形图的最新一行显示在顶部，只做两次图像块绘制，不复制像素。
    const int tail = image_.height() - writeRow_;
    painter.drawImage(QRect(area.left(), area.top(), area.width(), tail), image_,
                      QRect(0, writeRow_, image_.width(), tail));
    if (writeRow_ > 0) {
        painter.drawImage(QRect(area.left(), area.top() + tail,
                                area.width(), writeRow_), image_,
                          QRect(0, 0, image_.width(), writeRow_));
    }
}
