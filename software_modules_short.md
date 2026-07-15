# 2.3.2 软件各模块介绍

按"顶层入口 → 硬件驱动 → AI 交互 → 应用层"顺序，给出核心函数的功能、流程与关键变量。

---

## 1 顶层入口模块（main.cpp）

### 1.1 setup — 系统初始化函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 依次完成串口、GPIO、ADC、SD 卡、FreeRTOS 任务、DAC 定时器、矢量绘图默认画面、终端文本的初始化，构成系统上电启动流程。 |
| 执行流程 | ① Serial.begin → ② pinMode + attachInterrupt(readEncoderISR) 注册编码器中断 → ③ ADC 初始化 → ④ SD 卡挂载并扫描媒体文件 → ⑤ initTasks 创建 joystick / serial / GUI / web 任务 → ⑥ initDACoutput 配置 80 kHz 定时器 → ⑦ DRAW_Init 生成默认矢量画面 → ⑧ DRAW_Terminal_Init + DRAW_Terminal_Print 输出启动信息。 |
| 关键输入变量 | pins.h 中的引脚宏定义 |
| 关键输出变量 | 各子系统全局状态初始化完成 |

### 1.2 readEncoderISR — 正交编码器中断服务函数

| 项目 | 内容 |
|---|---|
| 功能描述 | GPIO A/B 任一引脚电平变化时触发中断，通过"旧值/新值"查表判断顺时针、逆时针或抖动，累加编码器计数值。 |
| 关键输入变量 | GPIO 引脚电平（瞬时采样） |
| 关键输出变量 | volatile int32_t encoderValue |

---

## 2 硬件驱动模块

### 2.1 DAC 输出模块（DACoutput.cpp）

#### 2.1.1 全局状态变量

| 变量 | 类型 | 含义 |
|---|---|---|
| player_mode | volatile int | 0=矢量，1=音频 2 通道，2=视频 4 通道 |
| bufA_ready / bufB_ready | volatile bool | 双缓冲 A/B 是否已填充并可播放 |
| bufA_count / bufB_count | volatile int | 缓冲区内有效样本数 |
| playing_A / play_idx | volatile bool / int | 当前播放 A/B 及播放位置游标 |

#### 2.1.2 initDACoutput — DAC 初始化函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 初始化 HSPI 总线、PSRAM 双音频缓冲、矢量绘图默认画面及 80 kHz 硬件定时器。 |
| 执行流程 | ① SPI.begin 并设置模式与频率 → ② 计算 GPIO 位掩码 csMask/mosiMask/sclkMask/ldacMask → ③ Init_Audio_Buffers 分配 4 通道 x 2 块 PSRAM 缓冲 → ④ DRAW_Init → ⑤ 配置并启动 hw_timer_t，attachInterrupt(onTimer)。 |
| 关键输出变量 | SPI 句柄、定时器句柄、bufA_* / bufB_*、player_mode = 0 |

#### 2.1.3 onTimer — 定时器中断服务函数（核心）

| 项目 | 内容 |
|---|---|
| 功能描述 | 约 80 kHz 触发一次，按当前 player_mode 分别输出矢量坐标、音频样本或视频样本至 DAC8554，是画面/声音生成的时间基准。 |
| 执行流程 | ① 进入临界区 → ② 模式判断：矢量(0)调用 DRAW_GetNextPoint(&x,&y)，对 X/Y 通道分别 sendDAC；音频(1)若 bufA_ready 则从 bufA_L/bufA_R[play_idx] 读取并 sendDAC，play_idx++，到 bufA_count 时切换 B 缓冲并复位；视频(2)同理扩展至 4 通道 L/R/X/Y，其中 X/Y 用于示波器像素扫描。③ 退出临界区。 |
| 关键输入变量 | player_mode、bufA_ready / bufB_ready、bufA_count / bufB_count、playing_A / play_idx、vector_draw 点序列 |
| 关键输出变量 | DAC8554 输出电压（示波器画面 / 扬声器声音） |

#### 2.1.4 sendDAC — DAC 单样本输出函数（IRAM_ATTR）

| 项目 | 内容 |
|---|---|
| 功能描述 | 通过直接写 GPIO 寄存器方式，将 24 位串行字（8 位配置 + 16 位样本）移位送入 DAC8554 并触发 LDAC 同步更新，避免 Arduino digitalWrite 开销以满足 80 kHz 时序。 |
| 关键输入变量 | uint8_t configRegister（通道 A/B/C/D）、uint16_t value（0~65535） |
| 关键输出变量 | DAC8554 输出电压 |

