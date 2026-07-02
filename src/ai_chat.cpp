/**
 * @file ai_chat.cpp
 * @brief AI 对话功能（示波器终端显示版）
 *
 * 流程：录音（INMP441 → PSRAM）→ 百度 ASR → DeepSeek → 示波器终端显示
 *
 * 引脚：MIC_SCK=47, MIC_WS=48, MIC_DATA=1 (定义在 pins.h)
 */

#include "ai_chat.h"
#include "pins.h"
#include "microphone.h"
#include "freertos.h"
#include "web_server.h"
#include "vector_draw.h"
#include "DACoutput.h"
#include "voice_control.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ai_config.h"
#include "network_manager.h"

#define SAMPLE_RATE      16000
#define MAX_RECORD_SEC   5       // 最长录 5 秒（PSRAM 可容纳）
#define CHUNK_SIZE       512

volatile bool ai_chat_active = false;

static Microphone* s_mic = nullptr;
static char baidu_token[256] = {0};
static unsigned long token_expires = 0;

static bool     wifi_connect();
static bool     baidu_get_token();
static String   baidu_asr(const int16_t* pcm, size_t samples);
static String   deepseek_chat(const String& text);
static void     ai_chat_task(void* pvParameters);

void AI_Chat_Start() {
    if (ai_chat_active) return;
    ai_chat_active = true;
    if (xTaskCreatePinnedToCore(ai_chat_task, "AIChatTask", 16384, NULL, 1, NULL, 0) != pdPASS) {
        ai_chat_active = false;
    }
}

// 阶段式共享显示 — ai_chat_task(Core 0) 写入, guiTask(Core 1) 渲染
char  ai_chat_display_text[512];
volatile AIChatPhase ai_chat_phase = AI_PHASE_WAITING;
volatile bool ai_chat_dirty = false;

// 设置屏幕文字（线程安全：只写一个字段 + dirty 标志）
static void ai_show(AIChatPhase phase, const char* text) {
    strncpy(ai_chat_display_text, text, sizeof(ai_chat_display_text) - 1);
    ai_chat_display_text[sizeof(ai_chat_display_text) - 1] = '\0';
    ai_chat_phase = phase;
    ai_chat_dirty = true;
}

// term_println 只输出到串口，不再显示在屏幕上
static void term_println(const char* msg) {
    USBSerial.println(msg);
}

static bool wifi_connect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    USBSerial.println("Connecting WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t0 = millis();
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        USBSerial.print(".");
        if (++tries > 40) {
            USBSerial.println();
            term_println("ERROR: WiFi failed!");
            return false;
        }
    }
    USBSerial.printf("WiFi OK, IP: %s (%ums)\n", WiFi.localIP().toString().c_str(), millis() - t0);
    return true;
}

static bool baidu_get_token() {
    if (strlen(baidu_token) > 0 && millis() / 1000 < token_expires) return true;
    if (WiFi.status() != WL_CONNECTED) {
        USBSerial.println("WiFi lost! Reconnecting...");
        if (!wifi_connect()) return false;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) { delay(2000); }

        WiFiClient client;
        HTTPClient http;
        String url = String("http://aip.baidubce.com/oauth/2.0/token")
                     + "?grant_type=client_credentials"
                     + "&client_id=" + BAIDU_API_KEY
                     + "&client_secret=" + BAIDU_SECRET_KEY;

        http.begin(client, url);
        http.setTimeout(10000);
        int code = http.GET();
        String resp = http.getString();
        http.end();

        if (code != 200) {
            USBSerial.printf("Baidu token fail: HTTP %d\n", code);
            continue;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, resp);
        if (err) { USBSerial.printf("Baidu token parse fail: %s\n", err.c_str()); continue; }

        const char* token = doc["access_token"];
        int expires_in = doc["expires_in"];
        if (!token) { USBSerial.println("Baidu token empty!"); continue; }

        strncpy(baidu_token, token, sizeof(baidu_token) - 1);
        token_expires = millis() / 1000 + expires_in - 86400;
        USBSerial.println("Baidu token OK");
        return true;
    }
    USBSerial.println("Baidu token exhausted retries");
    return false;
}

