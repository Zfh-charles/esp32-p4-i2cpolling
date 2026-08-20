#include "screen_presenter.h"
#include "eezui_display_adapter.h"
#include "face_route_v2.h"

#include <cstring>
#include <strings.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lvgl.h"
#include "reminder/boot_trace.h"
#include "src/draw/lv_draw_buf.h"
#include "src/others/snapshot/lv_snapshot.h"

#define TAG "ScreenPresenter"

ScreenPresenter::ScreenPresenter(EezuiDisplayAdapter* host) : host_(host) {
    esp_timer_create_args_t args = {
        .callback = &ScreenPresenter::PresentTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "screen_present",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &present_timer_) != ESP_OK) {
        present_timer_ = nullptr;
    }
    ESP_LOGW(TAG, "CTRL PRESENT init stage=%d compose_blit=%d (C2c=I4b pixels)",
             Stage(), UseComposeBlit() ? 1 : 0);
}

ScreenPresenter::~ScreenPresenter() {
    if (present_timer_ != nullptr) {
        esp_timer_stop(present_timer_);
        esp_timer_delete(present_timer_);
        present_timer_ = nullptr;
    }
    FreePresentBuf();
    FreeOverlayCache();
    if (face_rgb_ != nullptr) {
        heap_caps_free(face_rgb_);
        face_rgb_ = nullptr;
    }
}

void ScreenPresenter::EnterConversation() {
    // C2+: conversation state. Default C2c pixels = I4b (no compose hold).
    if (!StageAtLeast(2)) {
        ESP_LOGW(TAG, "CTRL PRESENT enter skipped stage=%d (need>=2)", Stage());
        return;
    }
    if (mode_ == Mode::kPresentConversation) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    esp_rom_printf("!!PRESENT_ENTER core=%d compose=%d\n", (int)xPortGetCoreID(),
                   UseComposeBlit() ? 1 : 0);
    BootTraceMark("PRESENT_ENTER", "begin");
    ESP_LOGW(TAG, "CTRL PRESENT DIAG enter begin core=%d compose=%d",
             (int)xPortGetCoreID(), UseComposeBlit() ? 1 : 0);

    mode_ = Mode::kPresentConversation;
    // C2c: keep I4b emotion bypass; LVGL owns captions between blits.
    legacy_passthrough_ = !UseComposeBlit();
    conversation_since_us_ = t0;
    last_soak_log_us_ = conversation_since_us_;
    present_count_ = 0;

    if (!UseComposeBlit()) {
        if (host_ != nullptr) {
            const esp_err_t serr = host_->PresenterSeedFaceToScreen();
            ESP_LOGW(TAG, "CTRL PRESENT DIAG enter c2c seed err=%s total_ms=%d",
                     esp_err_to_name(serr), (int)((esp_timer_get_time() - t0) / 1000));
        }
        BootTraceMark("PRESENT_ENTER", "done_c2c");
        return;
    }

    BootTraceMark("PRESENT_BUF", "alloc");
    const int64_t tb = esp_timer_get_time();
    EnsurePresentBuf();
    if (host_ != nullptr && (face_rgb_ == nullptr || face_size_ == 0)) {
        const uint8_t* seed = nullptr;
        uint32_t seed_sz = 0, seed_w = 0, seed_h = 0;
        if (host_->PresenterLastFace(&seed, &seed_sz, &seed_w, &seed_h)) {
            CacheFace(seed, seed_sz, seed_w, seed_h);
            ESP_LOGW(TAG, "CTRL PRESENT DIAG seed_face %ux%u", (unsigned)seed_w,
                     (unsigned)seed_h);
        }
    }
    ESP_LOGW(TAG, "CTRL PRESENT DIAG buf_ms=%d px=%u",
             (int)((esp_timer_get_time() - tb) / 1000), (unsigned)present_px_);

    BootTraceMark("PRESENT_STOP", "lvgl_stop");
    if (host_ != nullptr) {
        host_->PresenterAcquirePanel();
    }
    dirty_face_ = (face_rgb_ != nullptr && face_size_ > 0);
    dirty_text_ = true;
    CommitPresent("enter");
    BootTraceMark("PRESENT_ENTER", "done");
    ESP_LOGW(TAG, "CTRL PRESENT DIAG enter done total_ms=%d",
             (int)((esp_timer_get_time() - t0) / 1000));
}

