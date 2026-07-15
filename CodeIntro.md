# esp32_OscilloGame 模块设计说明

> 按"顶层入口 → 硬件驱动层（重点）→ AI 交互层（重点）→ 应用层（概要）"顺序展开。
> 每个函数给出 ASCII 流程图 + 关键输入 / 输出 / 状态变量。

---

## 1. 总体结构与分层

```
esp32_OscilloGame/
│
└── src/
    ├── 顶层入口                  main.cpp
    ├── 硬件驱动层（重点）        DACoutput.cpp/h · vector_draw.cpp/h · audio_common.cpp/h · pins.h
    ├── AI 交互层（重点）         ai_chat.cpp/h · voice_control.cpp/h · ai_prompt.h
    └── 应用层（概要）            freertos.cpp/h · network_manager.cpp/h · web_server.cpp/h
```

### 调用与数据流向

```
  ┌─────────────────────────────────────────────────────────────────────┐
  │                        AI 交互层                                    │
  │  ai_chat (录音/ASR/DeepSeek) ─► voice_control (抽取 action/reply)  │
  │                                          │                          │
  └──────────────────────────────────────────┼──────────────────────────┘
                                             │
  ┌──────────────────────────────────────────▼──────────────────────────┐
  │                        应用层                                        │
  │  freertos (guiTask / 游戏 Update_*)                                 │
  │       ▲                  ▲                 ▲                        │
  │       │                  │                 │                        │
  │  network_manager     web_server         (本地编码器/按键)           │
  │  (ESP-NOW 同步)     (SoftAP HTTP)                                    │
  └──────────────────────────────────────────┼──────────────────────────┘
                                             │
  ┌──────────────────────────────────────────▼──────────────────────────┐
  │                       硬件驱动层                                     │
  │  vector_draw (DRAW_Add* / DRAW_Render / DRAW_GetNextPoint)         │
  │                        │                                             │
  │                        └► DACoutput::onTimer (80 kHz HW Timer)      │
  │                                 └► sendDAC ─► DAC8554 SPI ─► 示波器 │
  └─────────────────────────────────────────────────────────────────────┘
```

---

## 2. 顶层入口 — main.cpp

### 2.1 全局状态

| 变量 | 类型 | 说明 |
|---|---|---|
| `encoderValue` | `volatile int32_t` | 正交编码器累加值，由 ISR 更新，GUI/乒乓球游戏据此移动球拍/光标 |

### 2.2 `readEncoderISR()` — 正交编码器中断

```
        GPIO A/B 任一变化触发中断
                │
                ▼
          读取 GPIO 电平 → curA, curB
                │
                ▼
           (oldA, oldB) 查表
                │
     ┌──────────┼──────────┐
     ▼          ▼          ▼
   CW(+1)     CCW(-1)    bounce(0)
                │
                ▼
        encoderValue += delta
                │
                ▼
        oldA=curA, oldB=curB
```

- **输入**：GPIO 引脚电平（瞬时）
- **输出**：`encoderValue`（增量）

### 2.3 `setup()` — 系统初始化

```
开始
 │
 ├─ Serial.begin(115200)                 ─► USB 终端
 ├─ pinMode(按钮/编码器 GPIO)             ─► attachInterrupt(readEncoderISR)
 ├─ ADC 初始化                            ─► 摇杆原始值读取
 ├─ SD 卡挂载 + listDir                  ─► 媒体文件列表
 ├─ initTasks()                          ─► FreeRTOS: joystick / serial / GUI / web
 ├─ initDACoutput()                      ─► 80 kHz 定时器 + SPI 缓冲
 ├─ DRAW_Init()                          ─► 默认 "X" 画帧
 └─ DRAW_Terminal_Init() + Print 启动信息
```

- **输入**：编译期 `pins.h`
- **输出**：所有子系统进入就绪态

### 2.4 `loop()` — 心跳 + 调试打印

```
每 500ms: digitalWrite LED 翻转
当前: 周期性打印 joystick / encoderValue（可注释，不影响功能）
```

---

## 3. 硬件驱动层（重点）

### 3.1 DACoutput.cpp/h — DAC8554 矢量 / 音频 / 视频 输出

**核心思路**：硬件定时器（默认 80 kHz）触发 `onTimer` → 一次中断输出一个 (X,Y) 采样对；矢量模式下从 `vector_draw` 取下一点，音频/视频模式下从双缓冲取。

#### 全局状态变量

| 变量 | 类型 | 说明 |
|---|---|---|
| `player_mode` | `volatile int` | 0=矢量, 1=音频(2ch LR), 2=视频(4ch LRXY) |
| `bufA_L/R/X/Y`, `bufB_L/R/X/Y` | `volatile uint16_t*` | PSRAM 双缓冲指针（各通道独立） |
| `bufA_ready`, `bufB_ready` | `volatile bool` | 缓冲区是否已被填充、可播放 |
| `bufA_count`, `bufB_count` | `volatile int` | 缓冲区内有效样本数 |
| `playing_A` | `volatile bool` | 当前正在播放 A 还是 B |
| `play_idx` | `volatile int` | 当前缓冲区内播放位置 |
| `csMask / mosiMask / sclkMask / ldacMask` | `uint32_t` | GPIO 位掩码，用于 `sendDAC` 里无库直接 bit-bang |

