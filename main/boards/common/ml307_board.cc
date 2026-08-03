#include "ml307_board.h"

#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#if CONFIG_USE_REMINDER_POLL && !CONFIG_REMINDER_MQTT_TLS_INSECURE
#include "reminder/reminder_mqtt_tls.h"
#endif

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <font_awesome.h>
#include <opus_encoder.h>

static const char *TAG = "Ml307Board";

Ml307Board::Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin,
                       int baud_rate)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin), baud_rate_(baud_rate) {
    PrepareModemUartPins();
}

void Ml307Board::PrepareModemUartPins() {
    if (tx_pin_ != GPIO_NUM_NC) {
        gpio_reset_pin(tx_pin_);
        gpio_set_level(tx_pin_, 1);
        gpio_set_direction(tx_pin_, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(tx_pin_, GPIO_FLOATING);
    }
    if (rx_pin_ != GPIO_NUM_NC) {
        gpio_reset_pin(rx_pin_);
        gpio_set_direction(rx_pin_, GPIO_MODE_INPUT);
        gpio_set_pull_mode(rx_pin_, GPIO_FLOATING);
    }
}

std::string Ml307Board::GetBoardType() {
    return "ml307";
}

void Ml307Board::StartNetwork() {
    auto& application = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::DETECTING_MODULE);

    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const bool cold_start = reset_reason == ESP_RST_POWERON ||
                            reset_reason == ESP_RST_BROWNOUT ||
                            reset_reason == ESP_RST_UNKNOWN;
    ESP_LOGI(TAG, "Modem startup: reset_reason=%d desired_baud=%d cold=%d",
             static_cast<int>(reset_reason), baud_rate_, cold_start ? 1 : 0);
    constexpr int64_t kColdBootUartReadyUs = 12 * 1000000LL;
    if (cold_start) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us < kColdBootUartReadyUs) {
            const unsigned wait_ms = static_cast<unsigned>(
                (kColdBootUartReadyUs - now_us + 999) / 1000);
            ESP_LOGW(TAG, "Cold boot: keep modem UART quiet for %u ms", wait_ms);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }

    unsigned detect_failures = 0;
    while (true) {
        modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, baud_rate_);
        if (modem_ != nullptr) {
            break;
        }
        ++detect_failures;
        PrepareModemUartPins();
        if (cold_start && detect_failures == 1) {
            ESP_LOGE(TAG, "Cold-boot modem handshake failed; restarting P4 once");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    modem_->OnNetworkStateChanged([this, &application](bool network_ready) {
        if (network_ready) {
            ESP_LOGI(TAG, "Network is ready");
        } else {
            ESP_LOGE(TAG, "Network is down");
            auto device_state = application.GetDeviceState();
            if (device_state == kDeviceStateListening || device_state == kDeviceStateSpeaking) {
                application.Schedule([this, &application]() {
                    application.SetDeviceState(kDeviceStateIdle);
                });
            }
        }
    });

    // Wait for network ready
    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
    while (true) {
        auto result = modem_->WaitForNetworkReady();
        if (result == NetworkStatus::ErrorInsertPin) {
            application.Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
        } else if (result == NetworkStatus::ErrorRegistrationDenied) {
            application.Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
        } else {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    // Print the ML307 modem information
    std::string module_revision = modem_->GetModuleRevision();
    std::string imei = modem_->GetImei();
    std::string iccid = modem_->GetIccid();
    ESP_LOGI(TAG, "ML307 Revision: %s", module_revision.c_str());
    ESP_LOGI(TAG, "ML307 IMEI: %s", imei.c_str());
    ESP_LOGI(TAG, "ML307 ICCID: %s", iccid.c_str());
#if CONFIG_USE_REMINDER_POLL && !CONFIG_REMINDER_MQTT_TLS_INSECURE
    ReminderTlsResetDefaultSslContext(modem_->GetAtUart());
#endif
}

NetworkInterface* Ml307Board::GetNetwork() {
    return modem_.get();
}

const char* Ml307Board::GetNetworkStateIcon() {
    if (modem_ == nullptr || !modem_->network_ready()) {
        return FONT_AWESOME_SIGNAL_OFF;
    }
    int csq = modem_->GetCsq();
    if (csq == -1) {
        return FONT_AWESOME_SIGNAL_OFF;
    } else if (csq >= 0 && csq <= 14) {
        return FONT_AWESOME_SIGNAL_WEAK;
    } else if (csq >= 15 && csq <= 19) {
        return FONT_AWESOME_SIGNAL_FAIR;
    } else if (csq >= 20 && csq <= 24) {
        return FONT_AWESOME_SIGNAL_GOOD;
    } else if (csq >= 25 && csq <= 31) {
        return FONT_AWESOME_SIGNAL_STRONG;
    }

    ESP_LOGW(TAG, "Invalid CSQ: %d", csq);
    return FONT_AWESOME_SIGNAL_OFF;
}

std::string Ml307Board::GetBoardJson() {
    // Set the board type for OTA
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    board_json += "\"revision\":\"" + modem_->GetModuleRevision() + "\",";
    board_json += "\"carrier\":\"" + modem_->GetCarrierName() + "\",";
    board_json += "\"csq\":\"" + std::to_string(modem_->GetCsq()) + "\",";
    board_json += "\"imei\":\"" + modem_->GetImei() + "\",";
    board_json += "\"iccid\":\"" + modem_->GetIccid() + "\",";
    board_json += "\"cereg\":" + modem_->GetRegistrationState().ToString() + "}";
    return board_json;
}

void Ml307Board::SetPowerSaveMode(bool enabled) {
    // TODO: Implement power save mode for ML307
}

std::string Ml307Board::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "cellular",
     *         "carrier": "CHINA MOBILE",
     *         "csq": 10
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "cellular");
    cJSON_AddStringToObject(network, "carrier", modem_->GetCarrierName().c_str());
    int csq = modem_->GetCsq();
    if (csq == -1) {
        cJSON_AddStringToObject(network, "signal", "unknown");
    } else if (csq >= 0 && csq <= 14) {
        cJSON_AddStringToObject(network, "signal", "very weak");
    } else if (csq >= 15 && csq <= 19) {
        cJSON_AddStringToObject(network, "signal", "weak");
    } else if (csq >= 20 && csq <= 24) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else if (csq >= 25 && csq <= 31) {
        cJSON_AddStringToObject(network, "signal", "strong");
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
