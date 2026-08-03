#include "mqtt_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "MQTT"

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            MqttProtocol* protocol = (MqttProtocol*)arg;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                ESP_LOGI(TAG, "Reconnecting to MQTT server");
                app.Schedule([protocol]() {
                    protocol->StartMqttClient(false);
                });
            }
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                Application::GetInstance().Schedule([this]() {
                    CloseAudioChannel();
                });
            }
        } else {
            if (strcmp(type->valuestring, "tts") == 0) {
                auto state = cJSON_GetObjectItem(root, "state");
                if (cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0) {
                    // MQTT and UDP can arrive on different network paths. Release any
                    // short tail held behind a confirmed sequence gap before TTS stop
                    // is handed to the application.
                    FlushAudioReorderBuffer();
                }
            }
            if (on_incoming_json_ != nullptr) {
                on_incoming_json_(root);
            }
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        broker_port = std::stoi(endpoint.substr(pos + 1));
    } else {
        broker_address = endpoint;
    }
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        ESP_LOGE(TAG, "Publish topic is empty");
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
            ESP_LOGW(TAG, "MQTT not connected, reconnecting (attempt %d)", attempt + 1);
            if (!StartMqttClient(false)) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }
        if (mqtt_->Publish(publish_topic_, text)) {
            return true;
        }
        ESP_LOGW(TAG, "Publish failed (attempt %d)", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
    SetError(Lang::Strings::SERVER_ERROR);
    return false;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return SendAudioLocked(*packet);
}

bool MqttProtocol::SendAudioLocked(const AudioStreamPacket& packet) {
    if (udp_ == nullptr) {
        return false;
    }
    if (aes_nonce_.size() != 16) {
        ESP_LOGE(TAG, "Invalid UDP audio nonce size: %u",
                 static_cast<unsigned>(aes_nonce_.size()));
        return false;
    }

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(packet.payload.size());
    *(uint32_t*)&nonce[8] = htonl(packet.timestamp);
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet.payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    if (!packet.payload.empty()) {
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet.payload.size(), &nc_off,
                (uint8_t*)nonce.c_str(), stream_block,
                (uint8_t*)packet.payload.data(),
                (uint8_t*)&encrypted[nonce.size()]) != 0) {
            ESP_LOGE(TAG, "Failed to encrypt audio data");
            return false;
        }
    }

    return udp_->Send(encrypted) > 0;
}

void MqttProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }
    ResetAudioReorderState();

    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_ = network->CreateUdp(2);
    udp_->OnMessage([this](const std::string& data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        if (aes_nonce_.size() != 16 || data.size() < aes_nonce_.size()) {
            ESP_LOGE(TAG, "Invalid audio packet size: %u", data.size());
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        uint32_t timestamp = ntohl(*(uint32_t*)&data[8]);
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);

        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)packet->payload.data());
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
        HandleIncomingAudioPacket(sequence, std::move(packet));
    });

    if (!udp_->Connect(udp_server_, udp_port_)) {
        ESP_LOGE(TAG, "Failed to open UDP audio transport");
        udp_.reset();
        return false;
    }

    // The MQTT gateway learns the device's UDP return address only after the
    // first client datagram. A text-only direct wake otherwise works over MQTT
    // while TTS audio has no route back through 4G/CGNAT. A zero-payload audio
    // frame is a valid session datagram and is ignored by the speech backend.
    AudioStreamPacket udp_prime;
    if (!SendAudioLocked(udp_prime)) {
        ESP_LOGE(TAG, "Failed to prime UDP audio return path");
        udp_.reset();
        return false;
    }
    ESP_LOGI(TAG, "UDP audio return path primed, sequence=%u",
             static_cast<unsigned>(local_sequence_));

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void MqttProtocol::HandleIncomingAudioPacket(uint32_t sequence,
                                             std::unique_ptr<AudioStreamPacket> packet) {
    std::vector<std::unique_ptr<AudioStreamPacket>> ready;
    {
        std::lock_guard<std::mutex> lock(audio_reorder_mutex_);
        const uint32_t expected = remote_sequence_ + 1;
        if (sequence <= remote_sequence_) {
            ++audio_duplicate_packets_;
            ESP_LOGW(TAG, "UDP audio old/duplicate: got=%u expected=%u",
                     static_cast<unsigned>(sequence), static_cast<unsigned>(expected));
            return;
        }

        auto inserted = remote_audio_reorder_.emplace(sequence, std::move(packet));
        if (!inserted.second) {
            ++audio_duplicate_packets_;
            ESP_LOGW(TAG, "UDP audio duplicate in reorder buffer: seq=%u",
                     static_cast<unsigned>(sequence));
            return;
        }

        if (sequence != expected) {
            ++audio_reordered_packets_;
            ESP_LOGW(TAG, "UDP audio reorder: got=%u expected=%u buffered=%u",
                     static_cast<unsigned>(sequence), static_cast<unsigned>(expected),
                     static_cast<unsigned>(remote_audio_reorder_.size()));
        }
        DrainAudioReorderLocked(ready, false);
    }
    DeliverIncomingAudioPackets(std::move(ready));
}

