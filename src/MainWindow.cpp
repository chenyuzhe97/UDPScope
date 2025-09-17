#include "MainWindow.hpp"
#include "PlotWidget.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QMessageBox>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    ring_ = std::make_unique<DecodedFrameRing>(200000); // 与你一致
    stats_ = std::make_unique<RuntimeStats>();

    central_ = new QWidget(this);
    setCentralWidget(central_);

    auto* v = new QVBoxLayout(central_);

    // 控制条
    auto* row = new QHBoxLayout();
    ifEdit_ = new QLineEdit("enp3s0");
    bpfEdit_ = new QLineEdit("udp and src host 12.0.0.2 and dst host 12.0.0.1 and src port 2827 and dst port 2827 and udp[4:2] = 1307");
    binsSpin_ = new QSpinBox(); binsSpin_->setRange(200, 4000); binsSpin_->setValue(1200);
    winSpin_  = new QDoubleSpinBox(); winSpin_->setRange(0.05, 60.0); winSpin_->setDecimals(2); winSpin_->setValue(1.0);
    startBtn_ = new QPushButton("Start");
    stopBtn_  = new QPushButton("Stop"); stopBtn_->setEnabled(false);

    row->addWidget(new QLabel("Interface:")); row->addWidget(ifEdit_, 0);
    row->addSpacing(8);
    row->addWidget(new QLabel("BPF:")); row->addWidget(bpfEdit_, 1);
    row->addSpacing(8);
    row->addWidget(new QLabel("Bins:")); row->addWidget(binsSpin_);
    row->addWidget(new QLabel("Window(s):")); row->addWidget(winSpin_);
    row->addSpacing(8);
    row->addWidget(startBtn_); row->addWidget(stopBtn_);

    v->addLayout(row);

    // 波形
    plot_ = new PlotWidget();
    plot_->attachRing(ring_.get());
    plot_->setChannel(0);
    plot_->setBins(binsSpin_->value());
    plot_->setWindowSeconds(winSpin_->value());
    v->addWidget(plot_, 1);

    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(stopBtn_,  &QPushButton::clicked, this, &MainWindow::onStop);
    connect(binsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), [this](int v){ plot_->setBins(v); });
    connect(winSpin_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this](double w){ plot_->setWindowSeconds(w); });

    setWindowTitle("UDP 10-bit Scope (Qt6 + pcap)"); resize(1200, 700);
}

MainWindow::~MainWindow() {
    onStop();
}

void MainWindow::onStart() {
    if (worker_) return;

    CaptureConfig cfg{};
    std::snprintf(cfg.ifname, sizeof(cfg.ifname), "%s", ifEdit_->text().toUtf8().constData());
    std::snprintf(cfg.bpf, sizeof(cfg.bpf), "%s", bpfEdit_->text().toUtf8().constData());

    worker_ = new PcapWorker(*ring_, cfg, *stats_);
    connect(worker_, &PcapWorker::frameAdvanced, plot_, &PlotWidget::onFrameAdvanced);
    connect(worker_, &PcapWorker::errorOccurred, this, &MainWindow::onError);

    worker_->start();

    startBtn_->setEnabled(false);
    stopBtn_->setEnabled(true);
}

void MainWindow::onStop() {
    if (!worker_) return;
    worker_->stop();
    delete worker_; worker_ = nullptr;
    startBtn_->setEnabled(true);
    stopBtn_->setEnabled(false);
}

void MainWindow::onError(const QString& msg) {
    QMessageBox::critical(this, "pcap error", msg);
}