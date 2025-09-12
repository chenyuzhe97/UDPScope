#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <cmath>

#ifdef _WIN32
  #define NOMINMAX
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

#include <pcap/pcap.h> // <-- libpcap

// ===== ImGui / ImPlot / GLFW / OpenGL =====
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

// ========================= Protocol & data constants =========================
static constexpr int FRAME_SIZE_BYTES  = 1299;  // total frame length
static constexpr int HEADER_BYTES      = 8;     // non-data header (inside UDP payload)
static constexpr int PAYLOAD_BYTES     = 1280;  // effective data bytes per frame
static constexpr int TAIL_BYTES        = 11;    // non-data tail (inside UDP payload)
static_assert(HEADER_BYTES + PAYLOAD_BYTES + TAIL_BYTES == FRAME_SIZE_BYTES, "Frame layout mismatch");

static constexpr int BITS_PER_SAMPLE   = 10;    // 10-bit samples
static constexpr int SAMPLES_PER_FRAME = 1024;  // 10240 bits / 10 = 1024
static constexpr int MAX_CHANNELS      = SAMPLES_PER_FRAME;

// ========================= Runtime stats =========================
struct RuntimeStats {
    std::atomic<uint64_t> frames_rx{0};    // successfully parsed frames
    std::atomic<uint64_t> bytes_rx{0};
    std::atomic<uint64_t> frames_drop{0};  // invalid/short frames
};

// 1024 samples @ 10-bit, packed as RAW10: 5 bytes -> 4 samples
static inline void unpack10bit_1024(const uint8_t* p, uint16_t* out) {
    // 1280 bytes = 256 groups of 5 bytes; each group yields 4 samples
    for (int g = 0, o = 0; g < 256; ++g) {
        const uint8_t b0 = p[g*5 + 0];
        const uint8_t b1 = p[g*5 + 1];
        const uint8_t b2 = p[g*5 + 2];
        const uint8_t b3 = p[g*5 + 3];
        const uint8_t b4 = p[g*5 + 4];
        out[o++] = (uint16_t)( b0 | ((b4 & 0x03) << 8) ); // s0: b0 + b4[1:0]
        out[o++] = (uint16_t)( b1 | ((b4 & 0x0C) << 6) ); // s1: b1 + b4[3:2]
        out[o++] = (uint16_t)( b2 | ((b4 & 0x30) << 4) ); // s2: b2 + b4[5:4]
        out[o++] = (uint16_t)( b3 | ((b4 & 0xC0) << 2) ); // s3: b3 + b4[7:6]
    }
}

// ========================= Decoded frame ring (SPSC) =========================
class DecodedFrameRing {
public:
    explicit DecodedFrameRing(size_t frame_capacity)
    : capacity_(frame_capacity),
      data_(frame_capacity * SAMPLES_PER_FRAME) {
        write_index_.store(0, std::memory_order_relaxed);
    }

    void push_frame(const uint16_t* samples1024) {
        uint64_t w = write_index_.load(std::memory_order_relaxed);
        size_t slot = size_t(w % capacity_);
        uint16_t* dst = &data_[slot * SAMPLES_PER_FRAME];
        std::memcpy(dst, samples1024, SAMPLES_PER_FRAME * sizeof(uint16_t));
        write_index_.store(w + 1, std::memory_order_release);
    }

    uint64_t snapshot_write_index() const {
        return write_index_.load(std::memory_order_acquire);
    }

    size_t capacity() const { return capacity_; }

    inline uint16_t get_sample(uint64_t abs_frame_index, int ch) const {
        size_t slot = size_t(abs_frame_index % capacity_);
        return data_[slot * SAMPLES_PER_FRAME + ch];
    }

private:
    size_t capacity_;
    std::vector<uint16_t> data_; // capacity_ * 1024
    std::atomic<uint64_t> write_index_;
};

// ========================= PCAP capture thread =========================
struct CaptureConfig {
    char ifname[64] = "eth0";                          // interface name
    char bpf[256]   = "udp and src host 12.0.0.1";     // BPF like Wireshark
    bool promisc    = true;
    int  snaplen    = 2048;
    int  timeout_ms = 1;                               // read timeout
};

class PcapReceiver {
public:
    PcapReceiver(DecodedFrameRing& ring, const CaptureConfig& cfg, RuntimeStats& stats)
    : ring_(ring), cfg_(cfg), stats_(stats) {}

    void start() {
        running_.store(true);
        rx_thread_ = std::thread(&PcapReceiver::rx_loop, this);
    }

    void stop() {
        running_.store(false);
        if (rx_thread_.joinable()) rx_thread_.join();
    }

