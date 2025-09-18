#include "Core.hpp"

ParserConfig g_cfg{}; // 默认值即为原先的常量，可在运行时修改其字段

// 专用：RAW10 (5 bytes -> 4 samples)
static inline bool unpack10bit_block(const uint8_t* p, int groups, uint16_t* out) {
    for (int g = 0, o = 0; g < groups; ++g) {
        const uint8_t b0 = p[g*5 + 0];
        const uint8_t b1 = p[g*5 + 1];
        const uint8_t b2 = p[g*5 + 2];
        const uint8_t b3 = p[g*5 + 3];
        const uint8_t b4 = p[g*5 + 4];
        out[o++] = static_cast<uint16_t>( b0 | ((b4 & 0x03) << 8) );
        out[o++] = static_cast<uint16_t>( b1 | ((b4 & 0x0C) << 6) );
        out[o++] = static_cast<uint16_t>( b2 | ((b4 & 0x30) << 4) );
        out[o++] = static_cast<uint16_t>( b3 | ((b4 & 0xC0) << 2) );
    }
    return true;
}

bool unpack_payload(const uint8_t* payload, uint16_t* out) {
    if (g_cfg.pack == PackMode::RAW10_PACKED) {
        // 校验：payload_bytes 必须是 5 的倍数，且 groups*4 == samples_per_frame
        if (g_cfg.payload_bytes % 5 != 0) return false;
        const int groups = g_cfg.payload_bytes / 5;
        if (groups * 4 != g_cfg.samples_per_frame) return false;
        return unpack10bit_block(payload, groups, out);
    }
    else if (g_cfg.pack == PackMode::RAW16_LE) {
        // 小端 16bit 按样本数直接解包
        const int need_bytes = g_cfg.samples_per_frame * 2;
        if (g_cfg.payload_bytes < need_bytes) return false;
        for (int i = 0; i < g_cfg.samples_per_frame; ++i) {
            out[i] = static_cast<uint16_t>(payload[2*i] | (payload[2*i + 1] << 8));
        }
        return true;
    }
    // TODO: RAW12/RAW14/MIPI 等模式可在此扩展
    return false;
}

Envelope build_envelope(const DecodedFrameRing& ring,
                        uint64_t widx_snapshot,
                        int channel,
                        double fps,
                        double window_seconds,
                        int bins) {
    Envelope env; env.x.resize(bins); env.ymin.resize(bins); env.ymax.resize(bins); env.mean.resize(bins);

    const uint64_t frames_available = std::min<uint64_t>(widx_snapshot, ring.capacity());
    const uint64_t window_frames    = (uint64_t)std::llround(window_seconds * fps);
    const uint64_t span             = std::min<uint64_t>(window_frames, frames_available);

    if (span == 0) {
        std::fill(env.ymin.begin(), env.ymin.end(), 0.0);
        std::fill(env.ymax.begin(), env.ymax.end(), 0.0);
        std::fill(env.mean.begin(), env.mean.end(), 0.0);
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

        double vmin = 1e300, vmax = -1e300, sum = 0.0;
        int count = 0;
        for (uint64_t f = f0; f < f1; ++f) {
            double v = (double)ring.get_sample(f, channel);
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
            sum += v; ++count;
        }
        env.ymin[b] = vmin;
        env.ymax[b] = vmax;
        env.mean[b] = sum / std::max(1, count);

        double t_center = -window_seconds + (b + 0.5) * (window_seconds / bins);
        env.x[b] = t_center;
    }

    return env;
}

void smooth_ema(std::vector<double>& y, double dt_sec, double tau_ms) {
    if (y.empty()) return;
    double tau = std::max(1e-6, tau_ms / 1000.0);
    double alpha = 1.0 - std::exp(-dt_sec / tau);
    double s = y[0];
    for (size_t i = 1; i < y.size(); ++i) {
        s = alpha * y[i] + (1.0 - alpha) * s;
        y[i] = s;
    }
}

void smooth_mavg(std::vector<double>& y, int w) {
    if (y.empty()) return;
    if (w < 1) w = 1;
    if (w % 2 == 0) ++w; // odd window
    int half = w / 2;
    size_t n = y.size();
    std::vector<double> out(n);
    for (int i = 0; i < (int)n; ++i) {
        int L = std::max(0, i - half), R = std::min((int)n - 1, i + half);
        double acc = 0.0; int cnt = 0;
        for (int j = L; j <= R; ++j) { acc += y[j]; ++cnt; }
        out[i] = acc / std::max(1, cnt);
    }
    y.swap(out);
}