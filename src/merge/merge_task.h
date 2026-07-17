#pragma once
#ifndef MERGE_TASK_H
#define MERGE_TASK_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "deepseek.h"

// ============================================================
// 独立串口对话模块 — 从 st7789IoT 移植
//
// 串口指令（在 loop() 中解析）:
//   .en      — 切换到 STA 模式（关 AP/ESP-NOW/BLE），初始化 I2S+ASR+LLM
//   .s       — 开始录音（INMP441 I2S）
//   .e       — 结束录音，送入百度 ASR，串口返回识别结果
//   .l       — 将上次 ASR 结果送入 DeepSeek LLM，串口返回补全
//   .u[文本] — 跳过 ASR，直接送入 LLM 对话
//   .dis     — 关闭串口对话模式，恢复示波器模式（重启 WiFi AP/ESP-NOW）
// ============================================================

// ---- 模块状态 ----
extern volatile bool merge_active;      // 串口对话模式是否启用
extern String        merge_asr_result;  // 最近一次 ASR 识别结果
extern String        merge_last_reply;  // 最近一次 LLM 回复
extern String        merge_user_suffix; // 始终附加在用户文本后的持久提示词

// GuiTask 可轮询的状态标志
extern volatile bool merge_is_recording;    // 正在录音
extern volatile bool merge_asr_done;        // ASR 已完成（GuiTask 消费后清零）
extern volatile bool merge_llm_done;        // LLM 已完成（GuiTask 消费后清零）

// ---- 初始化/反初始化 ----
void Merge_Init();      // 初始化模块（I2S mic, ASR token, DeepSeek client）
void Merge_Deinit();    // 反初始化（释放资源）

// ---- 串口指令处理 ----
void Merge_HandleCommand(const String& cmd);

// ---- GuiTask 接口（非阻塞）----
void Merge_GuiStartRecord();   // 开始录音（非阻塞）
void Merge_GuiStopRecord();    // 停止录音 → 自动 ASR + LLM（在后台执行）
void Merge_GuiPoll();          // 在 GuiTask 循环中调用，消费完成标志

// ---- LLM ----
void Merge_RunLLM(const String& text);

// ---- LLM ----
void Merge_RunLLM(const String& text);

#endif // MERGE_TASK_H
