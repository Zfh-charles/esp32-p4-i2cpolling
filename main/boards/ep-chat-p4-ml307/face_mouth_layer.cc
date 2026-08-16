#include "face_mouth_layer.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>

#define TAG "FaceMouth"

namespace {

constexpr const char* kPackRoot = "/sdcard/dialogue_v2";
constexpr size_t kLevelCount = 4;
constexpr const char* kLevelName[kLevelCount] = {"closed", "small", "medium", "large"};
constexpr size_t kEyeCount = 3;
constexpr size_t kLifeFrameMax = 9;
constexpr size_t kLifeTrackMax = 2;
constexpr const char* kEyeName[kEyeCount] = {"open", "half", "closed"};

std::atomic<bool> g_enabled{S1CR_H_MOUTH != 0};
std::atomic<bool> g_pack_present{false};
std::atomic<uint8_t> g_level{0};
std::atomic<uint8_t> g_level_drawn{0xff};
std::atomic<uint32_t> g_gen{0};
std::atomic<int64_t> g_last_pcm_us{0};

// If output PCM stops, a retained last sample must not freeze the mouth open.
// The face worker will ease this zero target through the available mouth poses.
constexpr int64_t kPcmSilenceTimeoutUs = 140 * 1000;

struct Patch {
    uint8_t* rgb = nullptr;
    uint16_t w = 0;
    uint16_t h = 0;
};

Patch g_patches[kLevelCount];
Patch g_hold_base;
Patch g_pose1_patches[kLevelCount];
Patch g_eye_patches[2][kEyeCount];
Patch g_pose1_base;
struct LifeFrame { Patch rgb; uint8_t* mask = nullptr; };
LifeFrame g_life[kLifeTrackMax][kLifeFrameMax];
uint8_t g_life_count[kLifeTrackMax] = {};
int g_life_x[kLifeTrackMax] = {}, g_life_y[kLifeTrackMax] = {};
uint8_t g_life_track_count = 0;
bool g_mouth_precomposited = false;
bool g_life_precomposited = false;
uint8_t g_pose_count = 1;
int g_roi_x = 0, g_roi_y = 0, g_roi_w = 0, g_roi_h = 0;
int g_eye_x = 0, g_eye_y = 0, g_eye_w = 0, g_eye_h = 0;
char g_bound_emo[32] = {0};
bool g_ready = false;

void FreePatches() {
    for (size_t i = 0; i < kLevelCount; i++) {
        if (g_patches[i].rgb != nullptr) {
            heap_caps_free(g_patches[i].rgb);
            g_patches[i].rgb = nullptr;
        }
        g_patches[i].w = g_patches[i].h = 0;
    }
    if (g_hold_base.rgb != nullptr) {
        heap_caps_free(g_hold_base.rgb);
        g_hold_base.rgb = nullptr;
    }
    g_hold_base.w = g_hold_base.h = 0;
    for (size_t i = 0; i < kLevelCount; i++) {
        if (g_pose1_patches[i].rgb != nullptr) {
            heap_caps_free(g_pose1_patches[i].rgb);
            g_pose1_patches[i] = {};
        }
    }
    for (size_t pose = 0; pose < 2; pose++) {
        for (size_t i = 0; i < kEyeCount; i++) {
            if (g_eye_patches[pose][i].rgb != nullptr) {
                heap_caps_free(g_eye_patches[pose][i].rgb);
                g_eye_patches[pose][i] = {};
            }
        }
    }
    if (g_pose1_base.rgb != nullptr) {
        heap_caps_free(g_pose1_base.rgb);
        g_pose1_base = {};
    }
    for (size_t track = 0; track < kLifeTrackMax; ++track) {
        for (size_t i = 0; i < kLifeFrameMax; ++i) {
            if (g_life[track][i].rgb.rgb) heap_caps_free(g_life[track][i].rgb.rgb);
            if (g_life[track][i].mask) heap_caps_free(g_life[track][i].mask);
            g_life[track][i] = {};
        }
        g_life_count[track] = 0;
    }
    g_life_track_count = 0;
    g_mouth_precomposited = false;
    g_life_precomposited = false;
    g_pose_count = 1;
    g_ready = false;
    g_bound_emo[0] = '\0';
    g_level_drawn.store(0xff, std::memory_order_relaxed);
}

bool LoadRgb565File(const char* path, Patch* out, size_t* out_bytes) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz <= 0 || (sz % 2) != 0) {
        fclose(f);
        return false;
    }
    rewind(f);
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(heap_caps_malloc((size_t)sz, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        fclose(f);
        return false;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        heap_caps_free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    out->rgb = buf;
    if (out_bytes) {
        *out_bytes = (size_t)sz;
    }
    return true;
}

bool LoadSizedPatch(const char* path, uint16_t w, uint16_t h, Patch* out) {
    size_t bytes = 0;
    if (!LoadRgb565File(path, out, &bytes) || bytes != (size_t)w * (size_t)h * 2) {
        if (out->rgb != nullptr) {
            heap_caps_free(out->rgb);
            *out = {};
        }
        return false;
    }
    out->w = w;
    out->h = h;
    return true;
}

bool LoadMaskFile(const char* path, size_t expected, uint8_t** out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t* p = static_cast<uint8_t*>(heap_caps_malloc(expected, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) p = static_cast<uint8_t*>(heap_caps_malloc(expected, MALLOC_CAP_8BIT));
    const bool ok = p && fread(p, 1, expected, f) == expected && fgetc(f) == EOF;
    fclose(f);
    if (!ok) { if (p) heap_caps_free(p); return false; }
    *out = p;
    return true;
}

void Sha256Hex(const uint8_t* data, size_t len, char out[65]) {
    uint8_t digest[32];
    mbedtls_sha256(data, len, digest, 0);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
}

/** Load mouth closed/small/medium/large under dir (flexible height if stride matches roi_w). */
bool LoadMouthLevelBank(const char* dir, int roi_w, int roi_h, Patch out[kLevelCount]) {
    for (size_t i = 0; i < kLevelCount; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s/%s/mouth/%s.rgb565", kPackRoot, dir, kLevelName[i]);
        Patch p{};
        size_t bytes = 0;
        if (!LoadRgb565File(path, &p, &bytes)) {
            ESP_LOGW(TAG, "s1cr-h bind_fail patch=%s", path);
            return false;
        }
        const size_t expect = (size_t)roi_w * (size_t)roi_h * 2;
        if (bytes == expect) {
            p.w = (uint16_t)roi_w;
            p.h = (uint16_t)roi_h;
        } else if (roi_w > 0 && (bytes % ((size_t)roi_w * 2)) == 0) {
            p.w = (uint16_t)roi_w;
            p.h = (uint16_t)(bytes / ((size_t)roi_w * 2));
        } else {
            heap_caps_free(p.rgb);
            ESP_LOGW(TAG, "s1cr-h size_mismatch emo=%s level=%s", dir, kLevelName[i]);
            return false;
        }
        out[i] = p;
    }
    return true;
}

/** Load eye open/half/closed from either eye/ or pose_1/eye/. */
bool LoadEyeLevelBank(const char* dir, const char* eye_subdir, uint16_t ew, uint16_t eh,
                      Patch out[kEyeCount]) {
    char path[192];
    for (size_t i = 0; i < kEyeCount; i++) {
        snprintf(path, sizeof(path), "%s/%s/%s/%s.rgb565", kPackRoot, dir, eye_subdir, kEyeName[i]);
        if (!LoadSizedPatch(path, ew, eh, &out[i])) {
            return false;
        }
    }
    return true;
}

const char* ResolveEmotionDir(const char* emotion_name) {
    if (emotion_name == nullptr || emotion_name[0] == '\0') {
        return "neutral";
    }
    if (strcasecmp(emotion_name, "laughing") == 0 || strcasecmp(emotion_name, "funny") == 0 ||
        strcasecmp(emotion_name, "excited") == 0) {
        return "happy";
    }
    if (strcasecmp(emotion_name, "relaxed") == 0 || strcasecmp(emotion_name, "winking") == 0 ||
        strcasecmp(emotion_name, "cool") == 0 || strcasecmp(emotion_name, "kissy") == 0) {
        return "loving";
    }
    return emotion_name;
}

}  // namespace

bool FaceMouth_Enabled(void) {
    return g_enabled.load(std::memory_order_relaxed);
}

void FaceMouth_SetEnabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
}

