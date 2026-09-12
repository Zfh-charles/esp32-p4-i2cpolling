#include "idle_flash_band_overlay.h"

#include "assets.h"
#include "idle_output_probe.h"
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_memory_utils.h>

#ifndef S1GY_IDLE_OUTPUT_PROBE
#define S1GY_IDLE_OUTPUT_PROBE 1
#endif


namespace {
#if S1GY_IDLE_OUTPUT_PROBE
IdleOutputProbe output_probe;
lv_display_t* probe_display = nullptr; // Identity only; no retained object/pixel pointer.
DRAM_ATTR const char kProbeTag[] = "FaceView";
DRAM_ATTR const char kShowProbe[] =
    "I (%lu) FaceView: seq=%lu f=%u visible=%u parent_hidden=%u xy=%d,%d wh=%d,%d src=%08lx\n";
DRAM_ATTR const char kFlushProbe[] =
    "I (%lu) FaceView: flush seq=%lu f=%lu reason=%lu xy=%d,%d,%d,%d full=%u hash=%08lx src=%08lx match=%u\n";
#endif
}  // namespace


bool IdleFlashBandOverlay::Initialize(lv_obj_t* parent) {
    if (image_ != nullptr) {
        return true;
    }
    if (parent == nullptr) {
        return false;
    }

    auto& assets = Assets::GetInstance();
    for (uint8_t i = 0; i < kFrameCount; ++i) {
        // Avoid permanent pointer/string tables in the P4 pre-IROM window.
        char frame_name[] = {'i', 'd', 'l', '0', '.', 'r', 'g', 'b', '\0'};
        frame_name[3] = static_cast<char>('0' + i);
        void* data = nullptr;
        size_t size = 0;
        if (!assets.GetAssetData(frame_name, data, size) || data == nullptr ||
            size != kFrameBytes) {
            return false;
        }
        auto& descriptor = descriptors_[i];
        descriptor = {};
        descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        descriptor.header.w = kWidth;
        descriptor.header.h = kRows;
        descriptor.header.stride = kWidth * 2U;
        descriptor.data_size = size;
        descriptor.data = static_cast<const uint8_t*>(data);
    }

    image_ = lv_image_create(parent);
    if (image_ == nullptr) {
        return false;
    }
    lv_image_set_src(image_, &descriptors_[kFrameCount - 1]);
    lv_obj_set_pos(image_, 0, kY);
    lv_obj_set_size(image_, kWidth, kRows);
    lv_obj_clear_flag(image_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_image_opa(image_, LV_OPA_COVER, 0);
    return true;
}


bool IdleFlashBandOverlay::ShowFrame(uint8_t frame) {
    if (image_ == nullptr || frame >= kFrameCount) {
        return false;
    }
    lv_image_set_src(image_, &descriptors_[frame]);
    lv_obj_clear_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(image_);
#if S1GY_IDLE_OUTPUT_PROBE
    if (output_probe.CanStart()) {
        lv_area_t coords{};
        lv_obj_get_coords(image_, &coords);
        const uint32_t now = esp_log_timestamp();
        const uint32_t hash = IdleOutputProbe::Hash(descriptors_[frame].data, kFrameBytes);
        const uint32_t seq = output_probe.Start(frame,
            {coords.x1, coords.y1, coords.x2, coords.y2}, hash, now);
        probe_display = lv_obj_get_display(image_);
        lv_obj_t* parent = lv_obj_get_parent(image_);
        esp_log_write(ESP_LOG_INFO, kProbeTag, kShowProbe, static_cast<unsigned long>(now),
            static_cast<unsigned long>(seq), static_cast<unsigned>(frame),
            lv_obj_is_visible(image_) ? 1U : 0U,
            parent && lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN) ? 1U : 0U,
            static_cast<int>(coords.x1), static_cast<int>(coords.y1),
            static_cast<int>(coords.x2 - coords.x1 + 1), static_cast<int>(coords.y2 - coords.y1 + 1),
            static_cast<unsigned long>(hash));
    } else {
        output_probe.Cancel();
    }
#endif
    return true;
}

void IdleFlashBandOverlay::TraceFlush(lv_display_t* display, const lv_area_t* area) {
#if S1GY_IDLE_OUTPUT_PROBE
    if (!output_probe.Active() || display != probe_display || !area) return;
    // In this LVGL version, PARTIAL refr_area binds layer->draw_buf=buf_act;
    // FLUSH_START is emitted before byte swap/driver and before buffer rotation.
    // Require a reshaped small RGB565 buffer, never a full-frame/direct canvas.
    const lv_draw_buf_t* buffer = lv_display_get_buf_active(display);
    bool supported = buffer && buffer->data && buffer->data_size > 0 &&
        buffer->data_size <= IdleOutputProbe::kMaxBufferBytes &&
        buffer->header.cf == LV_COLOR_FORMAT_RGB565 &&
        static_cast<int32_t>(buffer->header.w) == area->x2 - area->x1 + 1 &&
        static_cast<int32_t>(buffer->header.h) == area->y2 - area->y1 + 1 &&
        lv_display_get_horizontal_resolution(display) == kWidth &&
        lv_display_get_vertical_resolution(display) == 480 &&
        lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_0 &&
        lv_display_get_offset_x(display) == 0 && lv_display_get_offset_y(display) == 0;
    if (supported) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(buffer->data);
        supported = address <= UINTPTR_MAX - (buffer->data_size - 1U) &&
            esp_ptr_internal(buffer->data) &&
            esp_ptr_internal(reinterpret_cast<const void*>(address + buffer->data_size - 1U));
    }
    IdleOutputProbe::Result result;
    const uint32_t now = esp_log_timestamp();
    if (output_probe.Observe({area->x1, area->y1, area->x2, area->y2},
            buffer ? buffer->data : nullptr, buffer ? buffer->data_size : 0,
            buffer ? buffer->header.stride : 0, supported, now, result)) {
        esp_log_write(ESP_LOG_INFO, kProbeTag, kFlushProbe, static_cast<unsigned long>(now),
            static_cast<unsigned long>(result.seq), static_cast<unsigned long>(result.frame),
            static_cast<unsigned long>(result.reason), static_cast<int>(result.area.x1),
            static_cast<int>(result.area.y1), static_cast<int>(result.area.x2), static_cast<int>(result.area.y2),
            result.full ? 1U : 0U, static_cast<unsigned long>(result.hash),
            static_cast<unsigned long>(result.source_hash), result.match ? 1U : 0U);
    }
#else
    (void)display;
    (void)area;
#endif
}

void IdleFlashBandOverlay::Hide() {
#if S1GY_IDLE_OUTPUT_PROBE
    output_probe.Cancel();
#endif
    if (image_ != nullptr) {
        lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    }
}


void IdleFlashBandOverlay::Detach() {
#if S1GY_IDLE_OUTPUT_PROBE
    output_probe.Cancel();
#endif
    image_ = nullptr;
}