---

#### `initDACoutput()`

```
开始
 │
 ├─ SPI.begin(HSPI)  + 配置 SPI 模式/频率
 ├─ 计算 GPIO 位掩码 (csMask/mosiMask/sclkMask/ldacMask)
 ├─ Init_Audio_Buffers()        ─► PSRAM 4 通道 × 2 块
 ├─ DRAW_Init()                 ─► vector_draw 默认画面
 ├─ hw_timer_t 配置并 attachInterrupt(onTimer)
 └─ 起始频率 80 kHz（矢量默认）
```

- **输入**：无
- **输出**：`SPI` + 定时器 + 双缓冲 + `vector_draw` 就绪

#### `setDACfreq(freq)`

```
timerEnd(timer)
timerAlarmWrite(timer, 80_000_000 / freq, true)
timerAlarmEnable(timer)
```

- **输入**：`uint32_t freq`（80000 或 44100）
- **输出**：改写定时器周期

#### `Init_Audio_Buffers()`

```
对 (L,R,X,Y) × (A,B):
   posix_memalign 申请 AUDIO_BUF_SIZE × 2 字节（16-bit 样本）
   对应 bufX_* = 指针；bufX_ready = false；bufX_count = 0
playing_A = true; play_idx = 0
```

- **输出**：`bufA_L/R/X/Y` / `bufB_L/R/X/Y` 被分配

#### `sendDAC(configRegister, value)` — IRAM_ATTR 位操作

```
uint32_t packet = (configRegister << 16) | (value & 0xFFFF);
GPIO.out_w1tc = csMask;            // CS low
for (i=23; i>=0; i--):
   GPIO.out_w1tc = sclkMask;
   if (packet & (1<<i)) GPIO.out_w1ts = mosiMask;
   else                 GPIO.out_w1tc = mosiMask;
   GPIO.out_w1ts = sclkMask;       // rising edge latch
GPIO.out_w1ts = csMask;            // CS high
GPIO.out_w1tc = ldacMask;          // LDAC pulse → simultaneous update
delayMicroseconds(1);
GPIO.out_w1ts = ldacMask;
```

- **输入**：`configRegister`（DAC 通道 A/B/C/D）、`value`（0~65535）
- **输出**：DAC 输出一个样本电压

---

#### `onTimer()` — 核心中断处理

```
portENTER_CRITICAL_ISR
switch (player_mode):

  ├─ 0 (矢量):
  │    DRAW_GetNextPoint(&x, &y)   ← vector_draw
  │    sendDAC(CH_X, x); sendDAC(CH_Y, y)
  │
  ├─ 1 (音频):
  │    if (playing_A):
  │        if (bufA_ready):
  │            sample = bufA_L[play_idx], bufA_R[play_idx]
  │            sendDAC(CH_X, sample); sendDAC(CH_Y, sample)
  │            play_idx++
  │            if (play_idx >= bufA_count): bufA_ready=false; playing_A=false; play_idx=0
  │        else: ... 静音填充
  │    else: 同上 bufB
  │
  └─ 2 (视频):
       同上，但是 4 通道 (L/R/X/Y 各自独立)
       X/Y 送 DAC 通道 → 示波器矢量画像素
portEXIT_CRITICAL_ISR
```

- **输入**：`player_mode`, `bufX_ready`, `bufX_count`, `play_idx`, `playing_A`
- **输出**：GPIO SPI 位流 → DAC8554 更新一次输出

---

#### 双缓冲读写接口

| 函数 | 流程图 | 关键变量 |
|---|---|---|
| `Is_Buf_A_Free()` | `return !bufA_ready` | `bufA_ready` |
| `Is_Buf_B_Free()` | `return !bufB_ready` | `bufB_ready` |
| `Get_Buf_A_*()` | 直接返回对应 `volatile uint16_t*` | `bufA_L/R/X/Y` |
| `Get_Buf_B_*()` | 同上 | `bufB_L/R/X/Y` |
| `Mark_Buf_A_Ready(count)` | `bufA_count=count; bufA_ready=true` | `bufA_count`, `bufA_ready` |
| `Mark_Buf_B_Ready(count)` | 同上 | `bufB_count`, `bufB_ready` |
| `Set_Player_Mode(mode)` | `player_mode=mode; setDACfreq( (mode==0)?80000:44100 )` | `player_mode` |

---

### 3.2 vector_draw.cpp/h — 矢量绘图 / 字符 / 终端

