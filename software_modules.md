# 2.3.2 软件各模块介绍

> 本项目为基于 ESP32 的示波器矢量游戏机，整体软件采用分层设计，自上而下依次为：顶层入口模块、硬件驱动模块（DAC 输出模块、矢量绘图模块、音频缓冲模块）、AI 交互模块（语音对话模块、语音控制模块）、应用层模块（任务调度模块、网络管理模块、Web 服务器模块）。以下分别给出各模块的函数级设计说明。

---

## 1 顶层入口模块（main.cpp）

### 1.1 readEncoderISR — 正交编码器中断服务函数

| 项目 | 内容 |
|---|---|
| 功能描述 | GPIO A/B 任一引脚电平变化时触发中断，通过"旧值/新值"查表判断顺时针(CW)、逆时针(CCW)或抖动(bounce)，累加编码器计数值。 |
| 关键输入变量 | `GPIO` 引脚电平（瞬时采样） |
| 关键输出变量 | `volatile int32_t encoderValue`（全局增量） |

### 1.2 setup — 系统初始化函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 依次完成串口、GPIO、ADC、SD 卡、FreeRTOS 任务、DAC 定时器、矢量绘图默认画面、终端文本的初始化，构成系统上电启动流程。 |
| 执行流程 | ① `Serial.begin` → ② `pinMode` + `attachInterrupt(readEncoderISR)` → ③ ADC 初始化 → ④ SD 卡挂载并扫描媒体文件 → ⑤ `initTasks` 创建 joystick / serial / GUI / web 任务 → ⑥ `initDACoutput` 配置 80 kHz 定时器 → ⑦ `DRAW_Init` 生成默认画面 → ⑧ `DRAW_Terminal_Init` + `DRAW_Terminal_Print` 输出启动信息。 |
| 关键输入变量 | `pins.h` 中的引脚宏定义 |
| 关键输出变量 | 各子系统全局状态初始化完成 |

### 1.3 loop — 主循环函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 以 500 ms 周期翻转 LED 指示心跳，并周期性打印摇杆与编码器值用于调试。主业务逻辑由 FreeRTOS 各独立任务承载。 |
| 关键输入变量 | `encoderValue`、ADC 摇杆原始值 |
| 关键输出变量 | GPIO 输出电平、串口文本 |

---

## 2 硬件驱动模块

### 2.1 DAC 输出模块（DACoutput.cpp）

#### 2.1.1 全局状态变量

| 变量 | 类型 | 含义 |
|---|---|---|
| `player_mode` | `volatile int` | 0=矢量，1=音频 2 通道，2=视频 4 通道 |
| `bufA_L / bufA_R / bufA_X / bufA_Y` | `volatile uint16_t *` | 缓冲区 A 各通道指针（PSRAM） |
| `bufB_L / bufB_R / bufB_X / bufB_Y` | `volatile uint16_t *` | 缓冲区 B 各通道指针 |
| `bufA_ready / bufB_ready` | `volatile bool` | 缓冲区是否已填充并可播放 |
| `bufA_count / bufB_count` | `volatile int` | 缓冲区内有效样本数 |
| `playing_A` | `volatile bool` | 当前播放 A 还是 B |
| `play_idx` | `volatile int` | 当前缓冲区内播放位置 |
| `csMask / mosiMask / sclkMask / ldacMask` | `uint32_t` | GPIO 位掩码，供 `sendDAC` 直接位操作 |

#### 2.1.2 initDACoutput — DAC 初始化函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 初始化 HSPI 总线、GPIO 位掩码、PSRAM 双音频缓冲、矢量绘图默认画面及 80 kHz 硬件定时器。 |
| 执行流程 | ① `SPI.begin` 并设置模式与频率 → ② 计算 `csMask/mosiMask/sclkMask/ldacMask` → ③ `Init_Audio_Buffers` 分配 4 通道 × 2 块 PSRAM 缓冲 → ④ `DRAW_Init` 生成默认矢量画面 → ⑤ `hw_timer_t` 配置并 `attachInterrupt(onTimer)` → ⑥ 设置初始频率 80 kHz。 |
| 关键输出变量 | `SPI` 句柄、定时器句柄、`bufA_* / bufB_*` 指针 |

#### 2.1.3 setDACfreq — DAC 频率设置函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 动态切换 DAC 输出频率（矢量模式 80 kHz、音/视频模式 44.1 kHz）。 |
| 执行流程 | ① `timerEnd` 停止当前定时器 → ② `timerAlarmWrite` 写入新分频值 `80000000 / freq` → ③ `timerAlarmEnable` 重新启用。 |
| 关键输入变量 | `uint32_t freq` |
| 关键输出变量 | 定时器硬件寄存器 |

#### 2.1.4 Init_Audio_Buffers — 双缓冲初始化函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 在 PSRAM 中为 L/R/X/Y 四个通道分别申请 A/B 两份样本缓冲。 |
| 执行流程 | 对 (L, R, X, Y) × (A, B) → 使用 `ps_malloc` 申请 `AUDIO_BUF_SIZE × 2` 字节，将返回值写入 `bufA_* / bufB_*`，同时把 `bufA_ready / bufB_ready` 置 `false`、`bufA_count / bufB_count` 清零，并设 `playing_A = true`、`play_idx = 0`。 |
| 关键输出变量 | `bufA_L / bufA_R / bufA_X / bufA_Y / bufB_L / bufB_R / bufB_X / bufB_Y` |

