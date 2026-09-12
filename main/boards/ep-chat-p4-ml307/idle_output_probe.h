#pragma once

#include <cstddef>
#include <cstdint>

// Diagnostic bookkeeping only. Caller serializes with the existing LVGL lock.
// No pixel pointers are retained. A match is pre-driver evidence, not scanout.
class IdleOutputProbe {
public:
    struct Area { int32_t x1, y1, x2, y2; };
    struct Result {
        uint32_t seq = 0, frame = 0, hash = 0, source_hash = 0, reason = 0;
        Area area{};
        bool full = false, match = false;
    };
    static constexpr uint32_t kMaxFrames = 8, kMaxFlushes = 16;
    static constexpr size_t kMaxBufferBytes = 480U * 50U * 2U;

    static uint32_t Hash(const uint8_t* p, size_t n, uint32_t h = 2166136261U) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619U;
        return h;
    }
    bool CanStart() const { return seq_ < kMaxFrames; }
    bool Active() const { return active_ && flushes_ < kMaxFlushes; }
    uint32_t Start(uint32_t frame, Area area, uint32_t hash, uint32_t now) {
        active_ = false;
        if (!CanStart()) return 0;
        ++seq_;
        frame_ = frame; area_ = area; source_hash_ = hash; start_ = now;
        active_ = Valid(area) && area.x2 - area.x1 + 1 <= 480 && area.y2 - area.y1 + 1 <= 48;
        return seq_;
    }
    void Cancel() { active_ = false; }

    // Caller verifies these are the active PARTIAL RGB565 internal draw bytes.
    // Unsupported layouts still yield a bounded reason record without reading.
    bool Observe(Area flush, const uint8_t* pixels, size_t size, uint32_t stride,
                 bool supported_internal_buffer, uint32_t now, Result& out) {
        if (!Active()) return false;
        if (now - start_ > 1000U) { Cancel(); return false; }
        if (!Valid(flush)) return false;
        Area hit{Max(area_.x1, flush.x1), Max(area_.y1, flush.y1),
                 Min(area_.x2, flush.x2), Min(area_.y2, flush.y2)};
        if (hit.x1 > hit.x2 || hit.y1 > hit.y2) return false;
        ++flushes_;
        out = {};
        out.seq = seq_; out.frame = frame_; out.source_hash = source_hash_; out.area = hit;
        if (!supported_internal_buffer) { out.reason = 1; return true; }
        const size_t width = static_cast<size_t>(flush.x2 - flush.x1 + 1);
        const size_t height = static_cast<size_t>(flush.y2 - flush.y1 + 1);
        if (!pixels || size > kMaxBufferBytes || width > 480 || height > 50 ||
            stride < width * 2U || stride > 960U ||
            (height - 1U) * stride + width * 2U > size) {
            out.reason = 2; return true;
        }
        uint32_t hash = 2166136261U;
        const size_t row_bytes = static_cast<size_t>(hit.x2 - hit.x1 + 1) * 2U;
        for (int32_t y = hit.y1; y <= hit.y2; ++y) {
            const size_t offset = static_cast<size_t>(y - flush.y1) * stride +
                                  static_cast<size_t>(hit.x1 - flush.x1) * 2U;
            hash = Hash(pixels + offset, row_bytes, hash);
        }
        out.hash = hash;
        out.full = hit.x1 == area_.x1 && hit.y1 == area_.y1 && hit.x2 == area_.x2 && hit.y2 == area_.y2;
        out.match = out.full && hash == source_hash_;
        return true;
    }
private:
    static int32_t Min(int32_t a, int32_t b) { return a < b ? a : b; }
    static int32_t Max(int32_t a, int32_t b) { return a > b ? a : b; }
    static bool Valid(Area a) {
        return a.x1 >= -4096 && a.y1 >= -4096 && a.x2 <= 4096 && a.y2 <= 4096 &&
               a.x1 <= a.x2 && a.y1 <= a.y2;
    }
    Area area_{};
    uint32_t seq_ = 0, frame_ = 0, source_hash_ = 0, start_ = 0, flushes_ = 0;
    bool active_ = false;
};