**核心思路**：上层（游戏/GUI）调用 `DRAW_AddLine/Rect/Circle/String` 把几何对象写入一个"对象池"；`DRAW_Render` 把对象池变换并光栅化为点缓冲，双缓冲与 `onTimer` 消费端（`DRAW_GetNextPoint`）隔离。

#### 全局状态变量

| 变量 | 类型 | 说明 |
|---|---|---|
| `currentDrawMode` | `static DrawMode` | `DRAW_MODE_CPU`（线段插值实时算）/ `DRAW_MODE_DMA`（预光栅化为点表） |
| `dmaBuffers[2]`, `dmaBufferCounts[2]`, `dmaBufferCapacity` | `static uint32_t*` 等 | DMA 双缓冲（`activeDmaIdx` 给 ISR 读，`backDmaIdx` 给 Render 写） |
| `activeDmaIdx`, `backDmaIdx`, `dmaReadIndex` | `volatile uint8_t` 等 | 双缓冲切换 & ISR 读光标 |
| `drawStepSize` | `static uint8_t` | 线段插值步长（越小越精细，默认 16） |
| `global_scale_x_pct/y_pct`, `global_offset_x/y` | `static uint16_t / int16_t` | 全局缩放 & 偏移，应用于所有对象 |
| `term_cursor_y`, `term_line_height`, `term_spacing`, `term_scale_pct` | `static int32_t/uint16_t` | "终端模式"行布局参数 |
| `Shape pool[]`, `shape_count` | `Shape[]` / `int` | 对象池（Line/Rect/Circle/String slot） |
| `stringSlot_scroll[]` | `int32_t[]` | 文本 slot 的滚动像素偏移（用于跑马灯） |

#### ASCII 字符图案表

- `pattern_A..Z`, `pattern_a..z`, `pattern_0..9`, 以及 `pattern_excl/apos/hash/pct/caret` 等符号
- 每个字符 = 若干 `{x0,y0,x1,y1}` 浮点数线段，在 8×8 单位坐标系
- 另外还有一套 "大数字" 像素级线段（坐标 1200~3000），用于数码管风格显示

---

#### `DRAW_Init()`

```
DRAW_Clear()
DRAW_SetLetter('X')    ← 用 DRAW_AddLine 画两条对角线
_resetLine()           ← CPU 模式下初始化 current_line 游标
```

- **输出**：首帧画面（"X"）就绪

---

#### `DRAW_GetNextPoint(outX, outY)` — ISR 级

```
if (currentDrawMode == DMA):
    if (dmaBufferCounts[activeDmaIdx] == 0): outX=outY=2048; return
    idx = dmaReadIndex % dmaBufferCounts[activeDmaIdx]
    point = dmaBuffers[activeDmaIdx][idx]
    outX = point >> 16; outY = point & 0xFFFF
    dmaReadIndex++
else (CPU 模式):
    while (current_line 对象未完成一条线段插值):
        按 drawStepSize 推进当前线段 → outX, outY
        if (线段走完): 跳到下一个对象并 _resetLine()
        return
```

- **输入**：`activeDmaIdx`, `dmaReadIndex`, 对象池游标 / 当前线段插值游标
- **输出**：`outX, outY`（0~4095），供 `DACoutput::onTimer` 送 DAC

---

#### `DRAW_Render()` — 把对象池 → 后台缓冲

```
DRAW_DisableScroll() 自动：若有非滚动文本则清零各 slot 滚动
backDmaIdx = 1 - activeDmaIdx
清空后台缓冲（dmaBufferCounts[backDmaIdx] = 0）

for (shape in 对象池):
    if (shape.type == LINE/RECT/CIRCLE):
        展开为基本线段（矩形=4条，圆=N条短弦）
        _processLine → 应用 global_scale / offset → 剔除越界
        if (CPU 模式): 直接把线段追加到 "CPU 对象表"
        else (DMA 模式): _rasterizeLineToDMA → 按步长写点到 dmaBuffers[backDmaIdx]
    else if (shape.type == STRING):
        对 char in 文本:
            查询 _getPattern(c) → 每条线段 → 缩放到 scale_x/scale_y
            + spacing 字间距
            + stringSlot_scroll[slot] 整体水平偏移
        _processLine 同上，追加

全部处理完后，原子交换 activeDmaIdx / backDmaIdx
dmaReadIndex = 0
对象池 shape_count = 0（下次 DRAW_Add* 从头写）
```

- **输入**：对象池，`global_scale/offset`, `drawStepSize`, `stringSlot_scroll[]`
- **输出**：`dmaBuffers[backDmaIdx]` 成为新前台；`dmaBufferCounts` 更新；对象池被清

---

#### `DRAW_AddLine / Rect / Circle (x0,y0,x1,y1)`

```
对象池 shape[shape_count++] = {type=LINE/RECT/CIRCLE, 坐标原样保存}
（真正缩放/裁剪发生在 DRAW_Render，保证上层调用廉价）
```

- **输入**：逻辑坐标系坐标
- **输出**：对象池新增一条

---