void ScreenPresenter::LeaveConversation() {
    if (!StageAtLeast(2)) {
        return;
    }
    if (mode_ != Mode::kPresentConversation) {
        return;
    }
    if (present_timer_ != nullptr) {
        esp_timer_stop(present_timer_);
    }
    mode_ = Mode::kLegacyLvgl;
    legacy_passthrough_ = true;
    speech_turn_active_ = false;
    const int64_t dur_ms = (esp_timer_get_time() - conversation_since_us_) / 1000;
    ESP_LOGW(TAG, "CTRL PRESENT mode=legacy passthrough=1 soak_ms=%d presents=%u",
             (int)dur_ms, (unsigned)present_count_);
    // No sync restore/refr here — goodbye→idle arms wake; refr + emotion_decode
    // CONTEND_STALL kills AFE (neither hear nor speak after sad).
    if (host_ != nullptr) {
        host_->PresenterReleasePanel(false);
    }
}

uint32_t ScreenPresenter::NotifySpeechState(bool speaking) {
    if (!speaking) {
        speech_turn_active_ = false;
        return 0;
    }
    if (speech_turn_active_) {
        return 0;
    }
    speech_turn_active_ = true;
    ++speech_generation_;
    if (speech_generation_ == 0) {
        ++speech_generation_;
    }
    ESP_LOGW(TAG, "s1es speech_generation=%u", (unsigned)speech_generation_);
    esp_rom_printf("!!FACE_S1ES speech_gen=%u\n", (unsigned)speech_generation_);
    if (!emotion_pending_.empty() && emotion_pending_generation_ != speech_generation_) {
        ESP_LOGW(TAG, "s1ew pending_migrate emo=%s old_gen=%u new_gen=%u",
                 emotion_pending_.c_str(), (unsigned)emotion_pending_generation_,
                 (unsigned)speech_generation_);
        emotion_pending_generation_ = speech_generation_;
    }
    return speech_generation_;
}

void ScreenPresenter::NotifyEmotion(const char* emotion_name) {
    if (!emotion_name) {
        emotion_name = "neutral";
    }
    emotion_name_ = emotion_name;
    dirty_face_ = true;
    ESP_LOGW(TAG, "CTRL PRESENT notify emotion=%s mode=%d pass=%d", emotion_name,
             static_cast<int>(mode_), legacy_passthrough_ ? 1 : 0);
    ESP_LOGW(TAG, "SAD_DIAG PRESENT notify emo=%s mode=%d compose=%d task=%s", emotion_name,
             static_cast<int>(mode_), UseComposeBlit() ? 1 : 0, pcTaskGetName(nullptr));

    if (host_ == nullptr) {
        ESP_LOGW(TAG, "SAD_DIAG PRESENT notify abort=no_host emo=%s", emotion_name);
        return;
    }

    // s1cn-b: pending overwrite; commit only at safe points (or immediately if idle).
    if (FaceRouteV2_Emotion3Enabled()) {
        emotion_pending_ = emotion_name;
        emotion_pending_generation_ = speech_generation_;
        ESP_LOGW(TAG, "s1es requested→pending emo=%s gen=%u committed=%s cgen=%u",
                 emotion_name, (unsigned)emotion_pending_generation_,
                 emotion_committed_.empty() ? "-" : emotion_committed_.c_str(),
                 (unsigned)emotion_committed_generation_);
        esp_rom_printf("!!FACE_S1CN b=pending emo=%s\n", emotion_name);
        if (!host_->PresenterIsFaceRoiAnimActive()) {
            FlushPendingEmotion("notify_idle");
        } else {
            ESP_LOGW(TAG, "s1cn-b defer commit emo=%s (roi_active)", emotion_name);
        }
        return;
    }

    const int64_t t0 = esp_timer_get_time();
    ESP_LOGW(TAG, "CTRL PRESENT commit emo=%s via=presenter s1bj", emotion_name);
    host_->PresenterPlayEmotion(emotion_name);
    ESP_LOGW(TAG, "SAD_DIAG PRESENT play_done emo=%s cost_ms=%d", emotion_name,
             (int)((esp_timer_get_time() - t0) / 1000));
}

