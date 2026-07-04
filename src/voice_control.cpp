/**
 * @file voice_control.cpp
 * @brief 语音控制执行层 — 解析 DeepSeek JSON 回复，映射为 UI 动作
 *
 * 移植到新项目只需：
 * 1. 复制 voice_control.h / .cpp
 * 2. 修改 action 到实际 UI 的映射（在 VC_Execute 中）
 * 3. 在主循环中定期检查 voice_pending 并执行
 */

#include "voice_control.h"
#include <ArduinoJson.h>

volatile VC_Action voice_action = VC_NONE;
volatile bool voice_pending = false;

/**
 * @brief 将 JSON action 字符串映射为 VC_Action 枚举
 */
static VC_Action str_to_action(const String& action) {
    if (action == "open_music")   return VC_OPEN_MUSIC;
    if (action == "open_video")   return VC_OPEN_VIDEO;
    if (action == "open_games")   return VC_OPEN_GAMES;
    if (action == "open_online")  return VC_OPEN_ONLINE;
    if (action == "open_game_joy") return VC_OPEN_GAME_JOY;
    if (action == "open_ai_chat") return VC_OPEN_AI_CHAT;
    if (action == "open_about")   return VC_OPEN_ABOUT;
    if (action == "start_snake")  return VC_START_SNAKE;
    if (action == "start_breakout") return VC_START_BREAKOUT;
    if (action == "start_flappy") return VC_START_FLAPPY;
    if (action == "start_racing") return VC_START_RACING;
    if (action == "start_runtiny") return VC_START_RUNTINY;
    if (action == "start_tank")   return VC_START_TANK;
    if (action == "back")         return VC_BACK;
    if (action == "exit")         return VC_EXIT;
    return VC_NONE;
}

/**
 * @brief 从 DeepSeek 回复中提取 JSON 对象
 *
 * DeepSeek 常把 JSON 包在 markdown 代码块里:
 *   ```json
 *   {"reply":"...","action":"..."}
 *   ```
 * 或在 JSON 前后加文字。
 *
 * 策略:
 * 1. 去掉 ```json / ``` 标记
 * 2. 找到第一个 '{' 和最后一个 '}' 截取
 * 3. 如果整串不是 JSON，退回纯文本
 */
static String extract_json_object(const String& raw) {
    String s = raw;

    // Step 1: 去掉 ```json ... ``` 包裹
    s.replace("```json", "");
    s.replace("```", "");
    s.trim();

    // Step 2: 找到 { ... } 边界
    int brace_start = s.indexOf('{');
    int brace_end = s.lastIndexOf('}');
    if (brace_start != -1 && brace_end > brace_start) {
        return s.substring(brace_start, brace_end + 1);
    }

    return s; // 没有花括号，返回原始字符串
}

String VC_ParseReply(const String& json_reply) {
    // 提取纯净 JSON 子串
    String clean = extract_json_object(json_reply);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, clean);

    if (err || !doc["action"].is<const char*>()) {
        // 不是有效的 JSON action 格式 → 当作纯文本回复
        return json_reply;
    }

    // 提取回复文字
    String reply = doc["reply"].as<String>();
    // 提取动作
    String action_str = doc["action"].as<String>();
    VC_Action action = str_to_action(action_str);

    if (action != VC_NONE) {
        voice_action = action;
        voice_pending = true;
    }

    // 若 reply 为空但 action 有效，给个默认提示
    if (reply.length() == 0 && action != VC_NONE) {
        reply = "[Executing: " + action_str + "]";
    }

    return reply;
}
