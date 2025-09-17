#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QElapsedTimer>
#include <QMutex>
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

public slots:
    // 用 Qt 元类型：quint64
    void onFrameAdvanced(quint64 widx);

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    const DecodedFrameRing* ring_ = nullptr;
    quint64 widx_ = 0;
    int ch_ = 0;
    int bins_ = 1200;
    double windowSec_ = 1.0;
    bool useMeasuredFps_ = true;
    double measuredFps_ = 20000.0; // 初始估计
    QElapsedTimer fpsTimer_;
    quint64 lastFrameCount_ = 0;
};