#### 2.1.5 sendDAC — DAC 单样本输出函数（IRAM_ATTR）

| 项目 | 内容 |
|---|---|
| 功能描述 | 通过直接写 GPIO 寄存器方式，将 24 位串行字（8 位配置 + 16 位样本）移位送入 DAC8554 并触发 LDAC 更新。 |
| 执行流程 | ① 拼包 `packet = (configRegister << 16) | (value & 0xFFFF)` → ② CS 拉低 → ③ 循环 `i=23..0` 发送每一位（sclk 拉低 → 按位设置 mosi → sclk 拉高） → ④ CS 拉高 → ⑤ LDAC 低脉冲锁存。 |
| 关键输入变量 | `uint8_t configRegister`（DAC 通道 A/B/C/D）、`uint16_t value`（样本值 0~65535） |
| 关键输出变量 | DAC8554 输出电压（通过 SPI 写硬件完成） |

#### 2.1.6 onTimer — 定时器中断服务函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 核心中断处理函数，按 `player_mode` 分别将矢量坐标、音频样本或视频样本依次送 DAC 输出。 |
| 执行流程 | ① `portENTER_CRITICAL_ISR` 进入临界区 → ② 判断 `player_mode`：<br> **模式 0（矢量）**：调用 `DRAW_GetNextPoint(&x, &y)` → `sendDAC(CH_X, x)` → `sendDAC(CH_Y, y)`；<br> **模式 1（音频）**：若 `playing_A && bufA_ready` 读取 `bufA_L[play_idx]/bufA_R[play_idx]` 送两路 DAC，`play_idx++`，到达 `bufA_count` 时关闭 `bufA_ready`、切换 `playing_A=false`、`play_idx=0`；B 缓冲对称处理；<br> **模式 2（视频）**：同上扩展至 4 通道 L/R/X/Y，其中 X/Y 用于示波器像素扫描。③ `portEXIT_CRITICAL_ISR` 退出。 |
| 关键输入变量 | `player_mode`、`bufA_ready / bufB_ready`、`bufA_count / bufB_count`、`playing_A`、`play_idx`、`vector_draw` 模块提供的点序列 |
| 关键输出变量 | DAC 输出电压（示波器画面 / 扬声器声音） |

#### 2.1.7 双缓冲读写接口函数集

| 函数 | 功能 | 关键输入/输出变量 |
|---|---|---|
| `Is_Buf_A_Free` | 判断缓冲 A 是否空闲，返回 `!bufA_ready` | 输入：无；输出：`bool` |
| `Is_Buf_B_Free` | 判断缓冲 B 是否空闲，返回 `!bufB_ready` | 输入：无；输出：`bool` |
| `Get_Buf_A_L/R/X/Y` | 返回缓冲 A 对应通道指针，供解码器写入 | 输入：无；输出：`uint16_t *` |
| `Get_Buf_B_L/R/X/Y` | 返回缓冲 B 对应通道指针 | 输入：无；输出：`uint16_t *` |
| `Mark_Buf_A_Ready(count)` | 标记缓冲 A 填充完成，`bufA_count=count; bufA_ready=true` | 输入：`int count`；输出：`bufA_count / bufA_ready` |
| `Mark_Buf_B_Ready(count)` | 标记缓冲 B 填充完成 | 输入：`int count`；输出：`bufB_count / bufB_ready` |
| `Set_Player_Mode(mode)` | 切换播放模式并更新 DAC 频率 | 输入：`int mode`；输出：`player_mode` 与定时器分频值 |

### 2.2 矢量绘图模块（vector_draw.cpp）

#### 2.2.1 全局状态变量

| 变量 | 类型 | 含义 |
|---|---|---|
| `currentDrawMode` | `static DrawMode` | `DRAW_MODE_CPU` 实时插值 / `DRAW_MODE_DMA` 预光栅化 |
| `dmaBuffers[2]` | `static uint32_t *` | DMA 双缓冲点数组，存储 `(X<<16) | Y` |
| `dmaBufferCounts[2]` | `static uint32_t` | 各缓冲内有效点数 |
| `activeDmaIdx` | `volatile uint8_t` | 正在被 `DRAW_GetNextPoint` 读取的前台缓冲索引 |
| `backDmaIdx` | `uint8_t` | 正在被 `DRAW_Render` 写入的后台缓冲索引 |
| `dmaReadIndex` | `volatile uint32_t` | ISR 读游标 |
| `drawStepSize` | `static uint8_t` | 线段插值步长，越小越精细 |
| `global_scale_x_pct / global_scale_y_pct` | `static uint16_t` | 全局缩放百分比 |
| `global_offset_x / global_offset_y` | `static int16_t` | 全局偏移量 |
| `Shape pool[] / shape_count` | 对象池 | 存放本帧所有几何/文本对象 |
| `stringSlot_scroll[]` | `static int32_t[]` | 文本跑马灯水平偏移 |
| `term_cursor_y / term_line_height / term_spacing / term_scale_pct` | `static int32_t / uint16_t` | 终端模式行布局参数 |

#### 2.2.2 ASCII 字符图案表

`pattern_A` ~ `pattern_Z`、`pattern_a` ~ `pattern_z`、`pattern_0` ~ `pattern_9` 以及标点符号 `pattern_excl / pattern_apos / pattern_hash / pattern_pct / pattern_caret`。每个字符以若干 `{x0,y0,x1,y1}` 浮点线段表达，坐标系为 8×8；另提供一套大数字专用线段（坐标 1200~3000）用于数码管风格显示。

