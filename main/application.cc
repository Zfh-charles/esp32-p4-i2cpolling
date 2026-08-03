#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#if CONFIG_USE_REMINDER_POLL
#include "reminder/reminder_trace.h"
#include "reminder/reminder_diag.h"
#include "reminder/reminder_lifecycle_trace.h"
#include "reminder/reminder_hw_trace.h"
#include "reminder/boot_trace.h"
#if !CONFIG_REMINDER_MQTT_TLS_INSECURE
#include "reminder/reminder_mqtt_tls.h"
#include <at_modem.h>
#endif
#endif

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
#if CONFIG_USE_REMINDER_POLL
    "alarm",
#endif
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

#if CONFIG_USE_REMINDER_POLL
    esp_timer_create_args_t proactive_reminder_timer_args = {
        .callback = [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            app->Schedule([app]() { app->CancelPendingReminderAck(); });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reminder_timeout",
        .skip_unhandled_events = true
    };
    esp_timer_create(&proactive_reminder_timer_args, &proactive_reminder_timer_handle_);

    esp_timer_create_args_t proactive_alarm_timer_args = {
        .callback = [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            app->Schedule([app]() { app->StopProactiveAlarm("timeout"); });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reminder_alarm_timeout",
        .skip_unhandled_events = true
    };
    esp_timer_create(&proactive_alarm_timer_args, &proactive_alarm_timer_handle_);
#endif
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
#if CONFIG_USE_REMINDER_POLL
    if (proactive_reminder_timer_handle_ != nullptr) {
        esp_timer_stop(proactive_reminder_timer_handle_);
        esp_timer_delete(proactive_reminder_timer_handle_);
    }
    if (proactive_alarm_timer_handle_ != nullptr) {
        esp_timer_stop(proactive_alarm_timer_handle_);
        esp_timer_delete(proactive_alarm_timer_handle_);
    }
#endif
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        PlaySound(sound);
    }
#if CONFIG_USE_REMINDER_POLL
    if (!pending_reminder_ack_id_.empty() && !IsReminderAlarmRinging() &&
        !proactive_alarm_pending_start_ && status != nullptr &&
        strcmp(status, Lang::Strings::ERROR) == 0) {
        Schedule([this]() { CancelPendingReminderAck(); });
    }
#endif
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
      //  display->SetEmotion("neutral");
        display->SetEmotion("standby");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

#if CONFIG_USE_REMINDER_POLL
    if (IsReminderAlarmRinging()) {
        RequestStopReminderAlarm("toggle");
        return;
    }
#endif

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

#if CONFIG_USE_REMINDER_POLL
    if (session_kind_ == SessionKind::ProactiveReminder) {
        ESP_LOGW(TAG, "Ignore toggle during proactive reminder");
        return;
    }
#endif

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

#if CONFIG_USE_REMINDER_POLL
    if (IsReminderAlarmRinging()) {
        RequestStopReminderAlarm("start_listening");
        return;
    }
    if (session_kind_ == SessionKind::ProactiveReminder) {
        ESP_LOGW(TAG, "Ignore start listening during proactive reminder");
        return;
    }
#endif

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_capture_restored = [this]() {
        Schedule([this]() {
            if (device_state_ == kDeviceStateIdle && session_kind_ == SessionKind::None) {
                RestoreIdleReady(true);
            }
        });
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        auto push_audio = [this](std::unique_ptr<AudioStreamPacket> pkt) {
#if CONFIG_USE_REMINDER_POLL
            if (session_kind_ == SessionKind::ProactiveReminder) {
                proactive_audio_packets_++;
                ReminderLcTtsAudio(ReminderLcOwner::Proactive, proactive_audio_packets_, 1);
                if (proactive_audio_packets_ == 1) {
                    ReminderUiTraceTts("audio_first", pending_reminder_ack_id_.c_str(), 1);
                }
            } else if (session_kind_ == SessionKind::User) {
                ReminderLcTtsAudio(ReminderLcOwner::User, -1, 1);
            }
#endif
            audio_service_.PushPacketToDecodeQueue(std::move(pkt));
        };
        if (device_state_ == kDeviceStateSpeaking) {
            push_audio(std::move(packet));
#if CONFIG_USE_REMINDER_POLL
        } else if (session_kind_ == SessionKind::ProactiveReminder || audio_service_.IsSpeakerPlaybackHeld() ||
                   audio_service_.GetAudioRoute() == AudioRoute::Playback) {
            if (session_kind_ == SessionKind::ProactiveReminder &&
                !IsReminderAlarmRinging() &&
                device_state_ != kDeviceStateSpeaking) {
                audio_service_.PrepareSpeakerPlayback();
                Schedule([this]() {
                    if (device_state_ != kDeviceStateSpeaking) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            }
            push_audio(std::move(packet));
#endif
        } else {
#if CONFIG_USE_REMINDER_POLL
            ESP_LOGW(TAG, "Dropped audio packet (state=%s session=%d route=%s)",
                     STATE_STRINGS[device_state_], (int)session_kind_,
                     ReminderTraceAudioRoute(audio_service_.GetAudioRoute()));
            ReminderLcMark(session_kind_ == SessionKind::ProactiveReminder ? ReminderLcOwner::Proactive
                           : session_kind_ == SessionKind::User ? ReminderLcOwner::User
                           : ReminderLcOwner::Standby,
                           ReminderLcPhase::TtsAudio, 0, "dropped");
            ReminderUiTraceTts("audio_dropped", STATE_STRINGS[device_state_], 0);
#endif
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
#if CONFIG_USE_REMINDER_POLL
        if (session_kind_ == SessionKind::ProactiveReminder) {
            ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::ChannelOpen, 1, pending_reminder_ack_id_.c_str());
        } else if (session_kind_ == SessionKind::User) {
            ReminderLcMark(ReminderLcOwner::User, ReminderLcPhase::ChannelOpen, 1, nullptr);
        }
#endif
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            bool keep_reminder_alarm = false;
#if CONFIG_USE_REMINDER_POLL
            if (session_kind_ == SessionKind::ProactiveReminder) {
                audio_service_.ReleasePlaybackPrebuffer();
                if (IsReminderAlarmRinging() && proactive_alarm_tts_playing_) {
                    proactive_alarm_tts_playing_ = false;
                    QueueProactiveAlarmAfterSpeech();
                }
                if (IsReminderAlarmRinging() || proactive_alarm_pending_start_) {
                    keep_reminder_alarm = true;
                    ReminderTraceLog("channel_closed_alarm_continues");
                } else if (!pending_reminder_ack_id_.empty() &&
                           proactive_reminder_tts_started_ && proactive_audio_packets_ > 0) {
                    if (proactive_alarm_mode_ == ReminderAlarmMode::kRingRepeat) {
                        QueueProactiveAlarmAfterSpeech();
                        keep_reminder_alarm = true;
                    } else {
                        ReminderTraceLog("proactive_no_alarm_complete", pending_reminder_ack_id_.c_str());
                        CompletePendingReminderAck(true);
                    }
                } else if (!pending_reminder_ack_id_.empty()) {
                    ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::ChannelClose, 0, "early_close");
                    CancelPendingReminderAck();
                } else {
                    SetSessionKind(SessionKind::None, "channel_closed_proactive_done");
                }
            } else if (session_kind_ == SessionKind::User) {
                SetSessionKind(SessionKind::None, "channel_closed_user");
                ReminderLcMark(ReminderLcOwner::User, ReminderLcPhase::ChannelClose, 1, nullptr);
                ReminderLcSummary(ReminderLcOwner::User);
            }
#endif
            ReminderTraceLog("channel_closed");
            auto display = Board::GetInstance().GetDisplay();
            if (!keep_reminder_alarm && !suppress_display_clear_on_channel_close_) {
                display->SetChatMessage("system", "");
            }
            suppress_display_clear_on_channel_close_ = false;
#if CONFIG_USE_REMINDER_POLL
            if (keep_reminder_alarm) {
                // The cloud speech channel is no longer needed; the local alarm owns the reminder now.
            } else if (pending_reminder_after_channel_close_) {
                pending_reminder_after_channel_close_ = false;
                ReminderDeliverPayload payload = pending_reminder_payload_;
                Schedule([this, payload]() { RunReminderDelivery(payload); });
            } else if (session_kind_ == SessionKind::None && device_state_ == kDeviceStateIdle) {
                if (suppress_channel_close_rearm_) {
                    suppress_channel_close_rearm_ = false;
                    ReminderTraceLog("channel_closed_skip_rearm", "enter_idle_standby");
                } else {
                    const int64_t now_us = esp_timer_get_time();
                    if ((now_us - last_idle_standby_us_) >= 2 * 1000000LL) {
                        EnterIdleStandby(true);
                    } else {
                        ReminderTraceLog("channel_closed_skip_rearm", "debounce");
                    }
                }
            }
#endif
            if (!keep_reminder_alarm && device_state_ != kDeviceStateIdle) {
                SetDeviceState(kDeviceStateIdle);
            }
#if CONFIG_USE_REMINDER_POLL
            else if (!keep_reminder_alarm && session_kind_ == SessionKind::None) {
                RunReminderDiagnostics("channel_closed_idle", true);
            }
#endif
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
#if CONFIG_USE_REMINDER_POLL
                    if (session_kind_ == SessionKind::ProactiveReminder) {
                        proactive_alarm_tts_playing_ = true;
                        if (IsReminderAlarmRinging()) {
                            proactive_alarm_pending_since_us_ = 0;
                        }
                        audio_service_.PrepareSpeakerPlayback();
                    }
                    if (!pending_reminder_ack_id_.empty()) {
                        proactive_reminder_tts_started_ = true;
                        StopProactiveReminderTimeout();
                        ReminderTraceLog("proactive_tts_start", pending_reminder_ack_id_.c_str());
                        ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::TtsJsonStart, 1,
                                       pending_reminder_ack_id_.c_str());
                        ReminderUiTraceTts("json_start", pending_reminder_ack_id_.c_str(), 1);
                        if (!last_reminder_emotion_.empty()) {
                            auto display = Board::GetInstance().GetDisplay();
                            display->SetEmotion(last_reminder_emotion_.c_str());
                            ReminderLcEmotion(ReminderLcOwner::Proactive, last_reminder_emotion_.c_str(), 1);
                        }
                    } else if (session_kind_ == SessionKind::User) {
                        ReminderLcMark(ReminderLcOwner::User, ReminderLcPhase::TtsJsonStart, 1, nullptr);
                        ReminderUiTraceTts("json_start", "user", 1);
                    }
#endif
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    bool keep_proactive_alarm = false;
#if CONFIG_USE_REMINDER_POLL
                    const bool proactive = session_kind_ == SessionKind::ProactiveReminder;
                    if (proactive) {
                        // A short final sentence may contain fewer than the target
                        // number of frames. Release it before drain/ACK handling.
                        audio_service_.ReleasePlaybackPrebuffer();
                        proactive_alarm_tts_playing_ = false;
                        ReminderTraceLog("proactive_tts_stop", pending_reminder_ack_id_.c_str());
                        ESP_LOGI(TAG, "Proactive TTS stop id=%s audio_packets=%d",
                                 pending_reminder_ack_id_.c_str(), proactive_audio_packets_);
                        ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::TtsJsonStop,
                                       proactive_audio_packets_ > 0 ? 1 : 0,
                                       pending_reminder_ack_id_.c_str());
                        ReminderUiTraceTts("json_stop",
                                           proactive_audio_packets_ > 0 ? "ok" : "no_audio", 1);
                        if (proactive_audio_packets_ == 0) {
                            if (IsReminderAlarmRinging()) {
                                QueueProactiveAlarmAfterSpeech();
                                keep_proactive_alarm = true;
                            } else {
                            ESP_LOGW(TAG, "Proactive reminder %s: TTS stop with 0 audio packets",
                                     pending_reminder_ack_id_.c_str());
                            ReminderTraceLog("proactive_tts_no_audio", pending_reminder_ack_id_.c_str());
                            proactive_reminder_tts_started_ = false;
                            CancelPendingReminderAck();
                            }
                        } else if (proactive_alarm_mode_ == ReminderAlarmMode::kRingRepeat) {
                            QueueProactiveAlarmAfterSpeech();
                            keep_proactive_alarm = true;
                        } else {
                            ReminderTraceLog("proactive_no_alarm_complete",
                                             pending_reminder_ack_id_.c_str());
                            CompletePendingReminderAck(true);
                        }
                    } else if (session_kind_ == SessionKind::User) {
                        ReminderLcMark(ReminderLcOwner::User, ReminderLcPhase::TtsJsonStop, 1, nullptr);
                        ReminderUiTraceTts("json_stop", "user", 1);
                    }
#endif
#if CONFIG_USE_REMINDER_POLL
                    if (!keep_proactive_alarm && proactive &&
                        device_state_ == kDeviceStateSpeaking) {
                        // UDP audio is queued independently from MQTT tts:stop.
                        // Keep the speaker route until CompletePendingReminderAck()
                        // observes an empty local queue; switching to Listening here
                        // would ResetDecoder() and cut the buffered tail.
                        ReminderTraceLog("proactive_wait_local_audio_drain");
                    } else
#endif
                    if (!keep_proactive_alarm && device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
#if CONFIG_USE_REMINDER_POLL
                        const ReminderLcOwner owner = session_kind_ == SessionKind::ProactiveReminder
                            ? ReminderLcOwner::Proactive
                            : ReminderLcOwner::User;
                        ReminderLcDisplay(owner, "assistant", message.c_str(), 1);
#endif
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
#if CONFIG_USE_REMINDER_POLL
                    if (session_kind_ == SessionKind::ProactiveReminder) {
                        ReminderLcDisplay(ReminderLcOwner::Proactive, "user", message.c_str(), 0);
                        return;
                    }
                    ReminderLcDisplay(ReminderLcOwner::User, "user", message.c_str(), 1);
#endif
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
#if CONFIG_USE_REMINDER_POLL
                    if (session_kind_ == SessionKind::ProactiveReminder && !proactive_reminder_tts_started_) {
                        ReminderLcEmotion(ReminderLcOwner::Proactive, emotion_str.c_str(), 0);
                        return;
                    }
                    const ReminderLcOwner owner = session_kind_ == SessionKind::ProactiveReminder
                        ? ReminderLcOwner::Proactive
                        : session_kind_ == SessionKind::User ? ReminderLcOwner::User
                        : ReminderLcOwner::Standby;
                    ReminderLcEmotion(owner, emotion_str.c_str(), 1);
#endif
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
#if CONFIG_USE_REMINDER_POLL
    reminder_poller_ = new ReminderPoller();
#if CONFIG_REMINDER_MQTT_WAKE
    reminder_mqtt_wake_ = new ReminderMqttWake();
#endif
    deferred_reminder_services_pending_ = true;
    wake_running_since_us_ = 0;
    ESP_LOGI(TAG, "Reminder net deferred until idle + wake stable (%ds after wake)",
             CONFIG_REMINDER_BOOT_DEFER_SEC);
#endif
    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        audio_service_.DrainLocalPlayback(2000);
    }
    SetDeviceState(kDeviceStateIdle);
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
#if CONFIG_USE_REMINDER_POLL
            TryStartDeferredReminderNet();
            ServiceProactiveAlarm();
#endif
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
#if CONFIG_USE_REMINDER_POLL
            if (clock_ticks_ % 10 == 0) {
                RunReminderDiagnostics("tick_10s", true);
            }
            if (clock_ticks_ % 60 == 0) {
                ReminderDiagLogSnapshot("diag_minute", BuildReminderDiagSnapshot());
            }
#endif
        }
    }
}

