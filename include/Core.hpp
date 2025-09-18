#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>

// ========================= 可配置的解析参数 =========================
enum class PackMode { RAW10_PACKED, RAW16_LE }; // 可扩展：RAW12、RAW14、MIPI 等

struct ParserConfig {
    int frame_size_bytes  = 1299; // 整个 UDP 负载长度
    int header_bytes      = 8;    // 头部字节
    int payload_bytes     = 1280; // 有效负载字节
    int tail_bytes        = 11;   // 尾部/校验等

    int bits_per_sample   = 10;   // 每采样位数
    int samples_per_frame = 1024; // 每帧采样点数

    PackMode pack         = PackMode::RAW10_PACKED;

    inline uint16_t max_sample() const {
        if (bits_per_sample >= 16) return 0xFFFF;
        return static_cast<uint16_t>((1u << bits_per_sample) - 1u);
    }
};

// 全局配置：在 Core.cpp 中定义为默认值，你可以在任何 cpp 中修改 g_cfg 的字段
extern ParserConfig g_cfg;

// ========================= 运行时统计 =========================
struct RuntimeStats {
    std::atomic<uint64_t> frames_rx{0};
    std::atomic<uint64_t> bytes_rx{0};
    std::atomic<uint64_t> frames_drop{0};
};

// ========================= 解包接口（按 g_cfg.pack） =========================
// 输入：payload 指向数据区起始（已跳过 header_bytes）
// 输出：out 至少 samples_per_frame 个 uint16_t
// 返回：true=成功，false=长度/模式不匹配
bool unpack_payload(const uint8_t* payload, uint16_t* out);

// ========================= 解码后帧环（SPSC） =========================
class DecodedFrameRing {
public:
    explicit DecodedFrameRing(size_t frame_capacity)
    : capacity_(frame_capacity), data_(frame_capacity * static_cast<size_t>(g_cfg.samples_per_frame)) {
        write_index_.store(0, std::memory_order_relaxed);
    }

    void push_frame(const uint16_t* samples) {
        uint64_t w = write_index_.load(std::memory_order_relaxed);
        size_t slot = static_cast<size_t>(w % capacity_);
        uint16_t* dst = &data_[slot * static_cast<size_t>(g_cfg.samples_per_frame)];
        std::memcpy(dst, samples, static_cast<size_t>(g_cfg.samples_per_frame) * sizeof(uint16_t));
        write_index_.store(w + 1, std::memory_order_release);
    }

    uint64_t snapshot_write_index() const { return write_index_.load(std::memory_order_acquire); }
    size_t capacity() const { return capacity_; }

    inline uint16_t get_sample(uint64_t abs_frame_index, int ch) const {
        size_t slot = static_cast<size_t>(abs_frame_index % capacity_);
        return data_[slot * static_cast<size_t>(g_cfg.samples_per_frame) + static_cast<size_t>(ch)];
    }

private:
    size_t capacity_;
    std::vector<uint16_t> data_; // capacity_ * samples_per_frame
    std::atomic<uint64_t> write_index_;
};

// ========================= 包络/平滑（与原逻辑一致） =========================
struct Envelope {
    std::vector<double> x;     // seconds, length = bins
    std::vector<double> ymin;  // min per bin
    std::vector<double> ymax;  // max per bin
    std::vector<double> mean;  // mean per bin
};

Envelope build_envelope(const DecodedFrameRing& ring,
                        uint64_t widx_snapshot,
                        int channel,
                        double fps,
                        double window_seconds,
                        int bins);

void smooth_ema(std::vector<double>& y, double dt_sec, double tau_ms);
void smooth_mavg(std::vector<double>& y, int w);