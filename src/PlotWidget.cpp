#include "PlotWidget.hpp"
// 没有 DecodedFrameRing.hpp？这里临时从 MainWindow.hpp 把它“带进来”。
// 若你项目里 DecodedFrameRing 的声明在别的头，请把这行改成那个头。
#include "MainWindow.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFontMetrics>
#include <cmath>
#include <algorithm>

// --------- 构建 envelope：把环形缓冲按时间窗口聚合为 bins ---------
EnvelopeQT PlotWidget::buildEnvelope() const {
    EnvelopeQT env;
    env.x.resize(bins_);
    env.ymin.resize(bins_);
    env.ymax.resize(bins_);
    env.mean.resize(bins_);

    if (!ring_) {
        std::fill(env.ymin.begin(), env.ymin.end(), 0.0);
        std::fill(env.ymax.begin(), env.ymax.end(), 0.0);
        std::fill(env.mean.begin(), env.mean.end(), 0.0);
        for (int i=0;i<bins_;++i)
            env.x[i] = -windowSec_ + (windowSec_*(i+0.5)/bins_);
        return env;
    }

    const quint64 widx = ring_->snapshot_write_index();
    const quint64 framesAvail   = std::min<quint64>(widx, ring_->capacity());
    const quint64 windowFrames  = (quint64)std::llround(std::max(1.0, windowSec_ * 20000.0)); // 估算 20k fps
    const quint64 span          = std::min(framesAvail, std::max<quint64>(1, windowFrames));
    const quint64 startAbs      = widx > span ? (widx - span) : 0;
    const double  framesPerBin  = (double)span / std::max(1, bins_);

    for (int b=0;b<bins_;++b) {
        quint64 f0 = startAbs + (quint64)std::floor(b * framesPerBin);
        quint64 f1 = startAbs + (quint64)std::floor((b+1) * framesPerBin);
        if (f1 <= f0) f1 = f0 + 1;

        double vmin = 1e300, vmax = -1e300, sum = 0.0;
        int cnt = 0;
        for (quint64 f=f0; f<f1; ++f) {
            double v = (double)ring_->get_sample(f, ch_);
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
            sum += v; ++cnt;
        }
        env.ymin[b] = (cnt? vmin : 0.0);
        env.ymax[b] = (cnt? vmax : 0.0);
        env.mean[b] = (cnt? sum/std::max(1,cnt) : 0.0);

        env.x[b] = -windowSec_ + (b + 0.5) * (windowSec_ / std::max(1, bins_));
    }
    return env;
}

// --------- 一阶 RC 高通（对 mean 的副本） ---------
void PlotWidget::highPassRC(QVector<double>& y, double dt, double fc_hz) {
    if (y.isEmpty() || fc_hz <= 0.0) return;
    const double tau   = 1.0 / (2.0 * M_PI * fc_hz);
    const double alpha = tau / (tau + dt);
    double prevY = 0.0;
    for (int i=0;i<y.size();++i) {
        const double x = y[i];
        const double hp = alpha * (prevY + x - (i>0 ? y[i-1] : x));
        prevY = x;
        y[i] = hp;
    }
}

PlotWidget::PlotWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumHeight(120);
    setAutoFillBackground(false);
}

void PlotWidget::initializeGL() {
    initializeOpenGLFunctions();
}

void PlotWidget::onFrameAdvanced(quint64) {
    update();
}

