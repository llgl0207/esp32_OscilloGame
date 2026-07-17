/**
 * @file merge_task.cpp
 * @brief 独立串口对话模块 — 从 st7789IoT 移植
 *
 * 完整复制 IoT 工程的语音识别 + LLM 链路，运行在独立的 FreeRTOS 任务中，
 * 使用独立的 WiFi STA 连接、独立的 I2S 麦克风、独立的 DeepSeekClient。
 * 不与当前工程的 AP/ESP-NOW/WebServer/AIChatTask 共享任何网络资源。
 *
 * I2S 引脚复用当前工程的配置（pins.h 中的 MIC_*），不另选引脚。
 */

#include "merge_task.h"
#include "pins.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_now.h>

// 引入 Network_Manager 和 initWebServer 以在 .dis. 时恢复
#include "network_manager.h"
#include "voice_control.h"
extern void initWebServer();

// ============================================================
// 配置常量（从 st7789IoT main.cpp 移植）
// ============================================================

// ---- WiFi（独立连接，不干扰现有 AP/ESP-NOW）----
// 复用 ai_config.h 中的 WIFI_SSID / WIFI_PASSWORD
#include "ai_config.h"

// ---- I2S Mic (INMP441) ----
// 引脚复用当前工程的 pins.h 定义：MIC_SCK=47, MIC_WS=48, MIC_DATA=1
#define I2S_PORT        I2S_NUM_1       // 使用 I2S_NUM_1 避免与原有的 I2S_NUM_0 冲突
#define SAMPLE_RATE          16000
#define MAX_RECORD_SECONDS   10
#define MAX_RECORD_SAMPLES   (SAMPLE_RATE * MAX_RECORD_SECONDS)

// ---- Baidu ASR ----
// 凭据定义在 ai_config.h 中（MERGE_BAIDU_APIKEY, MERGE_BAIDU_SECKEY）
#define BAIDU_DEVPID    80001
#define BAIDU_AUTH_HOST "aip.baidubce.com"
#define BAIDU_AUTH_PATH "/oauth/2.0/token"
#define BAIDU_PRO_URL   "http://vop.baidu.com/pro_api"  // 用 HTTP 不走 SSL，更稳定

// ---- DeepSeek ----
// 凭据定义在 ai_config.h 中（MERGE_DEEPSEEK_API_KEY）
#define SYSTEM_PROMPT \
    "You control an ESP32 oscilloscope game console called OscilloGame. " \
    "The console uses a vector oscilloscope display (XY mode) to draw graphics. " \
    "It can play music from SD card, play video via oscilloscope output, " \
    "and run several games: Snake, Breakout, Flappy Bird, Racing, RunTiny, Tank. " \
    "It also has online multiplayer (ESP-NOW), Bluetooth gamepad support, " \
    "and a web remote control interface.\n" \
    "You MUST reply STRICTLY in ENGLISH ONLY. " \
    "ABSOLUTELY NEVER use Chinese or any non-ASCII characters. " \
    "Keep responses short and natural.\n" \
    "CRITICAL: NEVER wrap your reply in quotes, backticks, code blocks, " \
    "or any formatting markers. Return ONLY the raw sentence text.\n" \
    "For navigation actions, reply with JSON (no markdown):\n" \
    "{\"reply\":\"...\",\"action\":\"action_name\"}\n" \
    "The 'reply' text will be displayed on the oscilloscope. " \
    "The 'action' tells the system to navigate. " \
    "Actions: open_music, open_video, open_games, open_online, " \
    "open_game_joy, open_about, " \
    "start_snake, start_breakout, start_flappy, start_racing, " \
    "start_runtiny, start_tank, back"

// ============================================================
// 模块状态
// ============================================================
volatile bool merge_active = false;
String        merge_asr_result = "";
String        merge_last_reply = "";
String        merge_user_suffix = "(you can only answer in English)";