#### 2.2.3 DRAW_Init — 绘图初始化函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 清空对象池并写入一个 "X" 图案，复位线段插值游标。 |
| 执行流程 | ① `DRAW_Clear` 清空对象池 → ② `DRAW_SetLetter('X')` 用两条对角线段生成默认画面 → ③ `_resetLine` 初始化 CPU 模式当前线段游标。 |
| 关键输出变量 | `shape_count = 0`、对象池首两条线段写入完成 |

#### 2.2.4 DRAW_GetNextPoint — 取下一个 DAC 点函数（ISR 级）

| 项目 | 内容 |
|---|---|
| 功能描述 | 被 `DACoutput::onTimer` 每 12.5 μs 调用一次，依当前模式从 DMA 点表或 CPU 线段插值获取下一个输出坐标。 |
| 执行流程 | **DMA 模式**：若 `dmaBufferCounts[activeDmaIdx]==0`，输出 `(2048,2048)`；否则 `idx = dmaReadIndex % dmaBufferCounts[activeDmaIdx]`，取点 `outX = point >> 16`、`outY = point & 0xFFFF`，`dmaReadIndex++`。<br> **CPU 模式**：在当前线段上按 `drawStepSize` 推进插值，得到 `outX/outY`；线段走完则切换下一对象并调用 `_resetLine`。 |
| 关键输入变量 | `currentDrawMode`、`activeDmaIdx`、`dmaReadIndex`、对象池游标 |
| 关键输出变量 | `uint16_t &outX / outY`（供 DAC 中断消费） |

#### 2.2.5 DRAW_Render — 帧渲染函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 将当前帧对象池内容处理并写入后台 DMA 缓冲，最后与前台缓冲做无锁交换。 |
| 执行流程 | ① `DRAW_DisableScroll` 复位非滚动文本偏移 → ② `backDmaIdx = 1 - activeDmaIdx` → ③ `dmaBufferCounts[backDmaIdx] = 0` 清空后台 → ④ 遍历对象池：<br> 几何对象：矩形/圆展开为线段 → `_processLine` 做 `global_scale / offset` 变换与越界剔除 → CPU 模式直接挂"CPU 对象表"，DMA 模式调用 `_rasterizeLineToDMA` 按 `drawStepSize` 写入 `dmaBuffers[backDmaIdx]`；<br> 文本对象：对每个字符调用 `_getPattern` → 缩放 → 累加 `spacing` 字间距 → 叠加 `stringSlot_scroll[slot]` 水平偏移 → `_processLine` 后追加。<br> ⑤ 原子切换 `activeDmaIdx / backDmaIdx` → ⑥ `dmaReadIndex = 0` → ⑦ `shape_count = 0`。 |
| 关键输入变量 | 对象池内容、`global_scale_x_pct / global_scale_y_pct`、`global_offset_x / global_offset_y`、`drawStepSize`、`stringSlot_scroll[]` |
| 关键输出变量 | `dmaBuffers[activeDmaIdx]` 切换为新前台；`dmaBufferCounts[activeDmaIdx]` 更新 |

#### 2.2.6 DRAW_AddLine / DRAW_AddRect / DRAW_AddCircle — 几何图形绘制函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 上层游戏/GUI 统一调用的几何绘制接口，仅把"逻辑坐标 + 形状类型"写入对象池，真正缩放/裁剪在 `DRAW_Render` 阶段完成，以保证调用开销最小化。 |
| 执行流程 | `shape[shape_count++] = {type, x0, y0, x1, y1}`。 |
| 关键输入变量 | `int32_t x0 / y0 / x1 / y1`、圆参数 `x/y/r` |
| 关键输出变量 | `shape_count`、对象池对应条目 |

#### 2.2.7 DRAW_AddString — 文本绘制函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 把一串文本以矢量字形写入对象池，同时分配可滚动 slot。 |
| 执行流程 | ① `slot = shape_count++` 分配 STRING 槽 → ② 计算 `DRAW_CalcStringWidth(s, spacing, scale_x)` 用于布局/滚动判断 → ③ 写入 `shape[slot] = {STRING, s, spacing, x, y, scale_x, scale_y, slot}` → ④ `return slot`。 |
| 关键输入变量 | `const char *s`、`uint16_t spacing`、`int32_t x/y`、`uint16_t scale_x/scale_y` |
| 关键输出变量 | `int16_t slot`（返回给调用方以控制滚动） |

#### 2.2.8 DRAW_Update — 文本滚动更新函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 对每帧文本槽做"宽度超过屏幕则左移"处理，形成跑马灯效果。 |
| 执行流程 | 对每个 STRING 槽：若文本计算宽度 > 屏幕宽，`stringSlot_scroll[slot] -= 2`；若整串已移出左边界，复位偏移循环。 |
| 关键输入变量 | `DRAW_CalcStringWidth` 返回值 |
| 关键输出变量 | `stringSlot_scroll[]` |

#### 2.2.9 DRAW_SetTextScroll / DRAW_GetTextScroll — 滚动控制函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 对外暴露文本 slot 的滚动偏移，供上层（如菜单左右循环、AI 回复超长行滚动）主动控制。 |
| 关键输入变量 | `int16_t slot`、`int32_t scroll` |
| 关键输出变量 | `stringSlot_scroll[slot]` |

