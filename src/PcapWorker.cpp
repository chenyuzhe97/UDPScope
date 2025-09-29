#include "PcapWorker.hpp"
#include <QtCore/QDebug>
#include <QString>
#include <QtGlobal>
#include <cstdio>
#include <cstring>

#include <pcap/pcap.h>
#include <pcap/dlt.h>


// ---- 工具：大端读取 ----
static inline uint16_t be16(const u_char* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
static inline uint32_t be32(const u_char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// ---- 打印各层长度（支持常见链路类型） ----
// 返回是否成功解析（解析失败不会中断流程，只是不打印）
static bool print_frame_lengths(const u_char* pkt, size_t caplen, int linktype) {
    if (!pkt || caplen < 1) return false;

    size_t link_len = 0;
    uint16_t ether_type = 0;
    const u_char* l3 = nullptr;            // 指向 IP 头
    const u_char* l4 = nullptr;            // 指向 UDP/TCP 头
    size_t l3_len = 0;                     // IP 头长度
    size_t l4_len = 0;                     // UDP/TCP 头长度
    size_t udp_payload_len = 0;
    bool is_udp = false;

    // ---- 解析 L2（链路层）----
    switch (linktype) {
    case DLT_EN10MB: { // 以太网
        if (caplen < 14) return false;
        link_len = 14;
        ether_type = be16(pkt + 12);

        // VLAN tag 0x8100/0x88a8：每个 tag 额外 4 字节
        size_t off = 12;
        while (ether_type == 0x8100 || ether_type == 0x88A8) {
            if (caplen < link_len + 4) return false;
            link_len += 4;
            off += 4;
            if (caplen < off + 2) return false;
            ether_type = be16(pkt + off);
        }

        l3 = pkt + link_len;
        break;
    }
    case DLT_LINUX_SLL: { // Linux cooked v1
        if (caplen < 16) return false;
        link_len = 16;
        // proto 位于偏移 14-15
        ether_type = be16(pkt + 14);
        l3 = pkt + link_len;
        break;
    }
    case DLT_LINUX_SLL2: { // Linux cooked v2
        if (caplen < 20) return false;
        link_len = 20;
        // proto 位于偏移 16-17
        ether_type = be16(pkt + 16);
        l3 = pkt + link_len;
        break;
    }
    case DLT_NULL: { // loopback/null
        if (caplen < 4) return false;
        link_len = 4;
        // 这里不是以太类型，值平台相关；简单判断：0x00000002=AF_INET, 0x00000018=AF_INET6（BSD风格）
        uint32_t af = *(const uint32_t*)pkt; // 本地字节序；不跨平台时够用
        if (af == 2 /*AF_INET*/ || af == 0x02000000) ether_type = 0x0800;      // IPv4
        else if (af == 24 /*AF_INET6*/ || af == 0x18000000) ether_type = 0x86DD; // IPv6
        l3 = pkt + link_len;
        break;
    }
    case DLT_RAW: { // 直接是 IP
        link_len = 0;
        l3 = pkt;
        // 判断版本
        if (caplen >= 1) {
            uint8_t v = (l3[0] >> 4) & 0xF;
            ether_type = (v == 4) ? 0x0800 : (v == 6) ? 0x86DD : 0;
        }
        break;
    }
    default:
        // 未覆盖的链路类型：只打印总长
        qDebug() << "[LEN] total=" << caplen << "(unsupported linktype=" << linktype << ")";
        return false;
    }

    if (!l3 || size_t(l3 - pkt) > caplen) return false;
    size_t remain = caplen - size_t(l3 - pkt);

    // ---- 解析 L3（IPv4/IPv6）----
    if (ether_type == 0x0800) { // IPv4
        if (remain < 20) return false;
        uint8_t ver_ihl = l3[0];
        uint8_t ihl = (ver_ihl & 0x0F) * 4;
        if (ihl < 20 || remain < ihl) return false;
        l3_len = ihl;

        uint8_t proto = l3[9];
        const u_char* src = l3 + 12;
        const u_char* dst = l3 + 16;

        if (proto == 17 /*UDP*/) {
            is_udp = true;
            l4 = l3 + l3_len;
            if ((caplen - size_t(l4 - pkt)) < 8) return false;
            l4_len = 8;
            uint16_t udp_len = be16(l4 + 4);
            if (udp_len >= 8) udp_payload_len = udp_len - 8;
        } else {
            // 非 UDP：粗略给出 L4 头 0（这里你只关心 UDP，其他略过）
        }

        qDebug() << "[LEN]"
                 << "total=" << caplen
                 << "link=" << link_len
                 << "ip=" << l3_len
                 << "udp=" << (is_udp ? l4_len : 0)
                 << "udp_payload=" << (is_udp ? udp_payload_len : 0)
                 << "| IPv4"
                 << "src=" << QString("%1.%2.%3.%4").arg(src[0]).arg(src[1]).arg(src[2]).arg(src[3])
                 << "dst=" << QString("%1.%2.%3.%4").arg(dst[0]).arg(dst[1]).arg(dst[2]).arg(dst[3])
                 << (is_udp ? QString("sport=%1 dport=%2")
                                .arg(be16(l4)).arg(be16(l4 + 2))
                            : QString("proto=%1").arg(proto));
    }
    else if (ether_type == 0x86DD) { // IPv6
        if (remain < 40) return false;
        l3_len = 40; // IPv6 固定头 40
        uint8_t next = l3[6];
        const u_char* src = l3 + 8;
        const u_char* dst = l3 + 24;

        // 只处理“无扩展头 + 直接 UDP”的常见情形
        if (next == 17 /*UDP*/) {
            is_udp = true;
            l4 = l3 + l3_len;
            if ((caplen - size_t(l4 - pkt)) < 8) return false;
            l4_len = 8;
            uint16_t udp_len = be16(l4 + 4);
            if (udp_len >= 8) udp_payload_len = udp_len - 8;
        }

        auto ipv6_to_str = [](const u_char* p) {
            // 简单十六进制展现，不做压缩：xxxx:xxxx:... 共8段
            QStringList parts;
            for (int i = 0; i < 16; i += 2) parts << QString("%1").arg(be16(p + i), 4, 16, QChar('0'));
            return parts.join(":");
        };

        qDebug() << "[LEN]"
                 << "total=" << caplen
                 << "link=" << link_len
                 << "ip=" << l3_len
                 << "udp=" << (is_udp ? l4_len : 0)
                 << "udp_payload=" << (is_udp ? udp_payload_len : 0)
                 << "| IPv6"
                 << "src=" << ipv6_to_str(src)
                 << "dst=" << ipv6_to_str(dst)
                 << (is_udp ? QString("sport=%1 dport=%2")
                                .arg(be16(l4)).arg(be16(l4 + 2))
                            : QString("next=%1").arg(next));
    }
    else {
        // 其他 EtherType：不深究
        qDebug() << "[LEN] total=" << caplen
                 << "link=" << link_len
                 << "ip=0 udp=0 udp_payload=0"
                 << "etherType=0x" << QString::number(ether_type, 16);
    }

    return true;
}



// ------------------------ Ctor / Dtor ------------------------

PcapWorker::PcapWorker(DecodedFrameRing& ring, const CaptureConfig& cfg, RuntimeStats& stats)
: ring_(ring), cfg_(cfg), stats_(stats) {}

PcapWorker::~PcapWorker() {
    stop();
}

// ------------------------ Public slots ------------------------

void PcapWorker::start() {
    if (running_.exchange(true)) return;
    rx_thread_ = std::thread(&PcapWorker::rx_loop, this);
}

void PcapWorker::stop() {
    if (!running_.exchange(false)) return;
    if (rx_thread_.joinable()) rx_thread_.join();
}

// ------------------------ Helpers ------------------------

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

// ------------------------ RX loop ------------------------

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
        pcap_close(handle);
        running_.store(false);
        return;
    }

    bpf_program fp{};
    if (pcap_compile(handle, &fp, cfg_.bpf, 1, PCAP_NETMASK_UNKNOWN) < 0) {
        emit errorOccurred(QString("pcap_compile failed: %1").arg(pcap_geterr(handle)));
        pcap_close(handle);
        running_.store(false);
        return;
    }
    if (pcap_setfilter(handle, &fp) < 0) {
        emit errorOccurred(QString("pcap_setfilter failed: %1").arg(pcap_geterr(handle)));
        pcap_freecode(&fp);
        pcap_close(handle);
        running_.store(false);
        return;
    }
    pcap_freecode(&fp);

    int linktype = pcap_datalink(handle);
    std::vector<uint16_t> samples(static_cast<size_t>(g_cfg.samples_per_frame));

    while (running_.load(std::memory_order_relaxed)) {
        pcap_pkthdr* hdr = nullptr; const u_char* pkt = nullptr;
        int rc = pcap_next_ex(handle, &hdr, &pkt);
        if (rc == 1) {
            stats_.bytes_rx += hdr->len;

            const u_char* udp_payload = nullptr; size_t udp_len = 0;
            if (!extract_udp_payload(pkt, hdr->caplen, linktype, udp_payload, udp_len)) {
                stats_.frames_drop++; continue;
            }

            (void)print_frame_lengths(pkt, hdr->caplen, linktype);


            if (static_cast<int>(udp_len) != g_cfg.frame_size_bytes) {
                stats_.frames_drop++; continue;
            }

            const uint8_t* frame = reinterpret_cast<const uint8_t*>(udp_payload);
            const uint8_t* payload = frame + g_cfg.header_bytes;

            if (!unpack_payload(payload, samples.data())) {
                stats_.frames_drop++; continue;
            }

            ring_.push_frame(samples.data());
            stats_.frames_rx++;
            emit frameAdvanced(static_cast<quint64>(ring_.snapshot_write_index()));
        } else if (rc == 0) {
            // timeout
            continue;
        } else {
            // error or break
            break;
        }
    }

    pcap_close(handle);
}