#### `DRAW_AddString(s, spacing, x, y, scale_x, scale_y)`

```
slot = shape_count++           ← 分配一个 STRING 槽
计算 DRAW_CalcStringWidth(s)
shape[slot] = {STRING, s, spacing, x, y, scale_x, scale_y, slot}
return slot
```

- **输出**：`slot` 索引（可用于 `DRAW_SetTextScroll/DRAW_GetTextScroll` 动态滚动）

---

#### `DRAW_Update()` — 文本自动滚动

```
for (每个 STRING 槽):
    if (text 宽度 > 屏幕宽):
        stringSlot_scroll[slot] -= 2     ← 每调用一次向左 2 像素
    if (stringSlot_scroll[slot] + width < 0): 循环复位
```

---

#### 终端功能：`DRAW_Terminal_Init / Print`

```
Init(scale_pct, spacing):
    term_scale_pct = scale_pct
    term_spacing   = spacing
    term_line_height = 字符盒高 + spacing
    term_cursor_y  = 屏幕底部起点

Print(str):
    DRAW_AddString(str, ...) 于 (x居中, y=term_cursor_y)
    term_cursor_y -= term_line_height   ← 下一行向上走
    if (term_cursor_y 溢出顶部): 回到起点（滚动）
```

- **输入**：字符串
- **输出**：矢量画面增加一行文本

---

### 3.3 audio_common.cpp/h

仅提供共享变量（无函数，主要做"其他 cpp 音频消费者共享环形缓冲"桥接）：

| 变量 | 类型 | 说明 |
|---|---|---|
| `audioBuffer[]` | `uint16_t[]` | 音频环形缓冲（样本队列） |
| `bufferHead` / `bufferTail` | `volatile int` | 写 / 读游标 |

> 项目中 `DACoutput` 用自己的双缓冲实现播放；`audio_common` 保留作为"从 SD 卡解码 → 环形缓冲 → 其他任务读取"的备用方案入口。

---

## 4. AI 交互层（重点）

### 4.1 ai_chat.cpp/h — 语音对话主流程

**核心思路**：一个 `ai_chat_task`（Core 0）完成整个链路：
`按键录音 → INMP441 I2S PCM → 百度 ASR → DeepSeek SSE 流式回复 → 屏幕同步显示 → voice_control 抽取 action → GUI 切换菜单/启动游戏`。

#### 全局状态变量

| 变量 | 类型 | 说明 |
|---|---|---|
| `ai_chat_active` | `volatile bool` | 任务是否正在运行（防重复启动） |
| `ai_chat_activity_time` | `volatile unsigned long` | 最近一次活动时间（看门狗用，可扩展） |
| `ai_chat_phase` | `volatile AIChatPhase` | `AI_PHASE_WAITING / THINKING / REPLY` — 驱动屏幕画面 |
| `ai_chat_display_text[512]` | `char[]` | 当前要显示的文字（GUI 渲染 / 终端显示共用） |
| `ai_chat_dirty` | `volatile bool` | 是否需要刷新画面 |
| `baidu_token[256]` | `static char[]` | 百度 OAuth token（缓存） |
| `token_expires` | `static unsigned long` | token 过期 unix 秒 |

---

#### `AI_Chat_Start()`

```
if (ai_chat_active): return
ai_chat_active = true
xTaskCreatePinnedToCore(ai_chat_task, "ai_chat", 16*1024, NULL, 2, NULL, 0)
```

- **输入**：无（由 GUI 在用户选择"AI 对话"时调用）
- **输出**：Core 0 上拉起 `ai_chat_task`

---

#### `wifi_connect()` — 内部工具

```
WiFi.mode(WIFI_STA)
WiFi.begin(SSID, PASSWORD)
for (最多 20s):
    if (WiFi.status() == WL_CONNECTED): return true
    vTaskDelay(300)
return false
```

- **输入**：用户在 `ai_chat.cpp` 顶部宏里配置的 `AI_WIFI_SSID / AI_WIFI_PASS`
- **输出**：WiFi 连接状态

---

#### `baidu_get_token()`

```
if (当前 token 未过期): return true
HTTPClient GET https://aip.baidubce.com/oauth/2.0/token?grant_type=...
解析 JSON → access_token 写入 baidu_token；expires_in → token_expires
return OK
```

- **输入**：百度 API Key / Secret Key（用户配置）
- **输出**：`baidu_token` 填充；返回成功/失败

---

#### `baidu_asr(pcm, samples)`

```
POST https://vop.baidu.com/server_api
  header: token=baidu_token
  body:  JSON { "format":"pcm", "rate":16000, "channel":1,
                "token":"...", "cuid":"esp32", "speech": base64(pcm),
                "len": samples*2 }
wait for response → JSON["result"][0] → 返回文本
```

- **输入**：`int16_t* pcm`（16 kHz 单声道 16-bit PCM），`samples` 样本数
- **输出**：识别文本（String）

---

#### `deepseek_chat_stream(text)` — SSE 流式