#### 2.2.10 DRAW_Terminal_Init / DRAW_Terminal_Print — 终端模式函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 提供"追加式文本行"显示，用于启动信息、AI 回复对话。 |
| 执行流程 | **Init**：设置 `term_scale_pct / term_spacing / term_line_height / term_cursor_y`。<br> **Print**：调用 `DRAW_AddString(str, …)` 以 `term_cursor_y` 为 Y 起点居中显示；随后 `term_cursor_y -= term_line_height`，若光标超出顶部则回卷至初始行。 |
| 关键输入变量 | `const char *str` |
| 关键输出变量 | 矢量画面新增文本行 |

#### 2.2.11 DRAW_SetMode / DRAW_SetStepSize — 模式与步长设置函数

| 函数 | 功能 | 输入/输出 |
|---|---|---|
| `DRAW_SetMode(DrawMode)` | 切换 CPU/DMA 模式并在 DMA 时分配 PSRAM 缓冲 | 输入 `DRAW_MODE_CPU / DRAW_MODE_DMA`；输出 `dmaBuffers / dmaBufferCapacity` |
| `DRAW_SetStepSize(uint8_t)` | 设置线段插值步长，控制画面精细度 | 输入步长值（1~255，默认 16）；输出 `drawStepSize` |
| `DRAW_SetScale/Offset` | 设置全局缩放与偏移 | 输入 `scale_x_pct / scale_y_pct`、`offset_x / offset_y`；输出 `global_scale_*` 与 `global_offset_*` |

### 2.3 音频缓冲模块（audio_common.cpp）

本模块作为 DAC 双缓冲机制的备选共享通道，主要对外暴露以下变量：

| 变量 | 类型 | 含义 |
|---|---|---|
| `audioBuffer[]` | `uint16_t` | 音频样本环形缓冲 |
| `bufferHead` | `volatile int` | 写入游标，供解码器/录音任务写入 |
| `bufferTail` | `volatile int` | 读取游标，供播放任务消费 |

说明：当前 DAC 播放采用 DACoutput 内部双缓冲实现，本环形缓冲保留作为"解码/播放解耦"扩展入口，便于将来接入更多音频源。

---

## 3 AI 交互模块

### 3.1 语音对话模块（ai_chat.cpp）

#### 3.1.1 全局状态变量

| 变量 | 类型 | 含义 |
|---|---|---|
| `ai_chat_active` | `volatile bool` | 对话任务是否正在运行（防止重复启动） |
| `ai_chat_activity_time` | `volatile unsigned long` | 最后一次活动时间戳，用于看门狗扩展 |
| `ai_chat_phase` | `volatile AIChatPhase` | `AI_PHASE_WAITING / THINKING / REPLY` 三阶段，驱动屏幕画面 |
| `ai_chat_display_text[512]` | `char []` | 当前要显示的文本内容 |
| `ai_chat_dirty` | `volatile bool` | 是否需要刷新画面的标志 |
| `baidu_token[256]` | `static char []` | 百度 OAuth token 缓存 |
| `token_expires` | `static unsigned long` | token 到期 unix 秒 |

#### 3.1.2 AI_Chat_Start — 启动 AI 对话任务

| 项目 | 内容 |
|---|---|
| 功能描述 | 由 GUI 选择"AI 对话"时调用，防止重复启动，并在 Core 0 上拉起 `ai_chat_task`。 |
| 执行流程 | ① 若 `ai_chat_active == true` 直接返回 → ② 置 `ai_chat_active = true` → ③ `xTaskCreatePinnedToCore(ai_chat_task, "ai_chat", 16*1024, NULL, 2, NULL, 0)`。 |
| 关键输出变量 | `ai_chat_active`、新创建 FreeRTOS 任务句柄 |

#### 3.1.3 wifi_connect — WiFi 连接函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 切换至 STA 模式并连接用户配置的 WiFi，最长等待 20 s。 |
| 执行流程 | ① `WiFi.mode(WIFI_STA)` → ② `WiFi.begin(SSID, PASSWORD)` → ③ 在循环中 `vTaskDelay(300)` 并检查 `WiFi.status() == WL_CONNECTED` → ④ 成功返回 `true` 否则 `false`。 |
| 关键输入变量 | 宏 `AI_WIFI_SSID / AI_WIFI_PASS` |
| 关键输出变量 | WiFi 连接状态 |

#### 3.1.4 baidu_get_token — 百度 ASR token 获取函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 访问百度 OAuth 接口获取 token，并缓存以避免重复请求。 |
| 执行流程 | ① 若 `token_expires > now` 直接返回 `true` → ② HTTPClient GET `https://aip.baidubce.com/oauth/2.0/token?grant_type=...` → ③ 解析 JSON，将 `access_token` 写入 `baidu_token`，`expires_in` 换算为 unix 秒写入 `token_expires`。 |
| 关键输入变量 | 百度 API Key / Secret Key |
| 关键输出变量 | `baidu_token`、`token_expires` |

#### 3.1.5 baidu_asr — 百度语音识别函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 将 16 kHz 单声道 16-bit PCM 音频上传至百度 ASR 接口，返回识别文本。 |
| 执行流程 | POST 请求体 `{format:"pcm", rate:16000, channel:1, token:baidu_token, cuid:"esp32", speech:base64(pcm), len:samples*2}` → 响应 `result[0]` 作为识别文本返回。 |
| 关键输入变量 | `const int16_t *pcm`、`size_t samples`、`baidu_token` |
| 关键输出变量 | `String user_text` |

