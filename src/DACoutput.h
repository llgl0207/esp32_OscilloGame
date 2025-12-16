#pragma once
#include <Arduino.h>

void initDACoutput();
void IRAM_ATTR sendDAC(uint8_t configRegister, uint16_t value);

// 设置 DAC 输出频率 (Hz)
// 默认约 80000Hz
void setDACFreq(uint32_t freq);