### 2.2 矢量绘图模块（vector_draw.cpp）

#### 2.2.1 全局状态变量

| 变量 | 含义 |
|---|---|
| currentDrawMode | DRAW_MODE_CPU 实时插值 / DRAW_MODE_DMA 预光栅化 |
| dmaBuffers[2] / dmaBufferCounts[2] | DMA 双缓冲点数组与各缓冲点数，点以 (X<<16) | Y 压缩存储 |
| activeDmaIdx / backDmaIdx / dmaReadIndex | 前台缓冲索引、后台缓冲索引、ISR 读游标 |
| drawStepSize | 线段插值步长，越小画面越精细 |
| global_scale_x_pct / global_scale_y_pct | 全局缩放百分比 |
| global_offset_x / global_offset_y | 全局偏移量 |
| Shape pool[] / shape_count | 对象池，存放本帧几何/文本对象 |

#### 2.2.2 DRAW_Render — 帧渲染函数（核心）

| 项目 | 内容 |
|---|---|
| 功能描述 | 将对象池内容处理并光栅化写入后台 DMA 缓冲，完成后与前台缓冲做无锁交换，是"游戏逻辑绘制调用 → DAC 中断消费"的同步点。 |
| 执行流程 | ① backDmaIdx = 1 - activeDmaIdx → ② 清空后台缓冲计数 → ③ 遍历对象池：几何对象按 drawStepSize 光栅化为点写入 dmaBuffers[backDmaIdx]；文本对象逐字符调用 _getPattern 并叠加跑马灯偏移后追加。④ 原子交换 activeDmaIdx / backDmaIdx → ⑤ dmaReadIndex = 0 → ⑥ shape_count = 0。 |
| 关键输入变量 | 对象池、global_scale_*、global_offset_*、drawStepSize |
| 关键输出变量 | dmaBuffers[activeDmaIdx]、dmaBufferCounts[activeDmaIdx] |

#### 2.2.3 DRAW_GetNextPoint — 取下一个 DAC 点函数（ISR 级）

| 项目 | 内容 |
|---|---|
| 功能描述 | 被 DACoutput::onTimer 每 12.5 us 调用一次，依 activeDmaIdx 从 DMA 点表读取下一个 (X,Y) 坐标。 |
| 执行流程 | ① 若 dmaBufferCounts[activeDmaIdx]==0，输出 (2048,2048) 作为中心回零 → ② idx = dmaReadIndex % dmaBufferCounts[activeDmaIdx] 环形读取 → ③ 解压缩 outX = point >> 16; outY = point & 0xFFFF → ④ dmaReadIndex++。 |
| 关键输入变量 | activeDmaIdx、dmaReadIndex、dmaBufferCounts[2] |
| 关键输出变量 | uint16_t &outX / outY |

#### 2.2.4 DRAW_AddLine / DRAW_AddRect / DRAW_AddCircle / DRAW_AddString — 统一绘制接口

| 项目 | 内容 |
|---|---|
| 功能描述 | 上层游戏/GUI 统一调用的几何/文本绘制 API，仅把"逻辑坐标 + 类型"写入对象池，真正缩放与裁剪延迟到 DRAW_Render 阶段，保证每帧多次调用仍为 O(1) 复杂度。 |
| 关键输入变量 | int32_t x0/y0/x1/y1（线段）、int32_t x/y/w/h（矩形）、int32_t x/y/r（圆）、const char *s + spacing/scale（文本） |
| 关键输出变量 | shape_count、对象池对应条目 |

---

## 3 AI 交互模块

### 3.1 语音对话模块（ai_chat.cpp）

#### 3.1.1 全局状态变量

| 变量 | 含义 |
|---|---|
| ai_chat_active | 对话任务是否运行（防重复启动） |
| ai_chat_phase | WAITING / THINKING / REPLY 三阶段显示状态 |
| ai_chat_display_text[512] | 当前要显示的文本内容 |
| ai_chat_dirty | 是否需要刷新画面 |
| baidu_token / token_expires | 百度 OAuth token 及其到期时间 |

#### 3.1.2 ai_chat_task — AI 对话主循环（核心）

