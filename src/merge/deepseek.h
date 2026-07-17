#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#define DS_MAX_ROUNDS 6  // 最多保留 6 轮对话（12 条消息）

/// 单条消息
struct DS_Message {
    String role;    // "system", "user", "assistant"
    String content;
};

/// DeepSeek V4 Flash 聊天客户端
/// 所有公共方法都是阻塞的
class DeepSeekClient {
public:
    DeepSeekClient();

    /// 设置 system prompt（会在每次 resetHistory 后保留）
    void setSystemPrompt(const String& prompt);

    /// 设置 API Key
    void setApiKey(const String& key);

    /// 发送用户消息 → 调用 API → 添加回复到历史 → 返回回复文本
    /// 阻塞 1~5 秒，返回空字符串或 "__XXX__" 前缀表示出错
    String chat(const String& userMessage);

    /// 清空对话历史（保留 system prompt 和 API key）
    void resetHistory();

    /// 检查 API Key 是否已设置
    bool isReady() const;

private:
    String _apiKey;
    String _systemPrompt;
    DS_Message _history[DS_MAX_ROUNDS * 2 + 1];  // [system, u1, a1, u2, a2, ...]
    int _msgCount;  // 当前历史条数（包括 system prompt）

    /// 将 _history 序列化为 JSON 请求体
    bool buildRequest(String& outJson);

    /// 从 HTTP 响应中提取 choices[0].message.content
    String parseResponse(const String& raw);
};