void Application::OnWakeWordDetected() {
#if CONFIG_USE_REMINDER_POLL
    if (IsReminderAlarmRinging()) {
        StopProactiveAlarm("wake_word");
        return;
    }
#endif
    if (!protocol_) {
        return;
    }

#if CONFIG_USE_REMINDER_POLL
    if (session_kind_ == SessionKind::ProactiveReminder) {
        ReminderTraceLog("wake_word_blocked", "proactive_session");
        ESP_LOGW(TAG, "Ignore wake word during proactive reminder session");
        return;
    }
#endif

    if (device_state_ == kDeviceStateIdle) {
#if CONFIG_USE_REMINDER_POLL
        SetSessionKind(SessionKind::User, "wake_word_idle");
        ReminderLcBegin(ReminderLcOwner::User, "wake_word_idle");
#endif
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnsureIdleCapture();
                audio_service_.EnableWakeWordDetection(true);
#if CONFIG_USE_REMINDER_POLL
                ReminderLcMark(ReminderLcOwner::User, ReminderLcPhase::ChannelOpen, 0, "open_failed");
                ReminderLcSummary(ReminderLcOwner::User);
                SetSessionKind(SessionKind::None, "wake_open_channel_failed");
#endif
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_REMINDER_POLL
        ReminderTraceLog("wake_word_detected", wake_word.c_str());
        ReminderLcMark(ReminderLcOwner::User, ReminderLcPhase::WakeDetect, 1, wake_word.c_str());
#endif
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
#if CONFIG_USE_REMINDER_POLL
    else {
        ReminderTraceLog("wake_word_blocked", STATE_STRINGS[device_state_]);
        ESP_LOGW(TAG, "Wake word ignored in state %s (session=%s)",
                 STATE_STRINGS[device_state_],
                 ReminderTraceSessionKindName(static_cast<int>(session_kind_)));
    }
#else
    else {
        ESP_LOGW(TAG, "Wake word ignored in state %s", STATE_STRINGS[device_state_]);
    }
#endif
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::ApplyAudioPolicyForState(DeviceState state, DeviceState previous_state) {
    auto& audio = audio_service_;

    switch (state) {
        case kDeviceStateIdle:
            audio.EnableVoiceProcessing(false);
#if CONFIG_USE_REMINDER_POLL
            if (previous_state == kDeviceStateAlarm) {
                audio.EndSpeakerPlayback();
            }
#endif
#if CONFIG_USE_REMINDER_POLL
            if (session_kind_ != SessionKind::ProactiveReminder) {
                if (audio.IsSpeakerPlaybackHeld() || audio.GetAudioRoute() == AudioRoute::Playback) {
                    audio.EndSpeakerPlayback();
                } else {
                    const bool session_ended = previous_state == kDeviceStateSpeaking ||
                        previous_state == kDeviceStateListening ||
                        previous_state == kDeviceStateAlarm;
                    auto* codec = Board::GetInstance().GetAudioCodec();
                    const bool capture_ok = audio.GetAudioRoute() == AudioRoute::Capture &&
                        codec != nullptr && codec->input_enabled() &&
                        !session_ended;
                    if (!capture_ok) {
                        audio.SetAudioRoute(AudioRoute::Capture, session_ended);
                    }
                }
                audio.EnableWakeWordDetection(true);
            }
#else
            audio.EnableWakeWordDetection(true);
#endif
            break;

        case kDeviceStateListening:
#if CONFIG_USE_REMINDER_POLL
            if (session_kind_ == SessionKind::ProactiveReminder) {
                audio.SetAudioRoute(AudioRoute::Capture);
                if (protocol_ && !audio.IsAudioProcessorRunning()) {
                    protocol_->SendStartListening(listening_mode_);
                    audio.EnableVoiceProcessing(true);
                    audio.EnableWakeWordDetection(false);
                }
                break;
            }
#endif
            /* User conversation: ori-ref soft path — no I2S hard switch */
            if (protocol_ && !audio.IsAudioProcessorRunning()) {
                protocol_->SendStartListening(listening_mode_);
                audio.EnableVoiceProcessing(true);
                audio.EnableWakeWordDetection(false);
            }
            break;

        case kDeviceStateSpeaking:
#if CONFIG_USE_REMINDER_POLL
            if (session_kind_ == SessionKind::ProactiveReminder) {
                audio.EnableVoiceProcessing(false);
                audio.EnableWakeWordDetection(false);
                audio.SetAudioRoute(AudioRoute::Playback);
                break;
            }
#endif
            /* User conversation: ori-ref soft path — mic/speaker via Enable* only */
            if (listening_mode_ != kListeningModeRealtime) {
                audio.EnableVoiceProcessing(false);
                audio.EnableWakeWordDetection(audio.IsAfeWakeWord());
            }
            audio.ResetDecoder();
            break;

#if CONFIG_USE_REMINDER_POLL
        case kDeviceStateAlarm:
            audio.ResetDecoder();
            audio.EnableVoiceProcessing(false);
            audio.SetAudioRoute(AudioRoute::Duplex, true);
            // Switch the route first; StartProactiveAlarm() enables the detector
            // after the transition so a wake word can stop the active ring.
            audio.EnableWakeWordDetection(false);
            break;
#endif

        default:
            break;
    }
}

#if CONFIG_USE_REMINDER_POLL
void Application::SetSessionKind(SessionKind kind, const char* reason) {
    if (session_kind_ == kind) {
        return;
    }
    ESP_LOGI(REMINDER_TRACE_TAG, "session %s -> %s (%s)",
             ReminderTraceSessionKindName(static_cast<int>(session_kind_)),
             ReminderTraceSessionKindName(static_cast<int>(kind)),
             reason ? reason : "?");
    session_kind_ = kind;
    UpdateCapturePowerHold();
}

void Application::ReminderTraceLog(const char* event, const char* detail) const {
    ReminderDiagRecordEvent(event);
    auto* codec = Board::GetInstance().GetAudioCodec();
    const bool ch = protocol_ != nullptr && protocol_->IsAudioChannelOpened();
    if (detail != nullptr && detail[0] != '\0') {
        ESP_LOGI(REMINDER_TRACE_TAG,
                 "%s | session=%s state=%s route=%s ch=%d wake=%d voice=%d hold=%d codec_in=%d codec_out=%d pending=%s | %s",
                 event,
                 ReminderTraceSessionKindName(static_cast<int>(session_kind_)),
                 STATE_STRINGS[device_state_],
                 ReminderTraceAudioRoute(audio_service_.GetAudioRoute()),
                 ch ? 1 : 0,
                 audio_service_.IsWakeWordRunning() ? 1 : 0,
                 audio_service_.IsAudioProcessorRunning() ? 1 : 0,
                 audio_service_.IsSpeakerPlaybackHeld() ? 1 : 0,
                 codec != nullptr && codec->input_enabled() ? 1 : 0,
                 codec != nullptr && codec->output_enabled() ? 1 : 0,
                 pending_reminder_ack_id_.empty() ? "-" : pending_reminder_ack_id_.c_str(),
                 detail);
    } else {
        ESP_LOGI(REMINDER_TRACE_TAG,
                 "%s | session=%s state=%s route=%s ch=%d wake=%d voice=%d hold=%d codec_in=%d codec_out=%d pending=%s",
                 event,
                 ReminderTraceSessionKindName(static_cast<int>(session_kind_)),
                 STATE_STRINGS[device_state_],
                 ReminderTraceAudioRoute(audio_service_.GetAudioRoute()),
                 ch ? 1 : 0,
                 audio_service_.IsWakeWordRunning() ? 1 : 0,
                 audio_service_.IsAudioProcessorRunning() ? 1 : 0,
                 audio_service_.IsSpeakerPlaybackHeld() ? 1 : 0,
                 codec != nullptr && codec->input_enabled() ? 1 : 0,
                 codec != nullptr && codec->output_enabled() ? 1 : 0,
                 pending_reminder_ack_id_.empty() ? "-" : pending_reminder_ack_id_.c_str());
    }
}

ReminderDiagSnapshot Application::BuildReminderDiagSnapshot() {
    ReminderDiagSnapshot snap;
    snap.session_kind = static_cast<int>(session_kind_);
    snap.device_state = device_state_;
    snap.route = audio_service_.GetAudioRoute();
    snap.channel_open = protocol_ != nullptr && protocol_->IsAudioChannelOpened();
    snap.wake_running = audio_service_.IsWakeWordRunning();
    snap.voice_running = audio_service_.IsAudioProcessorRunning();
    snap.hold = audio_service_.IsSpeakerPlaybackHeld();
    auto* codec = Board::GetInstance().GetAudioCodec();
    snap.codec_in = codec != nullptr && codec->input_enabled();
    snap.codec_out = codec != nullptr && codec->output_enabled();
    snap.alarm_ringing = IsReminderAlarmRinging();
    snap.can_deliver = CanDeliverReminder();
    snap.audio_idle = audio_service_.IsIdle();
    auto audio_diag = audio_service_.GetDiagnosticState();
    snap.input_age_ms = audio_diag.input_age_ms;
    snap.output_age_ms = audio_diag.output_age_ms;
    snap.decode_q = audio_diag.decode_queue;
    snap.playback_q = audio_diag.playback_queue;
    snap.send_q = audio_diag.send_queue;
    snap.input_frames = audio_diag.input_frames;
    snap.playback_frames = audio_diag.playback_frames;
    snap.pending_id = pending_reminder_ack_id_.empty() ? "-" : pending_reminder_ack_id_.c_str();
    snap.boot_wake_deferred = deferred_reminder_services_pending_;
    return snap;
}

void Application::RunReminderDiagnostics(const char* trigger, bool allow_auto_heal) {
    auto snap = BuildReminderDiagSnapshot();
    const uint32_t mask = ReminderDiagEvaluate(snap, trigger);
    if (mask == kDiagNone) {
        return;
    }
#if CONFIG_REMINDER_DIAG_AUTO_HEAL
    if (!allow_auto_heal) {
        return;
    }
    if (deferred_reminder_services_pending_ && !audio_service_.IsWakeWordRunning()) {
        return;
    }
    if (deferred_reminder_services_pending_ && wake_running_since_us_ > 0) {
        const int64_t wake_elapsed_s =
            (esp_timer_get_time() - wake_running_since_us_) / 1000000LL;
        if (wake_elapsed_s < CONFIG_REMINDER_BOOT_DEFER_SEC) {
            return;
        }
    }
    static int64_t last_heal_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_heal_us < 30 * 1000000LL) {
        return;
    }
    const uint32_t heal_mask = kDiagIdleWakeOff | kDiagIdleMicOff | kDiagHoldStuckIdle |
        kDiagVoiceMicOff | kDiagIdleUserStale;
    if (snap.device_state == kDeviceStateIdle &&
        (snap.session_kind == 0 || (mask & kDiagIdleUserStale)) &&
        (mask & heal_mask)) {
        if (mask & kDiagIdleUserStale) {
            SetSessionKind(SessionKind::None, "auto_heal_user_stale");
        }
        last_heal_us = now_us;
        ReminderDiagLogAutoHeal(mask);
        Schedule([this]() { EnterIdleStandby(true); });
    }
#else
    (void)allow_auto_heal;
#endif
}
#endif

#if CONFIG_USE_REMINDER_POLL
void Application::TryStartDeferredReminderNet() {
    if (!deferred_reminder_services_pending_ || reminder_poller_ == nullptr) {
        return;
    }
    if (session_kind_ != SessionKind::None || device_state_ != kDeviceStateIdle) {
        return;
    }
    if (!audio_service_.IsWakeWordRunning()) {
        wake_running_since_us_ = 0;
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if (wake_running_since_us_ == 0) {
        wake_running_since_us_ = now_us;
        ESP_LOGI(TAG, "Wake armed — reminder net in %ds", CONFIG_REMINDER_BOOT_DEFER_SEC);
        return;
    }
    const int64_t wake_elapsed_s = (now_us - wake_running_since_us_) / 1000000LL;
    if (wake_elapsed_s < CONFIG_REMINDER_BOOT_DEFER_SEC) {
        return;
    }

    deferred_reminder_services_pending_ = false;
    BootTraceMarkHeap("REMINDER_NET_START");
#if CONFIG_USE_REMINDER_POLL && !CONFIG_REMINDER_MQTT_TLS_INSECURE
    if (auto* at_modem = dynamic_cast<AtModem*>(Board::GetInstance().GetNetwork())) {
        if (!ReminderMqttInstallCaCert(at_modem->GetAtUart())) {
            ESP_LOGW(TAG, "Reminder TLS CA install failed; MQTT/HTTPS may fail");
        }
    }
#endif
    reminder_poller_->Start();
#if CONFIG_REMINDER_MQTT_WAKE
    if (reminder_mqtt_wake_ != nullptr) {
        reminder_mqtt_wake_->Start(reminder_poller_);
    }
#endif
    ESP_LOGI(TAG, "Reminder network started %ds after wake ready", (int)wake_elapsed_s);
    ReminderTraceLog("reminder_net_deferred_start", nullptr);
}
#endif

void Application::UpdateCapturePowerHold() {
#if CONFIG_USE_REMINDER_POLL
    const bool hold = idle_rearm_in_progress_ ||
                      session_kind_ != SessionKind::None ||
                      device_state_ == kDeviceStateConnecting ||
                      device_state_ == kDeviceStateListening ||
                      device_state_ == kDeviceStateSpeaking;
#else
    const bool hold = device_state_ == kDeviceStateConnecting ||
                      device_state_ == kDeviceStateListening ||
                      device_state_ == kDeviceStateSpeaking;
#endif
#if CONFIG_USE_REMINDER_POLL
    if (device_state_ == kDeviceStateAlarm) {
        audio_service_.SetCapturePowerHold(true);
        return;
    }
#endif
    audio_service_.SetCapturePowerHold(hold);
}

void Application::EnterIdleStandby(bool show_wake_hint) {
#if CONFIG_USE_REMINDER_POLL
    const int64_t now_us = esp_timer_get_time();
    constexpr int64_t kIdleStandbyDebounceUs = 2 * 1000000LL;
    if (device_state_ == kDeviceStateIdle && session_kind_ == SessionKind::None &&
        (now_us - last_idle_standby_us_) < kIdleStandbyDebounceUs) {
        ReminderTraceLog("enter_idle_standby_skip", "debounce");
        if (reminder_poller_ != nullptr) {
            reminder_poller_->NotifyIdleReady();
        }
        if (show_wake_hint) {
            Board::GetInstance().GetDisplay()->SetChatMessage("system", "请说：你好，小易！唤醒我吧！");
        }
        return;
    }
    last_idle_standby_us_ = now_us;
    idle_rearm_in_progress_ = true;
    UpdateCapturePowerHold();
    proactive_alarm_active_.store(false);
    proactive_alarm_pending_start_ = false;
    proactive_alarm_tts_playing_ = false;
    proactive_alarm_ring_playing_ = false;
    proactive_alarm_repeat_speech_enabled_ = false;
    proactive_alarm_delivered_ = false;
    proactive_alarm_mode_ = ReminderAlarmMode::kNone;
    proactive_alarm_pending_since_us_ = 0;
    proactive_alarm_ring_until_us_ = 0;
    proactive_alarm_repeat_detect_text_.clear();
    StopProactiveAlarmTimeout();
    StopProactiveReminderTimeout();
    ReminderTraceLog("enter_idle_standby", show_wake_hint ? "wake_hint" : "silent");
    ReminderLcBegin(ReminderLcOwner::Standby, show_wake_hint ? "wake_hint" : "silent");
    SetSessionKind(SessionKind::None, "idle_standby");
#endif
    if (protocol_) {
        audio_service_.EnableVoiceProcessing(false);
        if (protocol_->IsAudioChannelOpened()) {
            protocol_->SendStopListening();
#if CONFIG_USE_REMINDER_POLL
            suppress_channel_close_rearm_ = true;
#endif
            protocol_->CloseAudioChannel();
        }
    }
    audio_service_.ResetDecoder();
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EndSpeakerPlayback();

    const auto previous_state = device_state_;
#if CONFIG_USE_REMINDER_POLL
    last_reminder_display_text_.clear();
    last_reminder_emotion_.clear();
#endif
    if (device_state_ != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }
    RestoreIdleReady(true);
#if CONFIG_USE_REMINDER_POLL
    {
        auto* codec = Board::GetInstance().GetAudioCodec();
        const bool wake_armed = audio_service_.IsWakeWordRunning() &&
            audio_service_.GetAudioRoute() == AudioRoute::Capture &&
            codec != nullptr && codec->input_enabled();
        ReminderLcMark(ReminderLcOwner::Standby, ReminderLcPhase::WakeArm, wake_armed ? 1 : 0,
                       wake_armed ? "armed" : "wake_or_mic_off");
    }
    idle_rearm_in_progress_ = false;
    UpdateCapturePowerHold();
    (void)previous_state;
#endif
    if (show_wake_hint) {
        Board::GetInstance().GetDisplay()->SetChatMessage("system", "请说：你好，小易！唤醒我吧！");
#if CONFIG_USE_REMINDER_POLL
        ReminderLcDisplay(ReminderLcOwner::Standby, "system", "请说：你好，小易！唤醒我吧！", 1);
#endif
    }
#if CONFIG_USE_REMINDER_POLL
    ReminderLcSummary(ReminderLcOwner::Standby);
    ReminderHwTraceSnapshot("enter_idle_standby");
    RunReminderDiagnostics("enter_idle_standby", false);
    if (reminder_poller_ != nullptr) {
        reminder_poller_->NotifyIdleReady();
    }
#endif
}

void Application::RestoreIdleReady(bool force_capture) {
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EndSpeakerPlayback();
    audio_service_.SetAudioRoute(AudioRoute::Capture, force_capture);
    auto* codec = Board::GetInstance().GetAudioCodec();
    const bool wake_ok = audio_service_.IsWakeWordRunning() &&
        audio_service_.GetAudioRoute() == AudioRoute::Capture &&
        codec != nullptr && codec->input_enabled();
    if (!wake_ok || force_capture) {
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.EnableWakeWordDetection(true);
    }
#if CONFIG_USE_REMINDER_POLL
    ReminderTraceLog("restore_idle_ready", force_capture ? "force" : "skip_wake_restart");
#else
    ESP_LOGI(TAG, "RestoreIdleReady (force=%d)", force_capture);
#endif
}

void Application::EndSessionAndRestoreIdle(bool force_capture) {
    (void)force_capture;
#if CONFIG_USE_REMINDER_POLL
    EnterIdleStandby(true);
#else
    if (protocol_) {
        audio_service_.EnableVoiceProcessing(false);
        if (protocol_->IsAudioChannelOpened()) {
            protocol_->SendStopListening();
            protocol_->CloseAudioChannel();
        }
    }
    audio_service_.ResetDecoder();
    RestoreIdleReady(true);
    if (device_state_ != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }
#endif
}

#if CONFIG_USE_REMINDER_POLL
bool Application::IsUserConversationActive() const {
    if (session_kind_ == SessionKind::ProactiveReminder) {
        return false;
    }
    return device_state_ == kDeviceStateConnecting ||
           device_state_ == kDeviceStateListening ||
           device_state_ == kDeviceStateSpeaking;
}

bool Application::CanDeliverReminder() const {
    if (session_kind_ != SessionKind::None) {
        return false;
    }
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }
    return true;
}
#endif

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }

