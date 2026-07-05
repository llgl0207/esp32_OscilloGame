#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void initTasks();

// AI chat 时暂停/恢复非必要任务
void suspendNonessentialTasks();
void suspendNonessentialTasksKeepGUI();  // 暂停但不挂起 GUI 任务（保持渲染）
void resumeNonessentialTasks();

// DMA 外设释放/恢复（用于 SSL 连接前释放 BLE/SD_MMC 占用的 DMA 连续内存）
void deinitHardwareDMA();
void reinitHardwareDMA();

// Web 控制接口
extern volatile int web_enc_delta;
extern volatile bool web_btn_pressed;
extern volatile int web_game_dir; // -1: 无, 0:上, 1:下, 2:左, 3:右

// Web 坦克控制（网页虚拟摇杆 — 全向模拟值）
extern volatile float web_tank_speed_val;  // -1.0~1.0, 正=前进
extern volatile float web_tank_turn_val;   // -1.0~1.0, 正=右转
extern volatile bool web_tank_fire;        // 开火触发

// Web 乒乓球控制（网页滑块）
extern volatile float web_pong_paddle;     // 0.0~1.0, 球拍 X 坐标比例
extern volatile bool web_pong_active;      // 网页端正在操控球拍


#ifdef __cplusplus
}
#endif