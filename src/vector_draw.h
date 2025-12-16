#pragma once
#include <Arduino.h>

// --- 核心功能 ---

// 初始化绘图逻辑
void DRAW_Init();

// 计算下一帧坐标 (在中断中调用)
// 返回值通过引用传出
void IRAM_ATTR DRAW_GetNextPoint(uint16_t &outX, uint16_t &outY);

// --- 绘图 API ---

// 设置单个字符 (测试用)
void DRAW_SetLetter(char c);

// 设置全局缩放比例 (100 = 1.0)
void DRAW_SetScale(uint16_t scale_x_percent, uint16_t scale_y_percent);

// 设置全局偏移量
void DRAW_SetOffset(int16_t offset_x, int16_t offset_y);

// 添加字符串 (高级)
// 返回: 对象的 slot 索引 (用于滚动控制)，失败返回 -1
int16_t DRAW_AddString(const char *s, uint16_t spacing, int32_t x, int32_t y, uint16_t scale_x, uint16_t scale_y);

// 获取/设置文本滚动偏移
int32_t DRAW_GetTextScroll(int16_t slot); 
void DRAW_SetTextScroll(int16_t slot, int32_t scroll);

// 几何图形
void DRAW_AddLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
void DRAW_AddRect(int32_t x, int32_t y, int32_t w, int32_t h);
void DRAW_AddCircle(int32_t x, int32_t y, int32_t r);

// 控制与更新
void DRAW_Clear(void);
void DRAW_Render(void); // 提交帧 (原 updateFrame)
void DRAW_Update(void); // 更新动画

// 终端功能
void DRAW_Terminal_Init(uint16_t scale_pct, int32_t spacing);
void DRAW_Terminal_SetSpacing(int32_t spacing);
void DRAW_Terminal_Print(const char *str);

// 绘图模式
enum DrawMode {
    DRAW_MODE_CPU = 0,      // 实时计算 (省内存，高CPU)
    DRAW_MODE_DMA           // PSRAM 预渲染 (高内存，低CPU)
};

void DRAW_SetMode(DrawMode mode);

// 设置绘制步长 (1-255)
// 较小的值 = 更平滑但点更多
// 较大的值 = 更快但更粗糙
// 默认 8
void DRAW_SetStepSize(uint8_t step);
