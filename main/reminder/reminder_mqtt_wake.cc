#include "reminder_mqtt_wake.h"

#include "reminder_poller.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "mqtt.h"

#include <esp_log.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>

#define TAG "ReminderMqtt"

#if CONFIG_USE_REMINDER_POLL && CONFIG_REMINDER_MQTT_WAKE

static void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

std::string ReminderMqttWake::ExpandDeviceIdInTopic(const std::string& topic_template) {
    std::string topic = topic_template;
    const std::string mac = SystemInfo::GetMacAddress();
    ReplaceAll(topic, "{device_id}", mac);
    ReplaceAll(topic, "{mac}", mac);
    return topic;
}

void ReminderMqttWake::EnsureNvsConfigured() {
    Settings settings("reminder_mqtt", true);
    if (!settings.GetString("broker").empty()) {
        return;
    }

#ifdef CONFIG_REMINDER_MQTT_WAKE_DEFAULT_BROKER
    std::string broker = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_BROKER;
#else
    std::string broker;
#endif
    if (broker.empty()) {
        ESP_LOGW(TAG, "MQTT wake broker not configured");
        return;
    }

    settings.SetString("broker", broker);
#ifdef CONFIG_REMINDER_MQTT_WAKE_DEFAULT_TOPIC
    std::string topic = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_TOPIC;
    if (!topic.empty()) {
        settings.SetString("topic", topic);
    }
#endif
#ifdef CONFIG_REMINDER_MQTT_WAKE_DEFAULT_USERNAME
    std::string username = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_USERNAME;
    if (!username.empty()) {
        settings.SetString("username", username);
    }
#endif
#ifdef CONFIG_REMINDER_MQTT_WAKE_DEFAULT_PASSWORD
    std::string password = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_PASSWORD;
    if (!password.empty()) {
        settings.SetString("password", password);
    }
#endif
    ESP_LOGI(TAG, "Wrote default MQTT wake broker to NVS (device=%s)", SystemInfo::GetMacAddress().c_str());
}

ReminderMqttWake::ReminderMqttWake() = default;

ReminderMqttWake::~ReminderMqttWake() {
    Stop();
}

void ReminderMqttWake::Start(ReminderPoller* poller) {
    if (running_ || poller == nullptr) {
        return;
    }
    EnsureNvsConfigured();

    Settings settings("reminder_mqtt", false);
    if (settings.GetString("broker").empty()) {
        ESP_LOGW(TAG, "MQTT wake disabled: broker not set in NVS reminder_mqtt.broker");
        return;
    }

    poller_ = poller;
    running_ = true;
    xTaskCreate(
        [](void* arg) {
            static_cast<ReminderMqttWake*>(arg)->MqttTask();
        },
        "reminder_mqtt", 8192, this, 2, &task_handle_);
    ESP_LOGI(TAG, "MQTT wake listener started, mac=%s", SystemInfo::GetMacAddress().c_str());
}

void ReminderMqttWake::Stop() {
    running_ = false;
    if (mqtt_) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
    poller_ = nullptr;
}

bool ReminderMqttWake::ConnectOnce() {
    Settings settings("reminder_mqtt", false);
    const std::string broker_endpoint = settings.GetString("broker");
    const std::string topic_template = settings.GetString("topic", "xiaozhi/reminder/wake/{device_id}");
    const std::string username = settings.GetString("username");
    const std::string password = settings.GetString("password");

    if (broker_endpoint.empty()) {
        return false;
    }

    std::string broker_address;
    int broker_port = 1883;
    const size_t pos = broker_endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = broker_endpoint.substr(0, pos);
        broker_port = std::stoi(broker_endpoint.substr(pos + 1));
    } else {
        broker_address = broker_endpoint;
    }

    const std::string subscribe_topic = ExpandDeviceIdInTopic(topic_template);
    std::string client_id = settings.GetString("client_id");
    if (client_id.empty()) {
        client_id = "xiaozhi-" + SystemInfo::GetMacAddress();
        ReplaceAll(client_id, ":", "");
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(1);
    if (!mqtt_) {
        ESP_LOGE(TAG, "Failed to create MQTT client (index 1)");
        return false;
    }

    ReminderPoller* poller = poller_;
    mqtt_->SetKeepAlive(120);
    mqtt_->OnMessage([poller](const std::string& topic, const std::string& payload) {
        ESP_LOGI(TAG, "Wake message [%s]: %s", topic.c_str(), payload.c_str());
        if (poller != nullptr) {
            poller->TriggerPoll();
        }
    });

    ESP_LOGI(TAG, "Connecting MQTT wake broker %s:%d topic=%s",
             broker_address.c_str(), broker_port, subscribe_topic.c_str());
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        ESP_LOGW(TAG, "MQTT wake connect failed");
        mqtt_.reset();
        return false;
    }

    if (!mqtt_->Subscribe(subscribe_topic)) {
        ESP_LOGW(TAG, "MQTT wake subscribe failed: %s", subscribe_topic.c_str());
        mqtt_->Disconnect();
        mqtt_.reset();
        return false;
    }

    ESP_LOGI(TAG, "MQTT wake subscribed: %s", subscribe_topic.c_str());

    while (running_ && mqtt_ && mqtt_->IsConnected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (mqtt_) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
    return true;
}

void ReminderMqttWake::MqttTask() {
    while (running_) {
        if (!ConnectOnce()) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

#endif  // CONFIG_USE_REMINDER_POLL && CONFIG_REMINDER_MQTT_WAKE