#### 3.1.6 deepseek_chat_stream — DeepSeek 流式对话函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 使用 HTTPS 向 DeepSeek `/chat/completions` 发送 `stream:true` 请求，逐块接收 SSE，边解析边刷新屏幕显示，最终返回完整回复。 |
| 执行流程 | ① HTTPS 连接 `api.deepseek.com` → ② POST body：`{model:"deepseek-chat", messages:[...system prompt... ...user text...], stream:true}` → ③ 解析响应头，按 `Transfer-Encoding: chunked` 读取 body → ④ 逐行解析：若以 `data: ` 开头则截取 payload；若等于 `[DONE]` 则结束；否则 JSON 解析得到 `choices[0].delta.content`，累加入 `accumulated` 并调用 `sse_process_line` 实时刷新 → ⑤ HTTPClient 关闭。 |
| 关键输入变量 | `const String &text`（ASR 文本）、`ai_prompt.h` 系统 prompt |
| 关键输出变量 | `String reply_text`、过程中 `ai_chat_display_text` 被持续更新 |

#### 3.1.7 sse_process_line — SSE 单行处理函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 对 DeepSeek 返回的单个 `data: ...` 行执行解析并更新累积文本。 |
| 执行流程 | ① 去除前导 `data: ` → ② 若等于 `[DONE]` 返回 `false` 指示流结束 → ③ JSON 解析 `choices[0].delta.content` → ④ 若存在则 `accumulated += content` 并调用 `ai_show(AI_PHASE_REPLY, accumulated.c_str())` 瞬时刷新屏幕 → 返回 `true`。 |
| 关键输入变量 | `const String &line` |
| 关键输出变量 | `String &accumulated`（引用传递）、`ai_chat_display_text / ai_chat_dirty` |

#### 3.1.8 ai_show — 线程安全屏幕刷新函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 对 `ai_chat_phase / ai_chat_display_text / ai_chat_dirty` 做临界区保护式写入，并同步更新 Web UI 状态监视器。 |
| 执行流程 | ① `portENTER_CRITICAL` → ② `ai_chat_phase = phase` → ③ `strncpy(ai_chat_display_text, text, sizeof-1)` 并以 `\0` 结尾 → ④ `ai_chat_dirty = true` → ⑤ `portEXIT_CRITICAL` → ⑥ 调用 `updateWebUIStatus(text)`。 |
| 关键输入变量 | `AIChatPhase phase`、`const char *text` |
| 关键输出变量 | `ai_chat_phase / ai_chat_display_text / ai_chat_dirty`、Web 端 `current_ui_status` |

#### 3.1.9 ai_chat_task — AI 对话主循环

| 项目 | 内容 |
|---|---|
| 功能描述 | 整体承载"暂停其他任务 → 录音 → 识别 → 对话 → 动作解析 → 恢复任务"的完整链路。 |
| 执行流程 | ① `suspendNonessentialTasks()` 暂停 joystick/serial/loop 任务 → ② `deinitHardwareDMA()` 释放 BLE DMA → ③ `Network_Manager::suspend_esp_now()` 暂停 ESP-NOW → ④ `wifi_connect()` 失败则显示"WiFi FAIL"并 goto 清理 → ⑤ 循环：<br> `ai_show(WAITING, "按住 OK 录音 / 长按退出")` → 按键检测（短按录音 3 秒至 PSRAM int16_t 数组；长按 break）→ `ai_show(THINKING, "识别中…")` → `baidu_asr`；失败则 `ai_show(REPLY, "ASR FAIL")` continue → `ai_show(THINKING, "思考中…")` → `deepseek_chat_stream`；失败则 `ai_show(REPLY, "DEEPSEEK FAIL")` continue → 调用 `VC_ParseReply(reply_text)` 解析 JSON → `ai_show(REPLY, reply_json)` → 若 `voice_pending == true` break 把动作交给 GUI 消费。<br> ⑥ CLEANUP：`Network_Manager::resume_esp_now()` → `reinitHardwareDMA()` → `resumeNonessentialTasks()` → `ai_chat_active = false` → `vTaskDelete(NULL)`。 |
| 关键输入变量 | 用户按键、I2S 音频流、`baidu_token`、DeepSeek API Key |
| 关键输出变量 | 屏幕显示文本、`voice_action / voice_pending`（供 GUI 消费） |

### 3.2 语音控制模块（voice_control.cpp）

#### 3.2.1 全局状态变量

| 变量 | 类型 | 含义 |
|---|---|---|
| `voice_action` | `volatile VC_Action` | 最近一次识别得到的动作枚举值 |
| `voice_pending` | `volatile bool` | GUI 是否有待执行动作 |

VC_Action 枚举包含：`VC_NONE / VC_OPEN_MUSIC / VC_OPEN_VIDEO / VC_OPEN_GAMES / VC_OPEN_ONLINE / VC_OPEN_GAME_JOY / VC_OPEN_AI_CHAT / VC_OPEN_ABOUT / VC_START_SNAKE / VC_START_BREAKOUT / VC_START_FLAPPY / VC_START_RACING / VC_START_RUNTINY / VC_START_TANK / VC_BACK / VC_EXIT`。

