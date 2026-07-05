/**
 * @file ai_chat.cpp
 * @brief AI 对话功能（示波器终端显示版）
 *
 * 流程：录音（INMP441 → PSRAM）→ 百度 ASR → DeepSeek 流式(SSE) → 示波器实时逐字显示
 *
 * 引脚：MIC_SCK=47, MIC_WS=48, MIC_DATA=1 (定义在 pins.h)
 */

#include "ai_chat.h"
#include "ai_prompt.h"
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

#include <esp_heap_caps.h>   // heap_caps_get_free_size / MALLOC_CAP_DMA

#define SAMPLE_RATE      16000
#define MAX_RECORD_SEC   10      // 最长录 10 秒（用户松手即停，上限保护）
#define CHUNK_SIZE       512

volatile bool ai_chat_active = false;
volatile unsigned long ai_chat_activity_time = 0;

static char baidu_token[256] = {0};
static unsigned long token_expires = 0;

static bool     wifi_connect();
static bool     baidu_get_token();
static String   baidu_asr(const int16_t* pcm, size_t samples);
static bool     sse_process_line(const String& line, String& accumulated);
static String   deepseek_chat_stream(const String& text);
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
    // 同步到 Web 状态监视器
    updateWebUIStatus(String("[AI] ") + text);
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

// 处理单行 SSE data 事件, 返回 true 表示流结束
// accumulated: 外部传入的累加字符串引用
// 实时更新屏幕显示
static bool sse_process_line(const String& line, String& accumulated) {
    if (!line.startsWith("data: ")) return false;
    String payload = line.substring(6);
    payload.trim();
    if (payload == "[DONE]") return true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) return false;

    JsonArray choices = doc["choices"].as<JsonArray>();
    if (choices.size() == 0) return false;

    const char* finish = choices[0]["finish_reason"];
    const char* delta  = choices[0]["delta"]["content"];

    if (delta && strlen(delta) > 0) {
        accumulated += delta;
        USBSerial.print(delta);  // 串口也实时输出
        ai_show(AI_PHASE_REPLY, accumulated.c_str());
    }

    if (finish && strlen(finish) > 0) return true;
    return false;
}