#if CONFIG_USE_REMINDER_POLL
    if (state == kDeviceStateIdle && session_kind_ == SessionKind::User) {
        /* User turn ended — must clear session so idle re-enables wake word + reminder poll. */
        SetSessionKind(SessionKind::None, "idle_user_standby");
    }
#endif
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);
#if CONFIG_USE_REMINDER_POLL
    if (state == kDeviceStateIdle) {
        BootTraceMarkHeap("STATE_IDLE");
    }
    ReminderTraceLog("state_change", STATE_STRINGS[previous_state]);
#endif

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("standby");
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("standby");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("standby");
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            break;
#if CONFIG_USE_REMINDER_POLL
        case kDeviceStateAlarm:
            display->SetStatus(Lang::Strings::ALARM);
            break;
#endif
        default:
            break;
    }

    ApplyAudioPolicyForState(state, previous_state);
    UpdateCapturePowerHold();
#if CONFIG_USE_REMINDER_POLL
    if (state == kDeviceStateIdle) {
        TryStartDeferredReminderNet();
        RunReminderDiagnostics("enter_idle", false);
        if (session_kind_ == SessionKind::None && !idle_rearm_in_progress_ &&
            reminder_poller_ != nullptr) {
            reminder_poller_->NotifyIdleReady();
        }
    }
