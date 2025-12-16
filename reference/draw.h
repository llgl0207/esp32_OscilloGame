#ifndef __DRAW_H__
#define __DRAW_H__

#include "main.h"
#include "tim.h"
#include "dac.h"

// --- 硬件抽象层 (HAL) 配置 ---
// 1. 定义输出类型
#define DRAW_OUTPUT_TYPE_DAC 0
#define DRAW_OUTPUT_TYPE_PWM 1

// *** 用户配置 ***
#define DRAW_CFG_OUTPUT_TYPE      DRAW_OUTPUT_TYPE_DAC
#define DRAW_CFG_DELAY_TIM        htim14  // 用于 CPU 延时的定时器

// DAC 配置 (如果 DRAW_CFG_OUTPUT_TYPE == DRAW_OUTPUT_TYPE_DAC)
#define DRAW_DAC_HANDLE           hdac
#define DRAW_DAC_CH_X             DAC_CHANNEL_1
#define DRAW_DAC_CH_Y             DAC_CHANNEL_2

// PWM 配置 (如果 DRAW_CFG_OUTPUT_TYPE == DRAW_OUTPUT_TYPE_PWM)
// 示例: TIM3 CH1 用于 X, TIM3 CH2 用于 Y
#define DRAW_PWM_TIM_X            htim3
#define DRAW_PWM_CH_X             TIM_CHANNEL_1
#define DRAW_PWM_CCR_X            CCR1 // TIM 结构体中的寄存器名称 (CCR1, CCR2...)
#define DRAW_PWM_TIM_Y            htim3
#define DRAW_PWM_CH_Y             TIM_CHANNEL_2
#define DRAW_PWM_CCR_Y            CCR2

// F103 移植说明:
// - 确保定时器句柄 (htimX) 与您的 CubeMX 配置匹配。
// - 如果使用 F103，请在 main.h 中包含 "stm32f1xx_hal.h"。
// ------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// DMA 缓冲区大小 (根据 RAM 可用性和复杂度进行调整)
// 增加到 16384 以支持更多文本 (需要约 64KB RAM)
#define DRAW_BUF_SIZE 16384

// 形状类型
typedef enum {
    DRAW_TYPE_TEXT = 0,
    DRAW_TYPE_LINE,
    DRAW_TYPE_RECT,
    DRAW_TYPE_CIRCLE
} DrawType;

typedef enum {
    DRAW_MODE_DMA = 0,
    DRAW_MODE_CPU
} DrawMode;

void DRAW_Init(uint32_t interval_ms);
void DRAW_SetMode(DrawMode mode);
void DRAW_SetCPUDelay(uint32_t delay);
uint32_t DRAW_GetCPUDelay(void);
void DRAW_SetCPUJumpDwell(uint32_t dwell);
uint32_t DRAW_GetCPUJumpDwell(void);
void DRAW_SetDrawDensity(uint32_t density);
uint32_t DRAW_GetDrawDensity(void);
void DRAW_SetLetter(char c);
void DRAW_SetScale(uint16_t scale_x_percent, uint16_t scale_y_percent);
void DRAW_SetOffset(int16_t offset_x, int16_t offset_y);

// 将字符串添加到显示池。如果成功返回槽索引，如果池满返回 -1。
int16_t DRAW_AddString(const char *s, uint16_t spacing, int32_t x, int32_t y,
                       uint16_t scale_x, uint16_t scale_y);

// 获取/设置文本对象的滚动偏移量
int32_t DRAW_GetTextScroll(const char *text);
void DRAW_SetTextScroll(int16_t slot, int32_t scroll);

// 添加几何形状
uint8_t DRAW_AddLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
uint8_t DRAW_AddRect(int32_t x, int32_t y, int32_t w, int32_t h);
uint8_t DRAW_AddCircle(int32_t x, int32_t y, int32_t r);

// 清除绘图
void DRAW_Clear(void);

// 更新 DMA 缓冲区的内部函数
void DRAW_Render(void);

// 动画的定期更新 (滚动)
void DRAW_Update(void);

// 终端函数
void DRAW_Terminal_Init(uint16_t scale_pct, int32_t spacing);
void DRAW_Terminal_SetSpacing(int32_t spacing);
void DRAW_Terminal_Print(const char *str);

#ifdef __cplusplus
}
#endif

#endif // __DRAW_H__
