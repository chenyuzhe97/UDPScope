#pragma once
#include <QMainWindow>
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

private:
    std::unique_ptr<DecodedFrameRing> ring_;
    std::unique_ptr<RuntimeStats> stats_;
    PcapWorker* worker_ = nullptr;

    // UI pointers
    QWidget* central_ = nullptr;
    class QLineEdit* ifEdit_ = nullptr;
    class QLineEdit* bpfEdit_ = nullptr;
    class QSpinBox*  binsSpin_ = nullptr;
    class QDoubleSpinBox* winSpin_ = nullptr;
    class QPushButton* startBtn_ = nullptr;
    class QPushButton* stopBtn_  = nullptr;
    PlotWidget* plot_ = nullptr;
};