#endif
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";
    
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);
    
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [display](int progress, size_t speed) {
        std::thread([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
        }).detach();
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveMode(true); // Restore power save mode
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

#if CONFIG_USE_REMINDER_POLL
    if (IsReminderAlarmRinging()) {
        RequestStopReminderAlarm("wake_invoke");
        return;
    }
    if (session_kind_ == SessionKind::ProactiveReminder) {
        ESP_LOGW(TAG, "Ignore wake invoke during proactive reminder");
        return;
    }
#endif

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnsureIdleCapture();
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

#if CONFIG_USE_REMINDER_POLL
    if (IsReminderAlarmRinging() || proactive_alarm_pending_start_) {
        return false;
    }
#endif
#if CONFIG_USE_REMINDER_POLL
    if (session_kind_ != SessionKind::None) {
        return false;
    }
#endif

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
    Schedule([this]() {
        auto wait = std::make_shared<std::function<void(int)>>();
        *wait = [this, wait](int attempt) {
            if (!audio_service_.IsIdle() && attempt < 120) {
                Schedule([wait, attempt]() { (*wait)(attempt + 1); });
                return;
            }
#if CONFIG_USE_REMINDER_POLL
            if (device_state_ == kDeviceStateIdle && session_kind_ == SessionKind::None) {
#else
            if (device_state_ == kDeviceStateIdle) {
#endif
                audio_service_.EnsureIdleCapture();
                audio_service_.EnableWakeWordDetection(true);
            }
        };
        (*wait)(0);
    });
}

static std::string SanitizeReminderText(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '\"'), text.end());
    return text;
}