| 项目 | 内容 |
|---|---|
| 功能描述 | 在 Core 0 独立运行，承载"暂停其他任务 → 按键录音 → 百度 ASR → DeepSeek 流式对话 → 动作解析 → GUI 执行动作 → 恢复任务"的完整链路，并在屏幕上持续刷新显示状态。 |
| 执行流程 | ① suspendNonessentialTasks 暂停 joystick / serial / loop 任务 → ② deinitHardwareDMA 释放 BLE DMA 以腾出 WiFi 资源 → ③ Network_Manager::suspend_esp_now 暂停 ESP-NOW → ④ wifi_connect，失败则显示 WiFi FAIL 并 goto 清理 → ⑤ 循环：ai_show(WAITING) → 按键检测（短按录音 3 秒存入 PSRAM int16_t 数组；长按 break 退出对话）→ ai_show(THINKING) → baidu_asr；失败 ASR FAIL continue → deepseek_chat_stream；失败 DEEPSEEK FAIL continue → VC_ParseReply(reply_text) 解析 JSON → ai_show(REPLY, reply_json) → 若 voice_pending == true break 把动作交给 GUI 消费。⑥ CLEANUP：恢复网络与任务、ai_chat_active = false、vTaskDelete(NULL)。 |
| 关键输入变量 | 用户按键、INMP441 I2S 音频流、baidu_token、DeepSeek API Key |
| 关键输出变量 | 屏幕显示文本；voice_action / voice_pending |

#### 3.1.3 deepseek_chat_stream — DeepSeek 流式对话函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 使用 HTTPS 向 DeepSeek /chat/completions 发送 stream:true 请求，逐块接收 Server-Sent Events 并实时刷新屏幕，最终返回完整回复供 VC_ParseReply 解析。流式接收既满足 TLS 握手后低带宽体验，又避免一次性载入超长 JSON。 |
| 执行流程 | ① HTTPS 连接 api.deepseek.com → ② POST body：{model:"deepseek-chat", messages:[系统prompt; user_text], stream:true} → ③ 按 chunked 读取 body，逐行解析：若以 "data: " 开头则截取 payload；若为 [DONE] 结束；否则 JSON 解析 choices[0].delta.content 累加并 sse_process_line 瞬时刷新 → ④ 关闭连接。 |
| 关键输入变量 | const String &text（ASR 识别文本） |
| 关键输出变量 | String reply_text、过程中 ai_chat_display_text 持续更新 |

### 3.2 语音控制模块（voice_control.cpp）

#### 3.2.1 VC_ParseReply — 对外主解析函数（核心）

| 项目 | 内容 |
|---|---|
| 功能描述 | 接受 DeepSeek 回复文本（可能为纯文本或内嵌 JSON），分离"可显示 reply"与"动作 action"，是 AI 对话与应用层的解耦接口。 |
| 执行流程 | ① extract_json_object(json_reply) 去除 json 代码块标记并截取首个 {...} 块 → ② 若 json_str 为空，voice_action = VC_NONE; voice_pending = false; return json_reply 按纯文本显示 → ③ JSON 解析 reply = obj["reply"]; action = obj["action"] → ④ voice_action = str_to_action(action) → ⑤ voice_pending = (voice_action != VC_NONE) → ⑥ return reply。 |
| 关键输入变量 | const String &json_reply |
| 关键输出变量 | 返回值 String reply；副作用变量 voice_action / voice_pending |
| GUI 消费约定 | guiTask 每帧检查 voice_pending，为 true 时执行对应 UI 跳转或启动对应游戏，完成后 voice_pending = false; voice_action = VC_NONE |

---

## 4 应用层模块（概要）

### 4.1 任务调度模块（freertos.cpp）

#### 4.1.1 任务表与工具函数

| 任务 | 亲和核心 | 功能 |
|---|---|---|
| guiTask | Core 1 | 菜单状态机、游戏帧循环、AI 对话画面渲染调度 |
| joystickCheckTask | Core 1 | 周期性读取 USB 手柄 |
| webServerTask | Core 0 | SoftAP HTTP 服务 |

| 函数 | 功能 |
|---|---|
| suspendNonessentialTasks / resumeNonessentialTasks | AI 对话期间暂停/恢复 joystick/serial 任务，降低干扰 |
| initTasks | 创建上述 FreeRTOS 任务并拉起 initWebServer |

#### 4.1.2 Update_Tank_Game — 坦克游戏主循环

