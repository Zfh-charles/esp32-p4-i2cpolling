#ifndef EEZUI_DISPLAY_ADAPTER_H
#define EEZUI_DISPLAY_ADAPTER_H

#include "display.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

// C头文件包含
extern "C" {
#include "emotion_video_player.h"
}

// 表情映射数据结构
struct EmotionMapping {
    const char* emotion_name;    // 表情名称
    const char* mjpeg_file;      // 对应的MJPEG文件名
};

// 设备状态枚举
enum class Status {
    kIdle,
    kListening,
    kThinking, 
    kSpeaking,
    kConnecting,
    kError
};

// 前向声明eezui组件
extern "C" {
#include "ui/ui.h"
#include "ui/screens.h"
}

// 前向声明
class EezuiDisplayAdapter;

// 定时器用户数据结构（改进的内存管理）
struct TimerData {
    EezuiDisplayAdapter* adapter;
    lv_obj_t* dialogue_box;
    lv_timer_t* hide_timer;
    
    // 析构函数声明，实现移到cpp文件中避免不完整类型问题
    ~TimerData();
};

class EezuiDisplayAdapter : public Display {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    // eezui UI对象引用
    lv_obj_t* dialogue_box_ = nullptr;
    lv_obj_t* main_image_ = nullptr;
    lv_obj_t* battery_panel_ = nullptr;
    lv_obj_t* battery_gauge_ = nullptr;
    lv_obj_t* battery_body_ = nullptr;
    lv_obj_t* battery_fill_ = nullptr;
    lv_obj_t* battery_tip_ = nullptr;
    lv_obj_t* battery_percent_label_ = nullptr;
    int last_battery_level_ = -1;
    bool last_battery_charging_ = false;
    bool last_battery_low_ = false;
    int64_t last_battery_update_us_ = 0;
    
    // 逐字显示效果相关
    std::string typewriter_text_;
    size_t typewriter_index_;
    lv_timer_t* typewriter_timer_;
    bool typewriter_active_;
    
    // 隐藏定时器管理
    lv_timer_t* hide_timer_ = nullptr;
    
    // 持久化消息标志（用于闹钟备注等需要持续显示的消息）
    bool is_persistent_message_ = false;
    
    // 改进的状态管理系统
    enum class InitState {
        NONE = 0,
        UI_READY = 1,
        EMOTION_READY = 2,
        
        ALL_READY = 3  // UI_READY | EMOTION_READY | BATTERY_READY
    };
    
    Status current_status_ = Status::kIdle;
    InitState init_state_ = InitState::NONE;
    
    // ========== 表情系统（优化的资源管理）==========
    emotion_video_handle_t emotion_player_ = nullptr;
    lv_obj_t* video_canvas_ = nullptr;
    std::string current_emotion_name_;
    
    // ========== 电池管理系统 ==========
    // 移除重复的初始化标志，使用统一的init_state_
    
    // ========== WiFi信号管理 ==========
   //  void UpdateWifiSignal();
   //  void UpdateWifiSignalFromBoard();
    // void EnsureWifiUIOnTop();
    
    void SetupUI();
    void SetupBatteryUI();
    void StartTypewriterEffect(const std::string& text);
    void StopTypewriterEffect();
    void UpdateDialogueBoxHeight();
    
    // ========== 表情系统方法 ==========
    bool InitEmotionSystem();
    void DeinitEmotionSystem();
    esp_err_t PlayMjpegEmotion(const char* emotion_name);
    void StopEmotionPlayback();
    void CreateVideoCanvas();
    
    // ========== UI管理方法 ==========
    void EnsureUILayerOrder();
    bool SafeLVGLLock(int timeout_ms = 100);
    void SafeLVGLUnlock();
    
    // ========== 回调函数==========
    static void EmotionVideoFrameCallback(emotion_video_handle_t handle, uint8_t *frame_data, 
                                          uint32_t frame_size, uint32_t width, uint32_t height, void *user_data);
    static void EmotionVideoEventCallback(emotion_video_event_t event, void *user_data);
    
    // ========== 电池管理系统方法 ==========
 //   bool InitBatteryManager();
 //   void DeinitBatteryManager();
 //   void UpdateBatteryUI();
  //  void EnsureBatteryUIOnTop();
    
    
    // 静态回调函数
    static void TypewriterTimerCallback(lv_timer_t* timer);
    static void DialogueBoxClickCallback(lv_event_t* e);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // 受保护的构造函数
    EezuiDisplayAdapter(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    virtual ~EezuiDisplayAdapter();
    
    
    // Display基类的虚函数
    virtual void SetEmotion(const char* emotion) override;
  //  virtual void SetIcon(const char* icon) override;
   // virtual void SetPreviewImage(const lv_img_dsc_t* img_dsc) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
  //  virtual void SetTheme(const std::string& theme_name) override;
    
    // 强制表情切换方法
    esp_err_t ForceStopAndSwitchEmotion(const char* emotion_name);
    
    // 新增状态显示方法
    virtual void SetStatus(const char* status, const char* time = nullptr);
    virtual void SetStatus(const char* status);
    virtual void SetStatus(Status status);  // 枚举版本状态设置（从范例移植）
    
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000) override;
    
    
    // 获取初始化状态
    bool IsUIInitialized() const { return (static_cast<int>(init_state_) & static_cast<int>(InitState::UI_READY)) != 0; }
    bool IsUIReady() const { return IsUIInitialized(); }
    bool IsEmotionSystemReady() const { return (static_cast<int>(init_state_) & static_cast<int>(InitState::EMOTION_READY)) != 0; }
  //  bool IsBatteryManagerReady() const { return (static_cast<int>(init_state_) & static_cast<int>(InitState::BATTERY_READY)) != 0; }
    bool IsFullyInitialized() const { return init_state_ == InitState::ALL_READY; }

    // 设置隐藏定时器（用于回调函数访问）
    void SetHideTimer(lv_timer_t* timer) { hide_timer_ = timer; }
    
    // 获取表情视频播放器句柄（用于缓存管理）
    emotion_video_handle_t GetEmotionPlayerHandle() const { return emotion_player_; }
};

// MIPI LCD显示器适配器
class MipiEezuiDisplayAdapter : public EezuiDisplayAdapter {
public:
    MipiEezuiDisplayAdapter(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // EEZUI_DISPLAY_ADAPTER_H
