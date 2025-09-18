#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QColor>
#include <QVector>
#include <QRectF>
#include <cstdint>

class DecodedFrameRing; // 仅前置声明，定义从别处引入

// 简单 envelope 容器
struct EnvelopeQT {
    QVector<double> x;     // [-window, 0]
    QVector<double> ymin;
    QVector<double> ymax;
    QVector<double> mean;  // 平均（每 bin）
};

class PlotWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget* parent=nullptr);

    // 数据源
    void attachRing(DecodedFrameRing* ring) { ring_ = ring; }

    // 基本参数
    void setChannel(int ch)          { ch_ = ch; update(); }
    void setBins(int bins)           { bins_ = qMax(10, bins); update(); }
    void setWindowSeconds(double s)  { windowSec_ = qMax(0.01, s); update(); }

    // 外观
    void setBgColor(QColor c)        { bg_ = c; update(); }
    void setEnvColor(QColor c)       { envColor_ = c; update(); }
    void setEnvAlpha(int a)          { envAlpha_ = qBound(0, a, 255); update(); }
    void setDrawOutline(bool on)     { drawOutline_ = on; update(); }

    // 纵轴
    void setAutoY(bool on)           { autoY_ = on; update(); }
    void setYRange(double ymin, double ymax) { yMin_=ymin; yMax_=ymax; autoY_=false; update(); }

    // 高通参数（Hz）
    void setHighPassCutHz(double hz) { hpfCutHz_ = qMax(0.0, hz); update(); }
    double highPassCutHz() const     { return hpfCutHz_; }

public slots:
    void onFrameAdvanced(quint64 /*widx*/); // 仅触发重绘

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    // 构建 envelope
    EnvelopeQT buildEnvelope() const;

    // 一阶高通（对 mean 的副本做）
    static void highPassRC(QVector<double>& y, double dt, double fc_hz);

    // 图例绘制与命中
    void drawLegend(QPainter& p);
    int  hitLegendItem(const QPointF& pos) const; // 返回索引，-1=miss

private:
    // 数据 & 参数
    DecodedFrameRing* ring_{nullptr};
    int     ch_{0};
    int     bins_{1200};
    double  windowSec_{1.0};

    // 外观
    QColor  bg_{QColor(26,26,30)};
    QColor  envColor_{QColor(90,130,200)};
    int     envAlpha_{70};
    bool    drawOutline_{false};

    // 纵轴
    bool    autoY_{true};
    double  yMin_{0.0};
    double  yMax_{1023.0};

    // 图例状态
    bool    showEnvelope_{true};
    bool    showMean_{true};
    bool    showHPF_{false};     // 新增：高通曲线
    // 颜色：mean(青绿)、HPF(橙)
    QColor  meanColor_{QColor(56, 198, 174)};
    QColor  hpfColor_{QColor(255, 149, 0)};

    // HPF 参数
    double  hpfCutHz_{50.0};

    // 图例 item 的可点击区域
    struct LegendItem { QString name; QColor color; bool* flag; QRectF rect; };
    mutable QVector<LegendItem> legend_;
};