static String deepseek_chat_stream(const String& text) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    unsigned long t0 = millis();
    if (!client.connect("api.deepseek.com", 443)) {
        USBSerial.printf("DeepSeek: connect FAILED @ %ums\n", millis() - t0);
        return String();
    }
    client.setNoDelay(true);  // 连接后才设 TCP_NODELAY
    USBSerial.printf("DeepSeek: connected @ %ums\n", millis() - t0);

    // ---- 构造 JSON body (stream: true) ----
    JsonDocument req_doc;
    req_doc["model"] = "deepseek-chat";
    req_doc["temperature"] = 0.7;
    req_doc["max_tokens"] = 200;
    req_doc["stream"] = true; // ← 开启流式

    JsonArray messages = req_doc["messages"].to<JsonArray>();
    JsonObject sys_msg = messages.add<JsonObject>();
    sys_msg["role"] = "system";
    sys_msg["content"] = AI_SYSTEM_PROMPT;

    JsonObject user_msg = messages.add<JsonObject>();
    user_msg["role"] = "user";
    user_msg["content"] = text;

    String body;
    serializeJson(req_doc, body);

    // ---- 发送原始 HTTP 请求 ----
    String http_request =
        String("POST /chat/completions HTTP/1.1\r\n") +
        "Host: api.deepseek.com\r\n" +
        "Content-Type: application/json\r\n" +
        "Authorization: Bearer " + DEEPSEEK_API_KEY + "\r\n" +
        "Content-Length: " + body.length() + "\r\n" +
        "Accept: text/event-stream\r\n" +
        "Connection: close\r\n" +
        "\r\n" +
        body;

    size_t sent = client.print(http_request);
    USBSerial.printf("DeepSeek: sent %u/%u bytes\n", sent, http_request.length());
    if (sent < http_request.length()) {
        client.stop();
        USBSerial.println("DeepSeek: write failed, retrying...");
        return String();
    }

    // ---- 读取响应头 ----
    int http_code = 0;
    bool chunked = false;
    t0 = millis();

    while (client.connected() && millis() - t0 < 15000) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;

        if (line.startsWith("HTTP/")) {
            int sp1 = line.indexOf(' ');
            int sp2 = line.indexOf(' ', sp1 + 1);
            http_code = line.substring(sp1 + 1, sp2).toInt();
        } else if (line.startsWith("Transfer-Encoding:") && line.indexOf("chunked") >= 0) {
            chunked = true;
        }
    }

    USBSerial.printf("DeepSeek: HTTP %d, chunked=%d (%ums)\n",
        http_code, chunked, millis() - t0);

    if (http_code != 200) {
        String err = client.readString();
        client.stop();
        USBSerial.printf("DeepSeek: error %d: %s\n", http_code, err.substring(0, 300).c_str());
        return String();
    }

    // ---- SSE 流式读取 & 实时显示 ----
    String accumulated = "";
    t0 = millis();
    unsigned long last_data = t0;

    // 行缓冲区
    String line_buf;

    // 总超时 30s, 数据停滞超时 8s
    const unsigned long TOTAL_TIMEOUT = 30000;
    const unsigned long STALL_TIMEOUT = 8000;

    if (chunked) {
        // chunked + SSE: 逐行读取 chunk 内容
        while (client.connected() && millis() - t0 < TOTAL_TIMEOUT) {
            // 读 chunk size 行
            String hex_line = client.readStringUntil('\n');
            hex_line.trim();
            if (hex_line.length() == 0) continue;
            long chunk_size = strtol(hex_line.c_str(), NULL, 16);
            if (chunk_size <= 0) break;

            // 逐字节读 chunk data, 拼出行, 处理 SSE
            long remain = chunk_size;
            while (remain > 0 && millis() - t0 < TOTAL_TIMEOUT) {
                if (millis() - last_data > STALL_TIMEOUT) {
                    USBSerial.println("\nDeepSeek: SSE stall timeout");
                    goto sse_done;
                }
                if (client.available()) {
                    char c = client.read();
                    remain--;
                    last_data = millis();

                    if (c == '\n') {
                        // 一行结束, 处理 SSE
                        line_buf.trim();
                        if (sse_process_line(line_buf, accumulated)) {
                            client.readStringUntil('\n'); // 跳过 chunk 尾 \r\n
                            goto sse_done;
                        }
                        line_buf = "";
                    } else if (c != '\r') {
                        line_buf += c;
                    }
                } else {
                    if (!client.connected()) break;
                    delay(1);
                }
            }
            client.readStringUntil('\n'); // 跳过 chunk 尾部 \r\n
        }
    } else {
        // 非 chunked (fallback): 逐字节读到连接关闭
        while (client.connected() && millis() - t0 < TOTAL_TIMEOUT) {
            if (millis() - last_data > STALL_TIMEOUT) break;
            if (client.available()) {
                char c = client.read();
                last_data = millis();
                if (c == '\n') {
                    line_buf.trim();
                    if (sse_process_line(line_buf, accumulated)) { break; }
                    line_buf = "";
                } else if (c != '\r') {
                    line_buf += c;
                }
            } else { delay(1); }
        }
        // 读残余
        while (client.available()) {
            char c = client.read();
            if (c == '\n') {
                line_buf.trim();
                if (sse_process_line(line_buf, accumulated)) break;
                line_buf = "";
            } else if (c != '\r') {
                line_buf += c;
            }
        }
    }

sse_done:
    client.stop();
    USBSerial.printf("\nDeepSeek: SSE done, %u chars, %ums\n",
        accumulated.length(), millis() - t0);

    if (accumulated.length() == 0) {
        USBSerial.println("DeepSeek: empty stream");
        return String();
    }

    USBSerial.println("\n=== DeepSeek RAW ===");
    USBSerial.println(accumulated);
    USBSerial.println("=== END RAW ===\n");

    return accumulated;
}

