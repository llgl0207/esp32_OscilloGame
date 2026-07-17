#include <WiFiClientSecure.h>
#include "deepseek.h"

// ============================================================
// 内部常量
// ============================================================
static const char* DS_HOST     = "api.deepseek.com";
static const char* DS_ENDPOINT = "/chat/completions";
static const int   DS_TIMEOUT  = 15000;   // ms
static const int   DS_MAX_TOKENS = 2048;

// ============================================================
// 构造函数
// ============================================================
DeepSeekClient::DeepSeekClient()
    : _msgCount(0)
{
}

// ============================================================
// 配置
// ============================================================
void DeepSeekClient::setSystemPrompt(const String& prompt) {
    _systemPrompt = prompt;
}

void DeepSeekClient::setApiKey(const String& key) {
    _apiKey = key;
}

bool DeepSeekClient::isReady() const {
    return _apiKey.length() > 0 && _apiKey != "sk-your-key-here";
}

// ============================================================
// 清空历史（保留 system prompt）
// ============================================================
void DeepSeekClient::resetHistory() {
    _msgCount = 0;
}

// ============================================================
// 构建 JSON 请求体
// ============================================================
bool DeepSeekClient::buildRequest(String& outJson) {
    JsonDocument doc;
    JsonArray msgs = doc["messages"].to<JsonArray>();

    // 1. system prompt（总是第一条）
    if (_systemPrompt.length() > 0) {
        JsonObject sys = msgs.add<JsonObject>();
        sys["role"]    = "system";
        sys["content"] = _systemPrompt;
    }

    // 2. 历史消息
    for (int i = 0; i < _msgCount; i++) {
        JsonObject m = msgs.add<JsonObject>();
        m["role"]    = _history[i].role;
        m["content"] = _history[i].content;
    }

    // 3. 其它参数
    doc["model"]       = "deepseek-v4-flash";
    doc["stream"]      = false;
    doc["max_tokens"]  = DS_MAX_TOKENS;
    doc["temperature"] = 0.7;
    doc["thinking"]["type"] = "disabled";

    // 序列化
    outJson = "";
    serializeJson(doc, outJson);
    return outJson.length() > 0;
}

// ============================================================
// 解析 HTTP 响应
// ============================================================
String DeepSeekClient::parseResponse(const String& raw) {
    // 查找 JSON 起始位置
    int jsonStart = raw.indexOf('{');
    if (jsonStart < 0) {
        Serial.printf("[MergeDeepSeek] no JSON in response\n");
        String prefix = raw.substring(0, 80);
        Serial.printf("[MergeDeepSeek] raw: %s\n", prefix.c_str());
        return "__NO_JSON__";
    }

    String body = raw.substring(jsonStart);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[MergeDeepSeek] JSON parse error: %s\n", err.c_str());
        return "__JSON_ERROR__";
    }

    // 检查 API 错误
    if (doc["error"].is<JsonObject>()) {
        String msg = doc["error"]["message"].as<String>();
        int code   = doc["error"]["code"] | -1;
        Serial.printf("[MergeDeepSeek] API error %d: %s\n", code, msg.c_str());
        return "__API_ERROR__:" + String(code);
    }

    // 提取 choices[0].message.content
    const char* text = doc["choices"][0]["message"]["content"];
    if (text) {
        return String(text);
    }

    // choices 存在但 message 缺失（finish_reason 非正常）
    const char* finish = doc["choices"][0]["finish_reason"];
    if (finish) {
        Serial.printf("[MergeDeepSeek] finish_reason: %s\n", finish);
        return "__FINISH_REASON__:" + String(finish);
    }

    Serial.println("[MergeDeepSeek] unexpected response structure");
    return "__UNEXPECTED__";
}

