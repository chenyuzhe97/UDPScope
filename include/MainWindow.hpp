#pragma once
#include <QMainWindow>
#include <QVector>
#include <memory>
#include "Core.hpp"
#include "PcapWorker.hpp"

class PlotWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget* parent=nullptr);
    ~MainWindow();

private slots:
    void onStart();
    void onStop();
    void onError(const QString& msg);

    void onApplyParserConfig();   // 解析配置（可热切换）
    void onRebuildPlots();        // 视图变化 → 重建

private:
    bool validateParserConfig(QString& why) const;
    void rebuildRingAndReconnect();
    QVector<int> parseChannelExpr(const QString& expr, int maxCh) const;
    void rebuildPlots();

    // 数据与采集
    std::unique_ptr<DecodedFrameRing> ring_;
    std::unique_ptr<RuntimeStats> stats_;
    PcapWorker* worker_ = nullptr;

    // 顶部抓包控制
    QWidget* central_ = nullptr;
    class QLineEdit* ifEdit_ = nullptr;
    class QLineEdit* bpfEdit_ = nullptr;
    class QSpinBox*  binsSpin_ = nullptr;
    class QDoubleSpinBox* winSpin_ = nullptr;
    class QPushButton* startBtn_ = nullptr;
    class QPushButton* stopBtn_  = nullptr;

    // 解析配置 UI
    class QComboBox* packCombo_ = nullptr;
    class QSpinBox*  bitsSpin_ = nullptr;
    class QSpinBox*  samplesSpin_ = nullptr;
    class QSpinBox*  frameSizeSpin_ = nullptr;
    class QSpinBox*  headerSpin_ = nullptr;
    class QSpinBox*  payloadSpin_ = nullptr;
    class QSpinBox*  tailSpin_ = nullptr;
    class QPushButton* applyCfgBtn_ = nullptr;

    // 视图控制
    class QLineEdit* channelEdit_ = nullptr;   // "0,1,5,10-20"
    class QSpinBox*  colsSpin_ = nullptr;
    class QPushButton* applyViewBtn_ = nullptr;

    // 颜色与纵轴 UI
    class QComboBox* themeCombo_ = nullptr;
    class QSpinBox*  alphaSpin_ = nullptr;
    class QCheckBox* autoYCheck_ = nullptr;
    class QDoubleSpinBox* yMinSpin_ = nullptr;
    class QDoubleSpinBox* yMaxSpin_ = nullptr;
    class QCheckBox* outlineCheck_ = nullptr;   // 新增：是否画上沿轮廓


    // 绘图容器
    class QWidget*   plotsContainer_ = nullptr;
    class QGridLayout* grid_ = nullptr;
    QVector<PlotWidget*> plots_;
};