```
  HTTPS 连接 api.deepseek.com
  POST /chat/completions
    body: {"model":"deepseek-chat", "messages":[...user text...], "stream":true}
  解析响应头 → Transfer-Encoding: chunked
  循环 read chunk:
      逐行读取 body:
          if (line starts with "data: "):
              payload = line.substring(6)
              if (payload == "[DONE]"):  break
              JSON.parse(payload)
              delta = choices[0].delta.content （可能为空）
              accumulated += delta
              sse_process_line(accumulated)  → 实时刷新屏幕
  HTTPClient end
  return accumulated
```

- **输入**：ASR 识别文本（作为 user message），外加 `ai_prompt.h` 里系统 prompt
- **输出**：完整回复（String）；过程中通过 `ai_show` 实时刷新 `ai_chat_display_text`

---

#### `sse_process_line(line, accumulated)`

```
去除前导 "data: "
if (line == "[DONE]"): return false（结束标志）
解析 JSON 片段 → choices[0].delta.content
if (content 存在): accumulated += content
                    ai_show(AI_PHASE_REPLY, accumulated.c_str())   ← 屏幕瞬时更新
return true
```

- **输入**：SSE 一行文本
- **输出**：`accumulated`（引用传参持续累积）；`ai_chat_display_text` 改写 + `dirty=true`

---

#### `ai_show(phase, text)` — 线程安全屏幕刷新

```
portENTER_CRITICAL
ai_chat_phase = phase
strncpy(ai_chat_display_text, text, sizeof-1)
ai_chat_display_text[sizeof-1] = 0
ai_chat_dirty = true
portEXIT_CRITICAL
同时 updateWebUIStatus(text) 让网页手柄也能看到文字
```

- **输入**：`phase`, `text`
- **输出**：`ai_chat_phase / ai_chat_display_text / ai_chat_dirty` 写入；Web 端状态同步

---

#### `ai_chat_task()` — 主循环（最顶层流程图）

```
开始
 │
 ├─ suspendNonessentialTasks()         ← 暂停 joystick / serial / loop 任务
 ├─ deinitHardwareDMA()                ← 释放 BLE（避免 WiFi+BLE+BLE 冲突）
 ├─ Network_Manager::suspend_esp_now() ← 关闭 ESP-NOW（让出 WiFi 资源）
 │
 ├─ if (wifi_connect() == false):
 │        ai_show(WAITING, "WiFi FAIL") → vTaskDelay → goto CLEANUP
 │
 │
 ◄──┐  循环：
 │   │
 │   ├─ ai_show(WAITING, "按住 OK 录音 / 长按退出")
 │   ├─ 等待按键：
 │   │      短按 → 录音 3 秒（16 kHz × 3 = 48000 samples）写入 PSRAM int16_t[]
 │   │      长按 → break（退出对话）
 │   │
 │   ├─ ai_show(THINKING, "识别中…")
 │   ├─ baidu_asr(pcm, samples) → user_text
 │   │      若失败：ai_show(REPLY, "ASR FAIL") → continue
 │   │
 │   ├─ ai_show(THINKING, "思考中…")
 │   ├─ reply_text = deepseek_chat_stream(user_text)
 │   │      若失败：ai_show(REPLY, "DEEPSEEK FAIL") → continue
 │   │
 │   ├─ 解析 reply_json = VC_ParseReply(reply_text)   ← voice_control 处理
 │   │      ai_show(REPLY, reply_json.c_str())
 │   │
 │   └─ if (voice_pending):   ← voice_control 置位了 action
 │          break（让 GUI 循环消费 action）
 │
 └─ CLEANUP:
     Network_Manager::resume_esp_now()
     reinitHardwareDMA()
     resumeNonessentialTasks()
     ai_chat_active = false
     vTaskDelete(NULL)
```

- **输入**：用户按键（短按录音 / 长按退出）；INMP441 I2S 音频流
- **输出**：屏幕文字；`voice_control` 的 `voice_action / voice_pending`

---

### 4.2 voice_control.cpp/h — JSON action 解析

**核心思路**：DeepSeek 回复里包含 "```json … {reply,action} ```" 或纯文本；`VC_ParseReply` 抽 `reply` 显示、抽 `action` 映射成 `VC_Action` 供 GUI 分支执行。

#### 全局状态变量

| 变量 | 类型 | 说明 |
|---|---|---|
| `voice_action` | `volatile VC_Action` | 最近一次抽取的动作（`VC_START_SNAKE` 等） |
| `voice_pending` | `volatile bool` | GUI 循环是否有动作待消费 |

#### VC_Action 枚举

```
VC_NONE / VC_OPEN_MUSIC / VC_OPEN_VIDEO / VC_OPEN_GAMES
VC_OPEN_ONLINE / VC_OPEN_GAME_JOY / VC_OPEN_AI_CHAT / VC_OPEN_ABOUT
VC_START_SNAKE / VC_START_BREAKOUT / VC_START_FLAPPY / VC_START_RACING
VC_START_RUNTINY / VC_START_TANK / VC_BACK / VC_EXIT
```