#### 3.2.2 str_to_action — 动作字符串映射函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 将 DeepSeek JSON 中的 `action` 文本字段映射为 `VC_Action`。 |
| 执行流程 | 以 `if-else` 串比较 `"start_snake"/"start_breakout"/"start_flappy"/...` 并返回对应枚举值，默认返回 `VC_NONE`。 |
| 关键输入变量 | `const String &action` |
| 关键输出变量 | `VC_Action` |

#### 3.2.3 extract_json_object — JSON 片段抽取函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 对 DeepSeek 可能返回的"纯文本包裹 ```json ``` "形式进行清洗，抽取首个 `{ ... }` 完整 JSON 块。 |
| 执行流程 | ① 去除所有 ` ```json ` 与 ` ``` ` 标记 → ② 找到第一个 `{` 和最后一个 `}` → ③ 返回 `substr` 子串；若任一端不存在则返回空串。 |
| 关键输入变量 | `const String &raw` |
| 关键输出变量 | `String json_str` |

#### 3.2.4 VC_ParseReply — 对外主解析函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 接受 DeepSeek 回复文本，分离"可显示 reply"与"动作 action"。 |
| 执行流程 | ① `json_str = extract_json_object(json_reply)` → ② 若 `json_str` 为空，`voice_action=VC_NONE; voice_pending=false; return json_reply` 按纯文本原样显示 → ③ JSON 解析为对象 → ④ `reply = obj["reply"]`、`action = obj["action"]` → ⑤ `voice_action = str_to_action(action)` → ⑥ `voice_pending = (voice_action != VC_NONE)` → ⑦ `return reply`。 |
| 关键输入变量 | `const String &json_reply` |
| 关键输出变量 | 返回值 `String reply`、副作用变量 `voice_action / voice_pending` |
| GUI 消费约定 | `guiTask` 每帧检查 `voice_pending`，为 `true` 时执行对应 UI 跳转或启动对应游戏，完成后 `voice_pending=false; voice_action=VC_NONE`。 |

---

## 4 应用层模块（概要）

### 4.1 任务调度模块（freertos.cpp）

#### 4.1.1 任务表

| 任务 | 亲和核心 | 功能 |
|---|---|---|
| `guiTask` | Core 1 | 菜单状态机、游戏帧循环、AI 对话等待画面的渲染调度 |
| `joystickCheckTask` | Core 1 | 周期性读取 USB 手柄，更新 `Gamepad.x / y / buttons` |
| `serialOutputTask` | Core 1 | 可选调试串口打印 |
| `webServerTask` | Core 0 | SoftAP HTTP 服务，在 `initWebServer` 中拉起 |

#### 4.1.2 工具函数集

| 函数 | 流程 |
|---|---|
| `suspendNonessentialTasks` | `vTaskSuspend` joystick / serial / loop 任务 |
| `resumeNonessentialTasks` | 对应 `vTaskResume` |
| `deinitHardwareDMA` | `delete bleMouse; NimBLEDevice::deinit` |
| `reinitHardwareDMA` | 反向恢复，SD 卡不重复初始化 |
| `initTasks` | `xTaskCreatePinnedToCore` 创建上述三个 Core 1 任务，并调用 `initWebServer` 在 Core 0 拉起 Web 任务 |

#### 4.1.3 Web→游戏数据通道（关键全局变量）

| 变量 | 类型 | 写入者 | 读取者 |
|---|---|---|---|
| `web_enc_delta` | `volatile int` | `web_server::handleUp/Down/Left/Right` | `guiTask` 菜单上下 |
| `web_btn_pressed` | `volatile bool` | `web_server::handleEnter` | `guiTask` 菜单确认 |
| `web_game_dir` | `volatile int` | `web_server::handleUp/Down/Left/Right` | `Update_Snake_Game` 方向输入 |
| `web_tank_speed_val / web_tank_turn_val` | `volatile float` | `web_server::handleTankJoy` | `Update_Tank_Game` |
| `web_tank_fire` | `volatile bool` | `web_server::handleTankFire` | `Update_Tank_Game` |
| `web_pong_paddle` | `volatile float [0..1]` | `web_server::handlePongMove` | `Update_Pong_Game` |
| `web_pong_active` | `volatile bool` | `web_server::handlePongMove` | `Update_Pong_Game`（优先使用网页值） |

#### 4.1.4 游戏逻辑函数（Update_*）

全部游戏函数遵循统一执行模式：**每帧（20–50 ms）读取输入 → 更新物理 → 碰撞检测 → 写入 `vector_draw` 对象 → 由 DAC 中断扫描渲染**。各函数关键输入输出如下：

