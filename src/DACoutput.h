#pragma once
#include <Arduino.h>

void initDACoutput();
void IRAM_ATTR sendDAC(uint8_t configRegister, uint16_t value);

// 设置 DAC 输出频率 (Hz)
// 默认约 80000Hz
void setDACFreq(uint32_t freq);

void Set_Audio_Mode(bool enable);

// 双缓冲接口
void Init_Audio_Buffers(); // 初始化 PSRAM 缓冲区
bool Is_Buf_A_Free();      // Buffer A 是否空闲（可写入）
bool Is_Buf_B_Free();      // Buffer B 是否空闲（可写入）
uint16_t* Get_Buf_A_L();   // 获取 Buffer A 左声道指针
uint16_t* Get_Buf_A_R();   // 获取 Buffer A 右声道指针
uint16_t* Get_Buf_A_X();   // 获取 Buffer A X指针
uint16_t* Get_Buf_A_Y();   // 获取 Buffer A Y指针
uint16_t* Get_Buf_B_L();   // 获取 Buffer B 左声道指针
uint16_t* Get_Buf_B_R();   // 获取 Buffer B 右声道指针
uint16_t* Get_Buf_B_X();   // 获取 Buffer B X指针
uint16_t* Get_Buf_B_Y();   // 获取 Buffer B Y指针
void Mark_Buf_A_Ready(int sample_count); // 标记 Buffer A 已填充完毕，供 ISR 播放
void Mark_Buf_B_Ready(int sample_count); // 标记 Buffer B 已填充完毕，供 ISR 播放

// Player Control
// 0: Vector (Default), 1: Audio (2ch), 2: Video (4ch)
void Set_Player_Mode(int mode);

// ---- 游戏音效接口 (DAC8554 通道 2/3, 矢量模式下混音) ----
void Init_GameAudio_Buffer();                  // 初始化游戏音效 PSRAM 缓冲区
uint16_t* Get_GameAudio_Buf_L();               // 获取音效左声道缓冲区指针
uint16_t* Get_GameAudio_Buf_R();               // 获取音效右声道缓冲区指针
void Start_GameAudio(int sample_count);        // 开始播放，自动清除准备标志
void Begin_GameAudio_Prepare();                // 标记开始写入缓冲区（暂停ISR读取）
void End_GameAudio_Prepare();                  // 取消准备状态（写入失败时调用）
bool Is_GameAudio_Finished();                  // 音效是否已播放完毕
int  Get_GameAudio_MaxSamples();               // 缓冲区最大样本数
