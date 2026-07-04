# ESP32 OscilloGame

示波器游戏 + AI 对话（ESP32-S3 + INMP441 + DAC8554）

## 项目简介

基于 ESP32-S3 的硬件项目，功能包括：
- 示波器向量图形显示（通过 DAC8554 输出 X/Y/Z 信号）
- 双人坦克对战（ESP-NOW 联机）
- AI 语音对话（INMP441 录音 → 百度 ASR → DeepSeek Chat → 示波器终端显示）
- Web 控制面板

## 硬件配置

| 组件 | 引脚 |
|------|------|
| DAC8554 (SPI) | CS=10, MOSI=11, SCLK=12, LDAC=9 |
| INMP441 (I2S) | SCK=47, WS=48, DATA=1 |
| SD Card (SDIO 1-bit) | CMD=38, CLK=39, D0=40 |
| 编码器 | A=41, B=42, SW=45 |
| 摇杆 1 | X=13, Y=3, SW=16 |
| 摇杆 2 | X=15, Y=14, SW=17 |
| 按键 | A=18, B=21 |

## 当前问题汇总

### 🔴 核心问题：DeepSeek HTTPS（SSL）握手失败

AI 对话的完整链路：录音 → 百度 ASR（成功）→ DeepSeek Chat（失败）

```
录音（INMP441 → LittleFS）→ 百度 ASR（HTTP:80 ✅）→ DeepSeek Chat（HTTPS:443 ❌）
```

**错误表现：** `DeepSeek response: HTTP -1`  
**底层错误：** `esp-sha: Failed to allocate buf memory` → SSL handshake 返回 -1

### 🔴 原因分析x

#### 1. ~PSRAM 被软件禁用（已修复）~

~~板载 8MB Octal PSRAM（S3N16R8），但引脚与部分外设冲突，初始化失败：~~
```
~~PSRAM Size: 0 MB~~
```
~~PSRAM 不可用意味着所有动态内存（堆）只能使用内部 DRAM。~~

**✅ 已修复！** 原因是在 `platformio.ini` 中 `board_build.arduino.memory_type = dio_opi` 被注释掉了，且 `sdkconfig.defaults` 中缺少 PSRAM 配置项。取消注释并添加对应 sdkconfig 后 PSRAM 可正常初始化。

#### 2. DRAM 总量不足

| 项目 | 容量 |
|------|------|
| ESP32-S3 内部 DRAM | 338 KB（0x54700） |
| 系统静态占用（.data + .bss） | ~68 KB |
| WiFi 协议栈（STA + SoftAP） | ~60 KB |
| SoftAP 额外开销 | ~30 KB |
| ESP-NOW | ~16 KB |
| FreeRTOS 内核 | ~20 KB |
| 音频环形缓冲 | ~25 KB |
| mbedTLS 预加载 | ~20 KB |
| 其他驱动/碎片 | ~50 KB |
| **SSL 握手前空闲** | **~49 KB** |

#### 3. SSL 握手需要额外 ~36 KB

| SSL 组件 | 内存需求 |
|----------|---------|
| SSL 内容缓冲 | 4 KB（已从 16KB 缩减） |
| mbedTLS 握手状态 | ~8 KB |
| 软件 SHA 工作区 | ~2 KB |
| TCP 收发窗口 | ~6 KB |
| RSA/ECC 密钥协商 | ~8 KB |
| 证书解析 | ~4 KB |
| 随机数/熵池 | ~2 KB |
| 连接重试缓冲 | ~2 KB |
| **合计** | **~36 KB** |

**49 KB - 36 KB = 13 KB 余量 → 实际因内存碎片导致分配失败。**

#### 4. WIFI_AP_STA 模式加剧内存压力

参考项目（D:\VSESP\AI_Chat，DeepSeek 正常工作）使用 **WIFI_STA 模式**，协议栈占用约 50KB。  
本项目需要同时启用 STA + SoftAP + ESP-NOW（双人联机），协议栈多占 ~90KB。

### 💠 SSL 握手各内存组件详细用途

调用 `WiFiClientSecure.setInsecure()` + `client.connect()` 后，mbedTLS 库在堆上分配以下内存：

#### 1. `mbedtls_ssl_session` — SSL 会话上下文（~4 KB）
```
保存：协议版本、密码套件、会话 ID、Master Secret（48 字节）
作用：握手完成后维持会话状态，用于后续加密/解密
位置：ssl_tls.c → mbedtls_ssl_session结构体
```
握手过程中还要额外暂存 ServerHello + ServerHelloDone 消息。