void ScreenPresenter::FlushPendingEmotion(const char* why) {
    if (!FaceRouteV2_Emotion3Enabled() || host_ == nullptr) {
        return;
    }
    if (emotion_pending_.empty()) {
        return;
    }
    if (!emotion_committed_.empty() &&
        strcasecmp(emotion_pending_.c_str(), emotion_committed_.c_str()) == 0 &&
        emotion_pending_generation_ == emotion_committed_generation_) {
        ESP_LOGW(TAG, "s1es dedupe_same_generation emo=%s gen=%u",
                 emotion_pending_.c_str(), (unsigned)emotion_pending_generation_);
        emotion_pending_.clear();
        return;
    }
    const std::string emo = emotion_pending_;
    const uint32_t generation = emotion_pending_generation_;
    emotion_pending_.clear();
    emotion_committed_ = emo;
    emotion_committed_generation_ = generation;
    emotion_name_ = emo;
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGW(TAG, "s1es commit emo=%s gen=%u why=%s via=presenter", emo.c_str(),
             (unsigned)generation, why ? why : "-");
    esp_rom_printf("!!FACE_S1CN b=commit emo=%s\n", emo.c_str());
    host_->PresenterPlayEmotion(emo.c_str());
    ESP_LOGW(TAG, "SAD_DIAG PRESENT play_done emo=%s cost_ms=%d s1cn-b", emo.c_str(),
             (int)((esp_timer_get_time() - t0) / 1000));
}

void ScreenPresenter::NotifyDialogue(const char* role, const char* content) {
    if (!content || content[0] == '\0') {
        return;
    }
    dialogue_text_ = content;
    caption_role_ = role ? role : "";
    dirty_text_ = true;

    // L3: queue only while Emotion owns panel; LVGL ROI MID → caption_live.
    if (StageAtLeast(1) && host_ != nullptr && host_->PresenterIsBypassAnimActive()) {
        queued_dialogue_ = content;
        queued_role_ = role ? role : "";
        queued_dialogue_pending_ = true;
        ESP_LOGW(TAG, "CTRL PRESENT dialogue queued (panel hold) len=%u s1an",
                 (unsigned)dialogue_text_.size());
        if (IsConversationMode() && UseComposeBlit()) {
            SchedulePresent("dialogue_queued");
        }
        return;
    }

    // s1bk: during FACE_SLICE ROI, defer paint to PresentFaceFrameToLvgl coalesce.
    if (host_ != nullptr && host_->PresenterIsFaceRoiAnimActive()) {
        ESP_LOGW(TAG, "SAD_DIAG L3 caption_defer coalesce role=%s len=%u s1bk",
                 role ? role : "-", (unsigned)dialogue_text_.size());
        return;
    }

    ApplyDialogueToLvglNow(role, content);
    dirty_text_ = false;
    ESP_LOGW(TAG, "SAD_DIAG L3 caption_live role=%s len=%u s1an", role ? role : "-",
             (unsigned)dialogue_text_.size());
    if (IsConversationMode() && UseComposeBlit()) {
        SchedulePresent("dialogue");
        return;
    }
    ESP_LOGW(TAG, "CTRL PRESENT dialogue apply len=%u stage=%d compose=%d",
             (unsigned)dialogue_text_.size(), Stage(), UseComposeBlit() ? 1 : 0);
}

