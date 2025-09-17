#pragma once
#include <atomic>
#include <thread>
#include <pcap/pcap.h>
#include <pcap/dlt.h>
#include <QObject>
#include "Core.hpp"

struct CaptureConfig {
    char ifname[64] = "enp3s0";
    char bpf[256]   = "udp and src host 12.0.0.2 and dst host 12.0.0.1 and src port 2827 and dst port 2827 and udp[4:2] = 1307";
    bool promisc    = true;
    int  snaplen    = 2048;
    int  timeout_ms = 1;
};

class PcapWorker : public QObject {
    Q_OBJECT
public:
    PcapWorker(DecodedFrameRing& ring, const CaptureConfig& cfg, RuntimeStats& stats);
    ~PcapWorker();

public slots:
    void start();
    void stop();

signals:
    void frameAdvanced(uint64_t widx);
    void statsUpdated(uint64_t framesRx, uint64_t framesDrop, uint64_t bytesRx);
    void errorOccurred(QString msg);

private:
    static bool extract_udp_payload(const u_char* data, size_t caplen, int linktype,
                                    const u_char*& udp_payload, size_t& udp_payload_len);
    void rx_loop();

    std::atomic<bool> running_{false};
    std::thread       rx_thread_;
    DecodedFrameRing& ring_;
    CaptureConfig     cfg_;
    RuntimeStats&     stats_;
};