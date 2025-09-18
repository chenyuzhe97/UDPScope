#include "PlotWidget.hpp"
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QPainterPath>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <algorithm>
#include <cmath>

static inline double nice_step(double raw) {
    double exp10 = std::pow(10.0, std::floor(std::log10(std::max(1e-12, raw))));
    double frac  = raw / exp10;
    double nice;
    if (frac < 1.5)      nice = 1.0;
    else if (frac < 3.5) nice = 2.0;
    else if (frac < 7.5) nice = 5.0;
    else                 nice = 10.0;
    return nice * exp10;
}

// 背景亮度判定
static inline bool is_dark_bg(const QColor& c) {
    auto ch = [](double u){ u/=255.0; return (u<=0.03928)?(u/12.92):std::pow((u+0.055)/1.055,2.4); };
    double L = 0.2126*ch(c.red()) + 0.7152*ch(c.green()) + 0.0722*ch(c.blue());
    return L < 0.5;
}

PlotWidget::PlotWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumHeight(240);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    fpsTimer_.start();
}

void PlotWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(bgColor_.redF(), bgColor_.greenF(), bgColor_.blueF(), 1.0f);
}

void PlotWidget::setBgColor(const QColor& c) {
    bgColor_ = c;
    if (context()) {
        makeCurrent();
        glClearColor(bgColor_.redF(), bgColor_.greenF(), bgColor_.blueF(), 1.0f);
        doneCurrent();
    }
    update();
}

