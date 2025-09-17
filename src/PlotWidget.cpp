#include "PlotWidget.hpp"
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QPainterPath>   // 修复：需要显式包含
#include <algorithm>

PlotWidget::PlotWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumHeight(240);
    fpsTimer_.start();
}

void PlotWidget::initializeGL() {
    initializeOpenGLFunctions();
}

// 修复：签名与 .hpp 一致，使用 quint64
void PlotWidget::onFrameAdvanced(quint64 widx) {
    // 简单的 FPS 估计（指数平滑）
    double dt = fpsTimer_.elapsed() / 1000.0;
    if (dt >= 0.3) {
        quint64 delta = (widx > lastFrameCount_) ? (widx - lastFrameCount_) : 0;
        double inst = delta / std::max(1e-6, dt);
        measuredFps_ = 0.9 * measuredFps_ + 0.1 * inst;
        lastFrameCount_ = widx;
        fpsTimer_.restart();
    }
    widx_ = widx;
    update();
}

void PlotWidget::paintGL() {
    QPainter p(this);
    p.fillRect(rect(), QColor(26,26,30));

    if (!ring_ || widx_ == 0) {
        p.setPen(Qt::white);
        p.drawText(rect(), Qt::AlignCenter, "waiting...");
        return;
    }

    const double fps = useMeasuredFps_ ? std::max(1.0, measuredFps_) : 20000.0;
    auto env = build_envelope(*ring_, widx_, ch_, fps, windowSec_, bins_);

    // 坐标变换
    const double tmin = -windowSec_, tmax = 0.0;
    double ymin = *std::min_element(env.ymin.begin(), env.ymin.end());
    double ymax = *std::max_element(env.ymax.begin(), env.ymax.end());
    if (ymax <= ymin) { ymin = 0; ymax = 1023; }

    QRectF R = rect().adjusted(10,10,-10,-10);
    auto X = [&](double t){ return R.left() + (t - tmin) / (tmax - tmin) * R.width(); };
    auto Y = [&](double v){ return R.bottom() - (v - ymin) / (ymax - ymin + 1e-9) * R.height(); };

    // envelope 阴影
    QPainterPath upper, lower;
    upper.moveTo(X(env.x.front()), Y(env.ymax.front()));
    lower.moveTo(X(env.x.front()), Y(env.ymin.front()));
    for (int i = 1; i < bins_; ++i) {
        upper.lineTo(X(env.x[i]), Y(env.ymax[i]));
        lower.lineTo(X(env.x[i]), Y(env.ymin[i]));
    }
    QPainterPath shaded = upper;
    for (int i = bins_ - 1; i >= 0; --i)
        shaded.lineTo(X(env.x[i]), Y(env.ymin[i]));
    shaded.closeSubpath();

    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillPath(shaded, QColor(120,160,255,60));

    // mean 线（可选平滑）
    std::vector<double> mean = env.mean;
    double dt_bin = windowSec_ / std::max(1, bins_);
    smooth_ema(mean, dt_bin, 8.0); // 先给个默认 EMA，可改成参数

    QPainterPath m;
    m.moveTo(X(env.x.front()), Y(mean.front()));
    for (int i = 1; i < bins_; ++i)
        m.lineTo(X(env.x[i]), Y(mean[i]));

    QPen pen(Qt::white); pen.setWidth(1); p.setPen(pen);
    p.drawPath(m);

    // 边框与标题
    p.setPen(QColor(180,180,180));
    p.drawRect(R);
    p.drawText(R.adjusted(4,4,-4,-4), Qt::AlignTop|Qt::AlignLeft,
               QString("Ch %1  |  FPS= %2  |  span=%3 s  |  bins=%4")
               .arg(ch_).arg((int)fps).arg(windowSec_).arg(bins_));
}