#### 2. `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` — 收发缓冲（当前 4 KB）
```
两条：HS_IN_BUFFER（收） + HS_OUT_BUFFER（发）= 8 KB 总量
            ↓
MAX_CONTENT_LEN=4096 时 = 8 KB 总缓冲
MAX_CONTENT_LEN=16384 时 = 32 KB 总缓冲（默认值！）

用途：
  IN_BUFFER ← 接收服务器证书链（DeepSeek 证书 ~3KB 链）
  OUT_BUFFER → 发送 ClientHello（~500B）和 ClientKeyExchange
```
这是最大的单块内存。从 16KB 降到 4KB 省了 **24 KB**。

#### 3. `mbedtls_x509_crt` — 证书链解析（~3 KB）

```
setInsecure() 不验证证书，但仍需要解析服务器发来的证书，
因为证书里包含服务器公钥，用来做密钥交换。

分配点：
  x509_crt.c → mbedtls_x509_crt_parse()
  解析流程：DER 解码 → Subject/Issuer DN → 公钥 → 有效期

证书链典型大小：
  DeepSeek 证书链：~2.5 KB（DER 格式）
  解析后内存：~3 KB（原始数据 + 解析后的结构化字段）
```

#### 4. `mbedtls_ssl_handshake_params` — 握手状态机（~6 KB）

```
这是握手过程中最大的一块运行时内存，包含：

├─ 密钥交换暂存区（ecdh_context 或 dhm_context）
│  ECDHE: 临时密钥对（私钥 32B + 公钥 64B）+ 预主密钥（32B）
│  椭圆曲线运算中途需要大量临时大数（bignum）
│  ecp.c.mul() 一次就要几百字节栈 + 堆
│
├─ 握手散列缓冲区（handshake->update_checksum）
│  SHA256 上下文 ×2（客户端 + 服务端）= 512B
│
├─ 随机数（random_bytes）
│  ClientRandom（32B）+ ServerRandom（32B）
│
├─ 握手消息排队区
│  收到的握手消息在解析前暂存
│
└─ 扩展协商缓冲
   SNI、ALPN、supported_groups 等
```

#### 5. `mbedtls_pk_context` — 公钥运算（~6 KB）

```
从证书里提取出公钥后，用来做密钥交换：

对 ECDHE（DeepSeek 使用的是 ECDHE）：
├─ 生成临时 ECDH 密钥对          ~500 B
├─ 加载服务器 ECDH 公钥          ~500 B
├─ 计算共享密钥（ecp_mul）       ~4 KB （大数乘法中间结果）
└─ 派生主密钥（PRF）              ~1 KB

mbedcrypto.a 中的大数运算（bignum.c）需要大量临时 bignum 结构，
每个 bignum 就是一个动态分配的数组→这是堆碎片最敏感的地方。
```

#### 6. `mbedtls_entropy_context` + `mbedtls_ctr_drbg_context` — 随机数（~2 KB）

```
用途：
├─ 熵池（entropy.c）        1 KB ← 收集硬件噪声
├─ CTR_DRBG（ctr_drbg.c）  1 KB ← 从熵池生成安全随机数
└─ 用于：生成 ECDHE 临时密钥对、TLS 随机数
```

#### 7. `ssl_client.cpp` 包装层（~2 KB）

```
WiFiClientSecure 在 mbedTLS 之上的薄封装：

├─ ssl_client.cpp 自身上下文     ~500 B
│  （socket fd、超时、缓冲区指针等）
├─ TCP 收发包装（net_sockets.c）  ~1 KB
│  （mbedtls_net_context：socket 句柄 + 收发状态）
└─ WiFiClientSecure 对象本身     ~500 B
```

#### 8. esp-sha 硬件驱动 DMA 缓冲（~4 KB）← 这是额外杀手

```
即使 sdkconfig.defaults 配置了软件 SHA，预编译库可能仍包含硬件 SHA 代码：

ESP32-S3 硬件 SHA 加速需要 DMA 缓冲（连续内存）：
  esp_sha.c → 为每次 SHA256/SHA512 操作分配 DMA buffer（~2 KB 连续）
  esp_sha.c → HMAC 预计算也分配 DMA buffer

WIFI_AP_STA 模式下 DMA 内存池被 WiFi + SoftAP 大量占用，
导致 `esp-sha: Failed to allocate buf memory` → SSL 连接直接失败。

失败后 mbedTLS 本应回退到软件 SHA，但回退路径在 ESP32 实现中
可能没有正确处理，导致整个 ssl_client 返回 -1。
```