#if CONFIG_USE_REMINDER_POLL
void Application::StartProactiveReminderTimeout() {
    if (proactive_reminder_timer_handle_ == nullptr) {
        return;
    }
    esp_timer_stop(proactive_reminder_timer_handle_);
    esp_timer_start_once(proactive_reminder_timer_handle_, 90 * 1000000LL);
}

void Application::StopProactiveReminderTimeout() {
    if (proactive_reminder_timer_handle_ != nullptr) {
        esp_timer_stop(proactive_reminder_timer_handle_);
    }
}

void Application::StartProactiveAlarmTimeout() {
    if (proactive_alarm_timer_handle_ == nullptr) {
        return;
    }
    esp_timer_stop(proactive_alarm_timer_handle_);
    esp_timer_start_once(proactive_alarm_timer_handle_,
                         (int64_t)CONFIG_REMINDER_ALARM_DURATION_SEC * 1000000LL);
}

void Application::StopProactiveAlarmTimeout() {
    if (proactive_alarm_timer_handle_ != nullptr) {
        esp_timer_stop(proactive_alarm_timer_handle_);
    }
}

void Application::RequestStopReminderAlarm(const char* reason) {
    const std::string stop_reason = reason != nullptr ? reason : "api";
    Schedule([this, stop_reason]() { StopProactiveAlarm(stop_reason.c_str()); });
}

