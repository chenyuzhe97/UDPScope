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

// ===== ImGui / ImPlot / GLFW / OpenGL =====
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

// ========================= 数据与协议常量 =========================
static constexpr int    FRAME_SIZE_BYTES   = 1299;  // 整帧
static constexpr int    HEADER_BYTES       = 8;     // 前导非数据
static constexpr int    PAYLOAD_BYTES      = 1280;  // 有效数据
static constexpr int    TAIL_BYTES         = 11;    // 末尾非数据
static_assert(HEADER_BYTES + PAYLOAD_BYTES + TAIL_BYTES == FRAME_SIZE_BYTES, "Frame layout mismatch");

static constexpr int    BITS_PER_SAMPLE    = 10;    // 10-bit 量化
static constexpr int    SAMPLES_PER_FRAME  = 1024;  // 10240 bits / 10 = 1024
static constexpr int    MAX_CHANNELS       = SAMPLES_PER_FRAME;

// 默认网络参数
struct NetConfig {
    uint16_t listen_port = 9000;         // 本地监听端口
    char     src_ip_filter[32] = "12.0.0.1"; // 仅接受该源IP（留空则不过滤）
    bool     enable_ip_filter = true;
};

// 统计与控制
struct RuntimeStats {
    std::atomic<uint64_t> frames_rx{0};    // 成功解析的帧数
    std::atomic<uint64_t> bytes_rx{0};
    std::atomic<uint64_t> frames_drop{0};  // 丢弃/错误帧
};

// ========================= 10-bit 解包 =========================
// 输入 1280B payload，输出 1024 个 10-bit 样本（0..1023）。
static inline void unpack10bit_1024(const uint8_t* ptr, uint16_t* out) {
    uint32_t bitpos = 0;
    for (int i = 0; i < SAMPLES_PER_FRAME; ++i, bitpos += BITS_PER_SAMPLE) {
        uint32_t byteIndex = bitpos >> 3;      // /8
        uint32_t bitOffset = bitpos & 7;       // %8
        // 取最多三字节组成 24bit 块
        uint32_t chunk = uint32_t(ptr[byteIndex]) |
                         (uint32_t(ptr[byteIndex + 1]) << 8) |
                         (uint32_t(ptr[byteIndex + 2]) << 16);
        uint16_t sample = (chunk >> bitOffset) & 0x3FFu; // 10位掩码
        out[i] = sample;
    }
}

// ========================= 解码帧环形缓冲（SPSC） =========================
// 保存最近 N 帧的解码结果（每帧 1024 个 uint16）。
class DecodedFrameRing {
public:
    explicit DecodedFrameRing(size_t frame_capacity)
    : capacity_(frame_capacity),
      data_(frame_capacity * SAMPLES_PER_FRAME) {
        write_index_.store(0, std::memory_order_relaxed);
    }

    // 写入一帧（由生产者调用）
    void push_frame(const uint16_t* samples1024) {
        uint64_t w = write_index_.load(std::memory_order_relaxed);
        size_t slot = size_t(w % capacity_);
        uint16_t* dst = &data_[slot * SAMPLES_PER_FRAME];
        std::memcpy(dst, samples1024, SAMPLES_PER_FRAME * sizeof(uint16_t));
        // 发布：先写数据，再递增索引
        write_index_.store(w + 1, std::memory_order_release);
    }

    // 拿到当前写入计数的快照
    uint64_t snapshot_write_index() const {
        return write_index_.load(std::memory_order_acquire);
    }

    size_t capacity() const { return capacity_; }

    // 读取指定帧索引（绝对帧号）的某通道样本
    inline uint16_t get_sample(uint64_t abs_frame_index, int ch) const {
        size_t slot = size_t(abs_frame_index % capacity_);
        return data_[slot * SAMPLES_PER_FRAME + ch];
    }

private:
    size_t capacity_;
    std::vector<uint16_t> data_; // capacity_ * 1024
    std::atomic<uint64_t> write_index_;
};

