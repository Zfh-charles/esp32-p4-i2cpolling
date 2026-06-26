#include "eezui_display_adapter.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include "ui.h"
#include "lvgl.h"
#include "src/misc/lv_timer.h"
#include "sd_scanner.h"
#include <sys/stat.h>
#include "application.h"
#include "board.h"
#include "font_awesome.h"

extern "C" {
#include "emotion_video_player.h"



}

#define TAG "EezuiDisplayAdapter"

// 逐字显示效果的延迟时间（毫秒）
#define TYPEWRITER_DELAY_MS 50

// 注意：表情映射表已统一到emotion_video_player.c中，这里不再维护重复的映射

// ========== TimerData析构函数实现 ==========
TimerData::~TimerData() {
    if (hide_timer && adapter) {
        adapter->SetHideTimer(nullptr);
    }
}

EezuiDisplayAdapter::EezuiDisplayAdapter(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : Display(), panel_io_(panel_io), panel_(panel), dialogue_box_(nullptr), 
      typewriter_index_(0), typewriter_timer_(nullptr), typewriter_active_(false),
      emotion_player_(nullptr), video_canvas_(nullptr) {
    width_ = width;
    height_ = height;
    
    
    // 初始化状态管理
    init_state_ = InitState::NONE;
    current_status_ = Status::kIdle;
    
}

EezuiDisplayAdapter::~EezuiDisplayAdapter() {
        StopTypewriterEffect();
        DeinitEmotionSystem();
    
        
        // 安全清理定时器
        if (hide_timer_ != nullptr) {
            // 获取并释放用户数据
            void* timer_data = lv_timer_get_user_data(hide_timer_);
            if (timer_data) {
                delete static_cast<TimerData*>(timer_data);
            }
            lv_timer_del(hide_timer_);
            hide_timer_ = nullptr;
        }
    }

void EezuiDisplayAdapter::SetupUI() {
    if (!SafeLVGLLock(200)) {
        ESP_LOGE(TAG, "无法获取LVGL锁，放弃UI初始化");
        return;
    }


    // 设置默认显示器
    if (display_ != nullptr) {
        lv_disp_set_default(display_);
    } else {
        ESP_LOGE(TAG, "Display is null!");
        SafeLVGLUnlock();
        return;
    }

    // 初始化 eezui
    ui_init();
    
    // 获取 EEZ UI 对象引用
    dialogue_box_ = objects.dialogue_box;
    // main_image 在新版UI中可能被移除或改名，设为nullptr
    main_image_ = nullptr;
    
    
    // 立即隐藏电池相关标签，防止开机时显示
    // 注意：charging和low_bat在当前EEZ UI中不存在，已注释
    // if (objects.charging) {
    //     lv_obj_add_flag(objects.charging, LV_OBJ_FLAG_HIDDEN);
    // }
    // if (objects.low_bat) {
    //     lv_obj_add_flag(objects.low_bat, LV_OBJ_FLAG_HIDDEN);
    // }
    
    // 初始化时隐藏音量条
    if (objects.volume_bar) {
        lv_obj_add_flag(objects.volume_bar, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 初始状态设置
    if (dialogue_box_ != nullptr) {
        // 设置对话框文本居中对齐
        lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
        // 设置初始文本
     //   lv_label_set_text(dialogue_box_, "你好，小智！");
          lv_label_set_text(dialogue_box_, "你好，小易！");
        lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
        
        // 添加点击事件处理
        lv_obj_add_event_cb(dialogue_box_, DialogueBoxClickCallback, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(dialogue_box_, LV_OBJ_FLAG_CLICKABLE);  // 确保对象可点击
        
        // 设置初始动态高度
        UpdateDialogueBoxHeight();
        
    } else {
        ESP_LOGE(TAG, "dialogue_box_ is null after ui_init!");
    }

    if (main_image_ != nullptr) {
        // 初始化时确保背景图片可见
        lv_obj_clear_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
    } else {
        ESP_LOGE(TAG, "main_image_ is null after ui_init!");
    }
    
    SafeLVGLUnlock();
    
    

    
    // 设置UI就绪标志
    init_state_ = static_cast<InitState>(static_cast<int>(init_state_) | static_cast<int>(InitState::UI_READY));
    ESP_LOGI(TAG, "✅ UI初始化完成");
}

void EezuiDisplayAdapter::StartTypewriterEffect(const std::string& text) {
    if (dialogue_box_ == nullptr) {
        return;
    }
    
    // 停止之前的逐字显示效果
    StopTypewriterEffect();
    
    typewriter_text_ = text;
    typewriter_index_ = 0;
    typewriter_active_ = true;
    
    // 清空对话框内容
    lv_label_set_text(dialogue_box_, "");
    
    // 确保对话标签在最顶层
    lv_obj_move_foreground(dialogue_box_);
    
    // 创建定时器
    typewriter_timer_ = lv_timer_create(TypewriterTimerCallback, TYPEWRITER_DELAY_MS, this);
    
}

void EezuiDisplayAdapter::StopTypewriterEffect() {
    if (typewriter_timer_ != nullptr) {
        lv_timer_del(typewriter_timer_);
        typewriter_timer_ = nullptr;
    }
    typewriter_active_ = false;
}

void EezuiDisplayAdapter::UpdateDialogueBoxHeight() {
    if (dialogue_box_ == nullptr) {
        return;
    }
    
    // 设置对话框高度为内容高度
    lv_obj_set_height(dialogue_box_, LV_SIZE_CONTENT);
    lv_obj_invalidate(dialogue_box_);
}



void EezuiDisplayAdapter::TypewriterTimerCallback(lv_timer_t* timer) {
    EezuiDisplayAdapter* adapter = static_cast<EezuiDisplayAdapter*>(lv_timer_get_user_data(timer));
    if (adapter == nullptr || !adapter->typewriter_active_ || adapter->dialogue_box_ == nullptr) {
        return;
    }
    
    if (adapter->typewriter_index_ < adapter->typewriter_text_.length()) {
        // 显示到当前索引的文本
        std::string current_text = adapter->typewriter_text_.substr(0, adapter->typewriter_index_ + 1);
        lv_label_set_text(adapter->dialogue_box_, current_text.c_str());
        adapter->UpdateDialogueBoxHeight();
        adapter->typewriter_index_++;
    } else {
        // 逐字显示完成
        adapter->StopTypewriterEffect();
    }
}

void EezuiDisplayAdapter::DialogueBoxClickCallback(lv_event_t* e) {
    EezuiDisplayAdapter* adapter = static_cast<EezuiDisplayAdapter*>(lv_event_get_user_data(e));
    if (adapter == nullptr) {
        return;
    }
    
    
    // 如果正在逐字显示，点击后立即显示完整文本
    if (adapter->typewriter_active_) {
        adapter->StopTypewriterEffect();
        if (adapter->dialogue_box_ != nullptr) {
            lv_label_set_text(adapter->dialogue_box_, adapter->typewriter_text_.c_str());
            adapter->UpdateDialogueBoxHeight();
        }
    }
}

bool EezuiDisplayAdapter::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void EezuiDisplayAdapter::Unlock() {
    lvgl_port_unlock();
}

// ========== 安全的LVGL锁机制 ==========

// 全局锁持有者跟踪（线程安全）
static TaskHandle_t g_lvgl_lock_holder = nullptr;
static portMUX_TYPE g_lock_holder_spinlock = portMUX_INITIALIZER_UNLOCKED;

bool EezuiDisplayAdapter::SafeLVGLLock(int timeout_ms) {
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    
    // 原子检查当前任务是否已经持有锁，避免重入死锁
    portENTER_CRITICAL(&g_lock_holder_spinlock);
    bool already_holds_lock = (g_lvgl_lock_holder == current_task);
    portEXIT_CRITICAL(&g_lock_holder_spinlock);
    
    if (already_holds_lock) {
        return true; // 避免重入死锁
    }
    
    bool result = lvgl_port_lock(pdMS_TO_TICKS(timeout_ms));
    if (result) {
        portENTER_CRITICAL(&g_lock_holder_spinlock);
        g_lvgl_lock_holder = current_task;
        portEXIT_CRITICAL(&g_lock_holder_spinlock);
    } else {
        // 对于超时时间很短的情况（如视频帧更新），不输出警告日志，避免日志洪水
        if (timeout_ms >= 50) {
        }
    }
    return result;
}

void EezuiDisplayAdapter::SafeLVGLUnlock() {
    // 原子清除锁持有者标记
    portENTER_CRITICAL(&g_lock_holder_spinlock);
    g_lvgl_lock_holder = nullptr;
    portEXIT_CRITICAL(&g_lock_holder_spinlock);
    lvgl_port_unlock();
}



void EezuiDisplayAdapter::SetEmotion(const char* emotion) {
    
    // 处理空字符串和空指针
    if (!emotion || strlen(emotion) == 0) {
        ESP_LOGW(TAG, "收到空表情，使用默认neutral");
       emotion = "neutral";
      
    }
    
    // 对特殊表情（alarm、music、charge等）允许重新触发，因为它们可能已被释放缓存
    // 特殊表情列表
    static const char* special_emotions[] = {"alarm", "music", "charge", "logo", nullptr};
    bool is_special = false;
    for (int i = 0; special_emotions[i] != nullptr; i++) {
        if (strcmp(emotion, special_emotions[i]) == 0) {
            is_special = true;
            break;
        }
    }
    
    // 检查是否与当前表情相同（特殊表情除外）
    if (!is_special && !current_emotion_name_.empty() && current_emotion_name_ == emotion) {
        return;
    }
    
    // 延迟初始化表情系统
    if (!IsEmotionSystemReady()) {
        if (!InitEmotionSystem()) {
            ESP_LOGE(TAG, "表情系统初始化失败，跳过表情显示");
            return;
        }
    }

    // 播放表情动画
    if (IsEmotionSystemReady() && emotion_player_ != nullptr) {
        esp_err_t ret = PlayMjpegEmotion(emotion);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "表情播放失败: %s，使用neutral", emotion);
            if (strcmp(emotion, "neutral") != 0) {
                PlayMjpegEmotion("neutral");
            }
        } else {
            // 成功播放后更新当前表情记录
            current_emotion_name_ = emotion;
        }
    } else {
        ESP_LOGW(TAG, "表情系统未就绪，跳过: %s", emotion);
    }
}





void EezuiDisplayAdapter::SetChatMessage(const char* role, const char* content) {
    if (dialogue_box_ == nullptr || content == nullptr) {
        return;
    }

    // 检查内容是否为空或只包含空白字符
    if (strlen(content) == 0 || strspn(content, " \t\n\r") == strlen(content)) {
        return;
    }

    DisplayLockGuard lock(this);
    
    // 检查是否是持久化消息（如闹钟备注）
    if (role != nullptr && (strcmp(role, "alarm_note") == 0 || strcmp(role, "system_persistent") == 0)) {
        is_persistent_message_ = true;
    } else if (role != nullptr && strcmp(role, "lyrics") != 0) {
        // 非歌词消息会清除持久化标志
        is_persistent_message_ = false;
    }
    
    // 检查是否是重要消息，如果是则取消隐藏定时器
    const char* important_keywords[] = {"配网模式", "热点", "192.168.4.1", "验证码", "输入", "请", "code", "Code", "验证", "密码", "连接", "xiaozhi.me"};
    bool is_important = false;
    for (int i = 0; i < sizeof(important_keywords)/sizeof(important_keywords[0]); i++) {
        if (strstr(content, important_keywords[i])) {
            is_important = true;
            break;
        }
    }
    
    if ((is_important || is_persistent_message_) && hide_timer_) {
        lv_timer_del(hide_timer_);
        hide_timer_ = nullptr;
    }
    
    // 根据角色决定是否使用逐字显示效果
    if (role != nullptr && strcmp(role, "assistant") == 0) {
        // AI助手回复使用逐字显示效果
        StartTypewriterEffect(std::string(content));
    } else {
        // 其他消息直接显示并居中对齐
        lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(dialogue_box_, content);
        // 确保对话标签在最顶层
        lv_obj_move_foreground(dialogue_box_);
        // 强制显示对话框，覆盖可能的隐藏状态
        lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
        UpdateDialogueBoxHeight();
    }
}

// 字符串版本状态设置
void EezuiDisplayAdapter::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    
    
    // 忽略时间格式（HH:MM）
    if (strlen(status) >= 5 && status[2] == ':' && 
        isdigit(status[0]) && isdigit(status[1]) && 
        isdigit(status[3]) && isdigit(status[4])) {
        return;
    }
    
    // 快速状态识别
    Status new_status = Status::kIdle;
    
    if (strstr(status, "listening") || strstr(status, "聆听")) {
        new_status = Status::kListening;
    } else if (strstr(status, "thinking") || strstr(status, "思考")) {
        new_status = Status::kThinking;
    } else if (strstr(status, "speaking") || strstr(status, "说话")) {
        new_status = Status::kSpeaking;
    } else if (strstr(status, "connecting") || strstr(status, "连接")) {
        new_status = Status::kConnecting;
        // 连接状态直接显示在对话框中
        if (dialogue_box_ != nullptr) {
            DisplayLockGuard lock(this);
            StopTypewriterEffect();
            lv_label_set_text(dialogue_box_, status);
            lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            UpdateDialogueBoxHeight();
        }
    } else if (strstr(status, "error") || strstr(status, "错误")) {
        new_status = Status::kError;
    } else if (strstr(status, "standby") || strstr(status, "待命") || strstr(status, "待机")) {
        // 明确识别待命/待机状态，避免重复处理
        new_status = Status::kIdle;
    }
    
    // 直接调用枚举版本的SetStatus方法，避免歧义
    EezuiDisplayAdapter::SetStatus(new_status);
}

// 从范例移植的ShowNotification方法
void EezuiDisplayAdapter::ShowNotification(const char* notification, int duration_ms) {
    if (notification == nullptr || dialogue_box_ == nullptr) {
        return;
    }
    
    DisplayLockGuard lock(this);
    
    // 停止逐字显示效果
    StopTypewriterEffect();
    
    // 设置通知文本
    lv_label_set_text(dialogue_box_, notification);
    lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
    
    // 确保对话标签在最顶层
    lv_obj_move_foreground(dialogue_box_);
    
    // 更新动态高度
    UpdateDialogueBoxHeight();
    
}

void EezuiDisplayAdapter::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

// 优化的定时器回调函数（改进内存管理）
static void OnIdleHideTimer(lv_timer_t* timer) {
    if (timer == nullptr) {
        return;
    }
    
    void* user_data = lv_timer_get_user_data(timer);
    if (user_data == nullptr) {
        lv_timer_del(timer);
        return;
    }
    
    // 获取EezuiDisplayAdapter指针和对话框
    TimerData* data = static_cast<TimerData*>(user_data);
    
    // 验证数据有效性
    if (!data->dialogue_box || !data->adapter) {
        lv_timer_del(timer);
        delete data;
        return;
    }
    
    // 检查是否为重要信息，如果是则不隐藏
    const char* current_text = lv_label_get_text(data->dialogue_box);
    const char* important_keywords[] = {"配网模式", "热点", "192.168.4.1", "验证码", "输入", "请", "code", "Code", "验证", "密码", "连接", "xiaozhi.me"};
    
    if (current_text) {
        for (int i = 0; i < sizeof(important_keywords)/sizeof(important_keywords[0]); i++) {
            if (strstr(current_text, important_keywords[i])) {
                // 安全清理定时器引用
                if (data->hide_timer == timer) {
                    data->adapter->SetHideTimer(nullptr);
                }
                lv_timer_del(timer);
                delete data;
                return;
            }
        }
    }
    
    // 隐藏对话框
    lv_obj_add_flag(data->dialogue_box, LV_OBJ_FLAG_HIDDEN);
    
    // 安全清理定时器引用
    if (data->hide_timer == timer) {
        data->adapter->SetHideTimer(nullptr);
    }
    
    // 清理资源
    lv_timer_del(timer);
    delete data;
    
}

void EezuiDisplayAdapter::SetStatus(Status status) {
    // 只有在状态变化时才更新，但是第一次idle状态需要强制更新
    if (current_status_ == status && !(status == Status::kIdle && current_status_ == Status::kIdle)) {
        if (status == Status::kIdle && dialogue_box_ != nullptr) {
            const char* current_text = lv_label_get_text(dialogue_box_);
            if (current_text != nullptr && strcmp(current_text, "请说：你好，小易！唤醒我吧！") == 0) {
                return;  // 文本已经是正确的，不需要更新
            }
        } else {
            return;
        }
    }

    current_status_ = status;


    // 根据状态更新dialogue_box内容
    if (dialogue_box_ != nullptr) {
        DisplayLockGuard lock(this);

        if (status == Status::kIdle) {
            // 检查当前对话框内容和可见性
            const char* current_text = lv_label_get_text(dialogue_box_);
            bool is_hidden = lv_obj_has_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            bool has_valid_content = false;
            
            // 如果对话框是隐藏的，说明之前的内容已经过期，应该显示待机提示
            if (!is_hidden && current_text && strlen(current_text) > 0) {
                // 检查是否是状态提示文本（这些文本在idle时应该被替换）
                const char* status_texts[] = {
                    "正在聆听...", "正在思考...", "正在说话...", "正在连接...", "出现错误"
                };
                bool is_status_text = false;
                for (int i = 0; i < sizeof(status_texts)/sizeof(status_texts[0]); i++) {
                    if (strcmp(current_text, status_texts[i]) == 0) {
                        is_status_text = true;
                        break;
                    }
                }
                
                // 如果不是状态文本且不是待机提示，说明是有效的对话内容，应该保留
                if (!is_status_text && strcmp(current_text, "请说：你好，小易！唤醒我吧！") != 0) {
                    has_valid_content = true;
                }
            }
            
            // 如果对话框被隐藏或没有有效内容，显示待机提示
            if (is_hidden || !has_valid_content) {
                StopTypewriterEffect();  // 停止逐字显示
                lv_label_set_text(dialogue_box_, "请说：你好，小易！唤醒我吧！");
                lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(dialogue_box_);
                UpdateDialogueBoxHeight();
            }

            // 清理之前的定时器
            if (hide_timer_ != nullptr) {
                lv_timer_del(hide_timer_);
                hide_timer_ = nullptr;
            }

            // 创建3秒后隐藏的定时器
            TimerData* timer_data = new TimerData{this, dialogue_box_, nullptr};
            hide_timer_ = lv_timer_create(OnIdleHideTimer, 3000, timer_data);
            if (hide_timer_) {
                timer_data->hide_timer = hide_timer_;
            } else {
                // 定时器创建失败，清理内存
                ESP_LOGE(TAG, "创建隐藏定时器失败，清理TimerData");
                delete timer_data;
                timer_data = nullptr;
            }
        } else if (status == Status::kListening) {
            // 监听状态处理
            StopTypewriterEffect();
            lv_label_set_text(dialogue_box_, "正在聆听...");
            lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(dialogue_box_);
            UpdateDialogueBoxHeight();
        } else if (status == Status::kThinking) {
            // 思考状态处理
            StopTypewriterEffect();
            lv_label_set_text(dialogue_box_, "正在思考...");
            lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(dialogue_box_);
            UpdateDialogueBoxHeight();
        } else if (status == Status::kSpeaking) {
            // 说话状态处理
            StopTypewriterEffect();
            lv_label_set_text(dialogue_box_, "正在说话...");
            lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(dialogue_box_);
            UpdateDialogueBoxHeight();
        } else if (status == Status::kConnecting) {
            // 连接状态处理
            StopTypewriterEffect();
            lv_label_set_text(dialogue_box_, "正在连接...");
            lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(dialogue_box_);
            UpdateDialogueBoxHeight();
        } else if (status == Status::kError) {
            // 错误状态处理
            StopTypewriterEffect();
            lv_label_set_text(dialogue_box_, "出现错误");
            lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(dialogue_box_);
            UpdateDialogueBoxHeight();
        } else {
            // 处理未预期的状态值
            ESP_LOGW(TAG, "未处理的枚举状态: %d", static_cast<int>(status));
        }
    }

}

void EezuiDisplayAdapter::SetStatus(const char* status, const char* time) {
    // 忽略时间参数，直接调用枚举版本的状态设置
    if (status == nullptr) {
        return;
    }
    
    // 快速状态识别并转换为枚举
    Status new_status = Status::kIdle;
    
    if (strstr(status, "listening") || strstr(status, "聆听")) {
        new_status = Status::kListening;
    } else if (strstr(status, "thinking") || strstr(status, "思考")) {
        new_status = Status::kThinking;
    } else if (strstr(status, "speaking") || strstr(status, "说话")) {
        new_status = Status::kSpeaking;
    } else if (strstr(status, "connecting") || strstr(status, "连接")) {
        new_status = Status::kConnecting;
    } else if (strstr(status, "error") || strstr(status, "错误")) {
        new_status = Status::kError;
    }
    
    // 调用枚举版本的SetStatus避免歧义
    EezuiDisplayAdapter::SetStatus(new_status);
}



// MipiEezuiDisplayAdapter 实现
MipiEezuiDisplayAdapter::MipiEezuiDisplayAdapter(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                                 int width, int height, int offset_x, int offset_y,
                                                 bool mirror_x, bool mirror_y, bool swap_xy)
    : EezuiDisplayAdapter(panel_io, panel, width, height) {
    
    // 注意：LVGL端口初始化应该由Board类负责，这里不再重复初始化
    
    // 配置LVGL显示器
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width),
        .vres = static_cast<uint32_t>(height),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add MIPI DSI display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // 初始化UI
    SetupUI();
}