void PlotWidget::onFrameAdvanced(quint64 widx) {
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

// 画图例（按钮）
void PlotWidget::drawLegend(QPainter& p) {
    const int pad = 8, box = 14, gap = 6;
    QRectF area = rect().adjusted(pad, pad, -pad, -pad);

    QFont f = p.font(); f.setPointSizeF(std::max(9.0, f.pointSizeF())); p.setFont(f);
    QFontMetrics fm(f);

    const QString envText  = "Env";
    const QString meanText = "Mean";

    const int envW  = fm.horizontalAdvance(envText)  + box + 6;
    const int meanW = fm.horizontalAdvance(meanText) + box + 6;
    const int h = std::max(box, fm.height());

    const QPoint topRight(area.right(), area.top());

    // 布局：右上角  Mean | Env
    legendRectEnv_  = QRectF(topRight.x() - envW,                 topRight.y(), envW,  h);
    legendRectMean_ = QRectF(legendRectEnv_.left() - gap - meanW, topRight.y(), meanW, h);

    auto drawItem = [&](const QRectF& r, const QString& text, bool on, const QColor& colorBox, bool asLine){
        const bool dark = is_dark_bg(bgColor_);
        QColor frame = dark ? QColor(200,200,200) : QColor(60,60,60);
        // 白底把面板透明度降一点，避免看起来像“阴影条”
        QColor panel = dark ? QColor(40,40,45,200) : QColor(240,240,240,160);

        p.setPen(frame); p.setBrush(panel);
        p.drawRect(r.adjusted(0,0,-1,-1));

        QRectF boxRect(r.left()+2, r.center().y() - box/2.0, box, box);
        if (asLine) {
            p.setPen(QPen(colorBox, 3));
            p.drawLine(QPointF(boxRect.left()+2, boxRect.center().y()),
                       QPointF(boxRect.right()-2, boxRect.center().y()));
        } else {
            p.setPen(Qt::NoPen); p.setBrush(colorBox); p.drawRect(boxRect);
        }

        if (!on) {
            p.setPen(QPen(QColor(180,80,80), 2));
            p.drawLine(boxRect.topLeft(), boxRect.bottomRight());
            p.drawLine(boxRect.bottomLeft(), boxRect.topRight());
        }

        p.setPen(frame);
        p.drawText(QRectF(boxRect.right()+4, r.top(), r.width()-box-6, r.height()),
                   Qt::AlignVCenter|Qt::AlignLeft, text);
    };

    QColor envBox  = envColor_; envBox.setAlpha(200);
    QColor meanCol = is_dark_bg(bgColor_) ? envColor_.lighter(120) : envColor_.darker(160);
    drawItem(legendRectMean_, meanText, showMean_,     meanCol, /*asLine=*/true);
    drawItem(legendRectEnv_,  envText,  showEnvelope_, envBox,  /*asLine=*/false);
}

void PlotWidget::drawAxesAndGrid(QPainter& p, const QRectF& R, double tmin, double tmax, double ymin, double ymax) {
    const bool dark = is_dark_bg(bgColor_);
    QColor axisCol  = dark ? QColor(150,150,155) : QColor(40,40,45);
    QColor gridCol  = dark ? QColor(90,90,100)   : QColor(160,160,170);
    QColor textCol  = dark ? QColor(200,200,205) : QColor(30,30,35);

    p.setPen(axisCol);
    p.drawRect(R);

    p.setRenderHint(QPainter::Antialiasing, false);
    QPen gridPen(gridCol);
    gridPen.setStyle(Qt::DashLine);
    gridPen.setWidth(dark ? 1 : 2);

    double xrange = tmax - tmin;
    double xtick = nice_step(xrange / 6.0);
    double x0 = std::ceil(tmin / xtick) * xtick;

    double yrange = ymax - ymin;
    if (yrange <= 0) { yrange = 1; ymax = ymin + 1; }
    double ytick = nice_step(yrange / 5.0);
    double y0 = std::ceil(ymin / ytick) * ytick;

    auto X = [&](double t){ return R.left() + (t - tmin) / (tmax - tmin) * R.width(); };
    auto Y = [&](double v){ return R.bottom() - (v - ymin) / (ymax - ymin) * R.height(); };

    p.setPen(gridPen);
    for (double x = x0; x <= tmax + 1e-9; x += xtick) p.drawLine(QPointF(X(x), R.top()), QPointF(X(x), R.bottom()));
    for (double y = y0; y <= ymax + 1e-9; y += ytick) p.drawLine(QPointF(R.left(), Y(y)), QPointF(R.right(), Y(y)));

    p.setPen(textCol);
    QFont f = p.font(); f.setPointSizeF(std::max(9.0, f.pointSizeF())); p.setFont(f);

    for (double x = x0; x <= tmax + 1e-9; x += xtick) {
        double xx = X(x);
        QString s = QString::number(x, 'f', (std::abs(xtick) >= 1.0) ? 0 : 2);
        p.drawText(QRectF(xx-30, R.bottom()+2, 60, 14), Qt::AlignHCenter|Qt::AlignTop, s);
    }
    for (double y = y0; y <= ymax + 1e-9; y += ytick) {
        double yy = Y(y);
        QString s = QString::number(y, 'f', (ytick >= 1.0) ? 0 : 2);
        p.drawText(QRectF(R.left()-48, yy-7, 44, 14), Qt::AlignRight|Qt::AlignVCenter, s);
    }
    p.drawText(QRectF(R.right()-60, R.bottom()+16, 60, 14), Qt::AlignRight|Qt::AlignVCenter, "t (s)");
    p.drawText(QRectF(R.left()-48, R.top()-2, 44, 14), Qt::AlignRight|Qt::AlignTop, "value");
}

void PlotWidget::drawMeanCurve(QPainter& p, const Envelope& env,
                               std::function<double(double)> X,
                               std::function<double(double)> Y)
{
    if (!showMean_ || env.mean.empty()) return;

    QPainterPath path;
    path.moveTo(X(env.x.front()), Y(env.mean.front()));
    for (int i = 1; i < bins_; ++i)
        path.lineTo(X(env.x[i]), Y(env.mean[i]));

    QColor line = is_dark_bg(bgColor_) ? envColor_.lighter(120) : envColor_.darker(160);
    QPen pen(line);
    pen.setWidth(2);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(pen);
    p.drawPath(path);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void PlotWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);

    QPainter p(this);
    p.fillRect(rect(), bgColor_);

    drawLegend(p);

    if (!ring_ || widx_==0) {
        QColor textCol = is_dark_bg(bgColor_) ? QColor(230,230,230) : QColor(30,30,30);
        p.setPen(textCol);
        p.drawText(rect(), Qt::AlignCenter, "waiting...");
        return;
    }

    const double fps = useMeasuredFps_ ? std::max(1.0, measuredFps_) : 20000.0;
    auto env = build_envelope(*ring_, widx_, ch_, fps, windowSec_, bins_);

    double ymin, ymax;
    if (autoY_) {
        ymin = *std::min_element(env.ymin.begin(), env.ymin.end());
        ymax = *std::max_element(env.ymax.begin(), env.ymax.end());
        if (!(ymax > ymin)) { ymin = 0; ymax = g_cfg.max_sample(); }
        double pad = (ymax - ymin) * 0.05; ymin -= pad; ymax += pad;
    } else {
        ymin = yUserMin_; ymax = yUserMax_;
        if (!(ymax > ymin)) { ymin = 0; ymax = g_cfg.max_sample(); }
    }

    const double tmin = -windowSec_, tmax = 0.0;
    QRectF R = rect().adjusted(56,24,-14,-26);

    auto X = [&](double t){ return R.left() + (t - tmin) / (tmax - tmin) * R.width(); };
    auto Y = [&](double v){ return R.bottom() - (v - ymin) / (ymax - ymin + 1e-9) * R.height(); };

    // ---- 裁剪：后续所有绘制都限制在绘图区 R 内 ----
    p.save();
    p.setClipRect(R);

    // 1) 包络阴影（无 Pen）+ 轮廓
    if (showEnvelope_) {
        QPainterPath shaded;
        // 先走 LOWER (左->右)
        shaded.moveTo(X(env.x.front()), Y(env.ymin.front()));
        for (int i = 1; i < bins_; ++i)
            shaded.lineTo(X(env.x[i]), Y(env.ymin[i]));
        // 再走 UPPER (右->左)
        for (int i = bins_ - 1; i >= 0; --i)
            shaded.lineTo(X(env.x[i]), Y(env.ymax[i]));
        shaded.closeSubpath();

        QColor fill = envColor_; fill.setAlpha(envAlpha_);

        QBrush br(fill);
        QPen   noPen(Qt::NoPen);   // 关键：不画闭合边的描边
        p.setPen(noPen);
        p.setBrush(br);
        p.drawPath(shaded);

        if (drawOutlineEdges_) {
            // 只画上沿（upper）
            QPainterPath upper;
            upper.moveTo(X(env.x.front()), Y(env.ymax.front()));
            for (int i = 1; i < bins_; ++i)
                upper.lineTo(X(env.x[i]), Y(env.ymax[i]));
            QPen pen(is_dark_bg(bgColor_) ? envColor_.lighter(110) : envColor_.darker(130));
            pen.setWidth(2);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(upper);
        }
    }

    // 2) 网格/坐标
    p.restore();          // 先恢复再画轴，不被裁剪掉轴框
    drawAxesAndGrid(p, R, tmin, tmax, ymin, ymax);

    // 3) mean（再裁一次剪）
    p.save();
    p.setClipRect(R);
    drawMeanCurve(p, env, X, Y);
    p.restore();

    // 4) 标题
    QColor titleCol = is_dark_bg(bgColor_) ? QColor(200,200,205) : QColor(40,40,45);
    p.setPen(titleCol);
    p.drawText(QRectF(R.left(), R.top()-18, R.width(), 16), Qt::AlignLeft|Qt::AlignVCenter,
               QString("Ch %1  |  span=%2 s  |  bins=%3").arg(ch_).arg(windowSec_).arg(bins_));
}

void PlotWidget::mousePressEvent(QMouseEvent* e) {
    const QPointF pt = e->position();
    if (legendRectEnv_.contains(pt))  { showEnvelope_ = !showEnvelope_; update(); return; }
    if (legendRectMean_.contains(pt)) { showMean_     = !showMean_;     update(); return; }
    QOpenGLWidget::mousePressEvent(e);
}