// GuiTask 可轮询的标志
volatile bool merge_is_recording = false;
volatile bool merge_asr_done = false;
volatile bool merge_llm_done = false;

// ============================================================
// 内部状态
// ============================================================
static TaskHandle_t s_mergeTaskHandle = NULL;
static TaskHandle_t s_workerTaskHandle = NULL;  // 后台 ASR/LLM 工作线程

// 工作线程触发标志
static volatile bool s_worker_trigger = false;

// 音频
static int16_t*      audio_buffer    = nullptr;
static volatile size_t  audio_samples = 0;
static volatile bool    is_recording  = false;

// ASR token
static String asr_token = "";
static unsigned long asr_token_expiry = 0;

// DeepSeek Client
static DeepSeekClient s_llm;

// 录音临时缓冲
static const size_t CHUNK_FRAMES = 256;
static int32_t chunk_buf[CHUNK_FRAMES];

// 录音任务句柄
static TaskHandle_t s_recordTaskHandle = NULL;

// ============================================================
// 前向声明
// ============================================================
static bool     initI2S();
static void     deinitI2S();
static void     record_task_func(void* pvParameters);
static void     worker_task_func(void* pvParameters);  // 后台 ASR+LLM 工作线程
static bool     wifi_connect_sta();
static void     wifi_disconnect_sta();
static bool     refreshToken();
static String   recognizeAudio();
static void     record_i2s_samples();
static void     merge_task_func(void* pvParameters);

// ============================================================
// I2S 麦克风初始化（I2S_NUM_1，避免与原有冲突）
// ============================================================
static bool initI2S() {
    i2s_config_t i2s_config = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0,
        .bits_per_chan        = I2S_BITS_PER_CHAN_32BIT,
    };
    i2s_pin_config_t pin_cfg = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = MIC_SCK,
        .ws_io_num    = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = MIC_DATA,
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[MERGE] I2S install failed: %s\n", esp_err_to_name(err));
        return false;
    }
    err = i2s_set_pin(I2S_PORT, &pin_cfg);
    if (err != ESP_OK) {
        Serial.printf("[MERGE] I2S set pin failed: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }
    Serial.println("[MERGE] I2S mic initialized (I2S_NUM_1)");
    return true;
}

static void deinitI2S() {
    i2s_driver_uninstall(I2S_PORT);
    Serial.println("[MERGE] I2S deinitialized");
}

// ============================================================
// 百度 Token 刷新（HTTP，不走 SSL）
// ============================================================
static bool refreshToken() {
    WiFiClient client;
    HTTPClient http;

    String url = String("http://") + BAIDU_AUTH_HOST + BAIDU_AUTH_PATH
               + "?grant_type=client_credentials"
               + "&client_id=" + MERGE_BAIDU_APIKEY
               + "&client_secret=" + MERGE_BAIDU_SECKEY;

    http.begin(client, url);
    http.setTimeout(10000);

    Serial.printf("[MERGE] Requesting Baidu token...\n");
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[MERGE] Token HTTP %d\n", code);
        http.end();
        return false;
    }

    String resp = http.getString();
    http.end();

    int jsonStart = resp.indexOf('{');
    if (jsonStart < 0) {
        Serial.printf("[MERGE] Token no JSON: %s\n", resp.c_str());
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp.substring(jsonStart));
    if (err) {
        Serial.printf("[MERGE] Token parse fail: %s\n", err.c_str());
        return false;
    }

    String token = doc["access_token"].as<String>();
    if (token.length() == 0) {
        String desc = doc["error_description"].as<String>();
        Serial.printf("[MERGE] Token failed: %s\n", desc.c_str());
        return false;
    }

    asr_token = token;
    int expiresIn = doc["expires_in"] | 2592000;
    asr_token_expiry = millis() + (expiresIn - 60) * 1000;
    Serial.printf("[MERGE] Baidu token OK (expires %ds)\n", expiresIn);
    return true;
}

