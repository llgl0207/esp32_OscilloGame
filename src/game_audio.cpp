#include "game_audio.h"
#include "DACoutput.h"

// ============================================================
// 游戏音效模块 — 方波合成 → DAC8554 通道 2/3
//
// 原理：
//   1. 预渲染方波到 PSRAM 缓冲区 (Get_GameAudio_Buf_L/R)
//   2. 调用 Start_GameAudio() 通知 ISR 开始播放
//   3. DAC8554 定时器 ISR (80kHz) 将缓冲区数据输出到通道 2/3
//
// 音色：纯方波 (单音)，通过频率变化实现不同音效
// 采样率：80kHz (与矢量显示共用定时器)
// 振幅：±8000 (中点 32768，范围 24768~40768)
// ============================================================

#define SFX_AMPLITUDE  8000    // 方波振幅
#define SFX_MIDPOINT    32768  // DAC 中点 (静音)
#define SFX_SAMPLE_RATE 80000  // 采样率

// ---- 内部：渲染方波到缓冲区 ----
static int renderSquareWave(uint16_t* buf, uint32_t freq, uint32_t duration_ms) {
    int max_samples = Get_GameAudio_MaxSamples();
    int total_samples = (int)((uint64_t)SFX_SAMPLE_RATE * duration_ms / 1000);
    if (total_samples > max_samples) total_samples = max_samples;
    if (total_samples <= 0) return 0;

    if (freq == 0) {
        for (int i = 0; i < total_samples; i++) {
            buf[i] = SFX_MIDPOINT;
        }
        return total_samples;
    }

    int period = SFX_SAMPLE_RATE / freq;
    if (period < 2) period = 2;
    int half = period / 2;

    for (int i = 0; i < total_samples; i++) {
        buf[i] = ((i % period) < half)
            ? SFX_MIDPOINT + SFX_AMPLITUDE
            : SFX_MIDPOINT - SFX_AMPLITUDE;
    }
    return total_samples;
}

// ---- 内部：渲染多步音效序列 ----
static void renderSequence(const uint32_t* freqs, const uint32_t* durs, int steps) {
    uint16_t* bufL = Get_GameAudio_Buf_L();
    uint16_t* bufR = Get_GameAudio_Buf_R();
    if (!bufL || !bufR) return;  // PSRAM 分配失败，安全退出
    
    Begin_GameAudio_Prepare();   // 通知 ISR 暂停读取
    
    int max_samples = Get_GameAudio_MaxSamples();
    int total = 0;

    for (int s = 0; s < steps && total < max_samples; s++) {
        int n = (int)((uint64_t)SFX_SAMPLE_RATE * durs[s] / 1000);
        if (total + n > max_samples) n = max_samples - total;
        if (n <= 0) continue;

        uint32_t freq = freqs[s];
        if (freq == 0) {
            for (int i = 0; i < n; i++) {
                bufL[total + i] = SFX_MIDPOINT;
                bufR[total + i] = SFX_MIDPOINT;
            }
        } else {
            int period = SFX_SAMPLE_RATE / freq;
            if (period < 2) period = 2;
            int half = period / 2;
            for (int i = 0; i < n; i++) {
                uint16_t val = ((total + i) % period) < half
                    ? SFX_MIDPOINT + SFX_AMPLITUDE
                    : SFX_MIDPOINT - SFX_AMPLITUDE;
                bufL[total + i] = val;
                bufR[total + i] = val;
            }
        }
        total += n;
    }

    if (total > 0) {
        Start_GameAudio(total);
    }
}

// ============================================================
// 公开接口
// ============================================================

void initGameAudio() {
    // DACoutput 已自动初始化游戏音效 PSRAM 缓冲区
}

void gameAudioUpdate() {
    // 音效由 ISR 自动播放，无需轮询
}

// ---- 预定义音效 ----

void sfxEat() {
    uint16_t* buf = Get_GameAudio_Buf_L();
    if (!buf) return;
    Begin_GameAudio_Prepare();
    int n = renderSquareWave(buf, 880, 60);
    if (n > 0) {
        uint16_t* bufR = Get_GameAudio_Buf_R();
        if (bufR) {
            memcpy((void*)bufR, (void*)buf, n * sizeof(uint16_t));
            Start_GameAudio(n);
        }
    } else {
        End_GameAudio_Prepare();
    }
}

void sfxHit() {
    uint16_t* buf = Get_GameAudio_Buf_L();
    if (!buf) return;
    Begin_GameAudio_Prepare();
    int n = renderSquareWave(buf, 440, 80);
    if (n > 0) {
        uint16_t* bufR = Get_GameAudio_Buf_R();
        if (bufR) {
            memcpy((void*)bufR, (void*)buf, n * sizeof(uint16_t));
            Start_GameAudio(n);
        }
    } else {
        End_GameAudio_Prepare();
    }
}

void sfxJump() {
    const uint32_t freqs[] = {400, 600, 900};
    const uint32_t durs[]  = {30, 30, 40};
    renderSequence(freqs, durs, 3);
}

void sfxScore() {
    const uint32_t freqs[] = {1000, 1400};
    const uint32_t durs[]  = {50, 60};
    renderSequence(freqs, durs, 2);
}

void sfxGameOver() {
    return;  // TODO: 临时禁用以排查崩溃
    // 下行自然小调音阶：G5 → E5 → C5 → A4 → E4 → C4
    const uint32_t freqs[] = {784, 659, 523, 440, 330, 262};
    const uint32_t durs[]  = { 70,  75,  85, 100, 120, 150};
    renderSequence(freqs, durs, 6);
}

void sfxShoot() {
    uint16_t* buf = Get_GameAudio_Buf_L();
    if (!buf) return;
    Begin_GameAudio_Prepare();
    int n = renderSquareWave(buf, 1500, 40);
    if (n > 0) {
        uint16_t* bufR = Get_GameAudio_Buf_R();
        if (bufR) {
            memcpy((void*)bufR, (void*)buf, n * sizeof(uint16_t));
            Start_GameAudio(n);
        }
    } else {
        End_GameAudio_Prepare();
    }
}

void sfxBounce() {
    uint16_t* buf = Get_GameAudio_Buf_L();
    if (!buf) return;
    Begin_GameAudio_Prepare();
    int n = renderSquareWave(buf, 660, 20);
    if (n > 0) {
        uint16_t* bufR = Get_GameAudio_Buf_R();
        if (bufR) {
            memcpy((void*)bufR, (void*)buf, n * sizeof(uint16_t));
            Start_GameAudio(n);
        }
    } else {
        End_GameAudio_Prepare();
    }
}

void sfxSelect() {
    uint16_t* buf = Get_GameAudio_Buf_L();
    if (!buf) return;
    Begin_GameAudio_Prepare();
    int n = renderSquareWave(buf, 1000, 50);
    if (n > 0) {
        uint16_t* bufR = Get_GameAudio_Buf_R();
        if (bufR) {
            memcpy((void*)bufR, (void*)buf, n * sizeof(uint16_t));
            Start_GameAudio(n);
        }
    } else {
        End_GameAudio_Prepare();
    }
}
