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
#include <QComboBox>
#include <QGridLayout>
#include <QSet>
#include <QCheckBox>

// 主题色
static QColor themeColor(int idx) {
    switch (idx) {
        case 0: return QColor(26,26,30);    // Dark
        case 1: return QColor(8,8,8);       // Black
        case 2: return QColor(34,39,46);    // Dark Slate
        case 3: return QColor(18,24,39);    // Navy
        case 4: return QColor(255,255,255); // White
        default: return QColor(26,26,30);
    }
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    ring_  = std::make_unique<DecodedFrameRing>(200000);
    stats_ = std::make_unique<RuntimeStats>();

    central_ = new QWidget(this);
    setCentralWidget(central_);
    auto* v = new QVBoxLayout(central_);

    // 行1：抓包
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

    // 行2：解析配置
    auto* cfg = new QHBoxLayout();
    packCombo_ = new QComboBox();
    packCombo_->addItem("RAW10_PACKED", static_cast<int>(PackMode::RAW10_PACKED));
    packCombo_->addItem("RAW16_LE",     static_cast<int>(PackMode::RAW16_LE));
    packCombo_->setCurrentIndex(g_cfg.pack == PackMode::RAW10_PACKED ? 0 : 1);

    bitsSpin_ = new QSpinBox(); bitsSpin_->setRange(1, 16); bitsSpin_->setValue(g_cfg.bits_per_sample);
    samplesSpin_ = new QSpinBox(); samplesSpin_->setRange(1, 65536); samplesSpin_->setValue(g_cfg.samples_per_frame);

    frameSizeSpin_ = new QSpinBox(); frameSizeSpin_->setRange(1, 1<<24); frameSizeSpin_->setValue(g_cfg.frame_size_bytes);
    headerSpin_    = new QSpinBox(); headerSpin_->setRange(0, 1<<20); headerSpin_->setValue(g_cfg.header_bytes);
    payloadSpin_   = new QSpinBox(); payloadSpin_->setRange(0, 1<<23); payloadSpin_->setValue(g_cfg.payload_bytes);
    tailSpin_      = new QSpinBox(); tailSpin_->setRange(0, 1<<20); tailSpin_->setValue(g_cfg.tail_bytes);

    applyCfgBtn_ = new QPushButton("Apply Parser Config");

    cfg->addWidget(new QLabel("Pack:"));            cfg->addWidget(packCombo_);
    cfg->addWidget(new QLabel("Bits:"));            cfg->addWidget(bitsSpin_);
    cfg->addWidget(new QLabel("Samples/Frame:"));   cfg->addWidget(samplesSpin_);
    cfg->addSpacing(12);
    cfg->addWidget(new QLabel("Frame(Bytes):"));    cfg->addWidget(frameSizeSpin_);
    cfg->addWidget(new QLabel("Header:"));          cfg->addWidget(headerSpin_);
    cfg->addWidget(new QLabel("Payload:"));         cfg->addWidget(payloadSpin_);
    cfg->addWidget(new QLabel("Tail:"));            cfg->addWidget(tailSpin_);
    cfg->addSpacing(12);
    cfg->addWidget(applyCfgBtn_);
    v->addLayout(cfg);

    // 行3：视图
    auto* rowView = new QHBoxLayout();
    channelEdit_ = new QLineEdit("0-7");
    colsSpin_ = new QSpinBox(); colsSpin_->setRange(1, 8); colsSpin_->setValue(4);
    applyViewBtn_ = new QPushButton("Apply View");

    themeCombo_ = new QComboBox();
    themeCombo_->addItems({"Dark","Black","Dark Slate","Navy","White"});
    alphaSpin_ = new QSpinBox(); alphaSpin_->setRange(0,255); alphaSpin_->setValue(70);
    outlineCheck_ = new QCheckBox("Outline"); outlineCheck_->setChecked(false);

    autoYCheck_ = new QCheckBox("Auto Y"); autoYCheck_->setChecked(true);
    yMinSpin_ = new QDoubleSpinBox(); yMinSpin_->setRange(-1e9, 1e9); yMinSpin_->setDecimals(2); yMinSpin_->setValue(0);
    yMaxSpin_ = new QDoubleSpinBox(); yMaxSpin_->setRange(-1e9, 1e9); yMaxSpin_->setDecimals(2); yMaxSpin_->setValue(g_cfg.max_sample());
    yMinSpin_->setEnabled(false); yMaxSpin_->setEnabled(false);

    rowView->addWidget(new QLabel("Channels (e.g. 0,1,5,10-20):"));
    rowView->addWidget(channelEdit_, 1);
    rowView->addWidget(new QLabel("Cols:")); rowView->addWidget(colsSpin_);
    rowView->addSpacing(12);
    rowView->addWidget(new QLabel("Theme:")); rowView->addWidget(themeCombo_);
    rowView->addWidget(new QLabel("Env Alpha:")); rowView->addWidget(alphaSpin_);
    rowView->addWidget(outlineCheck_);
    rowView->addSpacing(12);
    rowView->addWidget(autoYCheck_);
    rowView->addWidget(new QLabel("Ymin:")); rowView->addWidget(yMinSpin_);
    rowView->addWidget(new QLabel("Ymax:")); rowView->addWidget(yMaxSpin_);
    rowView->addWidget(applyViewBtn_);
    v->addLayout(rowView);

    // 行4：绘图网格
    plotsContainer_ = new QWidget();
    grid_ = new QGridLayout(plotsContainer_);
    grid_->setContentsMargins(0,0,0,0);
    grid_->setSpacing(6);
    v->addWidget(plotsContainer_, 1);

    rebuildPlots();

    // 连接
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(stopBtn_,  &QPushButton::clicked, this, &MainWindow::onStop);
    connect(applyCfgBtn_, &QPushButton::clicked, this, &MainWindow::onApplyParserConfig);

    connect(binsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onRebuildPlots);
    connect(winSpin_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::onRebuildPlots);
    connect(colsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onRebuildPlots);
    connect(applyViewBtn_, &QPushButton::clicked, this, &MainWindow::onRebuildPlots);
    connect(outlineCheck_, &QCheckBox::toggled, this, &MainWindow::onRebuildPlots);

    connect(themeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onRebuildPlots);
    connect(alphaSpin_,  QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onRebuildPlots);

    connect(autoYCheck_, &QCheckBox::toggled, this, [this](bool on){
        yMinSpin_->setEnabled(!on);
        yMaxSpin_->setEnabled(!on);
        onRebuildPlots();
    });
    connect(yMinSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::onRebuildPlots);
    connect(yMaxSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::onRebuildPlots);

    setWindowTitle("UDP Scope (Qt6 + pcap) — Axes + Adjustable Y + Theme");
    resize(1360, 900);
}