// ============================================================
// 百度 ASR 识别（HTTP，不走 SSL）
// ============================================================
static String recognizeAudio() {
    if (asr_token.length() == 0 || millis() >= asr_token_expiry) {
        if (!refreshToken()) return "__TOKEN_ERROR__";
    }

    size_t dataLen = audio_samples * sizeof(int16_t);
    if (dataLen == 0) return "__NO_AUDIO__";

    String url = String(BAIDU_PRO_URL)
               + "?format=pcm&rate=" + String(SAMPLE_RATE)
               + "&channel=1"
               + "&cuid=esp32s3-merge"
               + "&dev_pid=" + String(BAIDU_DEVPID)
               + "&token=" + asr_token;

    Serial.printf("[MERGE] ASR sending %zu bytes...\n", dataLen);

    WiFiClient client;
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Content-Type", "audio/pcm;rate=16000");
    http.setTimeout(15000);

    int httpCode = http.POST((uint8_t*)audio_buffer, dataLen);

    String result;
    if (httpCode > 0) {
        String resp = http.getString();
        Serial.printf("[MERGE] ASR HTTP %d\n", httpCode);

        int jsonStart = resp.indexOf('{');
        if (jsonStart >= 0) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, resp.substring(jsonStart));
            if (!err) {
                int errNo = doc["err_no"] | -1;
                if (errNo == 0) {
                    const char* text = doc["result"][0];
                    if (text) result = String(text);
                    else result = "(empty result)";
                } else {
                    const char* msg = doc["err_msg"] | "unknown";
                    result = "__ERROR__:" + String(errNo) + ":" + String(msg);
                    Serial.printf("[MERGE] ASR error %d: %s\n", errNo, msg);
                }
            } else {
                result = "__JSON_ERROR__";
            }
        } else {
            result = "__NO_JSON__";
        }
    } else {
        Serial.printf("[MERGE] ASR HTTP failed: %d\n", httpCode);
        result = "__HTTP_ERROR__:" + String(httpCode);
    }

    http.end();
    return result;
}

// ============================================================
// 独立 WiFi STA 连接
// ============================================================
static bool wifi_connect_sta() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("[MERGE] Connecting WiFi STA to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.setSleep(false);

    unsigned long t0 = millis();
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print(".");
        if (++tries > 50) {
            Serial.println("\n[MERGE] WiFi timeout!");
            return false;
        }
    }
    Serial.printf("\n[MERGE] WiFi OK, IP: %s (%ums)\n", WiFi.localIP().toString().c_str(), millis() - t0);
    return true;
}

static void wifi_disconnect_sta() {
    WiFi.disconnect(true);
    delay(100);

    // ---- 恢复 AP_STA 模式，重新初始化 ESP-NOW 和 WebServer ----
    Serial.println("[MERGE] Restoring AP+ESP-NOW...");

    // 先 deinit ESP-NOW（以防残留状态）
    esp_now_deinit();
    delay(20);

    // 切换回 AP_STA 模式并重新初始化网络
    WiFi.mode(WIFI_AP_STA);
    delay(50);

    // 直接调用 Network_Manager::enable()（内部 init + softAP）
    Network_Manager::enable();

    Serial.println("[MERGE] ESP-NOW and AP restored");
}

// ---- 录音任务（FreeRTOS，在 Core 1 上轮询 I2S）----
static void record_task_func(void* pvParameters) {
    Serial.println("[MERGE] Record task started");
    while (is_recording && merge_active) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_PORT, chunk_buf, CHUNK_FRAMES * sizeof(int32_t),
                                 &bytesRead, pdMS_TO_TICKS(5));
        if (err == ESP_OK && bytesRead > 0) {
            size_t frames = bytesRead / sizeof(int32_t);
            for (size_t i = 0; i < frames; i++) {
                if (audio_samples >= MAX_RECORD_SAMPLES) {
                    is_recording = false;
                    break;
                }
                int16_t sample = (int16_t)(chunk_buf[i] >> 16);
                audio_buffer[audio_samples++] = sample;
            }
        }
        taskYIELD();
    }
    Serial.printf("[MERGE] Record task exit, samples=%zu\n", audio_samples);
    s_recordTaskHandle = NULL;
    vTaskDelete(NULL);
}