    ~PcapReceiver(){ stop(); }

private:
    static bool extract_udp_payload(const u_char* data, size_t caplen, int linktype,
                                    const u_char*& udp_payload, size_t& udp_payload_len) {
        const u_char* p = data;
        size_t len = caplen;

        // Link-layer
        if (linktype == DLT_EN10MB) { // Ethernet
            if (len < 14) return false;
            const uint16_t eth_type = (p[12] << 8) | p[13];
            size_t off = 14;
            uint16_t type = eth_type;
            // Handle 802.1Q VLAN tags (0x8100 or 0x88a8)
            while (type == 0x8100 || type == 0x88a8) {
                if (len < off + 4) return false;
                type = (p[off + 2] << 8) | p[off + 3];
                off += 4;
            }
            if (type != 0x0800) return false; // IPv4 only here
            p += off; len -= off;
        }
        else if (linktype == DLT_LINUX_SLL || linktype == DLT_LINUX_SLL2) {
            // Linux "cooked" capture
            const size_t sll_len = (linktype == DLT_LINUX_SLL) ? 16 : 20;
            if (len < sll_len) return false;
            p += sll_len; len -= sll_len;
        }
        else if (linktype == DLT_RAW) {
            // already IP
        }
        else {
            // Unsupported link type
            return false;
        }

        // IPv4 header
        if (len < 20) return false;
        const uint8_t ipver = p[0] >> 4;
        if (ipver != 4) return false;
        const uint8_t ihl   = (p[0] & 0x0F) * 4;
        if (len < ihl + 8) return false;
        const uint8_t proto = p[9];
        if (proto != 17) return false; // UDP
        const u_char* udp = p + ihl;
        const uint16_t udp_len = (udp[4] << 8) | udp[5];
        if (udp_len < 8) return false;
        udp_payload = udp + 8;
        // caplen bound
        size_t max_avail = (data + caplen > udp_payload) ? size_t((data + caplen) - udp_payload) : 0;
        udp_payload_len = std::min<size_t>(udp_len - 8, max_avail);
        return true;
    }

    void rx_loop() {
        char errbuf[PCAP_ERRBUF_SIZE] = {0};
        pcap_t* handle = pcap_open_live(cfg_.ifname, cfg_.snaplen, cfg_.promisc ? 1 : 0, cfg_.timeout_ms, errbuf);
        if (!handle) {
            std::fprintf(stderr, "pcap_open_live failed: %s\n", errbuf);
            return;
        }

#if defined(PCAP_ERROR_BREAK)
        // immediate mode (low latency) if available
#ifdef pcap_set_immediate_mode
        pcap_set_immediate_mode(handle, 1);
        pcap_activate(handle);
#endif
#endif

        // compile & set BPF filter
        bpf_program fp{};
        if (pcap_compile(handle, &fp, cfg_.bpf, 1, PCAP_NETMASK_UNKNOWN) < 0) {
            std::fprintf(stderr, "pcap_compile failed: %s\n", pcap_geterr(handle));
            pcap_close(handle);
            return;
        }
        if (pcap_setfilter(handle, &fp) < 0) {
            std::fprintf(stderr, "pcap_setfilter failed: %s\n", pcap_geterr(handle));
            pcap_freecode(&fp);
            pcap_close(handle);
            return;
        }
        pcap_freecode(&fp);

        int linktype = pcap_datalink(handle);
        uint16_t samples[SAMPLES_PER_FRAME];

        while (running_.load(std::memory_order_relaxed)) {
            pcap_pkthdr* hdr = nullptr;
            const u_char* pkt = nullptr;
            int rc = pcap_next_ex(handle, &hdr, &pkt);
            if (rc == 1) {
                stats_.bytes_rx += hdr->len;

                // Extract UDP payload like Wireshark
                const u_char* udp_payload = nullptr;
                size_t udp_len = 0;
                if (!extract_udp_payload(pkt, hdr->caplen, linktype, udp_payload, udp_len)) {
                    stats_.frames_drop++;
                    continue;
                }
                if (udp_len != (size_t)FRAME_SIZE_BYTES) {
                    stats_.frames_drop++;
                    continue;
                }

                const uint8_t* frame = reinterpret_cast<const uint8_t*>(udp_payload);
                const uint8_t* payload = frame + HEADER_BYTES; // skip 8B header
                unpack10bit_1024(payload, samples);
                ring_.push_frame(samples);
                stats_.frames_rx++;
            }
            else if (rc == 0) {
                // timeout; just continue
                continue;
            }
            else {
                // error or break
                break;
            }
        }

        pcap_close(handle);
    }

    std::atomic<bool> running_{false};
    std::thread rx_thread_;
    DecodedFrameRing& ring_;
    CaptureConfig cfg_;
    RuntimeStats& stats_;
};