static void ai_chat_task(void* pvParameters) {
    ai_chat_active = true;
    ai_chat_activity_time = millis();

    String recognized, reply;
    int16_t* pcm_buffer = nullptr;
    size_t total_samples = 0;
    size_t max_samples = SAMPLE_RATE * MAX_RECORD_SEC;
    size_t alloc_bytes = 0;
    int16_t chunk_buf[CHUNK_SIZE];
    int high_cnt = 0;
    uint8_t saved_step = DRAW_GetStepSize();
    bool has_action = false;
    bool first_wait = true;
    Microphone* mic = nullptr;

    DRAW_SetStepSize(16);

    // ---- 暂停非必要任务，减少堆碎片给 SSL ----
    suspendNonessentialTasks();

    // ---- WiFi ----
    if (!wifi_connect()) { goto done; }

    // ---- 关闭 ESP-NOW（避免 WiFi 冲突） ----
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

    // ---- 多轮对话循环 ----
    // MIC 不在此初始化，每轮按需创建（录音前创建，DeepSeek 前释放 I2S DMA）
    while (ai_chat_active) {
        // 仅第一轮等待时显示提示（回复后提示会覆盖内容）
        if (first_wait) {
            ai_show(AI_PHASE_WAITING, "Long press=record, short=exit");
            ai_chat_activity_time = millis();
            USBSerial.println("Waiting: long press=record, short=exit");
        }
        first_wait = false;

        // ---- 等待按键 ----
        while (digitalRead(EN_S) == HIGH) {
            if (!ai_chat_active) goto done;
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // ---- 短按 vs 长按检测 ----
        unsigned long press_ms = millis();
        bool short_press = true;

        while (digitalRead(EN_S) == LOW) {
            if (!ai_chat_active) goto done;
            if (millis() - press_ms >= 300) {
                short_press = false;     // 超过300ms = 长按
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        if (short_press) {
            USBSerial.println("Short press → exit");
            break;  // 短按退出
        }

        // ---- 长按 → 录音（按住持续录，松手停止）----
        // 先释放 BLE DMA（如有），让堆合并成大块；再创建 MIC，避免碎片
        deinitHardwareDMA();
        if (!mic) {
            mic = new Microphone(MIC_SCK, MIC_WS, MIC_DATA, SAMPLE_RATE);
            if (!mic || !mic->init()) {
                term_println("ERROR: MIC re-init FAILED!");
                delete mic;
                mic = nullptr;
                continue;
            }
        }

        USBSerial.println("Recording... release to stop");
        ai_show(AI_PHASE_THINKING, "Recording...");

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
            size_t n = mic->read(chunk_buf, CHUNK_SIZE);
            if (n == 0) break;
            memcpy(pcm_buffer + total_samples, chunk_buf, n * sizeof(int16_t));
            total_samples += n;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (total_samples < SAMPLE_RATE / 4) {
            term_println("ERROR: Too short!");
            continue;
        }
        USBSerial.printf("Recorded: %.1fs (%zu samples)\n",
                         (float)total_samples / SAMPLE_RATE, total_samples);

        // ---- 释放 MIC (I2S DMA)，给后续 SSL 腾出 DMA 内存 ----
        if (mic) { delete mic; mic = nullptr; }

        // ---- ASR ----
        USBSerial.println("Recognizing...");
        ai_show(AI_PHASE_THINKING, "Thinking...");
        recognized = baidu_asr(pcm_buffer, total_samples);
        if (recognized.length() == 0) {
            term_println("ERROR: ASR failed");
            continue;
        }
        USBSerial.printf("You: %s\n", recognized.c_str());

        // ---- DeepSeek (流式 SSE，失败自动重试 1 次) ----
        ai_show(AI_PHASE_REPLY, "Asking DeepSeek...");
        {
            // 调试：查看内部 SRAM 可用量
            size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
            size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
            size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
            USBSerial.printf("Heap: DMA free=%u (largest=%u), DEFAULT free=%u\n",
                dma_free, dma_largest, heap_free);
        }
        for (int retry = 0;; retry++) {
            reply = deepseek_chat_stream(recognized);
            if (reply.length() > 0) break;          // 成功
            if (retry >= 1) break;                   // 重试 1 次还失败 → 放弃
            USBSerial.println("DeepSeek: retrying after 1s...");
            ai_show(AI_PHASE_REPLY, "Retrying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (reply.length() == 0) {
            term_println("ERROR: DeepSeek failed");
            voice_action = VC_NONE;
            has_action = false;
            ai_show(AI_PHASE_REPLY, "DeepSeek timed out");
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        // ---- 解析 JSON action ----
        {
            String parsed = VC_ParseReply(reply);
            has_action = (voice_action != VC_NONE);
            if (parsed.length() > 0) {
                ai_show(AI_PHASE_REPLY, parsed.c_str());
            }
        }

        // ---- 有动作时退出（guiTask 执行）----
        if (has_action) {
            USBSerial.println("[Executing action...]");
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }

        // ---- 回复保持可见，等待下一轮输入 ----
        // 不显示 "Long press=record, short=exit"（避免覆盖回复文本）
        // 直接回到按键等待，用户短按退出或长按继续录音
        USBSerial.println("Press ENTER: short=exit, long=record again");
        ai_chat_activity_time = millis();
        // 短暂停顿后继续（不覆盖屏幕，回复内容保持显示）
        vTaskDelay(pdMS_TO_TICKS(300));
    }

done:
    reinitHardwareDMA();  // BLE 后续由 joystickCheckTask 自动恢复
    Network_Manager::resume_esp_now();
    resumeNonessentialTasks();
    if (mic) { delete mic; mic = nullptr; }
    ai_chat_dirty = true;
    if (pcm_buffer) free(pcm_buffer);
    DRAW_SetStepSize(saved_step);
    ai_chat_active = false;
    vTaskDelete(NULL);
}