void Application::QueueProactiveAlarmAfterSpeech() {
    if (proactive_alarm_mode_ != ReminderAlarmMode::kRingRepeat ||
        session_kind_ != SessionKind::ProactiveReminder || proactive_alarm_pending_start_) {
        return;
    }
    proactive_alarm_pending_start_ = true;
    proactive_alarm_pending_since_us_ = esp_timer_get_time();
    proactive_alarm_tts_playing_ = false;
    proactive_alarm_delivered_ = true;
    StopProactiveReminderTimeout();
    ReminderTraceLog("alarm_wait_audio_drain", pending_reminder_ack_id_.c_str());
}

void Application::ServiceProactiveAlarm() {
    const int64_t now_us = esp_timer_get_time();
    if (proactive_alarm_pending_start_) {
        if (session_kind_ != SessionKind::ProactiveReminder) {
            proactive_alarm_pending_start_ = false;
            proactive_alarm_pending_since_us_ = 0;
            return;
        }
        constexpr int64_t kDrainLimitUs = 10 * 1000000LL;
        if (audio_service_.IsIdle()) {
            StartProactiveAlarm();
        } else if (now_us - proactive_alarm_pending_since_us_ >= kDrainLimitUs) {
            ESP_LOGW(TAG, "Reminder speech drain exceeded 10s; clearing stale audio before alarm");
            audio_service_.ResetDecoder();
            StartProactiveAlarm();
        }
        return;
    }

    if (!IsReminderAlarmRinging()) {
        return;
    }

    if (proactive_alarm_tts_playing_) {
        constexpr int64_t kRepeatTtsStartLimitUs = 8 * 1000000LL;
        if (proactive_alarm_pending_since_us_ > 0 &&
            now_us - proactive_alarm_pending_since_us_ >= kRepeatTtsStartLimitUs) {
            proactive_alarm_tts_playing_ = false;
            proactive_alarm_pending_start_ = true;
            proactive_alarm_pending_since_us_ = now_us;
            audio_service_.ResetDecoder();
        }
        return;
    }

    if (!proactive_alarm_ring_playing_ || proactive_alarm_ring_until_us_ == 0) {
        return;
    }

    if (now_us < proactive_alarm_ring_until_us_) {
        // The bundled alarm sound is only about two seconds long. Replay it as
        // soon as the local playback queue drains so the configured repeat
        // interval is an audible ring window instead of a silent delay.
        if (audio_service_.IsIdle()) {
            PlaySound(Lang::Sounds::OGG_ALARM_RING);
        }
        return;
    }

    proactive_alarm_ring_playing_ = false;
    proactive_alarm_ring_until_us_ = 0;
    ESP_LOGI(TAG, "Repeat proactive alarm after %ds", CONFIG_REMINDER_ALARM_REPEAT_SEC);
    ReminderTraceLog("alarm_ring_repeat", pending_reminder_ack_id_.c_str());
    audio_service_.EnableWakeWordDetection(false);
    if (!audio_service_.IsIdle()) {
        audio_service_.ResetDecoder();
    }
    if (proactive_alarm_repeat_speech_enabled_ && protocol_ != nullptr &&
        !proactive_alarm_repeat_detect_text_.empty()) {
        proactive_alarm_tts_playing_ = true;
        proactive_alarm_pending_since_us_ = now_us;
        proactive_audio_packets_ = 0;
        audio_service_.BeginPlaybackPrebuffer();
        audio_service_.PrepareSpeakerPlayback();
        if (device_state_ != kDeviceStateSpeaking) {
            SetDeviceState(kDeviceStateSpeaking);
        }
        if (protocol_->IsAudioChannelOpened() || protocol_->OpenAudioChannel()) {
            protocol_->SendWakeWordDetected(proactive_alarm_repeat_detect_text_);
            return;
        }
        proactive_alarm_tts_playing_ = false;
    }

    proactive_alarm_pending_start_ = true;
    proactive_alarm_pending_since_us_ = now_us;
    StartProactiveAlarm();
}

void Application::StartProactiveAlarm() {
    if (!proactive_alarm_pending_start_ || session_kind_ != SessionKind::ProactiveReminder) {
        return;
    }

    if (IsReminderAlarmRinging()) {
        proactive_alarm_pending_start_ = false;
        proactive_alarm_pending_since_us_ = 0;
        proactive_alarm_tts_playing_ = false;
        SetDeviceState(kDeviceStateAlarm);
        audio_service_.EnableWakeWordDetection(true);
        PlaySound(Lang::Sounds::OGG_ALARM_RING);
        proactive_alarm_ring_playing_ = true;
        proactive_alarm_ring_until_us_ = esp_timer_get_time() +
            (int64_t)CONFIG_REMINDER_ALARM_REPEAT_SEC * 1000000LL;
        return;
    }

    proactive_alarm_pending_start_ = false;
    proactive_alarm_pending_since_us_ = 0;
    proactive_alarm_tts_playing_ = false;
    proactive_alarm_delivered_ = true;
    proactive_alarm_active_.store(true);
    StopProactiveReminderTimeout();

    ReminderLcBegin(ReminderLcOwner::Alarm, pending_reminder_ack_id_.c_str());
    ReminderLcMark(ReminderLcOwner::Alarm, ReminderLcPhase::RingStart, 1, "after_tts");
    ReminderTraceLog("alarm_ring_start", pending_reminder_ack_id_.c_str());
    ESP_LOGI(TAG, "Proactive alarm started: repeat=%ds duration=%ds; wake word stops it",
             CONFIG_REMINDER_ALARM_REPEAT_SEC, CONFIG_REMINDER_ALARM_DURATION_SEC);

    SetDeviceState(kDeviceStateAlarm);
    auto display = Board::GetInstance().GetDisplay();
    if (!last_reminder_display_text_.empty()) {
        display->SetChatMessage("system", last_reminder_display_text_.c_str());
        ReminderLcDisplay(ReminderLcOwner::Alarm, "system", last_reminder_display_text_.c_str(), 1);
    }
    if (!last_reminder_emotion_.empty()) {
        display->SetEmotion(last_reminder_emotion_.c_str());
    }

    // Alarm uses the duplex route. Keep wake-word detection active so the user
    // can stop the alarm by voice even while the local ring sound is playing.
    audio_service_.EnableWakeWordDetection(true);
    PlaySound(Lang::Sounds::OGG_ALARM_RING);
    proactive_alarm_ring_playing_ = true;
    proactive_alarm_ring_until_us_ = esp_timer_get_time() +
        (int64_t)CONFIG_REMINDER_ALARM_REPEAT_SEC * 1000000LL;
    StartProactiveAlarmTimeout();
}