// ============================================================
// 核心：chat 方法（阻塞）
// ============================================================
String DeepSeekClient::chat(const String& userMessage) {
    if (userMessage.length() == 0) {
        return "__EMPTY__";
    }
    if (!isReady()) {
        Serial.println("[MergeDeepSeek] API key not set");
        return "__NO_KEY__";
    }

    // ---- 1. 添加用户消息到历史 ----
    // 如果历史满了，丢弃最旧的一对（user + assistant）
    int maxMsgs = DS_MAX_ROUNDS * 2;
    if (_msgCount >= maxMsgs) {
        // 丢弃 index 0 和 1（第一轮 user/assistant）
        for (int i = 2; i < _msgCount; i++) {
            _history[i - 2] = _history[i];
        }
        _msgCount -= 2;
    }
    _history[_msgCount].role    = "user";
    _history[_msgCount].content = userMessage;
    _msgCount++;

    // ---- 2. 构建请求体 ----
    String jsonBody;
    if (!buildRequest(jsonBody)) {
        return "__BUILD_FAIL__";
    }

    Serial.printf("[MergeDeepSeek] sending %u bytes...\n", jsonBody.length());
    if (jsonBody.length() > 200) {
        Serial.printf("[MergeDeepSeek] JSON: %.200s...\n", jsonBody.c_str());
    } else {
        Serial.printf("[MergeDeepSeek] JSON: %s\n", jsonBody.c_str());
    }

    // ---- 3. HTTPS POST (raw WiFiClientSecure) ----
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);

    Serial.printf("[MergeDeepSeek] connecting to %s:443...\n", DS_HOST);
    if (!client.connect(DS_HOST, 443)) {
        Serial.println("[MergeDeepSeek] connect failed");
        return "__SSL_CONNECT_FAIL__";
    }
    Serial.println("[MergeDeepSeek] connected, sending request...");

    // Build and send HTTP request
    String request = String("POST ") + DS_ENDPOINT + " HTTP/1.1\r\n"
                     + "Host: " + DS_HOST + "\r\n"
                     + "Content-Type: application/json\r\n"
                     + "Authorization: Bearer " + _apiKey + "\r\n"
                     + "Content-Length: " + jsonBody.length() + "\r\n"
                     + "Connection: close\r\n"
                     + "\r\n"
                     + jsonBody;

    client.print(request);

    // Read response — 持续读到连接关闭
    unsigned long t0 = millis();
    String resp;
    while (client.connected() && millis() - t0 < DS_TIMEOUT) {
        while (client.available()) {
            resp += (char)client.read();
            t0 = millis();  // 收到数据就重置超时
        }
        delay(2);
    }
    // 收尾数据
    while (client.available()) {
        resp += (char)client.read();
    }
    client.stop();

    Serial.printf("[MergeDeepSeek] response length: %d\n", resp.length());
    Serial.printf("[MergeDeepSeek] full response:\n%s\n", resp.c_str());

    // ---- 4. 解析响应 ----
    String result;
    int headerEnd = resp.indexOf("\r\n\r\n");
    String body;
    if (headerEnd >= 0) {
        String httpStatusLine = resp.substring(0, resp.indexOf("\r\n"));
        Serial.printf("[MergeDeepSeek] status: %s\n", httpStatusLine.c_str());
        body = resp.substring(headerEnd + 4);
    } else {
        // 可能只有 \n\n
        headerEnd = resp.indexOf("\n\n");
        if (headerEnd >= 0) {
            body = resp.substring(headerEnd + 2);
        } else {
            body = resp;  // 直接当 JSON 试
        }
    }

    Serial.printf("[MergeDeepSeek] body length: %d\n", body.length());

    // 跳过 chunk 编码（块大小行），直接找第一个 { 当 JSON
    int jsonStart = body.indexOf('{');
    if (jsonStart >= 0) {
        String jsonBody = body.substring(jsonStart);
        result = parseResponse(jsonBody);
    } else if (body.length() > 0) {
        Serial.printf("[MergeDeepSeek] body non-JSON prefix: %.80s\n", body.c_str());
        result = "__NO_JSON__";
    } else {
        result = "__EMPTY_RESPONSE__";
    }

    // ---- 4. 把助手回复加入历史 ----
    if (!result.startsWith("__")) {
        _history[_msgCount].role    = "assistant";
        _history[_msgCount].content = result;
        _msgCount++;

        Serial.printf("[MergeDeepSeek] reply: %.150s\n", result.c_str());
    }

    return result;
}