// ==================== 简化的表情系统实现（与LcdDisplay统一）====================

bool EezuiDisplayAdapter::InitEmotionSystem() {
    if (IsEmotionSystemReady()) {
        return true;
    }


    // 检查SD卡是否已挂载
    if (!sd_scanner_is_mounted()) {
        ESP_LOGE(TAG, "❌ SD卡未挂载，无法初始化表情系统");
        return false;
    }

    // 简化的视频播放器配置（与LcdDisplay一致，基于范例更新）
    emotion_video_config_t config = {
        .output_format = ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE,
        .frame_rate = 30,
        .canvas_width = static_cast<uint32_t>(width_),        // 修复narrowing conversion
        .canvas_height = static_cast<uint32_t>(height_)       // 修复narrowing conversion
    };
    
    // 创建播放器（不再显示加载提示，因为已改为按需加载）
    esp_err_t ret = emotion_video_player_init(&config, &emotion_player_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放器初始化失败: %s", esp_err_to_name(ret));
        return false;
    }

    // 注册回调（参考LcdDisplay的简单回调）
    emotion_video_player_register_frame_callback(emotion_player_, EmotionVideoFrameCallback, this);
    emotion_video_player_register_event_callback(emotion_player_, EmotionVideoEventCallback, this);

    // 创建视频画布
    CreateVideoCanvas();

    // 设置表情系统就绪标志
    init_state_ = static_cast<InitState>(static_cast<int>(init_state_) | static_cast<int>(InitState::EMOTION_READY));
    current_emotion_name_.clear();

    // 隐藏提示（如果之前有显示）
    if (dialogue_box_) {
        if (SafeLVGLLock(200)) {
            lv_obj_add_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            SafeLVGLUnlock();
        }
    }

    // 表情系统初始化完成，播放默认待机表情
    ESP_LOGI(TAG, "🎬 开始播放默认待机表情");
    PlayMjpegEmotion("standby");

    return true;
}