void ScreenPresenter::NotifyStatusPhase(const char* phase_label) {
    if (!phase_label) {
        return;
    }
    status_phase_ = phase_label;
    dialogue_text_ = phase_label;
    caption_role_ = "status";
    dirty_text_ = true;

    if (StageAtLeast(1) && host_ != nullptr && host_->PresenterIsBypassAnimActive()) {
        queued_dialogue_ = phase_label;
        queued_role_ = "status";
        queued_dialogue_pending_ = true;
        ESP_LOGW(TAG, "CTRL PRESENT status queued (panel hold) s1an");
        if (IsConversationMode() && UseComposeBlit()) {
            SchedulePresent("status_queued");
        }
        return;
    }

    if (host_ != nullptr && host_->PresenterIsFaceRoiAnimActive()) {
        ESP_LOGW(TAG, "SAD_DIAG L3 status_defer coalesce len=%u s1bk",
                 (unsigned)strlen(phase_label));
        return;
    }

    ApplyDialogueToLvglNow("status", phase_label);
    dirty_text_ = false;
    ESP_LOGW(TAG, "SAD_DIAG L3 caption_live role=status len=%u s1an",
             (unsigned)strlen(phase_label));
    if (IsConversationMode() && UseComposeBlit()) {
        SchedulePresent("status");
    }
}

void ScreenPresenter::OnFaceFrameCached(const uint8_t* rgb, uint32_t size, uint32_t w,
                                        uint32_t h) {
    if (!StageAtLeast(2)) {
        return;
    }
    CacheFace(rgb, size, w, h);
    dirty_face_ = true;
    if (IsConversationMode() && UseComposeBlit()) {
        SchedulePresent("face");
    }
}

void ScreenPresenter::OnBypassAnimEnded() {
    if (!StageAtLeast(1)) {
        return;
    }
    if (queued_dialogue_pending_) {
        ApplyDialogueToLvglNow(queued_role_.c_str(), queued_dialogue_.c_str());
        ClearQueuedDialogue();
        dirty_text_ = false;
    } else if (dirty_text_ && !dialogue_text_.empty()) {
        // s1cr: flush deferred assistant text after enter_hold / slice end.
        const char* role = caption_role_.empty() ? "assistant" : caption_role_.c_str();
        ApplyDialogueToLvglNow(role, dialogue_text_.c_str());
        dirty_text_ = false;
        ESP_LOGW(TAG, "SAD_DIAG L3 caption_flush_end role=%s len=%u s1cr", role,
                 (unsigned)dialogue_text_.size());
        esp_rom_printf("!!FACE_CAPTION flush_end s1cr\n");
    }
    if (IsConversationMode() && UseComposeBlit()) {
        CommitPresent("anim_end");
    }
}

bool ScreenPresenter::FlushCaptionIfDirty() {
    if (!dirty_text_ || dialogue_text_.empty() || host_ == nullptr) {
        return false;
    }
    const char* role = caption_role_.empty() ? "assistant" : caption_role_.c_str();
    host_->PresenterApplyDialogueAssumingLock(role, dialogue_text_.c_str());
    dirty_text_ = false;
    ESP_LOGW(TAG, "SAD_DIAG L3 caption_coalesce role=%s len=%u s1bk", role,
             (unsigned)dialogue_text_.size());
    return true;
}

void ScreenPresenter::ClearQueuedDialogue() {
    queued_dialogue_pending_ = false;
    queued_dialogue_.clear();
    queued_role_.clear();
}

void ScreenPresenter::SchedulePresent(const char* reason) {
    if (!IsConversationMode() || !UseComposeBlit()) {
        return;
    }
    // Always async via esp_timer — never Commit under LVGL task lock (typewriter).
    if (present_timer_ == nullptr) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = (last_present_us_ > 0) ? (now - last_present_us_) : kMinPresentIntervalUs;
    uint64_t wait_us = 1000;  // min 1ms
    if (elapsed < kMinPresentIntervalUs) {
        wait_us = (uint64_t)(kMinPresentIntervalUs - elapsed);
        if (wait_us < 1000) {
            wait_us = 1000;
        }
    }
    esp_timer_stop(present_timer_);
    esp_timer_start_once(present_timer_, wait_us);
    ESP_LOGW(TAG, "CTRL PRESENT schedule wait_us=%u reason=%s",
             (unsigned)wait_us, reason ? reason : "-");
}