---

#### `str_to_action(action)`

```
if (action == "start_snake")    return VC_START_SNAKE
if (action == "start_breakout") return VC_START_BREAKOUT
... （每个枚举值一张 if）
return VC_NONE
```

- **输入**：DeepSeek JSON 里 `action` 字段
- **输出**：`VC_Action` 值

---

#### `extract_json_object(raw)`

```
去掉所有 "```json" 与 "```" 标记
找到第一个 '{' 和最后一个 '}'
返回 substr(...)  —— 即 "{...}" 块
若未找到：返回空
```

- **输入**：DeepSeek 原始回复
- **输出**：被包裹在纯文本中的 JSON 块

---

#### `VC_ParseReply(json_reply)` — 对外主接口

```
json_str = extract_json_object(json_reply)
if (json_str 为空):
    voice_action = VC_NONE
    voice_pending = false
    return json_reply     ← 按纯文本原样显示

JSON.parse(json_str) → obj
reply  = obj["reply"].as<String>()   （缺省为整串）
action = obj["action"].as<String>()

voice_action  = str_to_action(action)
voice_pending = (voice_action != VC_NONE)

return reply
```

- **输入**：DeepSeek 完整回复（可能是纯文本、可能内嵌 JSON）
- **输出**：返回值 = 要显示在终端上的 reply 文本；副作用 = `voice_action / voice_pending`
- **GUI 消费约定**：`guiTask` 每帧检查 `voice_pending`，若 true → 执行对应 UI 跳转 → `voice_pending=false; voice_action=VC_NONE`

---

## 5. 应用层（概要）

### 5.1 freertos.cpp/h — 任务调度 / GUI / 游戏

**总览**：三个 FreeRTOS 任务 + 多组游戏 Init/Update 函数；另外提供 "AI 对话时暂停/恢复非必要任务" 和 "DMA 外设释放/恢复" 两套工具。

#### 任务与句柄

| 任务 | 亲和 Core | 说明 |
|---|---|---|
| `guiTask` | Core 1 | UI 状态机：主菜单 / 游戏菜单 / 各游戏 Update / AI 对话等待画面 |
| `joystickCheckTask` | Core 1 | 定期读取 USB 手柄（若连接）→ 更新 `Gamepad.x/y/buttons` |
| `serialOutputTask` | Core 1 | 调试串口打印（可选） |
| `webServerTask` | Core 0 | 在 `initWebServer()` 里拉起 |

#### 工具函数

| 函数 | 流程 |
|---|---|
| `suspendNonessentialTasks()` | `vTaskSuspend(joystickCheckTask / serialOutputTask / loopTask)` |
| `resumeNonessentialTasks()` | 对应 `vTaskResume` |
| `deinitHardwareDMA()` | `delete bleMouse; NimBLEDevice::deinit`（释放 WiFi/BLE 冲突 DMA） |
| `reinitHardwareDMA()` | 反向恢复（SD 卡不重复初始化） |
| `initTasks()` | 上面 4 个 `xTaskCreatePinnedToCore` + 启动 `initWebServer()` |

#### Web 手柄 → 游戏数据通道（关键全局变量）

| 变量 | 类型 | 生产者 | 消费者 |
|---|---|---|---|
| `web_enc_delta` | `volatile int` | `web_server::handleUp/Down/Left/Right` | guiTask（菜单上下） |
| `web_btn_pressed` | `volatile bool` | `web_server::handleEnter` | guiTask（菜单确认） |
| `web_game_dir` | `volatile int` | `web_server::handleLeft/Right/Up/Down` | `Update_Snake_Game`（方向） |
| `web_tank_speed_val / turn_val` | `volatile float` | `web_server::handleTankJoy` | `Update_Tank_Game` |
| `web_tank_fire` | `volatile bool` | `web_server::handleTankFire` | `Update_Tank_Game` |
| `web_pong_paddle` | `volatile float [0..1]` | `web_server::handlePongMove` | `Update_Pong_Game` |
| `web_pong_active` | `volatile bool` | `web_server::handlePongMove` | `Update_Pong_Game`（优先用网页值） |

---

### 5.2 游戏 Update_* 函数（流程概要）

每个游戏都遵循：**每帧（~20–50ms）读取输入 → 更新物理 → 碰撞检测 → 写 vector_draw 对象 → 由 DAC 中断扫描渲染**。

#### `Update_Snake_Game()`

```
snake_dir = 优先级: web_game_dir > 编码器方向 > 按键方向
if (now - last_game_tick > 200ms):
    new_head = head + dir
    if (撞墙 或 自撞):  game_over=1
    else: head=new_head; if (吃到 food): snake_len++; score++; 新食物随机
    last_game_tick = now
绘制: DRAW_AddRect 画蛇身每段 + 食物块 + 得分文本
```