static String baidu_asr(const int16_t* pcm, size_t samples) {
    if (!baidu_get_token()) return String();
    size_t pcm_bytes = samples * sizeof(int16_t);
    if (pcm_bytes == 0) return String();

    WiFiClient client;
    HTTPClient http;
    String url = String("http://vop.baidu.com/server_api")
               + "?cuid=ESP32_S3"
               + "&token=" + baidu_token
               + "&dev_pid=1537";

    http.begin(client, url);
    http.addHeader("Content-Type", "audio/pcm;rate=16000");
    http.setTimeout(30000);

    int code = http.POST((uint8_t*)pcm, pcm_bytes);
    if (code != 200) {
        USBSerial.printf("ASR fail: HTTP %d\n", code);
        http.end();
        return String();
    }

    String resp = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp);
    if (err) { USBSerial.printf("ASR parse fail: %s\n", err.c_str()); return String(); }

    int err_no = doc["err_no"];
    if (err_no != 0) {
        const char* err_msg = doc["err_msg"];
        USBSerial.printf("ASR err: %s (%d)\n", err_msg ? err_msg : "", err_no);
        return String();
    }

    JsonArray result = doc["result"].as<JsonArray>();
    if (result.size() == 0) return String();
    return result[0].as<String>();
}

static String deepseek_chat(const String& text) {
    // ---- 直接用 WiFiClientSecure 发原始 HTTP 请求 ----
    // 不用 HTTPClient -> https.getStreamPtr() 在 SSL 下 available() 经常返回 0 导致空等.
    // 用 Connection: close 避免 chunked transfer encoding, 简化响应读取.
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);

    unsigned long t0 = millis();
    if (!client.connect("api.deepseek.com", 443)) {
        USBSerial.printf("DeepSeek: connect FAILED @ %ums\n", millis() - t0);
        return String();
    }
    USBSerial.printf("DeepSeek: connected @ %ums\n", millis() - t0);

    // ---- 构造 JSON body ----
    JsonDocument req_doc;
    req_doc["model"] = "deepseek-chat";
    req_doc["temperature"] = 0.7;
    req_doc["stream"] = false;

    JsonArray messages = req_doc["messages"].to<JsonArray>();
    JsonObject sys_msg = messages.add<JsonObject>();
    sys_msg["role"] = "system";
    sys_msg["content"] =
        "CRITICAL: OUTPUT LANGUAGE MUST BE ENGLISH ONLY. "
        "The display terminal can ONLY render ASCII characters. "
        "Chinese, Japanese, emoji, or any non-ASCII characters will show as garbage/corrupted on screen.\n\n"
        "You are a helpful AI assistant controlling an oscilloscope game console. "
        "Rules:\n"
        "1. When user wants an action, answer ONLY raw JSON with NO markdown, NO code fences, NO extra text:\n"
        "{\"reply\":\"your message\",\"action\":\"action_name\"}\n"
        "2. When no action is needed, reply as plain text only.\n"
        "Available actions: "
        "open_music, open_video, open_games, open_online, open_game_joy, open_ai_chat, open_about, "
        "start_snake, start_breakout, start_flappy, start_racing, start_runtiny, start_tank, "
        "back, exit\n\n"
        "STRICT: Reply in ENGLISH ONLY. Use pure ASCII. "
        "NO Chinese. NO emoji. NO special characters. "
        "Even if the user speaks Chinese, you MUST reply in English.";

    JsonObject user_msg = messages.add<JsonObject>();
    user_msg["role"] = "user";
    user_msg["content"] = text;

    String body;
    serializeJson(req_doc, body);

    // ---- 发送原始 HTTP 请求 (Connection: close 避免 chunked) ----
    String http_request =
        String("POST /chat/completions HTTP/1.1\r\n") +
        "Host: api.deepseek.com\r\n" +
        "Content-Type: application/json\r\n" +
        "Authorization: Bearer " + DEEPSEEK_API_KEY + "\r\n" +
        "Content-Length: " + body.length() + "\r\n" +
        "Connection: close\r\n" +
        "\r\n" +
        body;

    client.print(http_request);
    USBSerial.printf("DeepSeek: sent %u bytes\n", http_request.length());

    // ---- 读取响应头 ----
    int http_code = 0;
    int content_length = 0;
    bool chunked = false;
    t0 = millis();

    while (client.connected() && millis() - t0 < 15000) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break; // 空行 = headers 结束

        if (line.startsWith("HTTP/")) {
            int sp1 = line.indexOf(' ');
            int sp2 = line.indexOf(' ', sp1 + 1);
            http_code = line.substring(sp1 + 1, sp2).toInt();
        } else if (line.startsWith("Content-Length:")) {
            content_length = line.substring(15).toInt();
        } else if (line.startsWith("Transfer-Encoding:") && line.indexOf("chunked") >= 0) {
            chunked = true;
        }
    }

    USBSerial.printf("DeepSeek: HTTP %d, CL=%d, chunked=%d (%ums)\n",
        http_code, content_length, chunked, millis() - t0);

    if (http_code != 200) {
        String err = client.readString();
        client.stop();
        USBSerial.printf("DeepSeek: error %d: %s\n", http_code, err.substring(0, 300).c_str());
        return String();
    }

    // ---- 读取响应体 ----
    String resp;
    t0 = millis();

    if (content_length > 0) {
        // 定长读取 — 最简单最可靠
        while ((int)resp.length() < content_length && millis() - t0 < 20000) {
            while (client.available() && (int)resp.length() < content_length) {
                resp += (char)client.read();
                t0 = millis(); // 有数据就刷新超时
            }
            if (!client.connected() && !client.available()) break;
            if (client.available() == 0) delay(1);
        }
    } else if (chunked) {
        // chunked 解码 — 逐行读取 chunk size
        while (client.connected() && millis() - t0 < 30000) {
            String hex_line = client.readStringUntil('\n');
            hex_line.trim();
            if (hex_line.length() == 0) continue;
            long chunk_size = strtol(hex_line.c_str(), NULL, 16);
            if (chunk_size <= 0) break; // 最后一块
            for (long i = 0; i < chunk_size; i++) {
                while (!client.available() && millis() - t0 < 30000) {
                    if (!client.connected()) break;
                    delay(1);
                }
                if (!client.connected() && !client.available()) break;
                resp += (char)client.read();
                t0 = millis();
            }
            client.readStringUntil('\n'); // 跳过尾部 \r\n
        }
    } else {
        // 读直到连接关闭 (兜底)
        while (client.connected() && millis() - t0 < 15000) {
            while (client.available()) {
                resp += (char)client.read();
                t0 = millis();
            }
            delay(1);
        }
        // 读干净
        while (client.available()) resp += (char)client.read();
    }

    client.stop();
    USBSerial.printf("DeepSeek: body %u bytes (%ums)\n", resp.length(), millis() - t0);

    if (resp.length() == 0) {
        USBSerial.println("DeepSeek: empty body");
        return String();
    }

    // ---- 解析 JSON ----
    // 深色: 有时服务器在 chunked 模式下在 body 前插了额外数据,
    // 尝试找到第一个 { 来定位 JSON
    int json_start = resp.indexOf('{');
    if (json_start > 0) resp = resp.substring(json_start);

    JsonDocument resp_doc;
    DeserializationError err = deserializeJson(resp_doc, resp);
    if (err) {
        USBSerial.printf("DeepSeek: JSON fail: %s\n", err.c_str());
        USBSerial.println("Body(500): " + resp.substring(0, 500));
        return String();
    }

    JsonArray choices = resp_doc["choices"].as<JsonArray>();
    if (choices.size() == 0) {
        USBSerial.println("DeepSeek: no choices");
        return String();
    }

    String reply_content = choices[0]["message"]["content"].as<String>();

    USBSerial.println("\n=== DeepSeek RAW ===");
    USBSerial.println(reply_content);
    USBSerial.println("=== END RAW ===\n");

    return reply_content;
}

