#pragma once

#ifndef AI_CHAT_H
#define AI_CHAT_H

#include <Arduino.h>

// AI Chat state
extern volatile bool ai_chat_active;
extern volatile unsigned long ai_chat_activity_time; // 最后活动时间（看门狗用）

// 阶段式显示 — 每个阶段只显示一条内容在屏幕中央
enum AIChatPhase {
    AI_PHASE_WAITING  = 0,   // "Press ENTER to record..."
    AI_PHASE_THINKING = 1,   // "Thinking..."
    AI_PHASE_REPLY    = 2,   // AI 回复
};
extern volatile AIChatPhase ai_chat_phase;
extern char  ai_chat_display_text[512];   // 要显示的文字
extern volatile bool ai_chat_dirty;

// 共享的屏幕参数 — guiTask 渲染时使用
#define AI_SCALE        40
#define AI_SPACING      0
#define AI_CENTER_X     800
#define AI_CENTER_Y     1024
#define AI_REPLY_START_Y 1024
#define AI_REPLY_SPACING 250

// Resume AI Chat task when triggered
void AI_Chat_Start();

// Stop AI Chat task
void AI_Chat_Stop();

#endif