#### `Update_Breakout_Game(encoder_delta)`

```
挡板 x += encoder_delta（限幅）
球 pos += vx,vy
碰左右墙 → vx = -vx
碰顶 → vy = -vy
碰挡板 → vy = -vy （根据击中位置加一点点水平扰动）
碰砖块 brk_bricks[r][c]:
   vy=-vy; brk_bricks[r][c]=0; score++
   if (剩余砖块==0): 胜利
球落底 → lives--; 若 lives==0 game_over=1
```

#### `Update_Flappy_Game(jump_requested)`

```
if (jump_requested): vy = -JUMP_SPEED   ← 向上跳
vy += GRAVITY; player.y += vy
if (碰天花板/地板): game_over=1
障碍物向左移动，穿过屏幕时得分
AABB 碰撞：矩形(player) vs 上/下障碍
绘制：圆作为鸟；矩形作为管道；得分文本
```

#### `Update_Racing_Game(encoder_delta)`

```
赛车 x += encoder_delta（限幅到赛道范围）
障碍物向下漂移；穿过底部时 score++
AABB(赛车 vs 任一障碍) → game_over=1
```

#### `Update_RunTiny_Game(jump_requested)`

```
跳跃物理同上；障碍物右→左移动；穿过屏幕得分；碰撞 game_over
```

#### `Update_Tank_Game()`

```
输入选择:  joystick_connected ? 手柄 : (web_tank_*)
x += speed_val * cos(angle)
y += speed_val * sin(angle)
angle += turn_val * ANGLE_STEP
if (Check_Tank_Collision(my_tank.x,y)): 回退移动
开火: (冷却 && web_tank_fire/扳机) → 生成 bullet (speed*cos, speed*sin)
子弹周期: pos+=v; Check_Bullet_Collision
         撞墙反弹（翻转入射法向分量）；超过反弹次数消失
         撞自身 → game_over
远程同步: sendGameData(my_tank state); 同时接收 remote_tank_data → 绘制敌方坦克+子弹
```

#### `Update_Pong_Game(enc_delta)`

```
本地球拍 x:  web_pong_active ? (web_pong_paddle * COURSE_WIDTH) : (pong_my_paddle_x += enc_delta)
若我是主机:
    球 pos += vx,vy
    左右墙 → vx=-vx
    碰底部球拍 → vy=-vy；球速根据球拍击中位置偏离中心增加额外 vx
    碰顶部球拍（从网络接收到的 paddle2_x）→ 同上反向
    球越过上/下边界 → 对手/我得分；重置球
若我是客户机:
    只上传自己球拍位置；接收主机球/得分并绘制
网络同步: 主机广播 PongData{ball, paddle1, paddle2, score1, score2}
         客户机上传 paddle2
```

---

### 5.3 network_manager.cpp/h — ESP-NOW 设备发现 / 配对 / 游戏同步

**核心思路**：状态机 `NET_IDLE → NET_DISCOVERING → NET_PAIRING → NET_CONNECTED → NET_IN_GAME`，通过固定大小 `NetMessage` 通信：

```
NetMessage {
    uint8_t type;        // MSG_DISCOVERY / PAIR_REQUEST / PAIR_ACCEPT / DATA
                         // START_GAME / GAME_DATA / END_GAME
    uint8_t src_mac[6];
    char    name[16];
    union payload {
        TankData  tank_data;   // x,y,angle,bullet_count,bullets[5]
        PongData  pong_data;   // ball(x/y, vx/vy), paddle1/2, score1/2
        struct { uint8_t game_id; uint32_t seed; } start_req;
        struct { uint8_t reason; } end_req;
    };
};
```

#### 关键函数

| 函数 | 流程 |
|---|---|
| `init()` | WiFi AP+STA 模式；`esp_now_init`；注册 `OnDataSent / OnDataRecv` 回调 |
| `update()` | 每 500 ms 广播 MSG_DISCOVERY；维护已发现设备列表 `discovered_peers`（3s 超时踢除） |
| `pair(target_mac)` | 发送 MSG_PAIR_REQUEST → 等待 MSG_PAIR_ACCEPT → 进入 `NET_CONNECTED` |
| `disconnect()` | 发送断开消息；`esp_now_del_peer`；回到 `NET_IDLE` |
| `startGame(gameId, seed)` | 发送 `MSG_START_GAME{gameId, seed}` 给 peer；本地 `NET_IN_GAME` |
| `sendGameData(TankData/PongData)` | 周期性 `MSG_GAME_DATA`，包含当前帧坦克/乒乓球状态 |
| `endGame(reason)` | 发送 `MSG_END_GAME{reason}`；退出 `NET_IN_GAME` |
| `hasGameRequest(&id, &seed)` / `clearGameRequest()` | GUI 轮询 "对方是否邀请我开始某游戏" |
| `getRemoteGameData(TankData/PongData*)` | 取最近一次收到的远程状态 → 本地绘制敌方/对方球拍 |
| `isRemoteGameEnded(&reason)` / `clearRemoteGameEnded()` | 对端退出 → 本端也退出 |
| `suspend_esp_now() / resume_esp_now()` | AI 对话期间临时关闭 ESP-NOW（避免与 WiFi STA 冲突） |