esp_err_t ScreenPresenter::CommitPresent(const char* reason) {
    if (host_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!IsConversationMode() || !UseComposeBlit()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dirty_face_ && !dirty_text_ && face_rgb_ == nullptr) {
        return ESP_OK;
    }
    if (!EnsurePresentBuf()) {
        ESP_LOGW(TAG, "CTRL PRESENT commit fail=no_buf reason=%s", reason ? reason : "-");
        return ESP_ERR_NO_MEM;
    }

    const int64_t t0 = esp_timer_get_time();
    BootTraceMark("PRESENT_COMPOSE", reason ? reason : "-");
    esp_rom_printf("!!PRESENT_COMPOSE %s core=%d\n", reason ? reason : "-",
                   (int)xPortGetCoreID());
    ESP_LOGW(TAG, "CTRL PRESENT DIAG commit begin reason=%s core=%d dirty_f=%d dirty_t=%d",
             reason ? reason : "-", (int)xPortGetCoreID(), dirty_face_ ? 1 : 0,
             dirty_text_ ? 1 : 0);

    esp_err_t err = RenderCompose();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CTRL PRESENT render fail err=%s reason=%s", esp_err_to_name(err),
                 reason ? reason : "-");
        return err;
    }

    BootTraceMark("PRESENT_BLIT", reason ? reason : "-");
    esp_rom_printf("!!PRESENT_BLIT\n");
    const int64_t tb = esp_timer_get_time();
    host_->PresenterAcquirePanel();
    err = host_->DirectPanelBlit(0, 0, host_->PresenterWidth(), host_->PresenterHeight(),
                                 present_buf_);
    const int64_t blit_ms = (esp_timer_get_time() - tb) / 1000;
    esp_rom_printf("!!PRESENT_BLIT_DONE ms=%d\n", (int)blit_ms);
    const int64_t dt = (esp_timer_get_time() - t0) / 1000;
    last_present_us_ = esp_timer_get_time();
    present_count_++;
    dirty_face_ = false;
    dirty_text_ = false;
    BootTraceMark("PRESENT_DONE", reason ? reason : "-");
    ESP_LOGW(TAG,
             "CTRL PRESENT DIAG commit ok reason=%s total_ms=%d blit_ms=%d n=%u "
             "face=%ux%u err=%s",
             reason ? reason : "-", (int)dt, (int)blit_ms, (unsigned)present_count_,
             (unsigned)face_w_, (unsigned)face_h_, esp_err_to_name(err));
    SoakTick();
    return err;
}

void ScreenPresenter::PresentTimerCallback(void* arg) {
    auto* self = static_cast<ScreenPresenter*>(arg);
    if (self == nullptr) {
        return;
    }
    self->CommitPresent("timer");
}

bool ScreenPresenter::EnsurePresentBuf() {
    if (host_ == nullptr) {
        return false;
    }
    const uint32_t px = (uint32_t)host_->PresenterWidth() * (uint32_t)host_->PresenterHeight();
    if (px == 0) {
        return false;
    }
    if (present_buf_ != nullptr && present_px_ == px) {
        return true;
    }
    FreePresentBuf();
    present_buf_ = static_cast<uint16_t*>(
        heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!present_buf_) {
        present_buf_ = static_cast<uint16_t*>(malloc(px * sizeof(uint16_t)));
    }
    if (!present_buf_) {
        return false;
    }
    present_px_ = px;
    memset(present_buf_, 0, px * sizeof(uint16_t));
    return true;
}

void ScreenPresenter::FreePresentBuf() {
    if (present_buf_ != nullptr) {
        heap_caps_free(present_buf_);
        present_buf_ = nullptr;
        present_px_ = 0;
    }
}

void ScreenPresenter::FreeOverlayCache() {
    if (overlay_rgb_ != nullptr) {
        heap_caps_free(overlay_rgb_);
        overlay_rgb_ = nullptr;
    }
    overlay_cap_px_ = 0;
    overlay_w_ = 0;
    overlay_h_ = 0;
    overlay_valid_ = false;
}

void ScreenPresenter::ApplyOverlayCache(int W, int H) {
    if (!overlay_valid_ || !overlay_rgb_ || overlay_w_ <= 0 || overlay_h_ <= 0 || !present_buf_) {
        return;
    }
    for (int y = 0; y < overlay_h_; y++) {
        const int dy = overlay_y_ + y;
        if (dy < 0 || dy >= H) {
            continue;
        }
        for (int x = 0; x < overlay_w_; x++) {
            const int dx = overlay_x_ + x;
            if (dx < 0 || dx >= W) {
                continue;
            }
            present_buf_[dy * W + dx] = overlay_rgb_[y * overlay_w_ + x];
        }
    }
}