void Application::StopProactiveAlarm(const char* reason, bool acknowledge) {
    const bool was_ringing = proactive_alarm_active_.exchange(false);
    const bool was_pending = proactive_alarm_pending_start_;
    if (!was_ringing && !was_pending) {
        return;
    }

    const std::string stop_reason = reason != nullptr ? reason : "unknown";
    proactive_alarm_pending_start_ = false;
    proactive_alarm_pending_since_us_ = 0;
    proactive_alarm_ring_until_us_ = 0;
    proactive_alarm_tts_playing_ = false;
    proactive_alarm_ring_playing_ = false;
    StopProactiveAlarmTimeout();
    StopProactiveReminderTimeout();

    ESP_LOGI(TAG, "Stop proactive alarm: %s", stop_reason.c_str());
    ReminderTraceLog("alarm_ring_stop", stop_reason.c_str());
    ReminderLcMark(ReminderLcOwner::Alarm, ReminderLcPhase::RingStop, 1, stop_reason.c_str());
    ReminderLcSummary(ReminderLcOwner::Alarm);

    audio_service_.EnableWakeWordDetection(false);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.ResetDecoder();

    if (acknowledge && !pending_reminder_ack_id_.empty()) {
        CompletePendingReminderAck(false);
    } else if (!acknowledge) {
        pending_reminder_ack_id_.clear();
        pending_reminder_ack_url_.clear();
        proactive_reminder_tts_started_ = false;
        proactive_alarm_delivered_ = false;
    }
    EnterIdleStandby(true);
}

void Application::CancelPendingReminderAck() {
    if (pending_reminder_ack_id_.empty()) {
        return;
    }
    ESP_LOGW(TAG, "Cancel proactive reminder %s (no ack, will retry on next poll)",
             pending_reminder_ack_id_.c_str());
    ReminderTraceLog("proactive_cancel", pending_reminder_ack_id_.c_str());
    ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::Ack, 0, "cancelled");
    ReminderLcSummary(ReminderLcOwner::Proactive);
    StopProactiveReminderTimeout();
    StopProactiveAlarmTimeout();
    const bool had_delivery = proactive_reminder_tts_started_ || proactive_alarm_delivered_ ||
        IsReminderAlarmRinging() || proactive_alarm_pending_start_;
    proactive_alarm_active_.store(false);
    proactive_alarm_pending_start_ = false;
    proactive_alarm_tts_playing_ = false;
    proactive_alarm_ring_playing_ = false;
    proactive_alarm_repeat_speech_enabled_ = false;
    proactive_alarm_mode_ = ReminderAlarmMode::kNone;
    proactive_alarm_pending_since_us_ = 0;
    proactive_alarm_ring_until_us_ = 0;
    proactive_alarm_repeat_detect_text_.clear();
    pending_reminder_ack_id_.clear();
    pending_reminder_ack_url_.clear();
    proactive_reminder_tts_started_ = false;
    proactive_alarm_delivered_ = false;
    last_reminder_display_text_.clear();
    last_reminder_emotion_.clear();
    audio_service_.ResetDecoder();
    EnterIdleStandby(had_delivery);
}

void Application::CompletePendingReminderAck(bool return_to_idle, bool show_wake_hint) {
    if (pending_reminder_ack_id_.empty()) {
        return;
    }
    if (!proactive_reminder_tts_started_ && !proactive_alarm_delivered_) {
        ESP_LOGW(TAG, "Proactive reminder %s ended before delivery", pending_reminder_ack_id_.c_str());
        ReminderTraceLog("proactive_not_delivered", pending_reminder_ack_id_.c_str());
        CancelPendingReminderAck();
        return;
    }

    ReminderTraceLog("proactive_ack_begin", pending_reminder_ack_id_.c_str());
    std::string ack_id = pending_reminder_ack_id_;
    std::string ack_url = pending_reminder_ack_url_;
    StopProactiveReminderTimeout();
    pending_reminder_ack_id_.clear();
    pending_reminder_ack_url_.clear();
    proactive_reminder_tts_started_ = false;
    proactive_alarm_delivered_ = false;

    auto finalize = std::make_shared<std::function<void(int)>>();
    *finalize = [this, ack_id, ack_url, return_to_idle, show_wake_hint, finalize](int attempt) {
        const bool wait_for_audio_drain =
            return_to_idle &&
            (!audio_service_.IsIdle() || audio_service_.IsAudioProcessorRunning());
        if (wait_for_audio_drain && attempt < 60) {
            if (attempt == 0 || attempt % 10 == 0) {
                ReminderTraceLog("proactive_drain_wait", ack_id.c_str());
            }
            Schedule([finalize, attempt]() { (*finalize)(attempt + 1); });
            return;
        }
        if (ReminderPoller::PostAck(ack_url, ack_id)) {
            Settings ack_settings("reminder_poll", true);
            ack_settings.SetString("last_id", ack_id);
            ack_settings.SetInt("last_ack_ts", (int32_t)(esp_timer_get_time() / 1000000LL));
            ReminderTraceLog("proactive_ack_ok", ack_id.c_str());
            ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::Ack, 1, ack_id.c_str());
        } else {
            ESP_LOGW(TAG, "Proactive reminder %s ack POST failed, not saving last_id", ack_id.c_str());
            ReminderTraceLog("proactive_ack_fail", ack_id.c_str());
            ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::Ack, 0, ack_id.c_str());
        }
        ReminderLcSummary(ReminderLcOwner::Proactive);
        if (return_to_idle) {
            EnterIdleStandby(show_wake_hint);
            ReminderTraceLog("proactive_finalized", ack_id.c_str());
        } else {
            ReminderTraceLog("proactive_ack_handoff", ack_id.c_str());
        }
    };
    Schedule([finalize]() { (*finalize)(0); });
}
#endif

#if CONFIG_USE_REMINDER_POLL
void Application::DeliverReminder(ReminderDeliveryMode mode, ReminderAlarmMode alarm_mode,
                                  const std::string& id, const std::string& prompt,
                                  const std::string& wake_text, const char* emotion,
                                  const std::string& ack_url) {
    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    std::string display_text = SanitizeReminderText(prompt);
    if (display_text.empty()) {
        return;
    }

    constexpr size_t kMaxDetectChars = 32;
    ReminderDeliveryMode effective_mode = mode;
    if (mode == ReminderDeliveryMode::kDirectWake && display_text.size() > kMaxDetectChars) {
        ESP_LOGW(TAG, "direct_wake prompt too long (%u chars), using mcp_wake", (unsigned)display_text.size());
        effective_mode = ReminderDeliveryMode::kMcpWake;
    }

    std::string detect_text = display_text;
    if (effective_mode == ReminderDeliveryMode::kMcpWake) {
        detect_text = SanitizeReminderText(wake_text);
        if (detect_text.empty()) {
#ifdef CONFIG_REMINDER_WAKE_PHRASE
            detect_text = CONFIG_REMINDER_WAKE_PHRASE;
#else
            detect_text = "查提醒";
#endif
        }
        if (detect_text.size() > kMaxDetectChars) {
            detect_text.resize(kMaxDetectChars);
        }
        ESP_LOGI(TAG, "Deliver reminder mcp_wake id=%s wake=%s prompt=%s",
                 id.c_str(), detect_text.c_str(), display_text.c_str());
    } else {
        ESP_LOGI(TAG, "Deliver reminder direct_wake: %s", detect_text.c_str());
    }

    Schedule([this, payload = ReminderDeliverPayload{
                  effective_mode, alarm_mode, id, display_text, detect_text,
                  std::string(emotion ? emotion : "neutral"), ack_url}]() {
        RunReminderDelivery(payload);
    });
}