static void ai_chat_task(void* pvParameters) {
    ai_chat_active = true;

    String recognized, reply;
    int16_t* pcm_buffer = nullptr;
    size_t total_samples = 0;
    size_t max_samples = SAMPLE_RATE * MAX_RECORD_SEC;
    size_t alloc_bytes = 0;
    int16_t chunk_buf[CHUNK_SIZE];
    int high_cnt = 0;
    uint8_t saved_step = DRAW_GetStepSize();
    bool has_action = false;

    USBSerial.printf("Heap before suspension: %u\n", ESP.getFreeHeap());
    suspendWebServer();
    suspendNonessentialTasks();
    USBSerial.printf("Heap after suspension: %u\n", ESP.getFreeHeap());
    USBSerial.println("Tasks suspended for AI");

    // ---- 紧凑模式 + 显示 "Press ENTER to record..." ----
    DRAW_SetStepSize(16);
    ai_show(AI_PHASE_WAITING, "Press ENTER to record...");

    // Init MIC
    if (!s_mic) {
        s_mic = new Microphone(MIC_SCK, MIC_WS, MIC_DATA, SAMPLE_RATE);
        if (!s_mic->init()) {
            term_println("ERROR: MIC init FAILED!");
            delete s_mic; s_mic = nullptr;
            goto done;
        }
        USBSerial.println("MIC ready");
    }

    // WiFi
    if (!wifi_connect()) { goto done; }

    // ---- 关闭 ESP-NOW ----
    Network_Manager::suspend_esp_now();

    // ---- 获取 Baidu token ----
    USBSerial.println("Getting Baidu token...");
    if (!baidu_get_token()) {
        term_println("ERROR: Baidu token FAILED!");
        goto done;
    }

    // ---- 分配 PSRAM 录音缓冲区 ----
    alloc_bytes = max_samples * sizeof(int16_t);
    pcm_buffer = (int16_t*)ps_malloc(alloc_bytes);
    if (!pcm_buffer) {
        term_println("ERROR: PSRAM alloc failed!");
        goto done;
    }
    USBSerial.printf("PSRAM buffer: %u bytes\n", alloc_bytes);

    // ---- Wait for button press ----
    USBSerial.println("Press ENTER to record...");
    while (digitalRead(EN_S) == HIGH) {
        if (!ai_chat_active) goto done;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    USBSerial.println("Recording... release to stop");

    // ---- Record to PSRAM ----
    total_samples = 0;
    high_cnt = 0;

    while (total_samples < max_samples) {
        if (!ai_chat_active) goto done;
        if (digitalRead(EN_S) == HIGH) {
            high_cnt++;
            if (high_cnt > 15 && total_samples > SAMPLE_RATE / 4) break;
        } else {
            high_cnt = 0;
        }

        size_t n = s_mic->read(chunk_buf, CHUNK_SIZE);
        if (n == 0) break;
        memcpy(pcm_buffer + total_samples, chunk_buf, n * sizeof(int16_t));
        total_samples += n;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (total_samples < SAMPLE_RATE / 4) {
        term_println("ERROR: Too short!");
        goto done;
    }

    USBSerial.printf("Recorded: %.1fs (%zu samples)\n", (float)total_samples / SAMPLE_RATE, total_samples);

    // ---- 释放 MIC (I2S) ----
    if (s_mic) { delete s_mic; s_mic = nullptr; }

    // ---- ASR (直接从 PSRAM POST) ----
    USBSerial.println("Recognizing...");
    recognized = baidu_asr(pcm_buffer, total_samples);
    if (recognized.length() == 0) {
        term_println("ERROR: ASR failed");
        goto done;
    }
    USBSerial.printf("You: %s\n", recognized.c_str());

    // ---- DeepSeek ----
    ai_show(AI_PHASE_THINKING, "Thinking...");
    // 释放 DMA 内存 (BLE) 给 SSL 用
    deinitHardwareDMA();
    reply = deepseek_chat(recognized);
    // 注意: reinitHardwareDMA 在 done: 标签统一恢复，确保异常路径也能恢复 BLE
    if (reply.length() == 0) {
        term_println("ERROR: DeepSeek failed");
        // 确保动作不执行（guiTask 会忽略 VC_NONE）
        voice_action = VC_NONE;
        has_action = false;
        // 仍然显示失败信息让用户看到
        ai_show(AI_PHASE_REPLY, "DeepSeek failed, check serial");
        vTaskDelay(pdMS_TO_TICKS(3000));
        goto done;
    }

    // ---- 解析 JSON action（若有） ----
    reply = VC_ParseReply(reply);
    has_action = (voice_action != VC_NONE);

    // ---- Display reply ----
    ai_show(AI_PHASE_REPLY, reply.c_str());

    USBSerial.println("\n" + String(50, '-'));
    USBSerial.println("AI Chat Done");
    USBSerial.println(String(50, '-') + "\n");

    // ---- 若有语音动作，直接退出（guiTask 会执行动作） ----
    if (has_action) {
        USBSerial.println("[Executing action...]");
        vTaskDelay(pdMS_TO_TICKS(500));
        goto done;
    }

    // ---- 等待用户按 EN_S 退出 ----
    USBSerial.println("Press ENTER to exit");
    vTaskDelay(pdMS_TO_TICKS(500));
    while (digitalRead(EN_S) == HIGH) {
        if (!ai_chat_active) goto done;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (digitalRead(EN_S) == LOW) {
        if (!ai_chat_active) goto done;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

done:
    // 恢复 DMA 外设 (BLE) — 必须在恢复任务之前执行
    reinitHardwareDMA();

    ai_chat_dirty = true; // 最后一刷
    // 恢复所有服务
    Network_Manager::resume_esp_now();
    resumeNonessentialTasks();
    resumeWebServer();

    if (pcm_buffer) free(pcm_buffer);
    DRAW_SetStepSize(saved_step);
    ai_chat_active = false;
    vTaskDelete(NULL);
}