// ========================= Min/Max envelope downsampling =========================
struct Envelope {
    std::vector<double> x;     // seconds, length = bins
    std::vector<double> ymin;  // min per bin
    std::vector<double> ymax;  // max per bin
};

static Envelope build_envelope(const DecodedFrameRing& ring,
                               uint64_t widx_snapshot,
                               int channel,
                               double fps,
                               double window_seconds,
                               int bins) {
    Envelope env; env.x.resize(bins); env.ymin.resize(bins); env.ymax.resize(bins);

    const uint64_t frames_available = std::min<uint64_t>(widx_snapshot, ring.capacity());
    const uint64_t window_frames    = (uint64_t)std::llround(window_seconds * fps);
    const uint64_t span             = std::min<uint64_t>(window_frames, frames_available);

    if (span == 0) {
        std::fill(env.ymin.begin(), env.ymin.end(), 0.0);
        std::fill(env.ymax.begin(), env.ymax.end(), 0.0);
        for (int i = 0; i < bins; ++i)
            env.x[i] = -window_seconds + (window_seconds * (i + 0.5) / bins);
        return env;
    }

    const uint64_t start_abs = widx_snapshot > span ? (widx_snapshot - span) : 0; // [start_abs, widx)
    const double frames_per_bin = (double)span / bins;

    for (int b = 0; b < bins; ++b) {
        uint64_t f0 = start_abs + (uint64_t)std::floor(b * frames_per_bin);
        uint64_t f1 = start_abs + (uint64_t)std::floor((b + 1) * frames_per_bin);
        if (f1 <= f0) f1 = f0 + 1;

        double vmin = 1e300, vmax = -1e300;
        for (uint64_t f = f0; f < f1; ++f) {
            uint16_t s = ring.get_sample(f, channel);
            double v = (double)s; // apply calibration here if needed
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
        }
        env.ymin[b] = vmin;
        env.ymax[b] = vmax;

        // time axis [-window, 0], left=older, right=newer
        double t_center = -window_seconds + (b + 0.5) * (window_seconds / bins);
        env.x[b] = t_center;
    }

    return env;
}

// ========================= GUI & main =========================
struct UiState {
    bool   running_capture   = false;
    int    max_draw_channels = 8;
    double window_seconds    = 1.0;
    int    bins_per_plot     = 1200;
    bool   show_stats        = true;
    char   search[64]        = "";
    std::vector<bool> selected;  // 1024 checkboxes

    // capture config (editable from UI)
    CaptureConfig capcfg;
};

static void glfw_error_callback(int error, const char* desc) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