bool ScreenPresenter::StoreOverlayCache(const uint16_t* src, int stride_px, int x0, int y0,
                                        int sw, int sh) {
    if (!src || sw <= 0 || sh <= 0 || stride_px <= 0) {
        overlay_valid_ = false;
        return false;
    }
    const uint32_t need = (uint32_t)sw * (uint32_t)sh;
    if (overlay_rgb_ == nullptr || overlay_cap_px_ < need) {
        FreeOverlayCache();
        overlay_rgb_ = static_cast<uint16_t*>(
            heap_caps_malloc(need * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!overlay_rgb_) {
            overlay_rgb_ = static_cast<uint16_t*>(malloc(need * sizeof(uint16_t)));
        }
        if (!overlay_rgb_) {
            return false;
        }
        overlay_cap_px_ = need;
    }
    for (int y = 0; y < sh; y++) {
        memcpy(overlay_rgb_ + y * sw, src + y * stride_px, (size_t)sw * sizeof(uint16_t));
    }
    overlay_x_ = x0;
    overlay_y_ = y0;
    overlay_w_ = sw;
    overlay_h_ = sh;
    overlay_valid_ = true;
    return true;
}

bool ScreenPresenter::CacheFace(const uint8_t* rgb, uint32_t size, uint32_t w, uint32_t h) {
    if (!rgb || size == 0 || w == 0 || h == 0) {
        return false;
    }
    if (face_rgb_ == nullptr || face_cap_ < size) {
        if (face_rgb_ != nullptr) {
            heap_caps_free(face_rgb_);
            face_rgb_ = nullptr;
            face_cap_ = 0;
        }
        face_rgb_ = static_cast<uint8_t*>(
            heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!face_rgb_) {
            face_rgb_ = static_cast<uint8_t*>(malloc(size));
        }
        if (!face_rgb_) {
            return false;
        }
        face_cap_ = size;
    }
    memcpy(face_rgb_, rgb, size);
    face_size_ = size;
    face_w_ = w;
    face_h_ = h;
    return true;
}

esp_err_t ScreenPresenter::RenderCompose() {
    if (!present_buf_ || host_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const int W = host_->PresenterWidth();
    const int H = host_->PresenterHeight();
    if (W <= 0 || H <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t t0 = esp_timer_get_time();
    // Base: face (or black).
    if (face_rgb_ && face_size_ > 0 && face_w_ > 0 && face_h_ > 0) {
        const uint32_t copy_w = (face_w_ < (uint32_t)W) ? face_w_ : (uint32_t)W;
        const uint32_t copy_h = (face_h_ < (uint32_t)H) ? face_h_ : (uint32_t)H;
        const auto* src = reinterpret_cast<const uint16_t*>(face_rgb_);
        if (copy_w == (uint32_t)W && copy_h == (uint32_t)H) {
            memcpy(present_buf_, src, (size_t)W * (size_t)H * sizeof(uint16_t));
        } else {
            memset(present_buf_, 0, (size_t)W * (size_t)H * sizeof(uint16_t));
            for (uint32_t y = 0; y < copy_h; y++) {
                memcpy(present_buf_ + y * W, src + y * face_w_, copy_w * sizeof(uint16_t));
            }
        }
    } else {
        memset(present_buf_, 0, (size_t)W * (size_t)H * sizeof(uint16_t));
    }
    const int64_t face_ms = (esp_timer_get_time() - t0) / 1000;

#if LV_USE_SNAPSHOT
    // Face-only update: reuse last dialogue overlay (no lv_snapshot → less HP_WDT risk).
    if (!dirty_text_ && overlay_valid_) {
        ApplyOverlayCache(W, H);
        ESP_LOGW(TAG, "CTRL PRESENT DIAG render face_ms=%d snap=reuse box=%dx%d@%d,%d",
                 (int)face_ms, overlay_w_, overlay_h_, overlay_x_, overlay_y_);
        return ESP_OK;
    }

    lv_obj_t* box = host_->PresenterDialogueBox();
    if (box == nullptr) {
        ESP_LOGW(TAG, "CTRL PRESENT DIAG render face_ms=%d snap=skip_no_box", (int)face_ms);
        return ESP_OK;
    }
    BootTraceMark("PRESENT_SNAP", "lock");
    esp_rom_printf("!!PRESENT_SNAP lock\n");
    const int64_t tl = esp_timer_get_time();
    if (!host_->PresenterSafeLVGLLock(80)) {
        esp_rom_printf("!!PRESENT_SNAP lock_fail\n");
        if (overlay_valid_) {
            ApplyOverlayCache(W, H);
        }
        ESP_LOGW(TAG, "CTRL PRESENT DIAG snapshot skip=lock_fail face_ms=%d reuse=%d",
                 (int)face_ms, overlay_valid_ ? 1 : 0);
        return ESP_OK;
    }
    const int64_t lock_ms = (esp_timer_get_time() - tl) / 1000;
    BootTraceMark("PRESENT_SNAP", "take");
    esp_rom_printf("!!PRESENT_SNAP take\n");
    const int64_t ts = esp_timer_get_time();
    lv_draw_buf_t* snap = lv_snapshot_take(box, LV_COLOR_FORMAT_RGB565);
    const int64_t snap_ms = (esp_timer_get_time() - ts) / 1000;
    esp_rom_printf("!!PRESENT_SNAP done ms=%d\n", (int)snap_ms);
    if (snap == nullptr || snap->data == nullptr) {
        host_->PresenterSafeLVGLUnlock();
        if (overlay_valid_) {
            ApplyOverlayCache(W, H);
        }
        ESP_LOGW(TAG, "CTRL PRESENT DIAG snapshot miss face_ms=%d lock_ms=%d snap_ms=%d",
                 (int)face_ms, (int)lock_ms, (int)snap_ms);
        return ESP_OK;
    }
    BootTraceMark("PRESENT_SNAP", "overlay");
    const int64_t to = esp_timer_get_time();
    lv_area_t coords;
    lv_obj_get_coords(box, &coords);
    const int x0 = coords.x1;
    const int y0 = coords.y1;
    const int sw = (int)snap->header.w;
    const int sh = (int)snap->header.h;
    const int stride_px = (int)(snap->header.stride / 2);
    const auto* s = reinterpret_cast<const uint16_t*>(snap->data);
    StoreOverlayCache(s, stride_px, x0, y0, sw, sh);
    ApplyOverlayCache(W, H);
    lv_draw_buf_destroy(snap);
    host_->PresenterSafeLVGLUnlock();
    const int64_t overlay_ms = (esp_timer_get_time() - to) / 1000;
    ESP_LOGW(TAG,
             "CTRL PRESENT DIAG render face_ms=%d lock_ms=%d snap_ms=%d "
             "overlay_ms=%d box=%dx%d@%d,%d",
             (int)face_ms, (int)lock_ms, (int)snap_ms, (int)overlay_ms, sw, sh, x0, y0);
#else
    ESP_LOGW(TAG, "CTRL PRESENT snapshot disabled face_ms=%d", (int)face_ms);
#endif
    return ESP_OK;
}

void ScreenPresenter::ApplyDialogueToLvglNow(const char* role, const char* content) {
    if (host_ == nullptr || !content) {
        return;
    }
    host_->PresenterApplyDialogue(role, content);
}

void ScreenPresenter::SoakTick() {
    if (!StageAtLeast(3)) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - last_soak_log_us_ < kSoakLogIntervalUs) {
        return;
    }
    last_soak_log_us_ = now;
    const int64_t dur_ms = (now - conversation_since_us_) / 1000;
    ESP_LOGW(TAG, "CTRL PRESENT soak_heartbeat ms=%d presents=%u emo=%s",
             (int)dur_ms, (unsigned)present_count_,
             emotion_name_.empty() ? "-" : emotion_name_.c_str());
}