| 项目 | 内容 |
|---|---|
| 功能描述 | 实现本地坦克移动、开火与碰撞判定，并通过 network_manager 与对端同步，双人对战模式核心逻辑。 |
| 执行流程 | ① 输入选择（手柄或 web_tank_*）→ ② 按速度/旋转更新位置，调用 Check_Tank_Collision 若与墙/水重叠则回退 → ③ 开火冷却检查，生成子弹并进入生命周期；子弹每帧调用 Check_Bullet_Collision 做墙壁反弹，超过反弹次数则销毁；自身命中检测 → tank_game_over = 1 → ④ sendGameData(my_tank) 发送至对端，同时读取 remote_tank_data 绘制敌方坦克与子弹 → ⑤ 几何对象写入 vector_draw。 |
| 关键输入变量 | 手柄值、web_tank_speed_val / web_tank_turn_val / web_tank_fire、remote_tank_data |
| 关键输出变量 | 游戏对象位置、tank_game_over、矢量画面 |

#### 4.1.3 Update_Pong_Game — 乒乓球游戏主循环

| 项目 | 内容 |
|---|---|
| 功能描述 | 主机/从机不对称实现：主机做物理模拟与得分判定，广播状态；客户机仅上传球拍位置并绘制主机发来的球/球拍/得分，从而在不可靠 ESP-NOW 链路上保持一致。 |
| 关键输入变量 | enc_delta、web_pong_paddle / web_pong_active、remote_pong_data |
| 关键输出变量 | 球与球拍坐标、pong_score1 / pong_score2、矢量画面 |

### 4.2 网络管理模块（network_manager.cpp）

#### 4.2.1 OnDataRecv — 接收回调消息分发（核心）

| 项目 | 内容 |
|---|---|
| 功能描述 | ESP-NOW 接收回调，依据消息类型更新本地网络状态或将游戏数据投递至缓冲，供游戏主循环消费。 |
| 执行流程 | ① 解析 NetMessage → ② switch(type)：MSG_DISCOVERY 加入/刷新 discovered_peers；MSG_PAIR_REQUEST 若未配对则回复 MSG_PAIR_ACCEPT 并进入 NET_CONNECTED；MSG_START_GAME 置 game_request_pending = true，存储 game_id / seed；MSG_GAME_DATA 写入 remote_tank_data / remote_pong_data；MSG_END_GAME 置 remote_game_ended = true，游戏循环据此退出并返回菜单；MSG_DISCONNECT 清空 connected_peer_mac，回到 NET_IDLE。 |
| 关键输入变量 | NetMessage msg |
| 关键输出变量 | discovered_peers、connected_peer_mac、game_request_pending、remote_tank_data、remote_pong_data、remote_game_ended |

### 4.3 Web 服务器模块（web_server.cpp）

#### 4.3.1 webServerTask — HTTP 服务任务（Core 0）

| 项目 | 内容 |
|---|---|
| 功能描述 | 在 Core 0 独立提供 SoftAP HTTP 服务，暴露主菜单与游戏手柄网页，作为无手柄/无键盘设备的备用控制入口。 |
| 关键输出变量 | web_enc_delta / web_btn_pressed / web_game_dir / web_tank_* / web_pong_* |

#### 4.3.2 handleTankJoy — 坦克网页摇杆处理函数

| 项目 | 内容 |
|---|---|
| 功能描述 | 将网页虚拟摇杆返回的归一化坐标 (x,y) 转换为坦克前进速度与转向速度，写入全局变量供 Update_Tank_Game 消费。 |
| 关键输入变量 | URL 查询参数 x / y（范围 -1.0~1.0） |
| 关键输出变量 | web_tank_speed_val / web_tank_turn_val |

---

## 5 顶层控制流总结

用户输入（手柄/网页按钮/编码器）
    --> freertos.cpp（guiTask / Update_*_Game）
          + 菜单导航：web_enc_delta / web_btn_pressed / encoderValue
          + "AI 对话"分支：ai_chat_task -> baidu_asr -> deepseek_chat_stream(SSE)
                              -> VC_ParseReply -> voice_action / voice_pending -> guiTask 消费
          + 游戏分支：Update_Tank_Game / Update_Pong_Game ...
                 network_manager.cpp::OnDataRecv / sendGameData
                 vector_draw.cpp::DRAW_Add* -> DRAW_Render
                      DACoutput.cpp::onTimer -> sendDAC -> DAC8554 -> 示波器