int main(int, char**) {
    const size_t RING_FRAMES = 200000; // ~2.048 s @ 20k fps, ~80MB
    DecodedFrameRing ring(RING_FRAMES);
    RuntimeStats stats;

    UiState ui; ui.selected.assign(MAX_CHANNELS, false);
    // default interface guess (eth0/enp... may vary)
    std::snprintf(ui.capcfg.ifname, sizeof(ui.capcfg.ifname), "enp3s0");
    std::snprintf(ui.capcfg.bpf, sizeof(ui.capcfg.bpf),
                "udp and src host 12.0.0.2 and dst host 12.0.0.1 and src port 2827 and dst port 2827 and udp[4:2] = 1307");
    ui.capcfg.promisc = true;
    ui.capcfg.snaplen = 2048;
    ui.capcfg.timeout_ms = 1;

    // graphics init
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;
    const char* glsl_version = "#version 130"; // GL 3.0+

    GLFWwindow* window = glfwCreateWindow(1600, 900, "UDP 10-bit Scope (PCAP, 1024ch)", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    PcapReceiver* rx = nullptr;

    double   last_stat_time  = glfwGetTime();
    uint64_t last_frames_rx  = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ===== Control / Settings =====
        ImGui::Begin("Control / Settings");
        ImGui::Text("Capture (Wireshark-like via libpcap)");
        ImGui::InputText("Interface", ui.capcfg.ifname, IM_ARRAYSIZE(ui.capcfg.ifname));
        ImGui::InputText("BPF Filter", ui.capcfg.bpf, IM_ARRAYSIZE(ui.capcfg.bpf));
        ImGui::Checkbox("Promiscuous", &ui.capcfg.promisc);
        ImGui::SliderInt("Snaplen", &ui.capcfg.snaplen, 256, 65535);
        ImGui::SliderInt("Timeout (ms)", &ui.capcfg.timeout_ms, 0, 50);

        if (!ui.running_capture) {
            if (ImGui::Button("Start Capture")) {
                if (rx) { delete rx; rx = nullptr; }
                rx = new PcapReceiver(ring, ui.capcfg, stats);
                rx->start();
                ui.running_capture = true;
            }
        } else {
            if (ImGui::Button("Stop Capture")) {
                if (rx) { rx->stop(); delete rx; rx = nullptr; }
                ui.running_capture = false;
            }
        }

        ImGui::Separator();
        ImGui::Text("Display");
        ImGui::SliderInt("Max Channels to Draw", &ui.max_draw_channels, 1, 32);
        ImGui::SliderFloat("Window Length (s)", (float*)&ui.window_seconds, 0.05f, 5.0f, "%.2f s");
        ImGui::SliderInt("Points per Channel (bins)", &ui.bins_per_plot, 200, 4000);
        ImGui::Checkbox("Show Stats", &ui.show_stats);

        if (ui.show_stats) {
            double now = glfwGetTime();
            if (now - last_stat_time > 0.5) {
                last_stat_time = now;
            }
            uint64_t frx   = stats.frames_rx.load();
            uint64_t fdrop = stats.frames_drop.load();
            uint64_t brx   = stats.bytes_rx.load();
            double fps_rx  = (double)(frx - last_frames_rx) / 0.5;
            last_frames_rx = frx;

            ImGui::Text("Frames RX: %llu", (unsigned long long)frx);
            ImGui::Text("Frames Dropped: %llu", (unsigned long long)fdrop);
            ImGui::Text("Bytes RX: %llu", (unsigned long long)brx);
            ImGui::Text("Estimated RX Rate: %.0f frames/s", fps_rx);
        }
        ImGui::End();

        // ===== Channel Selection =====
        ImGui::Begin("Channels");
        ImGui::InputText("Filter (e.g., 0,1,2...)", ui.search, IM_ARRAYSIZE(ui.search));
        if (ImGui::Button("Select All")) {
            std::fill(ui.selected.begin(), ui.selected.end(), true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Select None")) {
            std::fill(ui.selected.begin(), ui.selected.end(), false);
        }
        ImGui::Separator();

        ImGui::BeginChild("ch_list", ImVec2(0, 0), true);
        int shown = 0;
        for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
            if (ui.search[0] != '\0') {
                char buf[16]; std::snprintf(buf, sizeof(buf), "%d", ch);
                if (std::strstr(buf, ui.search) == nullptr) continue;
            }
            std::string label = "Ch " + std::to_string(ch);
            bool v = ui.selected[ch];
            if (ImGui::Checkbox((label + "##ch" + std::to_string(ch)).c_str(), &v)) {
                ui.selected[ch] = v;
            }
            if (++shown % 8 != 0) ImGui::SameLine();
        }
        ImGui::EndChild();
        ImGui::End();

        // ===== Waveforms =====
        ImGui::Begin("Waveforms");

        // 1) collect selected channels first
        std::vector<int> draw_list; 
        draw_list.reserve(ui.max_draw_channels);
        for (int ch = 0; ch < MAX_CHANNELS && (int)draw_list.size() < ui.max_draw_channels; ++ch) {
            if (ui.selected[ch]) draw_list.push_back(ch);
        }

        if (draw_list.empty()) {
            ImGui::TextUnformatted("No channels selected.");
        } else {
            // 2) make a reasonable grid: up to 4 columns
            const int n    = (int)draw_list.size();
            const int cols = std::min(4, n);
            const int rows = (n + cols - 1) / cols;

            if (ImPlot::BeginSubplots("plots", rows, cols, ImVec2(-1, -1), ImPlotSubplotFlags_LinkAllX)) {
                // use your current write index and an FPS (constant 20k for now)
                uint64_t widx = ring.snapshot_write_index();
                const double fps = 20000.0;

                for (int ch : draw_list) {
                    if (ImPlot::BeginPlot((std::string("Ch ") + std::to_string(ch)).c_str(), ImVec2(-1, 200))) {
                        auto env = build_envelope(ring, widx, ch, fps, ui.window_seconds, ui.bins_per_plot);

                        ImPlot::SetupAxes("t (s)", "value", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                        ImPlot::PlotShaded("minmax", env.x.data(), env.ymax.data(), env.ymin.data(), (int)env.x.size());

                        std::vector<double> ymid(env.ymin.size());
                        for (size_t i = 0; i < ymid.size(); ++i) ymid[i] = 0.5 * (env.ymin[i] + env.ymax[i]);
                        ImPlot::PlotLine("mid", env.x.data(), ymid.data(), (int)env.x.size());

                        ImPlot::EndPlot();
                    }
                }
                ImPlot::EndSubplots();
            }
        }

        ImGui::End();

        // ===== Render =====
        ImGui::Render();
        int display_w, display_h; glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    if (rx) { rx->stop(); delete rx; rx = nullptr; }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