| 函数 | 输入 | 输出与副作用 |
|---|---|---|
| `Init_Snake_Game` | 无 | `snake_len=3`、`snake_dir=右`、`snake_food` 随机位置、`game_score=0`、`game_over=0` |
| `Update_Snake_Game` | `web_game_dir`、编码器方向、按键方向（优先级 web>编码器>按键） | 移动蛇头；撞墙/自撞 `game_over=1`；吃到食物 `snake_len++/score++`；向 `vector_draw` 写蛇身段、食物、得分文本 |
| `Init_Breakout_Game` | 无 | 挡板居中，球置初始速度，`brk_bricks[][]` 全满，`brk_score=0`、`brk_lives=3` |
| `Update_Breakout_Game(encoder_delta)` | `encoder_delta` | 挡板水平跟随，球与左右墙、顶部、挡板、砖块碰撞并反弹；砖块碰撞时 `brk_bricks[r][c]=0` 且 `score++`；球落底 `lives--`，到 0 时 `brk_game_over=1` |
| `Init_Flappy_Game` | 无 | 玩家位置复位，障碍物随机，`flp_score=0`、`flp_game_over=0` |
| `Update_Flappy_Game(jump_requested)` | 跳跃请求（按键或 Web 按钮） | 跳跃时 `vy=-JUMP_SPEED`，否则 `vy+=GRAVITY`；碰天花板/地板 `game_over=1`；障碍物左移，穿过时得分；AABB 碰撞检测 |
| `Init_Racing_Game` | 无 | 赛车居中，障碍物随机分布，`race_score=0`、`race_game_over=0` |
| `Update_Racing_Game(encoder_delta)` | `encoder_delta` | 赛车水平移动并限幅在赛道内；障碍物向下漂移；碰撞 `race_game_over=1`；穿过障碍物得分 |
| `Init_RunTiny_Game` | 无 | 玩家、障碍物位置复位 |
| `Update_RunTiny_Game(jump_requested)` | 跳跃请求 | 重力物理 + 障碍物右→左移动；AABB 碰撞结束游戏 |
| `Init_Tank_Game` | 网络状态 + 随机种子 | 地图随机生成、坦克出生点、子弹列表清空、记录摇杆是否可用 |
| `Check_Tank_Collision(x,y)` | 坦克新坐标 | 返回是否与地图对象（墙/水）重叠 |
| `Check_Bullet_Collision(bullet)` | 子弹对象 | 墙壁反弹（按入射法向翻转速度分量），超过最大反弹次数消失 |
| `Update_Tank_Game` | 手柄/`web_tank_speed_val/turn_val/fire`、`remote_tank_data` | 位置/旋转更新 + 碰撞回退；子弹生命周期管理 + 自身命中检测；调用 `sendGameData(my_tank)` 发送至对端；绘制两方坦克与子弹 |
| `Init_Pong_Game(seed, is_initiator)` | 随机种子、是否为发起方 | 球速随机；主机/从机模式标记；球拍初始位置 |
| `Update_Pong_Game(enc_delta)` | `enc_delta` / `web_pong_paddle` / `web_pong_active` / `remote_pong_data` | 主机做物理模拟（左右墙反弹、球拍击中位置影响反弹 vx、上下边界失分），主机广播 `PongData{ball, paddle1, paddle2, score1, score2}`，客户机仅上传球拍位置 |

### 4.2 网络管理模块（network_manager.cpp）

#### 4.2.1 状态机与消息结构

状态机：`NET_IDLE → NET_DISCOVERING → NET_PAIRING → NET_CONNECTED → NET_IN_GAME`。

`NetMessage` 为固定大小报文：

```
type                uint8_t   (MSG_DISCOVERY / PAIR_REQUEST / PAIR_ACCEPT / DATA / START_GAME / GAME_DATA / END_GAME)
src_mac[6]          uint8_t   源 MAC
name[16]            char      设备名
union payload:
    TankData        float x,y,angle; uint8_t bullet_count; {x,y} bullets[5]
    PongData        float ball_x/ball_y, ball_vx/ball_vy, paddle1_x, paddle2_x; uint8_t score1/score2
    start_req       uint8_t game_id; uint32_t seed
    end_req         uint8_t reason
```

#### 4.2.2 核心函数集

| 函数 | 流程 | 关键输入/输出 |
|---|---|---|
| `Network_Manager::init` | WiFi AP+STA 模式 → `esp_now_init` → 注册 `OnDataSent / OnDataRecv` 回调 | 输入：无；输出：ESP-NOW 协议栈启动 |
| `Network_Manager::disable` | 反初始化 ESP-NOW 并关闭 WiFi | 输入：无；输出：网络关闭 |
| `Network_Manager::update` | 每 500 ms 广播 `MSG_DISCOVERY`；维护 `discovered_peers`（3 s 超时剔除） | 输入：当前时间；输出：`discovered_peers` 列表 |
| `Network_Manager::pair(target_mac)` | 发送 `MSG_PAIR_REQUEST` → 等待对方 `MSG_PAIR_ACCEPT` → 进入 `NET_CONNECTED` | 输入：`uint8_t *target_mac`；输出：`connected_peer_mac` |
| `Network_Manager::disconnect` | 发送 `MSG_DISCONNECT` → `esp_now_del_peer` → 回到 `NET_IDLE` | 输入：无；输出：配对状态清空 |
| `Network_Manager::startGame(gameId, seed)` | 发送 `MSG_START_GAME{gameId, seed}` → 本地进入 `NET_IN_GAME` | 输入：`uint8_t gameId / uint32_t seed`；输出：`active_game_id` |
| `Network_Manager::sendGameData(TankData/PongData)` | 周期发送 `MSG_GAME_DATA` 给对端 | 输入：游戏状态结构体；输出：对端更新 |
| `Network_Manager::endGame(reason)` | 发送 `MSG_END_GAME{reason}` → 退出 `NET_IN_GAME` | 输入：`uint8_t reason` |
| `Network_Manager::hasGameRequest(&id, &seed)` | GUI 轮询对端游戏邀请 | 输出：`gameId / seed` 是否有值 |
| `Network_Manager::clearGameRequest` | 清除邀请标志 | 输出：`game_request_pending = false` |
| `Network_Manager::getRemoteGameData(TankData/PongData*)` | 取最近一次接收的远程状态 | 输出：`remote_tank_data / remote_pong_data` |
| `Network_Manager::isRemoteGameEnded(&reason)` | 对端退出检测 | 输出：`remote_game_ended / reason` |
| `Network_Manager::clearRemoteGameEnded` | 清除退出标志 | 输出：`remote_game_ended = false` |
| `Network_Manager::suspend_esp_now / resume_esp_now` | AI 对话期间暂停 / 恢复 ESP-NOW，避免与 WiFi STA 冲突 | 输出：ESP-NOW 注册 / 反注册 |