void Application::RunReminderDelivery(const ReminderDeliverPayload& payload) {
    const auto& effective_mode = payload.effective_mode;
    const auto& alarm_mode = payload.alarm_mode;
    const auto& id = payload.id;
    const auto& display_text = payload.display_text;
    const auto& detect_text = payload.detect_text;
    const auto& emotion_str = payload.emotion_str;
    const auto& ack_url = payload.ack_url;

    if (!CanDeliverReminder()) {
        ReminderTraceLog("deliver_skip", id.c_str());
        ESP_LOGW(TAG, "Skip reminder %s, higher priority activity (session=%d state=%s)",
                 id.c_str(), (int)session_kind_, STATE_STRINGS[device_state_]);
        if (!id.empty() && reminder_poller_ != nullptr) {
            reminder_poller_->DeferPoll("run_deliver_busy");
        }
        return;
    }

    ReminderLcBegin(ReminderLcOwner::Proactive, id.c_str());
    ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::DeliverSchedule, 1, nullptr);

    if (effective_mode != ReminderDeliveryMode::kAlertOnly &&
        session_kind_ == SessionKind::None &&
        protocol_->IsAudioChannelOpened()) {
        ReminderTraceLog("deliver_close_stale_channel", id.c_str());
        ESP_LOGW(TAG, "Closing stale audio channel before proactive reminder %s", id.c_str());
        suppress_display_clear_on_channel_close_ = true;
        pending_reminder_payload_ = payload;
        pending_reminder_after_channel_close_ = true;
        protocol_->CloseAudioChannel();
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    last_reminder_display_text_ = display_text;
    last_reminder_emotion_ = emotion_str;
    const bool silent_until_tts = (effective_mode == ReminderDeliveryMode::kMcpWake);
    if (!silent_until_tts) {
        display->SetEmotion(emotion_str.c_str());
        display->SetChatMessage("system", display_text.c_str());
        ReminderLcEmotion(ReminderLcOwner::Proactive, emotion_str.c_str(), 1);
        ReminderLcDisplay(ReminderLcOwner::Proactive, "system", display_text.c_str(), 1);
    } else {
        ReminderTraceLog("deliver_ui_deferred", id.c_str());
    }

    SetSessionKind(SessionKind::ProactiveReminder,
                   effective_mode == ReminderDeliveryMode::kMcpWake ? "deliver_mcp_wake" :
                   effective_mode == ReminderDeliveryMode::kDirectWake ? "deliver_direct_wake" :
                   "deliver_alert_only");
    listening_mode_ = kListeningModeAutoStop;
    pending_reminder_ack_id_ = id;
    pending_reminder_ack_url_ = ack_url;
    proactive_reminder_tts_started_ = false;
    proactive_alarm_active_.store(false);
    proactive_alarm_pending_start_ = false;
    proactive_alarm_tts_playing_ = false;
    proactive_alarm_ring_playing_ = false;
    proactive_alarm_mode_ = alarm_mode;
    proactive_alarm_repeat_speech_enabled_ =
        alarm_mode == ReminderAlarmMode::kRingRepeat &&
        effective_mode != ReminderDeliveryMode::kAlertOnly && !detect_text.empty();
    proactive_alarm_repeat_detect_text_ = detect_text;
    proactive_alarm_delivered_ = false;
    proactive_alarm_pending_since_us_ = 0;
    proactive_alarm_ring_until_us_ = 0;
    proactive_audio_packets_ = 0;

    if (effective_mode != ReminderDeliveryMode::kAlertOnly) {
        audio_service_.BeginPlaybackPrebuffer();
        audio_service_.PrepareSpeakerPlayback();
    } else {
        proactive_alarm_delivered_ = true;
        if (alarm_mode == ReminderAlarmMode::kRingRepeat) {
            QueueProactiveAlarmAfterSpeech();
            ServiceProactiveAlarm();
        } else {
            ReminderTraceLog("alert_only_complete", id.c_str());
            CompletePendingReminderAck(true, false);
        }
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->OpenAudioChannel()) {
            ESP_LOGE(TAG, "Failed to open channel for reminder");
            ReminderTraceLog("deliver_channel_fail", id.c_str());
            ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::ChannelOpen, 0, "open_failed");
            CancelPendingReminderAck();
            Alert(Lang::Strings::ERROR, Lang::Strings::SERVER_NOT_CONNECTED, "circle_xmark",
                  Lang::Sounds::OGG_EXCLAMATION);
            return;
        }
        if (effective_mode != ReminderDeliveryMode::kMcpWake &&
            effective_mode != ReminderDeliveryMode::kDirectWake) {
            SetDeviceState(kDeviceStateIdle);
        }
    }

    audio_service_.EnableWakeWordDetection(false);

    if (effective_mode == ReminderDeliveryMode::kMcpWake) {
        int wake_packets = 0;
#if CONFIG_SEND_WAKE_WORD_DATA
        audio_service_.EncodeWakeWord();
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
            wake_packets++;
        }
#endif
        ESP_LOGI(TAG, "Proactive wake audio packets: %d", wake_packets);
        protocol_->SendWakeWordDetected(detect_text);
        ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::WakeSent, 1, detect_text.c_str());
        if (device_state_ == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        StartProactiveReminderTimeout();
        ReminderTraceLog("deliver_dispatched", id.c_str());
        ReminderHwTraceSnapshot("proactive_dispatched");
        ESP_LOGI(TAG, "Proactive reminder %s dispatched [session-v12]", id.c_str());
        return;
    }

    if (effective_mode == ReminderDeliveryMode::kDirectWake) {
        protocol_->SendWakeWordDetected(detect_text);
        ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::WakeSent, 1, detect_text.c_str());
        if (device_state_ == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        StartProactiveReminderTimeout();
        ESP_LOGI(TAG, "Direct wake reminder %s dispatched [session-v12]", id.c_str());
        return;
    }

    /* Unreachable for reminder poll — kept as fallback. */
    listening_mode_ = aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
    protocol_->SendWakeWordDetected(detect_text);
    SetListeningMode(listening_mode_);
#if !CONFIG_SEND_WAKE_WORD_DATA
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
}
#endif

void Application::DeliverReminderSpeech(const std::string& message, const char* emotion) {
#if CONFIG_USE_REMINDER_POLL
    DeliverReminder(ReminderDeliveryMode::kMcpWake, ReminderAlarmMode::kNone,
                    "", message, "", emotion, "");
#else
    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    std::string text = SanitizeReminderText(message);
    if (text.empty()) {
        return;
    }
    ESP_LOGI(TAG, "Deliver reminder speech: %s", text.c_str());
    Schedule([this, text, emotion_str = std::string(emotion ? emotion : "neutral")]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion(emotion_str.c_str());
        display->SetChatMessage("system", text.c_str());
        if (device_state_ == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
        }
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            audio_service_.EnableVoiceProcessing(false);
        }
        listening_mode_ = kListeningModeManualStop;
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                ESP_LOGE(TAG, "Failed to open channel for reminder");
                Alert(Lang::Strings::ERROR, Lang::Strings::SERVER_NOT_CONNECTED, "circle_xmark",
                      Lang::Sounds::OGG_EXCLAMATION);
                return;
            }
        }
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
        protocol_->SendWakeWordDetected(text);
        SetListeningMode(kListeningModeManualStop);
    });
#endif
}

#if 0  // Legacy local-timer message helper removed from the firmware build.

void Application::SendMessage(std::string& message) {
    ESP_LOGI(TAG, "Send message: %s", message.c_str());
    message.erase(std::remove(message.begin(), message.end(), '\n'), message.end());
    message.erase(std::remove(message.begin(), message.end(), '\r'), message.end());
    message.erase(std::remove(message.begin(), message.end(), '\"'), message.end());
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, message]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(message); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this, message]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
            if (protocol_) {
                protocol_->SendWakeWordDetected(message); 
            }
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this, message]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(message); 
            }
        });
    }

}

#endif