// ========================= UDP 接收线程 =========================
class UdpReceiver {
public:
    UdpReceiver(DecodedFrameRing& ring, NetConfig cfg, RuntimeStats& stats)
    : ring_(ring), cfg_(cfg), stats_(stats) {}

    void start() {
        running_.store(true);
        rx_thread_ = std::thread(&UdpReceiver::rx_loop, this);
    }

    void stop() {
        running_.store(false);
        if (rx_thread_.joinable()) rx_thread_.join();
    }

    ~UdpReceiver(){ stop(); }

private:
    void rx_loop() {
#ifdef _WIN32
        WSADATA wsaData; WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            std::perror("socket");
            return;
        }

        // 绑定本地端口（接收任何来源的数据，再按源IP过滤）
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(cfg_.listen_port);
        if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::perror("bind");
#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            return;
        }

#ifndef _WIN32
        // 非阻塞
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

        uint8_t buf[2048];
        uint16_t samples[SAMPLES_PER_FRAME];

        // 源IP过滤
        in_addr filter_addr{}; filter_addr.s_addr = 0;
        bool do_filter = cfg_.enable_ip_filter && std::strlen(cfg_.src_ip_filter) > 0;
        if (do_filter) {
            inet_pton(AF_INET, cfg_.src_ip_filter, &filter_addr);
        }

        // 简单轮询接收（也可升级 recvmmsg/epoll/io_uring）
        while (running_.load(std::memory_order_relaxed)) {
            sockaddr_in src{}; socklen_t slen = sizeof(src);
#ifdef _WIN32
            int n = ::recvfrom(fd, (char*)buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
#else
            int n = ::recvfrom(fd, buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
#endif
            if (n < 0) {
                // 没有数据，稍微睡一会降低 CPU
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            stats_.bytes_rx += n;

            if (n < FRAME_SIZE_BYTES) {
                stats_.frames_drop++;
                continue; // 错帧
            }
            if (do_filter && src.sin_addr.s_addr != filter_addr.s_addr) {
                continue; // 丢弃非目标源的帧
            }

            const uint8_t* payload = buf + HEADER_BYTES;
            unpack10bit_1024(payload, samples);
            ring_.push_frame(samples);
            stats_.frames_rx++;
        }

#ifdef _WIN32
        closesocket(fd);
        WSACleanup();
#else
        close(fd);
#endif
    }

    std::atomic<bool> running_{false};
    std::thread rx_thread_;
    DecodedFrameRing& ring_;
    NetConfig cfg_;
    RuntimeStats& stats_;
};

// ========================= 包络降采样（min/max per bin） =========================
struct Envelope {
    std::vector<double> x;     // 相对时间（秒），长度 = bins
    std::vector<float>  ymin;  // 每个 bin 的最小值
    std::vector<float>  ymax;  // 每个 bin 的最大值
};

static Envelope build_envelope(const DecodedFrameRing& ring,
                               uint64_t widx_snapshot,
                               int channel,
                               double fps,
                               double window_seconds,
                               int bins) {
    Envelope env; env.x.resize(bins); env.ymin.resize(bins); env.ymax.resize(bins);

    const uint64_t frames_available = std::min<uint64_t>(widx_snapshot, ring.capacity());
    const uint64_t window_frames = (uint64_t)std::llround(window_seconds * fps);
    const uint64_t span = std::min<uint64_t>(window_frames, frames_available);

    if (span == 0) {
        std::fill(env.ymin.begin(), env.ymin.end(), 0.f);
        std::fill(env.ymax.begin(), env.ymax.end(), 0.f);
        for (int i = 0; i < bins; ++i) env.x[i] = -window_seconds + (window_seconds * (i+0.5) / bins);
        return env;
    }

    const uint64_t start_abs = widx_snapshot > span ? (widx_snapshot - span) : 0; // [start_abs, widx_snapshot)
    const double frames_per_bin = (double)span / bins;

    for (int b = 0; b < bins; ++b) {
        uint64_t f0 = start_abs + (uint64_t)std::floor(b * frames_per_bin);
        uint64_t f1 = start_abs + (uint64_t)std::floor((b + 1) * frames_per_bin);
        if (f1 <= f0) f1 = f0 + 1;

        float vmin = 1e9f, vmax = -1e9f;
        for (uint64_t f = f0; f < f1; ++f) {
            uint16_t s = ring.get_sample(f, channel);
            float v = (float)s; // 可在此处做零位/量纲转换
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
        }
        env.ymin[b] = vmin;
        env.ymax[b] = vmax;
        // 把时间轴设为负向（左旧右新）：[-window, 0]
        double t_center = -window_seconds + (b + 0.5) * (window_seconds / bins);
        env.x[b] = t_center;
    }

    return env;
}

// ========================= GUI & 主程序 =========================
struct UiState {
    bool running_capture = false;
    int  max_draw_channels = 8;       // 同时显示上限
    double window_seconds  = 1.0;     // 观察窗口长度
    int  bins_per_plot     = 1200;    // 每通道绘制点数（min/max 包络）
    bool show_stats        = true;
    char search[64]        = "";      // 通道筛选
    std::vector<bool> selected;        // 1024 个 bool
};

static void glfw_error_callback(int error, const char* desc) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

int main(int, char**) {
    // ===== 数据面板初始化 =====
    const size_t RING_FRAMES = 40960; // ~2.048 秒 @ 20k fps，每帧 2KB，共 ~80MB
    DecodedFrameRing ring(RING_FRAMES);
    RuntimeStats stats;
    NetConfig netcfg; // 默认源IP过滤 12.0.0.1，端口 9000

    UiState ui; ui.selected.assign(MAX_CHANNELS, false);

    // ===== 图形初始化 =====
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;
    const char* glsl_version = "#version 130"; // GL 3.0+

    GLFWwindow* window = glfwCreateWindow(1600, 900, "UDP 10-bit Scope (1024ch)", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // ===== UDP 接收器 =====
    UdpReceiver* rx = nullptr;

    // FPS 统计
    double last_stat_time = glfwGetTime();
    uint64_t last_frames_rx = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ===== 控制面板 =====
        ImGui::Begin("控制 / 设置");
        ImGui::Text("网络");
        ImGui::InputScalar("监听端口", ImGuiDataType_U16, &netcfg.listen_port);
        ImGui::Checkbox("按源IP过滤", &netcfg.enable_ip_filter);
        ImGui::SameLine();
        ImGui::InputText("源IP", netcfg.src_ip_filter, IM_ARRAYSIZE(netcfg.src_ip_filter));
        if (!ui.running_capture) {
            if (ImGui::Button("开始采集")) {
                if (rx) { delete rx; rx = nullptr; }
                rx = new UdpReceiver(ring, netcfg, stats);
                rx->start();
                ui.running_capture = true;
            }
        } else {
            if (ImGui::Button("停止采集")) {
                if (rx) { rx->stop(); delete rx; rx = nullptr; }
                ui.running_capture = false;
            }
        }
        ImGui::Separator();

        ImGui::Text("显示");
        ImGui::SliderInt("同时显示通道数上限", &ui.max_draw_channels, 1, 32);
        ImGui::SliderFloat("窗口长度(秒)", (float*)&ui.window_seconds, 0.05f, 5.0f, "%.2f s");
        ImGui::SliderInt("每通道点数(包络bins)", &ui.bins_per_plot, 200, 4000);
        ImGui::Checkbox("显示统计", &ui.show_stats);

        if (ui.show_stats) {
            double now = glfwGetTime();
            if (now - last_stat_time > 0.5) {
                last_stat_time = now;
            }
            uint64_t frx = stats.frames_rx.load();
            uint64_t fdrop = stats.frames_drop.load();
            uint64_t brx = stats.bytes_rx.load();
            double fps_rx = (double)(frx - last_frames_rx) / 0.5; // 约半秒窗口
            last_frames_rx = frx;

            ImGui::Text("已收帧: %llu", (unsigned long long)frx);
            ImGui::Text("丢弃帧: %llu", (unsigned long long)fdrop);
            ImGui::Text("累计字节: %llu", (unsigned long long)brx);
            ImGui::Text("当前接收速率(估计): %.0f 帧/秒", fps_rx);
        }
        ImGui::End();

        // ===== 通道选择面板 =====
        ImGui::Begin("通道选择");
        ImGui::InputText("筛选(如 0,1,2 或关键字)", ui.search, IM_ARRAYSIZE(ui.search));
        if (ImGui::Button("全选")) {
            std::fill(ui.selected.begin(), ui.selected.end(), true);
        }
        ImGui::SameLine();
        if (ImGui::Button("全不选")) {
            std::fill(ui.selected.begin(), ui.selected.end(), false);
        }
        ImGui::Separator();

        ImGui::BeginChild("ch_list", ImVec2(0, 0), true);
        int shown = 0;
        for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
            // 简单数字筛选（包含关系）
            if (ui.search[0] != '\0') {
                char buf[16]; std::snprintf(buf, sizeof(buf), "%d", ch);
                if (std::strstr(buf, ui.search) == nullptr) continue;
            }
            ImGui::CheckboxFlags(std::string("Ch").append(std::to_string(ch)).c_str(), (unsigned int*)&ui.selected[ch], true);
            if (++shown % 8 != 0) ImGui::SameLine();
        }
        ImGui::EndChild();
        ImGui::End();

        // ===== 波形窗口 =====
        ImGui::Begin("波形");
        if (ImPlot::BeginSubplots("plots", std::min(ui.max_draw_channels, 4), 0, ImVec2(-1, -1), ImPlotSubplotFlags_LinkAllX)) {
            // 收集当前勾选的通道，最多 max_draw_channels
            std::vector<int> draw_list; draw_list.reserve(ui.max_draw_channels);
            for (int ch = 0; ch < MAX_CHANNELS && (int)draw_list.size() < ui.max_draw_channels; ++ch) {
                if (ui.selected[ch]) draw_list.push_back(ch);
            }

            uint64_t widx = ring.snapshot_write_index();
            const double fps = 20000.0; // 20k 帧/秒

            for (int ch : draw_list) {
                if (ImPlot::BeginPlot((std::string("Ch ") + std::to_string(ch)).c_str(), ImVec2(-1, 200))) {
                    auto env = build_envelope(ring, widx, ch, fps, ui.window_seconds, ui.bins_per_plot);
                    // 用 Shaded 画出 min/max 包络
                    ImPlot::SetupAxes("t (s)", "value", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                    ImPlot::PlotShadedG("minmax",
                        [](int idx, void* data) { return ((Envelope*)data)->x[idx]; },
                        [](int idx, void* data) { return ((Envelope*)data)->ymax[idx]; },
                        [](int idx, void* data) { return ((Envelope*)data)->ymin[idx]; },
                        (void*)&env, env.x.size());
                    // 再画一条中线（(min+max)/2）
                    std::vector<float> ymid(env.ymin.size());
                    for (size_t i = 0; i < ymid.size(); ++i) ymid[i] = 0.5f*(env.ymin[i]+env.ymax[i]);
                    ImPlot::PlotLineG("mid",
                        [](int idx, void* data) { return ((Envelope*)data)->x[idx]; },
                        [](int idx, void* data) { return ((std::vector<float>*)data)->at(idx); },
                        (void*)&env, (void*)&ymid, env.x.size());
                    ImPlot::EndPlot();
                }
            }
            ImPlot::EndSubplots();
        }
        ImGui::End();

        // ===== 渲染 =====
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