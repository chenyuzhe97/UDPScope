#include "PcapWorker.hpp"
#include <QString>
#include <cstdio>
#include <cstring>

PcapWorker::PcapWorker(DecodedFrameRing& ring, const CaptureConfig& cfg, RuntimeStats& stats)
: ring_(ring), cfg_(cfg), stats_(stats) {}

PcapWorker::~PcapWorker() { stop(); }

void PcapWorker::start() {
    if (running_.exchange(true)) return;
    rx_thread_ = std::thread(&PcapWorker::rx_loop, this);
}

void PcapWorker::stop() {
    if (!running_.exchange(false)) return;
    if (rx_thread_.joinable()) rx_thread_.join();
}

bool PcapWorker::extract_udp_payload(const u_char* data, size_t caplen, int linktype,
                                     const u_char*& udp_payload, size_t& udp_payload_len) {
    const u_char* p = data; size_t len = caplen;

    if (linktype == DLT_EN10MB) { // Ethernet
        if (len < 14) return false;
        const uint16_t eth_type = (p[12] << 8) | p[13];
        size_t off = 14; uint16_t type = eth_type;
        while (type == 0x8100 || type == 0x88a8) { // VLAN tags
            if (len < off + 4) return false;
            type = (p[off + 2] << 8) | p[off + 3];
            off += 4;
        }
        if (type != 0x0800) return false; // IPv4
        p += off; len -= off;
    } else if (linktype == DLT_LINUX_SLL || linktype == DLT_LINUX_SLL2) {
        const size_t sll_len = (linktype == DLT_LINUX_SLL) ? 16 : 20;
        if (len < sll_len) return false;
        p += sll_len; len -= sll_len;
    } else if (linktype == DLT_RAW) {
        // already IP
    } else {
        return false;
    }

    if (len < 20) return false; // IPv4 header
    const uint8_t ipver = p[0] >> 4; if (ipver != 4) return false;
    const uint8_t ihl   = (p[0] & 0x0F) * 4; if (len < ihl + 8) return false;
    const uint8_t proto = p[9]; if (proto != 17) return false; // UDP

    const u_char* udp = p + ihl;
    const uint16_t udp_len = (udp[4] << 8) | udp[5]; if (udp_len < 8) return false;

    udp_payload = udp + 8;
    size_t max_avail = (data + caplen > udp_payload) ? size_t((data + caplen) - udp_payload) : 0;
    udp_payload_len = std::min<size_t>(udp_len - 8, max_avail);
    return true;
}

void PcapWorker::rx_loop() {
    char errbuf[PCAP_ERRBUF_SIZE] = {0};
    pcap_t* handle = pcap_create(cfg_.ifname, errbuf);
    if (!handle) {
        emit errorOccurred(QString("pcap_create failed: %1").arg(errbuf));
        running_.store(false);
        return;
    }
    pcap_set_snaplen(handle, cfg_.snaplen);
    pcap_set_promisc(handle, cfg_.promisc ? 1 : 0);
    pcap_set_timeout(handle, cfg_.timeout_ms);
#ifdef pcap_set_immediate_mode
    pcap_set_immediate_mode(handle, 1);
#endif
    if (pcap_activate(handle) < 0) {
        emit errorOccurred(QString("pcap_activate failed: %1").arg(pcap_geterr(handle)));
        pcap_close(handle); running_.store(false); return;
    }

    bpf_program fp{};
    if (pcap_compile(handle, &fp, cfg_.bpf, 1, PCAP_NETMASK_UNKNOWN) < 0) {
        emit errorOccurred(QString("pcap_compile failed: %1").arg(pcap_geterr(handle)));
        pcap_close(handle); running_.store(false); return;
    }
    if (pcap_setfilter(handle, &fp) < 0) {
        emit errorOccurred(QString("pcap_setfilter failed: %1").arg(pcap_geterr(handle)));
        pcap_freecode(&fp); pcap_close(handle); running_.store(false); return;
    }
    pcap_freecode(&fp);

    int linktype = pcap_datalink(handle);
    uint16_t samples[SAMPLES_PER_FRAME];

    while (running_.load(std::memory_order_relaxed)) {
        pcap_pkthdr* hdr = nullptr; const u_char* pkt = nullptr;
        int rc = pcap_next_ex(handle, &hdr, &pkt);
        if (rc == 1) {
            stats_.bytes_rx += hdr->len;
            const u_char* udp_payload = nullptr; size_t udp_len = 0;
            if (!extract_udp_payload(pkt, hdr->caplen, linktype, udp_payload, udp_len)) {
                stats_.frames_drop++; continue;
            }
            if (udp_len != (size_t)FRAME_SIZE_BYTES) { stats_.frames_drop++; continue; }

            const uint8_t* frame = reinterpret_cast<const uint8_t*>(udp_payload);
            const uint8_t* payload = frame + HEADER_BYTES;
            unpack10bit_1024(payload, samples);
            ring_.push_frame(samples);
            stats_.frames_rx++; emit frameAdvanced(ring_.snapshot_write_index());
        } else if (rc == 0) {
            continue; // timeout
        } else {
            break;    // error or break
        }
    }

    pcap_close(handle);
}