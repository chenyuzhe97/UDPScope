#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QElapsedTimer>
#include <QRectF>
#include <functional>
#include "Core.hpp"

class PlotWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget* parent=nullptr);
    void attachRing(const DecodedFrameRing* ring) { ring_ = ring; }
    void setChannel(int ch) { ch_ = ch; }
    void setWindowSeconds(double w) { windowSec_ = w; }
    void setBins(int b) { bins_ = b; }
    void setUseMeasuredFps(bool v) { useMeasuredFps_ = v; }

    // 主题 & 透明度
    void setBgColor(const QColor& c);
    void setEnvColor(const QColor& c) { envColor_ = c; update(); }
    void setEnvAlpha(int a) { envAlpha_ = std::clamp(a,0,255); update(); }

    // 纵轴控制
    void setAutoY(bool on) { autoY_ = on; update(); }
    void setYRange(double ymin, double ymax) { yUserMin_ = ymin; yUserMax_ = ymax; autoY_ = false; update(); }

    // 显示控制
    void setDrawOutline(bool on) { drawOutlineEdges_ = on; update(); }
    void setShowMean(bool on)    { showMean_ = on; update(); }

public slots:
    void onFrameAdvanced(quint64 widx);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void drawLegend(QPainter& p);
    void drawAxesAndGrid(QPainter& p, const QRectF& R, double tmin, double tmax, double ymin, double ymax);
    void drawMeanCurve(QPainter& p, const Envelope& env, std::function<double(double)> X, std::function<double(double)> Y);

    QRectF legendRectEnv_;
    QRectF legendRectMean_;   // <-- 新增：Mean 图例按钮区域

    const DecodedFrameRing* ring_ = nullptr;
    quint64 widx_ = 0;
    int ch_ = 0;
    int bins_ = 1200;
    double windowSec_ = 1.0;
    bool useMeasuredFps_ = true;
    double measuredFps_ = 20000.0;
    QElapsedTimer fpsTimer_;
    quint64 lastFrameCount_ = 0;

    // 显示项
    bool showEnvelope_ = true;
    bool showMean_     = true;

    // 主题/颜色
    QColor bgColor_{26,26,30};
    QColor envColor_{90,130,200};
    int    envAlpha_ = 70;

    // 纵轴
    bool   autoY_ = true;
    double yUserMin_ = 0.0;
    double yUserMax_ = 1023.0;

    // 轮廓线（上沿）
    bool   drawOutlineEdges_ = false;
};