MainWindow::~MainWindow() { onStop(); }

void MainWindow::onStart() {
    if (worker_) return;
    CaptureConfig cfg{};
    std::snprintf(cfg.ifname, sizeof(cfg.ifname), "%s", ifEdit_->text().toUtf8().constData());
    std::snprintf(cfg.bpf, sizeof(cfg.bpf), "%s", bpfEdit_->text().toUtf8().constData());
    worker_ = new PcapWorker(*ring_, cfg, *stats_);
    for (auto* w : plots_) {
        connect(worker_, &PcapWorker::frameAdvanced, w, &PlotWidget::onFrameAdvanced, Qt::QueuedConnection);
    }
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

bool MainWindow::validateParserConfig(QString& why) const {
    const auto pack = static_cast<PackMode>(packCombo_->currentData().toInt());
    const int bits = bitsSpin_->value();
    const int samples = samplesSpin_->value();
    const int frameSz = frameSizeSpin_->value();
    const int hdr = headerSpin_->value();
    const int pay = payloadSpin_->value();
    const int tail = tailSpin_->value();

    if (hdr + pay + tail != frameSz) { why = "HEADER + PAYLOAD + TAIL 必须等于 FRAME_SIZE_BYTES"; return false; }
    if (bits < 1 || bits > 16) { why = "bits_per_sample 仅支持 1..16"; return false; }

    if (pack == PackMode::RAW10_PACKED) {
        if (pay % 5 != 0) { why = "RAW10: payload_bytes 必须是 5 的整数倍"; return false; }
        int groups = pay / 5;
        if (groups * 4 != samples) { why = "RAW10: (payload/5)*4 必须等于 samples_per_frame"; return false; }
        if (bits != 10) { why = "RAW10: 建议 bits_per_sample = 10"; return false; }
    } else if (pack == PackMode::RAW16_LE) {
        if (pay < samples * 2) { why = "RAW16: payload_bytes 必须 >= samples_per_frame * 2"; return false; }
        if (bits > 16) { why = "RAW16: bits_per_sample <= 16"; return false; }
    } else { why = "未知 Pack 模式"; return false; }
    return true;
}

void MainWindow::rebuildRingAndReconnect() {
    ring_.reset();
    ring_ = std::make_unique<DecodedFrameRing>(200000);
    for (auto* w : plots_) w->attachRing(ring_.get());
    const bool wasRunning = (worker_ != nullptr);
    if (wasRunning) { onStop(); onStart(); }
}

QVector<int> MainWindow::parseChannelExpr(const QString& expr, int maxCh) const {
    QSet<int> set;
    const auto parts = expr.split(',', Qt::SkipEmptyParts);
    for (const auto& partRaw : parts) {
        const QString part = partRaw.trimmed();
        if (part.isEmpty()) continue;
        const int dash = part.indexOf('-');
        if (dash < 0) {
            bool ok=false; int v = part.toInt(&ok);
            if (ok && v >= 0 && v < maxCh) set.insert(v);
        } else {
            bool ok1=false, ok2=false;
            int a = part.left(dash).trimmed().toInt(&ok1);
            int b = part.mid(dash+1).trimmed().toInt(&ok2);
            if (ok1 && ok2) { if (a>b) std::swap(a,b); for (int v=a; v<=b; ++v) if (v>=0 && v<maxCh) set.insert(v); }
        }
    }
    QVector<int> out = set.values().toVector();
    std::sort(out.begin(), out.end());
    return out;
}

void MainWindow::onApplyParserConfig() {
    QString why;
    if (!validateParserConfig(why)) { QMessageBox::warning(this, "Invalid Parser Config", why); return; }
    const bool wasRunning = (worker_ != nullptr);
    if (wasRunning) onStop();

    g_cfg.pack = static_cast<PackMode>(packCombo_->currentData().toInt());
    g_cfg.bits_per_sample   = bitsSpin_->value();
    g_cfg.samples_per_frame = samplesSpin_->value();
    g_cfg.frame_size_bytes  = frameSizeSpin_->value();
    g_cfg.header_bytes      = headerSpin_->value();
    g_cfg.payload_bytes     = payloadSpin_->value();
    g_cfg.tail_bytes        = tailSpin_->value();

    rebuildRingAndReconnect();
    onRebuildPlots();
    if (wasRunning) onStart();
}

void MainWindow::onRebuildPlots() {
    rebuildPlots();
    if (worker_) {
        for (auto* w : plots_) {
            connect(worker_, &PcapWorker::frameAdvanced, w, &PlotWidget::onFrameAdvanced, Qt::QueuedConnection);
        }
    }
}

void MainWindow::rebuildPlots() {
    // 清空旧绘图
    for (auto* w : plots_) { grid_->removeWidget(w); w->deleteLater(); }
    plots_.clear();

    auto chs = parseChannelExpr(channelEdit_->text(), g_cfg.samples_per_frame);
    if (chs.isEmpty()) { plotsContainer_->update(); return; }

    const int cols = colsSpin_->value();
    const int rows = (chs.size() + cols - 1) / cols;

    QColor bg     = themeColor(themeCombo_->currentIndex());
    bool   isWhiteTheme = (themeCombo_->currentIndex() == 4);

    int    alpha   = alphaSpin_->value();
    bool   autoY   = autoYCheck_->isChecked();
    double ymin    = yMinSpin_->value();
    double ymax    = yMaxSpin_->value();
    bool   outline = outlineCheck_->isChecked();

    // White 主题时强制更清晰：降低填充、打开 Outline
    if (isWhiteTheme) {
        if (alpha > 25) alpha = 25;           // 阴影更淡
        outline = true;                        // 打开上沿
        outlineCheck_->blockSignals(true);
        outlineCheck_->setChecked(true);
        outlineCheck_->blockSignals(false);
        alphaSpin_->blockSignals(true);
        alphaSpin_->setValue(alpha);
        alphaSpin_->blockSignals(false);
    }

    // 同步顶层与容器背景
    central_->setStyleSheet(QString("background:%1;").arg(bg.name()));
    plotsContainer_->setStyleSheet(QString("background:%1;").arg(bg.name()));

    // 调色板：暗背景用亮色，白背景用深色
    static const QVector<QColor> paletteDark = {
        QColor(255, 99, 132), QColor(100, 181, 246), QColor(255, 202, 40), QColor(129, 199, 132),
        QColor(244, 143, 177), QColor(77, 182, 172), QColor(255, 167, 38), QColor(171, 71, 188),
    };
    static const QVector<QColor> paletteLight = {
        QColor(200, 0, 0), QColor(25,118,210), QColor(0,121,107), QColor(46,125,50),
        QColor(123,31,162), QColor(230,81,0), QColor(0,105,92), QColor(173,20,87),
    };
    const QVector<QColor>& palette = isWhiteTheme ? paletteLight : paletteDark;

    for (int i = 0; i < chs.size(); ++i) {
        int r = i / cols, c = i % cols;
        auto* pw = new PlotWidget(plotsContainer_);
        pw->attachRing(ring_.get());
        pw->setBins(binsSpin_->value());
        pw->setWindowSeconds(winSpin_->value());
        pw->setChannel(chs[i]);

        pw->setBgColor(bg);
        pw->setEnvColor(palette[i % palette.size()]);
        pw->setEnvAlpha(alpha);
        pw->setDrawOutline(outline);

        pw->setAutoY(autoY);
        if (!autoY) pw->setYRange(ymin, ymax);

        grid_->addWidget(pw, r, c);
        plots_.push_back(pw);
    }

    plotsContainer_->setLayout(grid_);
    plotsContainer_->update();
}