void FaceMouth_BootProbe(void) {
    if (!FaceMouth_Enabled()) {
        ESP_LOGW(TAG, "s1cr-h mouth flag=0");
        esp_rom_printf("!!FACE_S1CR h=0\n");
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/pack_manifest.json", kPackRoot);
    FILE* f = fopen(path, "rb");
    g_pack_present.store(f != nullptr, std::memory_order_relaxed);
    if (f != nullptr) {
        fclose(f);
        ESP_LOGW(TAG, "s1cr-h pack_present path=%s", path);
        esp_rom_printf("!!FACE_S1CR h=1 pack=1\n");
    } else {
        ESP_LOGW(TAG, "s1cr-h pack_missing path=%s (static degrade)", path);
        esp_rom_printf("!!FACE_S1CR h=1 pack=0\n");
    }
}

void FaceMouth_Clear(void) {
    FreePatches();
}

bool FaceMouth_BindEmotion(const char* emotion_name) {
    if (!FaceMouth_Enabled() || !g_pack_present.load(std::memory_order_relaxed)) {
        return false;
    }
    const char* dir = ResolveEmotionDir(emotion_name);
    if (g_ready && strcasecmp(g_bound_emo, dir) == 0) {
        return true;
    }
    FreePatches();

    char man_path[128];
    snprintf(man_path, sizeof(man_path), "%s/%s/manifest.json", kPackRoot, dir);
    FILE* mf = fopen(man_path, "rb");
    if (mf == nullptr) {
        ESP_LOGW(TAG, "s1cr-h bind_fail no_manifest emo=%s", dir);
        return false;
    }
    if (fseek(mf, 0, SEEK_END) != 0) {
        fclose(mf);
        return false;
    }
    long msz = ftell(mf);
    if (msz <= 0 || msz > 256 * 1024) {
        fclose(mf);
        return false;
    }
    rewind(mf);
    char* json = static_cast<char*>(heap_caps_malloc((size_t)msz + 1, MALLOC_CAP_8BIT));
    if (json == nullptr) {
        fclose(mf);
        return false;
    }
    if (fread(json, 1, (size_t)msz, mf) != (size_t)msz) {
        heap_caps_free(json);
        fclose(mf);
        return false;
    }
    fclose(mf);
    json[msz] = '\0';

    cJSON* root = cJSON_Parse(json);
    heap_caps_free(json);
    if (root == nullptr) {
        ESP_LOGW(TAG, "s1cr-h bind_fail json emo=%s", dir);
        return false;
    }

    cJSON* render = cJSON_GetObjectItem(root, "render");
    cJSON* mode = render ? cJSON_GetObjectItem(render, "mode") : nullptr;
    if (!cJSON_IsString(mode) || strcmp(mode->valuestring, "layered") != 0) {
        ESP_LOGW(TAG, "s1cr-h bind_skip mode!=layered emo=%s", dir);
        cJSON_Delete(root);
        return false;
    }
    cJSON* roi = cJSON_GetObjectItem(render, "mouth_roi");
    if (!cJSON_IsObject(roi)) {
        cJSON_Delete(root);
        return false;
    }
    g_roi_x = cJSON_GetObjectItem(roi, "x") ? cJSON_GetObjectItem(roi, "x")->valueint : 0;
    g_roi_y = cJSON_GetObjectItem(roi, "y") ? cJSON_GetObjectItem(roi, "y")->valueint : 0;
    g_roi_w = cJSON_GetObjectItem(roi, "width") ? cJSON_GetObjectItem(roi, "width")->valueint : 0;
    g_roi_h = cJSON_GetObjectItem(roi, "height") ? cJSON_GetObjectItem(roi, "height")->valueint : 0;
    if (g_roi_w < 8 || g_roi_h < 8) {
        cJSON_Delete(root);
        return false;
    }
    cJSON* eye_roi = cJSON_GetObjectItem(render, "eye_roi");
    if (cJSON_IsObject(eye_roi)) {
        g_eye_x = cJSON_GetObjectItem(eye_roi, "x") ? cJSON_GetObjectItem(eye_roi, "x")->valueint : 0;
        g_eye_y = cJSON_GetObjectItem(eye_roi, "y") ? cJSON_GetObjectItem(eye_roi, "y")->valueint : 0;
        g_eye_w = cJSON_GetObjectItem(eye_roi, "width") ? cJSON_GetObjectItem(eye_roi, "width")->valueint : 0;
        g_eye_h = cJSON_GetObjectItem(eye_roi, "height") ? cJSON_GetObjectItem(eye_roi, "height")->valueint : 0;
    }

    cJSON* hold = cJSON_GetObjectItem(render, "hold_base");
    cJSON* hold_file = hold ? cJSON_GetObjectItem(hold, "rgb565") : nullptr;
    cJSON* hold_w = hold ? cJSON_GetObjectItem(hold, "width") : nullptr;
    cJSON* hold_h = hold ? cJSON_GetObjectItem(hold, "height") : nullptr;
    if (!cJSON_IsString(hold_file) || !cJSON_IsNumber(hold_w) || !cJSON_IsNumber(hold_h) ||
        hold_w->valueint <= 0 || hold_h->valueint <= 0) {
        ESP_LOGW(TAG, "s1cx bind_fail canonical_base_manifest emo=%s", dir);
        cJSON_Delete(root);
        return false;
    }
    char hold_path[192];
    snprintf(hold_path, sizeof(hold_path), "%s/%s/%s", kPackRoot, dir, hold_file->valuestring);
    size_t hold_bytes = 0;
    if (!LoadRgb565File(hold_path, &g_hold_base, &hold_bytes) ||
        hold_bytes != (size_t)hold_w->valueint * (size_t)hold_h->valueint * 2) {
        ESP_LOGW(TAG, "s1cx bind_fail canonical_base_file emo=%s", dir);
        FreePatches();
        cJSON_Delete(root);
        return false;
    }
    g_hold_base.w = (uint16_t)hold_w->valueint;
    g_hold_base.h = (uint16_t)hold_h->valueint;

    if (!LoadMouthLevelBank(dir, g_roi_w, g_roi_h, g_patches)) {
        FreePatches();
        cJSON_Delete(root);
        return false;
    }
    cJSON* mouth_composite = cJSON_GetObjectItem(render, "mouth_composite");
    cJSON* mouth_method = mouth_composite ? cJSON_GetObjectItem(mouth_composite, "method") : nullptr;
    g_mouth_precomposited = cJSON_IsString(mouth_method) &&
                            strcmp(mouth_method->valuestring, "canonical_base_feather_v1") == 0;

    if (strcasecmp(dir, "neutral") == 0 || strcasecmp(dir, "happy") == 0) {
        cJSON* life = cJSON_GetObjectItem(render, "life_layer");
        cJSON* composite_mode = life ? cJSON_GetObjectItem(life, "composite_mode") : nullptr;
        g_life_precomposited = cJSON_IsString(composite_mode) &&
                               strcmp(composite_mode->valuestring, "canonical_base_precomposited_v1") == 0;
        cJSON* base_hash = life ? cJSON_GetObjectItem(life, "base_rgb565_sha256") : nullptr;
        cJSON* tracks = life ? cJSON_GetObjectItem(life, "tracks") : nullptr;
        char actual[65]; Sha256Hex(g_hold_base.rgb, hold_bytes, actual);
        const bool hash_ok = cJSON_IsString(base_hash) && strcasecmp(base_hash->valuestring, actual) == 0;
        for (size_t track = 0; hash_ok && track < kLifeTrackMax; ++track) {
            cJSON* track_json = cJSON_IsArray(tracks) ? cJSON_GetArrayItem(tracks, (int)track) : nullptr;
            cJSON* lr = track_json ? cJSON_GetObjectItem(track_json, "roi") : nullptr;
            cJSON* lf = track_json ? cJSON_GetObjectItem(track_json, "frames") : nullptr;
            const int lw = cJSON_IsObject(lr) && cJSON_GetObjectItem(lr, "width") ? cJSON_GetObjectItem(lr, "width")->valueint : 0;
            const int lh = cJSON_IsObject(lr) && cJSON_GetObjectItem(lr, "height") ? cJSON_GetObjectItem(lr, "height")->valueint : 0;
            g_life_x[track] = cJSON_IsObject(lr) && cJSON_GetObjectItem(lr, "x") ? cJSON_GetObjectItem(lr, "x")->valueint : 0;
            g_life_y[track] = cJSON_IsObject(lr) && cJSON_GetObjectItem(lr, "y") ? cJSON_GetObjectItem(lr, "y")->valueint : 0;
            const int count = cJSON_IsArray(lf) ? cJSON_GetArraySize(lf) : 0;
            bool ok = lw >= 8 && lh > 0 && lh <= 48 && count > 1 && count <= (int)kLifeFrameMax;
            for (int i = 0; ok && i < count; ++i) {
                cJSON* item = cJSON_GetArrayItem(lf, i);
                cJSON* rf = item ? cJSON_GetObjectItem(item, "rgb565") : nullptr;
                cJSON* mf = item ? cJSON_GetObjectItem(item, "mask_a8") : nullptr;
                if (!cJSON_IsString(rf) || !cJSON_IsString(mf)) { ok = false; break; }
                char rp[224], mp[224];
                snprintf(rp, sizeof(rp), "%s/%s/%s", kPackRoot, dir, rf->valuestring);
                snprintf(mp, sizeof(mp), "%s/%s/%s", kPackRoot, dir, mf->valuestring);
                ok = LoadSizedPatch(rp, (uint16_t)lw, (uint16_t)lh, &g_life[track][i].rgb) &&
                     LoadMaskFile(mp, (size_t)lw * (size_t)lh, &g_life[track][i].mask);
            }
            if (!ok) {
                for (size_t i = 0; i < kLifeFrameMax; ++i) {
                    if (g_life[track][i].rgb.rgb) heap_caps_free(g_life[track][i].rgb.rgb);
                    if (g_life[track][i].mask) heap_caps_free(g_life[track][i].mask);
                    g_life[track][i] = {};
                }
                ESP_LOGW(TAG, "s1ej life_track_reject emo=%s track=%u count=%d", dir,
                         (unsigned)track, count);
                break;
            }
            g_life_count[track] = (uint8_t)count;
            g_life_track_count = (uint8_t)(track + 1);
            ESP_LOGW(TAG, "s1ej life_track_bind emo=%s track=%u frames=%u roi=%d,%d %dx%d hash=ok", dir,
                     (unsigned)track, (unsigned)g_life_count[track], g_life_x[track], g_life_y[track], lw, lh);
        }
        if (!hash_ok) {
            ESP_LOGW(TAG, "s1ej life_reject emo=%s hash=0", dir);
        }
    }

    cJSON* pose_bank = cJSON_GetObjectItem(render, "pose_bank");
    cJSON* pose_count = pose_bank ? cJSON_GetObjectItem(pose_bank, "count") : nullptr;
    if (cJSON_IsNumber(pose_count) && pose_count->valueint >= 2 && g_eye_w >= 8 && g_eye_h >= 8) {
        bool pose_ok = true;
        char path[192];
        pose_ok &= LoadEyeLevelBank(dir, "eye", (uint16_t)g_eye_w, (uint16_t)g_eye_h, g_eye_patches[0]);
        snprintf(path, sizeof(path), "%s/%s/pose_1/hold_base.rgb565", kPackRoot, dir);
        pose_ok &= LoadSizedPatch(path, g_hold_base.w, g_hold_base.h, &g_pose1_base);
        for (size_t i = 0; i < kLevelCount; i++) {
            snprintf(path, sizeof(path), "%s/%s/pose_1/mouth/%s.rgb565", kPackRoot, dir, kLevelName[i]);
            pose_ok &= LoadSizedPatch(path, (uint16_t)g_roi_w, (uint16_t)g_roi_h, &g_pose1_patches[i]);
        }
        pose_ok &=
            LoadEyeLevelBank(dir, "pose_1/eye", (uint16_t)g_eye_w, (uint16_t)g_eye_h, g_eye_patches[1]);
        if (pose_ok) {
            g_pose_count = 2;
        } else {
            ESP_LOGW(TAG, "s1cy pose_bank incomplete emo=%s (single-pose fallback)", dir);
            for (size_t i = 0; i < kLevelCount; i++) {
                if (g_pose1_patches[i].rgb) { heap_caps_free(g_pose1_patches[i].rgb); g_pose1_patches[i] = {}; }
            }
            for (size_t pose = 0; pose < 2; pose++) for (size_t i = 0; i < kEyeCount; i++) {
                if (g_eye_patches[pose][i].rgb) { heap_caps_free(g_eye_patches[pose][i].rgb); g_eye_patches[pose][i] = {}; }
            }
            if (g_pose1_base.rgb) { heap_caps_free(g_pose1_base.rgb); g_pose1_base = {}; }
        }
    }
    cJSON_Delete(root);

    strncpy(g_bound_emo, dir, sizeof(g_bound_emo) - 1);
    g_ready = true;
    g_level.store(0, std::memory_order_relaxed);
    g_last_pcm_us.store(0, std::memory_order_relaxed);
    g_level_drawn.store(0xff, std::memory_order_relaxed);
    g_gen.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(TAG, "s1cy bind_ok emo=%s roi=%d,%d %dx%d poses=%u", dir, g_roi_x, g_roi_y,
             g_roi_w, g_roi_h, (unsigned)g_pose_count);
    esp_rom_printf("!!FACE_S1CY bind emo=%s poses=%u\n", dir, (unsigned)g_pose_count);
    return true;
}

bool FaceMouth_Ready(void) {
    return FaceMouth_Enabled() && g_ready;
}

void FaceMouth_PublishFromPcm(const int16_t* pcm, size_t samples) {
    if (!FaceMouth_Enabled() || pcm == nullptr || samples == 0) {
        return;
    }
    // Cheap mean-abs → level; audio task must stay light.
    uint64_t acc = 0;
    const size_t n = samples > 320 ? 320 : samples;
    for (size_t i = 0; i < n; i++) {
        int v = pcm[i];
        acc += (uint64_t)(v < 0 ? -v : v);
    }
    const uint32_t mean = (uint32_t)(acc / n);
    uint8_t level = 0;
    if (mean > 2200) {
        level = 3;
    } else if (mean > 1100) {
        level = 2;
    } else if (mean > 400) {
        level = 1;
    } else {
        level = 0;
    }
    // Release hysteresis: prefer closing slowly via sticky mid when falling.
    static uint8_t s_prev = 0;
    if (level < s_prev && (s_prev - level) == 1 && mean > 250) {
        level = s_prev;  // hold one step
    }
    s_prev = level;
    g_level.store(level, std::memory_order_relaxed);
    g_last_pcm_us.store(esp_timer_get_time(), std::memory_order_relaxed);
}

uint8_t FaceMouth_Level(void) {
    const int64_t last_pcm_us = g_last_pcm_us.load(std::memory_order_relaxed);
    if (last_pcm_us == 0 || esp_timer_get_time() - last_pcm_us > kPcmSilenceTimeoutUs) {
        return 0;
    }
    return g_level.load(std::memory_order_relaxed);
}

uint8_t FaceMouth_ConsumeLevelIfChanged(uint8_t* out_level) {
    uint8_t lv = g_level.load(std::memory_order_relaxed);
    uint8_t drawn = g_level_drawn.load(std::memory_order_relaxed);
    if (out_level) {
        *out_level = lv;
    }
    if (lv == drawn) {
        return 0;
    }
    g_level_drawn.store(lv, std::memory_order_relaxed);
    return 1;
}

const uint8_t* FaceMouth_PatchRgb565(uint8_t level, uint16_t* out_w, uint16_t* out_h) {
    if (!g_ready || level >= kLevelCount) {
        return nullptr;
    }
    if (out_w) {
        *out_w = g_patches[level].w;
    }
    if (out_h) {
        *out_h = g_patches[level].h;
    }
    return g_patches[level].rgb;
}

const uint8_t* FaceMouth_HoldBaseRgb565(uint16_t* out_w, uint16_t* out_h) {
    if (!g_ready || g_hold_base.rgb == nullptr) {
        return nullptr;
    }
    if (out_w) {
        *out_w = g_hold_base.w;
    }
    if (out_h) {
        *out_h = g_hold_base.h;
    }
    return g_hold_base.rgb;
}

uint8_t FaceMouth_PoseCount(void) { return g_ready ? g_pose_count : 0; }

const uint8_t* FaceMouth_PoseBaseRgb565(uint8_t pose, uint16_t* out_w, uint16_t* out_h) {
    Patch* p = pose == 1 && g_pose_count > 1 ? &g_pose1_base : &g_hold_base;
    if (!g_ready || p->rgb == nullptr) return nullptr;
    if (out_w) *out_w = p->w;
    if (out_h) *out_h = p->h;
    return p->rgb;
}

const uint8_t* FaceMouth_PosePatchRgb565(uint8_t pose, uint8_t level,
                                        uint16_t* out_w, uint16_t* out_h) {
    if (!g_ready || level >= kLevelCount) return nullptr;
    Patch* p = pose == 1 && g_pose_count > 1 ? &g_pose1_patches[level] : &g_patches[level];
    if (out_w) *out_w = p->w;
    if (out_h) *out_h = p->h;
    return p->rgb;
}

const uint8_t* FaceMouth_EyePatchRgb565(uint8_t pose, uint8_t level,
                                       uint16_t* out_w, uint16_t* out_h) {
    if (!g_ready || g_pose_count < 2 || pose >= g_pose_count || level >= kEyeCount) return nullptr;
    Patch* p = &g_eye_patches[pose][level];
    if (out_w) *out_w = p->w;
    if (out_h) *out_h = p->h;
    return p->rgb;
}

void FaceMouth_EyeRoi(int* x, int* y, int* w, int* h) {
    if (x) *x = g_eye_x;
    if (y) *y = g_eye_y;
    if (w) *w = g_eye_w;
    if (h) *h = g_eye_h;
}

void FaceMouth_Roi(int* x, int* y, int* w, int* h) {
    if (x) {
        *x = g_roi_x;
    }
    if (y) {
        *y = g_roi_y;
    }
    if (w) {
        *w = g_roi_w;
    }
    if (h) {
        *h = g_roi_h;
    }
}

bool FaceMouth_LifeReady(void) { return g_ready && g_life_track_count > 0; }
bool FaceMouth_MouthPrecomposited(void) { return g_ready && g_mouth_precomposited; }
bool FaceMouth_LifePrecomposited(void) { return FaceMouth_LifeReady() && g_life_precomposited; }
uint8_t FaceMouth_LifeTrackCount(void) { return FaceMouth_LifeReady() ? g_life_track_count : 0; }
uint8_t FaceMouth_LifeFrameCount(uint8_t track) {
    return FaceMouth_LifeReady() && track < g_life_track_count ? g_life_count[track] : 0;
}
bool FaceMouth_LifeOverlapsMouth(uint8_t track) {
    if (!FaceMouth_LifeReady() || track >= g_life_track_count || g_life_count[track] == 0) return true;
    const int life_w = g_life[track][0].rgb.w;
    const int life_h = g_life[track][0].rgb.h;
    return g_life_x[track] < g_roi_x + g_roi_w && g_life_x[track] + life_w > g_roi_x &&
           g_life_y[track] < g_roi_y + g_roi_h && g_life_y[track] + life_h > g_roi_y;
}
bool FaceMouth_LifeFrame(uint8_t track, uint8_t index, const uint8_t** rgb565, const uint8_t** mask_a8,
                         uint16_t* w, uint16_t* h, int* x, int* y) {
    if (!FaceMouth_LifeReady() || track >= g_life_track_count || index >= g_life_count[track] ||
        !g_life[track][index].rgb.rgb || !g_life[track][index].mask) return false;
    if (rgb565) *rgb565 = g_life[track][index].rgb.rgb;
    if (mask_a8) *mask_a8 = g_life[track][index].mask;
    if (w) *w = g_life[track][index].rgb.w;
    if (h) *h = g_life[track][index].rgb.h;
    if (x) *x = g_life_x[track];
    if (y) *y = g_life_y[track];
    return true;
}