### 📊 总结账本

```
SSL 连接前空闲堆:         ~49 KB

SSL 握手分配明细：
  ssl_session + 暂存       4 KB
  IN_BUFFER（收）           4 KB  ← MAX_CONTENT_LEN
  OUT_BUFFER（发）          4 KB  ← MAX_CONTENT_LEN
  证书链解析                 3 KB
  握手状态机                6 KB
  ECDHE 公钥运算            6 KB
  CTR_DRBG + 熵池           2 KB
  ssl_client 包装            2 KB
  esp-sha DMA 缓冲          4 KB  ← 硬件 SHA 还在抢
  ─────────────────────────────
  SSL 握手合计             35 KB

余量：49 - 35 = 14 KB
→ 但 ECDHE 的 bignum 运算需要大块连续内存（>4KB）
→ 堆碎片导致实际可用 < 14 KB → OOM → HTTP -1
```

### 🔑 关键结论

| 组件 | 能否省 | 怎么省 |
|------|--------|--------|
| IN/OUT_BUFFER（8 KB） | ✅ 已省 | 从 16KB 缩到 4KB 各，省了 24KB |
| 握手状态机（6 KB） | ❌ 不能省 | mbedTLS 内部硬性要求 |
| ECDHE 公钥运算（6 KB） | ❌ 不能省 | 密钥协商必须做 |
| **esp-sha DMA 缓冲（4 KB）** | **✅ 应该能省** | 强制用软件 SHA |
| 证书链解析（3 KB） | ❌ 不能省 | 即使 setInsecure 也要拿公钥 |
| 随机数（2 KB） | ❌ 不能省 | TLS 协议要求 |

**最关键的突破点仍然是彻底禁用硬件 SHA DMA。如果 sdkconfig.defaults 无效，就要改用 build_flags 强行定义预处理器宏。**

#### 5. board_build.sdkconfig_path 可能未生效

`sdkconfig.defaults` 中配置了：
- 软件 SHA（`CONFIG_MBEDTLS_SHA256_SOFTWARE=y`）
- SSL 缓冲上限 4KB（`CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=4096`）

但 `esp-sha: Failed to allocate buf memory` 错误仍然出现，表明硬件 SHA 驱动仍在活动，配置可能未被编译系统正确应用。

### 🟡 次要问题

#### SD 卡挂载失败
```
sdmmc_init_ocr: send_op_cond (1) returned 0x107
Card Mount Failed
```
需要检查 SD 卡线路的上拉电阻。

#### 编码器波动
编码器值（ENC）在空闲时偶有跳动，可能需要硬件去抖或软件滤波。

## 解决方向

### 短期（当前尝试）
- [x] 缩减 SSL 缓冲（16KB → 4KB）
- [x] 软件 SHA 替代硬件 SHA（sdkconfig.defaults）
- [x] ESP-NOW 暂停/恢复框架（suspend_esp_now / resume_esp_now）
- [x] WiFi 完全重启（WIFI_OFF → WIFI_STA）释放 DMA 内存
- [ ] 强制 build_flags 确保配置生效
- [ ] 进一步缩减 SSL 相关内存占用

### 中期
- [ ] 考虑切换到 `esp_http_client`（比 WiFiClientSecure 更省内存）
- [ ] 在 SSL 握手前释放更多非必要内存
- [ ] 使用 `heap_caps_get_free_size(MALLOC_CAP_DMA)` 监控 DMA 内存

### 长期
- [ ] 排查 PSRAM 硬件问题，争取启用 PSRAM
- [ ] 或重新设计 PCB，避免 PSRAM 引脚被占用

## 构建说明

```bash
# PlatformIO 构建
pio run

# 上传
pio run -t upload --upload-port COM10

# 串口监视器
pio device monitor -b 115200 --port COM10
```

## 依赖库

- ArduinoJson 7.x
- ESP32-BLE-Mouse
- DAC8554（自定义库，位于 lib/DAC8554/）

## 分区

- 分区表：`default_16MB.csv`
- 文件系统：LittleFS（用于存储音频文件）
