#include "signal_plot_widgets.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace {
const QColor kBackground(3, 3, 3);
const QColor kPlotBackground(0, 0, 0);
const QColor kMajorGrid(72, 72, 72, 180);
const QColor kMinorGrid(35, 35, 35, 150);
const QColor kText(205, 205, 205);
const QColor kCurrent(255, 222, 45);       // 主流频谱仪风格：实时迹线为黄色
const QColor kAverage(55, 205, 255, 210);  // 平均：青蓝色
const QColor kMaxHold(255, 72, 72, 220);   // Max Hold：红色
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

void SpectrumWidget::setSpectrum(const QVector<float>& currentDb,
                                 const QVector<float>& averageDb,
                                 const QVector<float>& maxHoldDb,
                                 double sampleRate, double centerFrequency)
{
    currentDb_ = currentDb;
    averageDb_ = averageDb;
    maxHoldDb_ = maxHoldDb;
    sampleRate_ = sampleRate;
    centerFrequency_ = centerFrequency;
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
    if (currentTraceVisible_) {
        const auto peak = std::max_element(currentDb_.cbegin(), currentDb_.cend());
        const qsizetype peakIndex = std::distance(currentDb_.cbegin(), peak);
        const qreal peakX = area.left() + peakIndex * area.width() /
            qreal(currentDb_.size() - 1);
        const qreal peakY = dbToY(*peak, area, float(top));
        const double peakHz = centerFrequency_ - sampleRate_ / 2.0 +
            peakIndex * sampleRate_ / double(currentDb_.size() - 1);
        painter.setBrush(kCurrent);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(peakX, peakY), 2.5, 2.5);
        painter.setPen(kText);
        painter.drawText(area.right() - 210, 20,
            tr("PEAK %1 MHz  %2 dBFS")
                .arg(peakHz / 1e6, 0, 'f', 6)
                .arg(*peak, 0, 'f', 1));
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
