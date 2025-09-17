#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>

// ========================= Protocol & data constants =========================
static constexpr int FRAME_SIZE_BYTES  = 1299;  // total UDP payload length per frame
static constexpr int HEADER_BYTES      = 8;     // non-data header bytes inside UDP payload
static constexpr int PAYLOAD_BYTES     = 1280;  // effective data bytes per frame (10-bit packed)
static constexpr int TAIL_BYTES        = 11;    // non-data tail bytes inside UDP payload
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

// ========================= RAW10 unpack (5 bytes -> 4 samples) =========================
static inline void unpack10bit_1024(const uint8_t* p, uint16_t* out) {
    for (int g = 0, o = 0; g < 256; ++g) {
        const uint8_t b0 = p[g*5 + 0];
        const uint8_t b1 = p[g*5 + 1];
        const uint8_t b2 = p[g*5 + 2];
        const uint8_t b3 = p[g*5 + 3];
        const uint8_t b4 = p[g*5 + 4];
        out[o++] = (uint16_t)( b0 | ((b4 & 0x03) << 8) );
        out[o++] = (uint16_t)( b1 | ((b4 & 0x0C) << 6) );
        out[o++] = (uint16_t)( b2 | ((b4 & 0x30) << 4) );
        out[o++] = (uint16_t)( b3 | ((b4 & 0xC0) << 2) );
    }
}

// ========================= Decoded frame ring (SPSC) =========================
class DecodedFrameRing {
public:
    explicit DecodedFrameRing(size_t frame_capacity)
    : capacity_(frame_capacity), data_(frame_capacity * SAMPLES_PER_FRAME) {
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

// ========================= Envelope (min/max + mean) =========================
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