void MqttProtocol::DrainAudioReorderLocked(
        std::vector<std::unique_ptr<AudioStreamPacket>>& ready, bool force) {
    while (!remote_audio_reorder_.empty()) {
        const uint32_t expected = remote_sequence_ + 1;
        auto next = remote_audio_reorder_.find(expected);
        if (next != remote_audio_reorder_.end()) {
            ready.push_back(std::move(next->second));
            remote_audio_reorder_.erase(next);
            remote_sequence_ = expected;
            continue;
        }

        if (!force && remote_audio_reorder_.size() < kAudioReorderWindowPackets) {
            break;
        }

        const uint32_t next_sequence = remote_audio_reorder_.begin()->first;
        const uint32_t skipped = next_sequence - expected;
        audio_skipped_packets_ += skipped;
        ESP_LOGW(TAG, "UDP audio gap confirmed: expected=%u next=%u skipped=%u buffered=%u",
                 static_cast<unsigned>(expected), static_cast<unsigned>(next_sequence),
                 static_cast<unsigned>(skipped),
                 static_cast<unsigned>(remote_audio_reorder_.size()));
        remote_sequence_ = next_sequence - 1;
    }
}

void MqttProtocol::FlushAudioReorderBuffer() {
    std::vector<std::unique_ptr<AudioStreamPacket>> ready;
    {
        std::lock_guard<std::mutex> lock(audio_reorder_mutex_);
        DrainAudioReorderLocked(ready, true);
    }
    DeliverIncomingAudioPackets(std::move(ready));
}

void MqttProtocol::ResetAudioReorderState() {
    std::scoped_lock lock(audio_reorder_mutex_, audio_delivery_mutex_);
    if (audio_delivered_packets_ > 0 || audio_reordered_packets_ > 0 ||
        audio_skipped_packets_ > 0 || audio_duplicate_packets_ > 0 ||
        !remote_audio_reorder_.empty()) {
        ESP_LOGI(TAG,
                 "UDP audio summary: delivered=%u reordered=%u skipped=%u duplicate=%u pending=%u",
                 static_cast<unsigned>(audio_delivered_packets_),
                 static_cast<unsigned>(audio_reordered_packets_),
                 static_cast<unsigned>(audio_skipped_packets_),
                 static_cast<unsigned>(audio_duplicate_packets_),
                 static_cast<unsigned>(remote_audio_reorder_.size()));
    }
    remote_audio_reorder_.clear();
    remote_sequence_ = 0;
    audio_reordered_packets_ = 0;
    audio_skipped_packets_ = 0;
    audio_duplicate_packets_ = 0;
    audio_delivered_packets_ = 0;
}

void MqttProtocol::DeliverIncomingAudioPackets(
        std::vector<std::unique_ptr<AudioStreamPacket>>&& packets) {
    // MQTT tts:stop and the UDP receive task may flush/deliver concurrently.
    // Serialize the callback so application playback order remains monotonic.
    std::lock_guard<std::mutex> lock(audio_delivery_mutex_);
    for (auto& packet : packets) {
        ++audio_delivered_packets_;
        if (audio_delivered_packets_ <= 3 || audio_delivered_packets_ % 100 == 0) {
            ESP_LOGI(TAG, "UDP audio packet #%u (%u bytes)",
                     static_cast<unsigned>(audio_delivered_packets_),
                     static_cast<unsigned>(packet->payload.size()));
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
    }
    if (!packets.empty()) {
        last_incoming_time_ = std::chrono::steady_clock::now();
    }
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);
    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    ResetAudioReorderState();
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