void EezuiDisplayAdapter::DeinitEmotionSystem() {
    if (!IsEmotionSystemReady()) {
        return;
    }


    // 停止播放
    StopEmotionPlayback();
    
    // 清理画布
    if (video_canvas_) {
        if (!SafeLVGLLock(10)) {
            ESP_LOGW(TAG, "⚠️ 无法获取LVGL锁，跳过画布清理");
        } else {
            lv_obj_del(video_canvas_);
            video_canvas_ = nullptr;
            SafeLVGLUnlock();
        }
    }
    
    // 清理播放器
    if (emotion_player_) {
        emotion_video_player_deinit(emotion_player_);
        emotion_player_ = nullptr;
    }

    // 清除表情系统就绪标志
    init_state_ = static_cast<InitState>(static_cast<int>(init_state_) & ~static_cast<int>(InitState::EMOTION_READY));
    current_emotion_name_.clear();

}

// FindMjpegFile函数已移除，映射逻辑统一到emotion_video_player中处理

esp_err_t EezuiDisplayAdapter::PlayMjpegEmotion(const char* emotion_name) {
    if (!IsEmotionSystemReady()) {
        ESP_LOGE(TAG, "表情系统未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    // 处理空字符串和空指针
    if (!emotion_name || strlen(emotion_name) == 0) {
        ESP_LOGW(TAG, "PlayMjpegEmotion收到空表情，使用默认neutral");
        emotion_name = "neutral";
        
    }
    
    
    // 显示视频画布，隐藏背景图片（EezUI特有逻辑）
    if (video_canvas_) {
        if (!SafeLVGLLock(100)) {  // 使用安全的锁机制
            ESP_LOGW(TAG, "无法获取LVGL锁，跳过UI更新");
            // 继续尝试播放，不因UI锁问题阻止视频播放
        } else {
            lv_obj_clear_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
            
            // 隐藏背景图片，显示视频
            if (main_image_) {
                lv_obj_add_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
            }
            
            // 确保对话框在最顶层
            if (dialogue_box_) {
                lv_obj_move_foreground(dialogue_box_);
            }
            
            SafeLVGLUnlock();
        }
    }
    
    // 开始播放（与LcdDisplay一致）
    esp_err_t ret = emotion_video_player_play_emotion(emotion_player_, emotion_name);
    if (ret == ESP_OK) {
        current_emotion_name_ = emotion_name;
    } else {
        ESP_LOGE(TAG, "表情播放失败: %s", esp_err_to_name(ret));
        // 恢复显示
        if (video_canvas_) {
            if (!SafeLVGLLock(100)) {
                ESP_LOGW(TAG, "无法获取LVGL锁，跳过UI恢复");
            } else {
                lv_obj_add_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
                if (main_image_) {
                    lv_obj_clear_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
                }
                SafeLVGLUnlock();
            }
        }
    }
    
    return ret;
}

esp_err_t EezuiDisplayAdapter::ForceStopAndSwitchEmotion(const char* emotion_name) {
    // 所有表情都常驻内存，直接使用普通播放函数
    return PlayMjpegEmotion(emotion_name);
}

void EezuiDisplayAdapter::StopEmotionPlayback() {
    if (!IsEmotionSystemReady()) {
        return;
    }
    
    
    if (emotion_player_) {
        emotion_video_player_play_emotion(emotion_player_, "standby");
    }
    
    // 恢复显示（EezUI特有逻辑）
    if (video_canvas_) {
        if (!SafeLVGLLock(100)) {
            ESP_LOGW(TAG, "无法获取LVGL锁，跳过UI恢复");
        } else {
            lv_obj_add_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
            
            // 恢复背景图片
            if (main_image_) {
                lv_obj_clear_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
            }
            
            SafeLVGLUnlock();
        }
    }
    
    current_emotion_name_.clear();
}

void EezuiDisplayAdapter::CreateVideoCanvas() {
    if (video_canvas_ != nullptr) {
        return;
    }
    
    
    // 获取父容器（EezUI屏幕）
    lv_obj_t* parent_screen = nullptr;
    if (main_image_ && lv_obj_get_parent(main_image_)) {
        parent_screen = lv_obj_get_parent(main_image_);
    } else {
        parent_screen = lv_screen_active();
    }
    
    if (!parent_screen) {
        ESP_LOGE(TAG, "无法获取父容器");
        return;
    }
    
    if (!SafeLVGLLock(100)) {
        ESP_LOGE(TAG, "无法获取LVGL锁");
        return;
    }
    
    // 创建画布对象
    video_canvas_ = lv_canvas_create(parent_screen);
    if (!video_canvas_) {
        ESP_LOGE(TAG, "创建画布失败");
        SafeLVGLUnlock();
        return;
    }
    
    // 分配画布缓冲区（RGB565格式）
    const uint32_t canvas_width = 480;
    const uint32_t canvas_height = 480;
    const uint32_t buf_size = canvas_width * canvas_height * 2; // RGB565 = 2字节/像素
    
    // 优化内存分配：优先使用PSRAM，使用更大的对齐以提高性能
    void* canvas_buf = heap_caps_aligned_calloc(64, 1, buf_size, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        // 回退到内部RAM，使用较小的对齐
        canvas_buf = heap_caps_aligned_calloc(4, 1, buf_size, MALLOC_CAP_8BIT);
        if (!canvas_buf) {
            ESP_LOGW(TAG, "内部RAM分配失败，尝试使用默认分配");
            canvas_buf = malloc(buf_size);
        }
    }
    
    if (!canvas_buf) {
        ESP_LOGE(TAG, "分配画布缓冲区失败 (%lu bytes)", buf_size);
        lv_obj_del(video_canvas_);
        video_canvas_ = nullptr;
        SafeLVGLUnlock();
        return;
    }
    
    // 设置画布缓冲区
    lv_canvas_set_buffer(video_canvas_, canvas_buf, canvas_width, canvas_height, LV_COLOR_FORMAT_RGB565);
    
    // 基础设置
    lv_obj_add_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(video_canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(video_canvas_, lv_color_black(), 0);
    lv_obj_set_style_border_width(video_canvas_, 0, 0);
    lv_obj_set_style_pad_all(video_canvas_, 0, 0);
    lv_obj_set_size(video_canvas_, canvas_width, canvas_height);
    lv_obj_set_pos(video_canvas_, 0, 0);
    
    // 保存缓冲区指针用于清理（简化方案）
    lv_obj_set_user_data(video_canvas_, canvas_buf);
    
    // 添加删除事件回调来清理缓冲区
    lv_obj_add_event_cb(video_canvas_, [](lv_event_t* e) {
        lv_obj_t* target = lv_event_get_target_obj(e);
        void* canvas_buf = lv_obj_get_user_data(target);
        if (canvas_buf) {
            heap_caps_free(canvas_buf);
        }
    }, LV_EVENT_DELETE, nullptr);
    
    SafeLVGLUnlock();
    
    // 创建视频画布后确保电池UI在最顶层
    EnsureUILayerOrder();
    
}

void EezuiDisplayAdapter::EnsureUILayerOrder() {
    if (!SafeLVGLLock(50)) {
        ESP_LOGW(TAG, "无法获取LVGL锁，跳过UI层级更新");
        return;
    }
    

    
    // 确保对话框在前景（但在电池/WiFi UI下方）
    if (dialogue_box_) {
        lv_obj_move_foreground(dialogue_box_);
        lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
    }

    SafeLVGLUnlock();
}

void EezuiDisplayAdapter::EmotionVideoFrameCallback(emotion_video_handle_t handle, uint8_t *frame_data, 
                                                    uint32_t frame_size, uint32_t width, uint32_t height, void *user_data) {
    EezuiDisplayAdapter* adapter = static_cast<EezuiDisplayAdapter*>(user_data);
    
    // 增强参数验证，防止空指针访问
    if (!adapter) {
        return;
    }
    
    // 检查表情系统是否已初始化
    if (!adapter->IsEmotionSystemReady()) {
        return;
    }
    
    if (!adapter->video_canvas_ || !frame_data || frame_size == 0) {
        return;
    }
    
    // 使用非阻塞式锁获取，避免阻塞高频的视频解码任务
    if (!adapter->SafeLVGLLock(5)) {
        // 如果锁被占用，静默跳过本帧，不影响下一帧显示
        return;
    }
    
    // 获取画布缓冲区 (LVGL 9.x API)
    const lv_image_dsc_t* img_dsc = lv_canvas_get_image(adapter->video_canvas_);
    if (!img_dsc) {
        ESP_LOGW(TAG, "无法获取画布图像描述符");
        adapter->SafeLVGLUnlock();
        return;
    }
    if (!img_dsc->data) {
        ESP_LOGW(TAG, "画布图像数据为空");
        adapter->SafeLVGLUnlock();
        return;
    }
    uint8_t* canvas_buf = const_cast<uint8_t*>(static_cast<const uint8_t*>(img_dsc->data));
    
    // 获取画布信息
    lv_coord_t canvas_width = lv_obj_get_width(adapter->video_canvas_);
    lv_coord_t canvas_height = lv_obj_get_height(adapter->video_canvas_);
    
    // 计算实际需要复制的尺寸
    uint32_t copy_width = (width < (uint32_t)canvas_width) ? width : (uint32_t)canvas_width;
    uint32_t copy_height = (height < (uint32_t)canvas_height) ? height : (uint32_t)canvas_height;
    uint32_t expected_size = copy_width * copy_height * 2; // RGB565 = 2字节/像素
    
    // 验证帧大小
    if (frame_size < expected_size) {
        ESP_LOGW(TAG, "帧数据大小不足: %lu < %lu", frame_size, expected_size);
        adapter->SafeLVGLUnlock();
        return;
    }
    
    // 优化内存复制：使用更高效的方法
    if (copy_width == (uint32_t)canvas_width && copy_height == (uint32_t)canvas_height) {
        // 尺寸完全匹配，使用DMA友好的内存复制
        memcpy(canvas_buf, frame_data, expected_size);
    } else {
        // 按行复制（处理尺寸不匹配的情况），优化内存访问模式
        uint8_t* dst_ptr = canvas_buf;
        uint8_t* src_ptr = frame_data;
        const uint32_t dst_stride = canvas_width * 2;
        const uint32_t src_stride = copy_width * 2;
        
        for (uint32_t y = 0; y < copy_height; y++) {
            memcpy(dst_ptr, src_ptr, src_stride);
            dst_ptr += dst_stride;
            src_ptr += src_stride;
        }
    }
    
    // 标记画布需要重绘
    lv_obj_invalidate(adapter->video_canvas_);
    
    // 确保对话框在最前景
    if (adapter->dialogue_box_) {
        lv_obj_move_foreground(adapter->dialogue_box_);
    }
    
    adapter->SafeLVGLUnlock();
}

void EezuiDisplayAdapter::EmotionVideoEventCallback(emotion_video_event_t event, void *user_data) {
    EezuiDisplayAdapter* adapter = static_cast<EezuiDisplayAdapter*>(user_data);
    if (!adapter) {
        return;
    }
    
    // 处理视频加载和播放事件
    switch (event) {
        case EMOTION_VIDEO_EVENT_LOADING_START:
            // 显示"正在加载视频资源"提示
            if (adapter->dialogue_box_) {
                if (adapter->SafeLVGLLock(200)) {
                    lv_label_set_text(adapter->dialogue_box_, "正在加载视频资源");
                    lv_obj_set_style_text_align(adapter->dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_clear_flag(adapter->dialogue_box_, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(adapter->dialogue_box_);
                    adapter->SafeLVGLUnlock();
                }
            }
            break;
            
        case EMOTION_VIDEO_EVENT_LOADING_END:
            // 隐藏"正在加载视频资源"提示（除非是持久化消息）
            if (adapter->dialogue_box_ && !adapter->is_persistent_message_) {
                if (adapter->SafeLVGLLock(200)) {
                    lv_obj_add_flag(adapter->dialogue_box_, LV_OBJ_FLAG_HIDDEN);
                    adapter->SafeLVGLUnlock();
                }
            }
            adapter->EnsureUILayerOrder();
            break;
            
        case EMOTION_VIDEO_EVENT_PLAY_START:
            adapter->EnsureUILayerOrder();
            break;
            
        case EMOTION_VIDEO_EVENT_PLAY_END:
            // 所有表情都常驻内存，无需异步加载
            adapter->EnsureUILayerOrder();
            break;
            
        // EMOTION_VIDEO_EVENT_RELEASE_CACHE 已被移除，不再需要处理
            
        default:
            ESP_LOGW(TAG, "未知视频事件: %d", event);
            break;
    }
}