void PlotWidget::paintGL() {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 背景
    p.fillRect(rect(), bg_);

    // 边框
    p.setPen(QPen(bg_.darker(140), 1));
    p.drawRect(rect().adjusted(0,0,-1,-1));

    // 绘图区
    const double lpad = 44, rpad = 8, tpad = 18, bpad = 18;
    const QRectF plotR(rect().left()+lpad, rect().top()+tpad,
                       rect().width()-lpad-rpad, rect().height()-tpad-bpad);

    if (plotR.width() <= 1 || plotR.height() <= 1) { drawLegend(p); return; }

    // 数据
    const auto env = buildEnvelope();
    if (env.x.isEmpty()) { drawLegend(p); return; }

    // X/Y 映射
    const auto X = [&](double t) {
        const double a = plotR.left();
        const double b = plotR.right();
        return a + (t + windowSec_) / windowSec_ * (b - a);
    };
    double ymin = yMin_, ymax = yMax_;
    if (autoY_) {
        ymin =  1e300; ymax = -1e300;
        for (int i=0;i<env.ymin.size();++i) {
            ymin = std::min(ymin, env.ymin[i]);
            ymax = std::max(ymax, env.ymax[i]);
        }
        if (ymax <= ymin) { ymin = 0; ymax = 1; }
        const double pad = (ymax - ymin) * 0.05;
        ymin -= pad; ymax += pad;
    }
    const auto Y = [&](double v) {
        return plotR.bottom() - (v - ymin) / (ymax - ymin) * plotR.height();
    };

    // 坐标轴
    p.setPen(QPen(QColor(160,160,160), 1));
    p.drawLine(QPointF(plotR.left(), plotR.bottom()), QPointF(plotR.right(), plotR.bottom())); // x
    p.drawLine(QPointF(plotR.left(), plotR.top()),    QPointF(plotR.left(),  plotR.bottom())); // y

    // y 轴上下端刻度文字
    p.setPen(QPen(QColor(180,180,180)));
    p.drawText(QRectF(plotR.left()-38, plotR.top()-2, 36, 14), Qt::AlignRight|Qt::AlignVCenter, QString::number(ymax, 'f', 0));
    p.drawText(QRectF(plotR.left()-38, plotR.bottom()-12, 36, 14), Qt::AlignRight|Qt::AlignVCenter, QString::number(ymin, 'f', 0));

    // Raw 原始数据曲线（按帧直接取该通道的样本）
    if (showRaw_ && ring_) {
        // 估算帧率与窗口帧数（与 buildEnvelope 保持一致）
        const quint64 widx2 = ring_->snapshot_write_index();
        const quint64 framesAvail2   = std::min<quint64>(widx2, ring_->capacity());
        const quint64 windowFrames2  = (quint64)std::llround(std::max(1.0, windowSec_ * 20000.0)); // 假设 20k fps
        const quint64 span2          = std::min(framesAvail2, std::max<quint64>(1, windowFrames2));
        const quint64 startAbs2      = widx2 > span2 ? (widx2 - span2) : 0;

        const int wpx = std::max(1, (int)std::floor(plotR.width()));
        const quint64 stride = std::max<quint64>(1, span2 / std::max(1, wpx)); // 降采样：每像素取1点
        QPainterPath rpath;
        bool hasStart=false;
        for (quint64 f = startAbs2; f < startAbs2 + span2; f += stride) {
            double t = -windowSec_ + ((double)(f - startAbs2) + 0.5) / (double)span2 * windowSec_;
            double v = (double)ring_->get_sample(f, ch_);
            double xx = X(t);
            double yy = Y(v);
            if (!hasStart) { rpath.moveTo(xx, yy); hasStart=true; }
            else           { rpath.lineTo(xx, yy); }
        }
        p.setPen(QPen(rawColor_, 1.2));
        p.drawPath(rpath);
    }

    // Envelope 阴影
    if (showEnvelope_) {
        QPainterPath upper, lower;
        upper.moveTo(X(env.x.front()), Y(env.ymax.front()));
        lower.moveTo(X(env.x.front()), Y(env.ymin.front()));
        for (int i=1;i<env.x.size();++i) {
            upper.lineTo(X(env.x[i]), Y(env.ymax[i]));
            lower.lineTo(X(env.x[i]), Y(env.ymin[i]));
        }
        QPainterPath area = upper;
        for (int i=env.x.size()-1;i>=0;--i) area.lineTo(X(env.x[i]), Y(env.ymin[i]));
        QColor fill = envColor_; fill.setAlpha(envAlpha_);
        p.fillPath(area, fill);

        if (drawOutline_) {
            p.setPen(QPen(envColor_.darker(110), 1.0));
            p.drawPath(upper);
            p.drawPath(lower);
        }
    }

    // mean 曲线（青绿）
    if (showMean_) {
        QPainterPath m;
        m.moveTo(X(env.x.front()), Y(env.mean.front()));
        for (int i=1;i<env.x.size();++i) m.lineTo(X(env.x[i]), Y(env.mean[i]));
        p.setPen(QPen(meanColor_, 1.8));
        p.drawPath(m);
    }

    // HPF(mean)（橙色）
    if (showHPF_) {
        QVector<double> yhp = env.mean; // 副本
        const double dt_bin = windowSec_ / std::max(1, bins_);
        highPassRC(yhp, dt_bin, hpfCutHz_);
        QPainterPath h;
        h.moveTo(X(env.x.front()), Y(yhp.front()));
        for (int i=1;i<env.x.size();++i) h.lineTo(X(env.x[i]), Y(yhp[i]));
        p.setPen(QPen(hpfColor_, 1.8));
        p.drawPath(h);
    }

    // 通道标题
    p.setPen(QPen(QColor(200,200,200)));
    p.drawText(QRectF(plotR.left(), rect().top()+2, plotR.width(), 14),
               Qt::AlignLeft|Qt::AlignVCenter,
               QString("Ch %1").arg(ch_));

    drawLegend(p);
}

void PlotWidget::drawLegend(QPainter& p) {
    legend_.clear();
    const double pad = 6;
    const double sw = 14; // 色块宽
    const double sh = 12; // 色块高
    double x = width() - 10;
    double y = 8;

    auto push = [&](const QString& name, const QColor& c, bool& flag) {
        QFont f = p.font(); f.setPointSizeF(9); p.setFont(f);
        const QFontMetrics fm(f);
        const int tw = fm.horizontalAdvance(name) + 10;
        x -= (sw + 4 + tw + pad);
        QRectF r(x, y, sw+4+tw, sh+6);

        // 背板（半透明）
        p.setPen(Qt::NoPen);
        QColor back(0,0,0,128);
        p.fillRect(r, back);

        // 颜色块（显示 on/off）
        QColor box = flag ? c : QColor(130,130,130);
        QRectF rc(x+3, y+3, sw, sh);
        p.fillRect(rc, box);
        p.setPen(QPen(QColor(230,230,230), 1));
        p.drawRect(rc.adjusted(0,0,-1,-1));

        // 文本
        p.drawText(QRectF(x+sw+6, y, tw, sh+6), Qt::AlignVCenter|Qt::AlignLeft, name);

        legend_.push_back({name, c, &flag, r});
    };

    // 顺序：Raw / Env / Mean / HPF
    push("Raw",  rawColor_,  showRaw_);
    push("Env",  envColor_,  showEnvelope_);
    push("Mean", meanColor_, showMean_);
    push("HPF",  hpfColor_,  showHPF_);
}

int PlotWidget::hitLegendItem(const QPointF& pos) const {
    for (int i=0;i<legend_.size();++i) {
        if (legend_[i].rect.contains(pos)) return i;
    }
    return -1;
}

void PlotWidget::mousePressEvent(QMouseEvent* e) {
    const int idx = hitLegendItem(e->position());
    if (idx >= 0) {
        if (legend_[idx].flag) {
            *(legend_[idx].flag) = !*(legend_[idx].flag);
            update();
        }
        return;
    }
    QOpenGLWidget::mousePressEvent(e);
}
