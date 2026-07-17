#pragma once
#include <Arduino.h>

// ============================================================
// 游戏音效模块 — 单音音色 (方波合成)
// 通过 DAC8554 通道 2/3 输出，在矢量模式 ISR 中混音
// 音频直接渲染到 PSRAM 缓冲区，由定时器 ISR 以 80kHz 播放
// ============================================================

// 初始化游戏音效 (在 initDACoutput 中自动调用)
void initGameAudio();

// 音效状态更新 (在游戏循环中调用，无实际操作，保留兼容)
void gameAudioUpdate();

// ---- 预定义游戏音效 (便捷接口) ----
void sfxEat();        // 吃食物/得分 — 短促高音 880Hz 60ms
void sfxHit();        // 碰撞/击中砖块 — 短促中音 440Hz 80ms
void sfxJump();       // 跳跃 — 上行滑音 400→600→900Hz
void sfxScore();      // 通过障碍得分 — 双音叮咚 1000→1400Hz
void sfxGameOver();   // 游戏结束 — 下行滑音 600→400→250→150Hz
void sfxShoot();      // 射击 — 尖锐短音 1500Hz 40ms
void sfxBounce();     // 反弹 — 极短 blip 660Hz 20ms
void sfxSelect();     // 菜单选择确认 1000Hz 50ms