#### 4.2.3 OnDataRecv — 接收回调消息分发

| 消息类型 | 处理逻辑 |
|---|---|
| `MSG_DISCOVERY` | 把对方加入 `discovered_peers` 或刷新 `last_seen` |
| `MSG_PAIR_REQUEST` | 若未配对，回复 `MSG_PAIR_ACCEPT` 并进入 `NET_CONNECTED` |
| `MSG_PAIR_ACCEPT` | 进入 `NET_CONNECTED` |
| `MSG_START_GAME` | 置 `game_request_pending = true`，存储 `game_id / seed` |
| `MSG_GAME_DATA` | 写入 `remote_tank_data / remote_pong_data`，供游戏循环绘制/判定 |
| `MSG_END_GAME` | `remote_game_ended = true; remote_game_end_reason = reason` |
| `MSG_DISCONNECT` | 清空 `connected_peer_mac`，回到 `NET_IDLE` |

### 4.3 Web 服务器模块（web_server.cpp）

#### 4.3.1 URL 路由表

| 路径 | 函数 | 功能 |
|---|---|---|
| `/` | `handleRoot` | 返回主菜单 HTML 控制页（Up / Down / Left / Right / Enter） |
| `/status` | `handleStatus` | 返回当前 UI 状态 JSON（来自 `updateWebUIStatus` 写入的 `current_ui_status`） |
| `/up / /down / /left / /right` | `handleUp / handleDown / handleLeft / handleRight` | 写入 `web_enc_delta` 与 `web_game_dir` |
| `/enter` | `handleEnter` | 置位 `web_btn_pressed` |
| `/tank` | `handleTankPage` | 返回坦克 HTML 控制页（全向摇杆 + 开火按钮） |
| `/tankJoy?x=..&y=..` | `handleTankJoy` | 写入 `web_tank_speed_val / web_tank_turn_val` |
| `/tankFire` | `handleTankFire` | 置位 `web_tank_fire` |
| `/tankStatus` | `handleTankStatus` | 返回坦克输入 JSON 快照，供网页 JS 轮询显示 |
| `/pong` | `handlePongPage` | 返回乒乓球 HTML 控制页（横向触摸滑块） |
| `/pongMove?pos=0.3` | `handlePongMove` | 写入 `web_pong_paddle = constrain(atof("pos"),0,1)`；置位 `web_pong_active` |
| `/pongStatus` | `handlePongStatus` | 返回球拍位置 JSON 快照 |

#### 4.3.2 核心函数集

| 函数 | 流程 | 关键输入/输出 |
|---|---|---|
| `updateWebUIStatus(status)` | 写全局 `current_ui_status`，供 `/status` 返回 | 输入：`String status`；输出：`current_ui_status` |
| `handleTankJoy` | `x = atof(arg("x")); y = atof(arg("y")); web_tank_speed_val = -y; web_tank_turn_val = x;` 返回 OK JSON | 输入：URL 查询参数；输出：`web_tank_speed_val / web_tank_turn_val` |
| `handlePongMove` | `web_pong_paddle = constrain(atof("pos"),0,1); web_pong_active = true` | 输入：URL 查询参数；输出：`web_pong_paddle / web_pong_active` |
| `webServerTask` | `WiFi.softAP(SSID, PASS)` → `WebServer server(80)` → `server.on(...)` 注册全部路由 → `server.begin()` → 循环 `server.handleClient()` | 输入：无；输出：HTTP 服务在 Core 0 运行 |
| `initWebServer` | `xTaskCreatePinnedToCore(webServerTask, ..., 0)` 在 Core 0 拉起 Web 任务 | 输出：`webServerTask` 任务句柄 |
| `suspendWebServer / resumeWebServer` | `vTaskSuspend / vTaskResume` 挂起 Web 任务 | 输出：Web 服务可用状态 |

---

## 5 顶层控制流总结

```
用户输入（手柄/网页按钮/编码器）
    └── freertos.cpp（guiTask / Update_*）
         ├── 菜单导航：读取 web_enc_delta / web_btn_pressed / encoderValue
         ├── 进入"AI 对话"分支：
         │     ai_chat.cpp::AI_Chat_Start → wifi_connect → baidu_get_token
         │     INMP441 录音 → baidu_asr → deepseek_chat_stream(SSE)
         │     voice_control.cpp::VC_ParseReply → voice_action / voice_pending
         │     guiTask 下一帧消费 voice_pending
         └── 进入游戏分支：Update_*_Game()
               network_manager.cpp::sendGameData / OnDataRecv（远程对战）
               vector_draw.cpp::DRAW_AddLine/Rect/Circle/String → DRAW_Render
                    DACoutput.cpp::onTimer（80 kHz）
                         sendDAC → DAC8554 → 示波器显示 / 扬声器发声
```