#### `OnDataRecv(mac, data, len)` — 核心消息分发

```
switch (msg.type):
    MSG_DISCOVERY     → 把对方加入 discovered_peers（或刷新 last_seen）
    MSG_PAIR_REQUEST  → 若未配对：回 MSG_PAIR_ACCEPT；进入 NET_CONNECTED
    MSG_PAIR_ACCEPT   → 进入 NET_CONNECTED
    MSG_START_GAME    → 置位 game_request_pending；保留 game_id / seed
    MSG_GAME_DATA     → 写入 remote_tank_data / remote_pong_data（由游戏 Update_* 读取）
    MSG_END_GAME      → remote_game_ended=true; reason=...（由 Update_* 检测并切回菜单）
    MSG_DISCONNECT    → 清空 connected_peer；回到 NET_IDLE
```

---

### 5.4 web_server.cpp/h — SoftAP HTTP 手柄

**总览**：在 Core 0 上跑一个轻量 HTTP Server；对外暴露主菜单按钮页 + 坦克虚拟摇杆页 + 乒乓球滑块页；网页点击 → 写入全局变量 → 由 `freertos` 的 GUI/游戏循环消费。

#### URL 路由

| 路径 | 对应函数 | 作用 |
|---|---|---|
| `/` | `handleRoot()` | 返回 HTML 主菜单控制页（Up/Down/Left/Right/Enter） |
| `/status` | `handleStatus()` | 返回 JSON 文本的当前 UI 状态（来自 `updateWebUIStatus()` 写入） |
| `/up /down /left /right` | `handleUp/...` | 写入 `web_enc_delta / web_game_dir` |
| `/enter` | `handleEnter()` | 置位 `web_btn_pressed` |
| `/tank` | `handleTankPage()` | 返回坦克 HTML 控制页（全向摇杆 + 开火按钮） |
| `/tankJoy?x=..&y=..` | `handleTankJoy()` | 写入 `web_tank_speed_val / web_tank_turn_val` |
| `/tankFire` | `handleTankFire()` | 置位 `web_tank_fire` |
| `/tankStatus` | `handleTankStatus()` | 返回坦克输入 JSON 快照（页面 JS 轮询显示） |
| `/pong` | `handlePongPage()` | 返回乒乓球 HTML 控制页（横向触摸滑块） |
| `/pongMove?pos=0.3` | `handlePongMove()` | 写入 `web_pong_paddle`；置位 `web_pong_active` |
| `/pongStatus` | `handlePongStatus()` | 返回球拍位置 JSON 快照 |

#### 主要函数

| 函数 | 流程 |
|---|---|
| `updateWebUIStatus(status)` | 写入全局 `current_ui_status` → `/status` 返回 |
| `handleTankJoy()` | `x = atof(arg("x")); y = atof(arg("y")); web_tank_speed_val = -y; web_tank_turn_val = x;` 返回 OK JSON |
| `handlePongMove()` | `web_pong_paddle = constrain(atof("pos"), 0, 1); web_pong_active = true;` |
| `webServerTask()` | `WiFi.softAP(SSID, PASS); WebServer server(80); server.on(...) 注册所有路由; server.begin(); loop server.handleClient();` |
| `initWebServer()` | `xTaskCreatePinnedToCore(webServerTask, ..., 0)`（Core 0 上拉起） |
| `suspendWebServer() / resumeWebServer()` | `vTaskSuspend / vTaskResume` |

---

## 6. 关键数据与控制流汇总（一张图）

```
 用户（手柄/网页/编码器）
       │
       ▼
 freertos.cpp (guiTask) ← web_server.cpp (HTTP 写全局变量)
       │
       ├─ 菜单导航：读取 web_enc_delta / web_btn_pressed / encoderValue
       ├─ 选择 "AI 对话" → ai_chat.cpp::AI_Chat_Start()
       │                     │
       │                     ├─ wifi_connect + baidu_get_token
       │                     ├─ INMP441 录音 → baidu_asr
       │                     └─ deepseek_chat_stream(SSE) ─► voice_control::VC_ParseReply
       │                                                              ├─ voice_action
       │                                                              └─ voice_pending ─► guiTask 下一帧消费
       │
       └─ 选择游戏 → Update_*_Game()
                          │
                          ├─ network_manager.cpp::sendGameData/OnDataRecv （远程对战）
                          │
                          └─ vector_draw.cpp::DRAW_AddLine/Rect/Circle/String  → DRAW_Render
                                                                                      │
                                                                                      ▼
                                                                 DACoutput.cpp::onTimer (80 kHz)
                                                                           │
                                                                           ▼
                                                                   sendDAC ─► DAC8554 ─► 示波器
```