// ---- 后台工作线程（在 Core 0 上执行 ASR + LLM，不阻塞 GuiTask）----
static void worker_task_func(void* pvParameters) {
    Serial.println("[MERGE] Worker task started");
    while (merge_active) {
        if (s_worker_trigger) {
            s_worker_trigger = false;

            // 1. ASR
            merge_asr_done = false;
            String asr_result = recognizeAudio();
            if (asr_result.length() > 0 && !asr_result.startsWith("__")) {
                merge_asr_result = asr_result;
                Serial.printf("[MERGE] ASR result: \"%s\"\n", asr_result.c_str());
            } else {
                Serial.printf("[MERGE] ASR failed: %s\n", asr_result.c_str());
                merge_asr_result = "";
                merge_llm_done = true;  // 标记完成（空结果）
                continue;
            }
            merge_asr_done = true;

            // 2. LLM（将 ASR 结果送入）
            String text = merge_asr_result;
            if (merge_user_suffix.length() > 0) {
                text += " " + merge_user_suffix;
            }
            Serial.println("[MERGE] Calling DeepSeek LLM...");
            String reply = s_llm.chat(text);
            s_llm.resetHistory();

            if (reply.length() > 0 && !reply.startsWith("__")) {
                // 先解析动作（VC_ParseReply 会设置 voice_pending 和 voice_action）
                String display = VC_ParseReply(reply);
                if (voice_pending) {
                    // 有动作：显示 reply 文本，动作由 GuiTask 消费
                    merge_last_reply = display;
                    Serial.printf("[MERGE] LLM reply+action: \"%s\" (action=%d)\n",
                        display.c_str(), (int)voice_action);
                } else {
                    // 纯文本
                    merge_last_reply = reply;
                    Serial.printf("[MERGE] LLM reply: \"%s\"\n", reply.c_str());
                }
            } else {
                Serial.printf("[MERGE] LLM failed: %s\n", reply.c_str());
                merge_last_reply = "(LLM failed)";
            }
            merge_llm_done = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    Serial.println("[MERGE] Worker task exit");
    s_workerTaskHandle = NULL;
    vTaskDelete(NULL);
}

// ============================================================
// 公共接口实现
// ============================================================

void Merge_Init() {
    if (merge_active) {
        Serial.println("[MERGE] Already active");
        return;
    }

    // 先输出一个醒目标记，确认串口通信正常
    Serial.println("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println   ("!!! MERGE MODE: INIT START !!!");
    Serial.println   ("!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    // 1. 分配 PSRAM 音频缓冲区
    size_t bufBytes = MAX_RECORD_SAMPLES * sizeof(int16_t);
    audio_buffer = (int16_t*)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
    if (!audio_buffer) {
        Serial.printf("[MERGE] PSRAM alloc failed (%zu bytes)\n", bufBytes);
        Serial.println("[MERGE] INIT FAILED");
        return;
    }
    Serial.printf("[MERGE] Audio buffer: %zu bytes in PSRAM\n", bufBytes);

    // 2. 初始化 I2S 麦克风
    if (!initI2S()) {
        Serial.println("[MERGE] I2S init FAILED");
        free(audio_buffer);
        audio_buffer = nullptr;
        return;
    }

    // 3. 连接 WiFi STA
    if (!wifi_connect_sta()) {
        Serial.println("[MERGE] WiFi FAILED");
        deinitI2S();
        free(audio_buffer);
        audio_buffer = nullptr;
        return;
    }

    // 4. 预取百度 ASR Token
    Serial.println("[MERGE] Pre-fetching Baidu token...");
    if (!refreshToken()) {
        Serial.println("[MERGE] Baidu token FAILED (will retry on first ASR)");
    }

    // 5. 初始化 DeepSeek Client
    s_llm.setApiKey(MERGE_DEEPSEEK_API_KEY);
    s_llm.setSystemPrompt(SYSTEM_PROMPT);
    if (s_llm.isReady()) {
        Serial.println("[MERGE] DeepSeek client ready");
    } else {
        Serial.println("[MERGE] DeepSeek NOT ready — check API key");
    }

    merge_active = true;
    merge_asr_result = "";
    merge_last_reply = "";
    merge_user_suffix = "(you can only answer in English, do not include any non-ASCII Characters)";
    merge_is_recording = false;
    merge_asr_done = false;
    merge_llm_done = false;
    s_worker_trigger = false;
    audio_samples = 0;
    is_recording = false;

    // 启动后台工作线程（Core 0，执行 ASR + LLM）
    xTaskCreatePinnedToCore(
        worker_task_func,
        "MergeWorkerTask",
        8192,
        NULL,
        1,
        &s_workerTaskHandle,
        0
    );

    Serial.println("===== MERGE MODE: READY =====");
    Serial.println("Commands: .s.=record  .e.=ASR  .l.=LLM  .u[text]  .dis.=exit");
}

void Merge_Deinit() {
    Serial.println("\n===== MERGE MODE: DEINIT =====");

    merge_active = false;
    is_recording = false;

    // 等待工作线程退出
    unsigned long t0 = millis();
    while (s_workerTaskHandle != NULL && millis() - t0 < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 释放 I2S
    deinitI2S();

    // 释放 PSRAM 音频缓冲区
    if (audio_buffer) {
        free(audio_buffer);
        audio_buffer = nullptr;
        Serial.println("[MERGE] Audio buffer freed");
    }

    // 断开 WiFi
    wifi_disconnect_sta();

    // 清空状态
    merge_asr_result = "";
    merge_last_reply = "";
    audio_samples = 0;
    asr_token = "";

    Serial.println("===== MERGE MODE: OFF =====");
}

// ============================================================
// GuiTask 非阻塞接口
// ============================================================
void Merge_GuiStartRecord() {
    if (!merge_active || !audio_buffer) {
        Serial.println("[MERGE] Not initialized");
        return;
    }
    audio_samples = 0;
    is_recording = true;
    merge_is_recording = true;
    merge_asr_done = false;
    merge_llm_done = false;

    // 创建录音任务（Core 1，低优先级）
    xTaskCreatePinnedToCore(
        record_task_func,
        "MergeRecordTask",
        2048,
        NULL,
        1,
        &s_recordTaskHandle,
        1
    );
    Serial.println("[MERGE] Recording started...");
}

void Merge_GuiStopRecord() {
    if (!is_recording) {
        Serial.println("[MERGE] Not recording");
        return;
    }
    is_recording = false;
    merge_is_recording = false;
    // 等待录音任务退出
    unsigned long t0 = millis();
    while (s_recordTaskHandle != NULL && millis() - t0 < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    Serial.printf("[MERGE] Recording done: %.1fs (%zu samples)\n",
                  (float)audio_samples / SAMPLE_RATE, audio_samples);

    if (audio_samples >= SAMPLE_RATE / 4) {
        // 触发后台工作线程执行 ASR + LLM
        s_worker_trigger = true;
        Serial.println("[MERGE] Worker triggered for ASR+LLM");
    } else {
        Serial.println("[MERGE] Audio too short, skipped");
        merge_llm_done = true;  // 标记完成（无结果）
    }
}

void Merge_GuiPoll() {
    // GuiTask 在循环中调用此函数消费完成标志
    // 目前标志直接由 freertos.cpp 读取，此函数预留用于扩展
}

void Merge_StartRecording() {
    if (!merge_active || !audio_buffer) {
        Serial.println("[MERGE] Not initialized");
        return;
    }
    if (is_recording) {
        Serial.println("[MERGE] Already recording");
        return;
    }
    audio_samples = 0;
    is_recording = true;

    // 创建录音任务（Core 1，低优先级）
    xTaskCreatePinnedToCore(
        record_task_func,
        "MergeRecordTask",
        2048,
        NULL,
        1,
        &s_recordTaskHandle,
        1
    );

    Serial.println("[MERGE] Recording... (.e to stop)");
}

void Merge_StopRecording() {
    if (!is_recording) {
        Serial.println("[MERGE] Not recording");
        return;
    }
    is_recording = false;
    // 等待录音任务退出
    unsigned long t0 = millis();
    while (s_recordTaskHandle != NULL && millis() - t0 < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    Serial.printf("[MERGE] Recording done: %.1fs (%zu samples)\n",
                  (float)audio_samples / SAMPLE_RATE, audio_samples);
}

void Merge_RunASR() {
    if (!merge_active) {
        Serial.println("[MERGE] Not initialized");
        return;
    }
    if (audio_samples < SAMPLE_RATE / 4) {
        Serial.println("[MERGE] Audio too short, need >= 0.25s");
        return;
    }

    Serial.println("[MERGE] Running ASR...");
    String result = recognizeAudio();

    if (result.length() > 0 && !result.startsWith("__")) {
        merge_asr_result = result;
        Serial.printf("[MERGE] ASR result: \"%s\"\n", result.c_str());
        Serial.printf(">>> ASR: %s\n", result.c_str());
    } else {
        Serial.printf("[MERGE] ASR failed: %s\n", result.c_str());
    }
}

void Merge_RunLLM(const String& text) {
    if (!merge_active) {
        Serial.println("[MERGE] Not initialized");
        return;
    }
    if (text.length() == 0) {
        Serial.println("[MERGE] Empty text, skipping LLM");
        return;
    }

    // 拼接持久后缀
    String text_with_suffix = text;
    if (merge_user_suffix.length() > 0) {
        text_with_suffix += " " + merge_user_suffix;
    }

    Serial.println("[MERGE] Calling DeepSeek LLM...");
    String reply = s_llm.chat(text_with_suffix);
    s_llm.resetHistory();

    if (reply.length() > 0 && !reply.startsWith("__")) {
        merge_last_reply = reply;
        Serial.printf("[MERGE] LLM reply: \"%s\"\n", reply.c_str());
        Serial.printf(">>> LLM: %s\n", reply.c_str());
    } else {
        Serial.printf("[MERGE] LLM failed: %s\n", reply.c_str());
    }
}

// ============================================================
// 串口指令处理（在 Arduino loop() 中调用）
// ============================================================
void Merge_HandleCommand(const String& cmd) {
    // 无论如何先回显确认收到
    Serial.printf("\n[CMD] Received: \"%s\"\n", cmd.c_str());

    if (cmd == ".en.") {
        Serial.println("[CMD] -> Calling Merge_Init()...");
        Merge_Init();
    }
    else if (cmd == ".dis.") {
        Merge_Deinit();
    }
    else if (cmd == ".s.") {
        Merge_StartRecording();
    }
    else if (cmd == ".e.") {
        Merge_StopRecording();
        Merge_RunASR();
    }
    else if (cmd == ".l.") {
        if (merge_asr_result.length() == 0) {
            Serial.println("[MERGE] No ASR result, do .s. .e. first or use .u[text]");
        } else {
            Merge_RunLLM(merge_asr_result);
        }
    }
    else if (cmd.startsWith(".u[") && cmd.endsWith("]")) {
        // 提取 .u[文本] 中的文本
        String text = cmd.substring(3, cmd.length() - 1);
        if (text.length() > 0) {
            Merge_RunLLM(text);
        } else {
            Serial.println("[MERGE] Empty text in .u[]");
        }
    }
    else {
        Serial.println("[MERGE] Unknown command. Available: .en. .s. .e. .l. .u[text] .dis.");
    }
}
