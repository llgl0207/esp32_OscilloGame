#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "freertos.h"

// Web服务器对象，端口80
WebServer server(80);
// 当前UI状态字符串
String current_ui_status = "{}";

// Web 服务器任务句柄，用于暂停/恢复
static TaskHandle_t s_webServerTaskHandle = NULL;

// HTML页面内容
const char* html = u8R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { 
        font-family: Arial; 
        text-align: center; 
        margin:0px auto; 
        padding-top: 30px; 
        background-color: #222; 
        color: #fff; 
        position: relative;
        min-height: 100vh;
    }
    body::before {
        content: "";
        position: fixed;
        top: 0; left: 0; width: 100%; height: 100%;
        background-image: url('/background.jpg');
        background-size: cover;
        background-position: center;
        opacity: 0.5;
        z-index: -1;
    }
    .button { display: inline-block; padding: 20px 30px; font-size: 24px; cursor: pointer; text-align: center; text-decoration: none; outline: none; color: #fff; background-color: #4CAF50; border: none; border-radius: 15px; box-shadow: 0 9px #999; margin: 10px; -webkit-user-select: none; user-select: none; }
    .button:active { background-color: #3e8e41; box-shadow: 0 5px #666; transform: translateY(4px); }
    .nav-btn { background-color: #008CBA; }
    .enter-btn { background-color: #f44336; }
    .row { display: flex; justify-content: center; align-items: center; }
    #ui-status { margin-top: 20px; padding: 10px; border: 1px solid #555; background: rgba(51, 51, 51, 0.8); text-align: left; font-family: monospace; white-space: pre-wrap; min-height: 100px; max-height: 400px; overflow-y: auto; }
  </style>
  <script>
    var t = 0;
    function c(path, e) {
      if (e.type === 'touchstart') {
        t = Date.now();
        e.preventDefault();
      } else if (Date.now() - t < 500) {
        return;
      }
      fetch(path);
    }
    
    setInterval(function() {
      fetch('/status').then(response => response.text()).then(data => {
        document.getElementById('ui-status').innerText = data;
      });
    }, 500);
  </script>
</head>
<body>
  <h1>ESP32游戏控制台</h1>
  
  <div class="row">
    <button class="button nav-btn" onmousedown="c('/up', event)" ontouchstart="c('/up', event)">UP / PREV</button>
  </div>
  <div class="row">
    <button class="button nav-btn" onmousedown="c('/left', event)" ontouchstart="c('/left', event)">LEFT / VOL-</button>
    <button class="button enter-btn" onmousedown="c('/enter', event)" ontouchstart="c('/enter', event)">ENTER</button>
    <button class="button nav-btn" onmousedown="c('/right', event)" ontouchstart="c('/right', event)">RIGHT / VOL+</button>
  </div>
  <div class="row">
    <button class="button nav-btn" onmousedown="c('/down', event)" ontouchstart="c('/down', event)">DOWN / NEXT</button>
  </div>
  <p>当前UI状态: </p>
  <div id="ui-status">Loading UI Status...</div>

  <p>连接至WiFi: ESP32_Game_XX:XX/密码: 12345678</p>
</body>
</html>
)rawliteral";

// 处理根路径请求
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", html);
}

// 处理状态查询请求
void handleStatus() {
  server.send(200, "text/plain; charset=utf-8", current_ui_status);
}

// 更新Web UI状态
void updateWebUIStatus(String status) {
    current_ui_status = status;
}

// 处理向上按钮
void handleUp() {
  web_enc_delta = -1; // 菜单向上/上一个
  web_game_dir = 0;   // 贪吃蛇向上
  server.send(200, "text/plain", "OK");
}

// 处理向下按钮
void handleDown() {
  web_enc_delta = 1;  // 菜单向下/下一个
  web_game_dir = 1;   // 贪吃蛇向下
  server.send(200, "text/plain", "OK");
}

// 处理向左按钮
void handleLeft() {
  web_enc_delta = -5; // 音量减小（更快）
  web_game_dir = 2;   // 贪吃蛇向左
  server.send(200, "text/plain", "OK");
}

// 处理向右按钮
void handleRight() {
  web_enc_delta = 5;  // 音量增加（更快）
  web_game_dir = 3;   // 贪吃蛇向右
  server.send(200, "text/plain", "OK");
}

// 坦克游戏手柄 HTML 页面 — 虚拟摇杆 + 开火按钮
const char* tank_html = u8R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: Arial, sans-serif;
      background: #1a1a2e;
      color: #eee;
      text-align: center;
      height: 100dvh;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      touch-action: none;
      user-select: none;
      -webkit-user-select: none;
      overflow: hidden;
    }
    h2 { font-size: 20px; margin-bottom: 8px; letter-spacing: 2px; color: #e94560; }
    #joy-container {
      position: relative;
      width: 85vmin;
      max-width: 340px;
      aspect-ratio: 1;
      margin: 0 auto;
      touch-action: none;
    }
    #joy-base {
      position: absolute;
      top: 0; left: 0; width: 100%; height: 100%;
      border-radius: 50%;
      background: radial-gradient(circle, #1e2a4a, #0f1a30);
      border: 3px solid #2a3a6a;
      box-shadow: 0 0 40px rgba(0,0,0,0.5), inset 0 0 60px rgba(0,0,0,0.3);
      touch-action: none;
    }
    #joy-base::after {
      content: '';
      position: absolute;
      top: 50%; left: 50%;
      width: 30%; height: 30%;
      transform: translate(-50%, -50%);
      border-radius: 50%;
      border: 1px solid rgba(255,255,255,0.06);
    }
    /* 十字准线 */
    #joy-base .crosshair-h,
    #joy-base .crosshair-v {
      position: absolute;
      background: rgba(255,255,255,0.05);
    }
    #joy-base .crosshair-h { top: 50%; left: 10%; width: 80%; height: 1px; transform: translateY(-50%); }
    #joy-base .crosshair-v { left: 50%; top: 10%; height: 80%; width: 1px; transform: translateX(-50%); }
    /* 方向标签 */
    #joy-base .label { position: absolute; font-size: 12px; font-weight: bold; color: rgba(255,255,255,0.15); }
    #joy-base .label-t { top: 6%; left: 50%; transform: translateX(-50%); }
    #joy-base .label-b { bottom: 6%; left: 50%; transform: translateX(-50%); }
    #joy-base .label-l { left: 6%; top: 50%; transform: translateY(-50%); }
    #joy-base .label-r { right: 6%; top: 50%; transform: translateY(-50%); }
    /* 摇杆旋钮 */
    #joy-knob {
      position: absolute;
      top: 50%; left: 50%;
      width: 35%; height: 35%;
      transform: translate(-50%, -50%);
      border-radius: 50%;
      background: radial-gradient(circle at 35% 35%, #4a7aff, #2a4acc);
      box-shadow: 0 0 20px rgba(74,122,255,0.5), inset 0 -4px 8px rgba(0,0,0,0.3);
      transition: box-shadow 0.15s;
      touch-action: none;
      pointer-events: none;
    }
    #joy-knob::after {
      content: '';
      position: absolute;
      top: 25%; left: 30%;
      width: 25%; height: 20%;
      border-radius: 50%;
      background: rgba(255,255,255,0.2);
    }
    .joy-active #joy-knob {
      box-shadow: 0 0 40px rgba(74,122,255,0.8), inset 0 -4px 8px rgba(0,0,0,0.3);
    }
    /* 开火按钮 */
    #btnFireWrap {
      margin: 12px auto 8px;
      touch-action: none;
    }
    #btnFire {
      width: 80vmin;
      max-width: 280px;
      height: 64px;
      border: none;
      border-radius: 32px;
      background: linear-gradient(180deg, #e94560, #c23152);
      color: #fff;
      font-size: 22px;
      font-weight: bold;
      letter-spacing: 4px;
      cursor: pointer;
      box-shadow: 0 0 30px rgba(233,69,96,0.3);
      touch-action: none;
      -webkit-tap-highlight-color: transparent;
      transition: transform 0.08s, box-shadow 0.08s;
    }
    #btnFire:active, #btnFire.pressed {
      transform: scale(0.94);
      box-shadow: 0 0 50px rgba(233,69,96,0.6);
    }
    #status {
      padding: 6px 12px;
      background: rgba(255,255,255,0.04);
      border-radius: 8px;
      font-family: monospace;
      font-size: 12px;
      white-space: pre-wrap;
      width: 85vmin;
      max-width: 340px;
      min-height: 30px;
      line-height: 1.4;
      color: #888;
    }
  </style>
</head>
<body>
  <h2>⚡ TANK CONTROLLER</h2>
  <div id="joy-container">
    <div id="joy-base">
      <div class="crosshair-h"></div>
      <div class="crosshair-v"></div>
      <div class="label label-t">▲ FWD</div>
      <div class="label label-b">▼ BWD</div>
      <div class="label label-l">◀ LEFT</div>
      <div class="label label-r">RIGHT ▶</div>
    </div>
    <div id="joy-knob"></div>
  </div>
  <div id="btnFireWrap"><button id="btnFire">🔥 FIRE</button></div>
  <div id="status">Drag the joystick to drive the tank</div>
  <script>
    (function(){
      var container = document.getElementById('joy-container');
      var knob = document.getElementById('joy-knob');
      var fireBtn = document.getElementById('btnFire');
      var statusDiv = document.getElementById('status');

      var R = 1; // radius ratio (1 = full base radius)
      var cx = 0, cy = 0; // normalized center
      var active = false;
      var touchId = null;
      var lastX = 0, lastY = 0;

      // --- Send joystick position to ESP32 ---
      function sendJoy(x, y) {
        x = Math.max(-1, Math.min(1, x));
        y = Math.max(-1, Math.min(1, y));
        // Y → speed (forward/backward), X → turn (left/right)
        fetch('/tank/joy?x=' + x.toFixed(3) + '&y=' + y.toFixed(3));
      }

      // --- Reset joystick to center ---
      function resetJoy() {
        knob.style.transform = 'translate(-50%, -50%)';
        container.classList.remove('joy-active');
        if (active) {
          active = false;
          touchId = null;
          sendJoy(0, 0); // stop
        }
      }

      // --- Update knob position from touch/mouse coordinates ---
      function updatePos(clientX, clientY) {
        var rect = container.getBoundingClientRect();
        var baseR = Math.min(rect.width, rect.height) / 2;
        var dx = clientX - (rect.left + rect.width/2);
        var dy = clientY - (rect.top + rect.height/2);
        var dist = Math.sqrt(dx*dx + dy*dy);
        var maxR = baseR * 0.85; // 85% of base radius = max displacement
        var nx = dx / maxR;
        var ny = dy / maxR;
        var d = Math.sqrt(nx*nx + ny*ny);
        if (d > 1) { nx /= d; ny /= d; d = 1; }
        // Move knob
        var px = nx * maxR / baseR * 50;
        var py = ny * maxR / baseR * 50;
        knob.style.transform = 'translate(calc(-50% + ' + px + 'px), calc(-50% + ' + (-py) + 'px))';
        container.classList.add('joy-active');
        // Send normalized values (Y inverted: screen down = -1, screen up = +1 for forward)
        sendJoy(nx, -ny);
        lastX = nx; lastY = -ny;
      }

      // --- Pointer Events (unified mouse + touch) ---
      container.addEventListener('pointerdown', function(e) {
        e.preventDefault();
        container.setPointerCapture(e.pointerId);
        active = true;
        updatePos(e.clientX, e.clientY);
      });
      container.addEventListener('pointermove', function(e) {
        if (!active) return;
        e.preventDefault();
        updatePos(e.clientX, e.clientY);
      });
      container.addEventListener('pointerup', function(e) {
        e.preventDefault();
        resetJoy();
      });
      container.addEventListener('pointerleave', function(e) {
        if (active) {
          resetJoy();
        }
      });

      // --- Fire button ---
      fireBtn.addEventListener('pointerdown', function(e) {
        e.preventDefault();
        fireBtn.classList.add('pressed');
        fetch('/tank/fire');
      });
      fireBtn.addEventListener('pointerup', function(e) {
        fireBtn.classList.remove('pressed');
      });
      fireBtn.addEventListener('pointerleave', function(e) {
        fireBtn.classList.remove('pressed');
      });

      // --- Status polling ---
      setInterval(function(){
        fetch('/tank-status').then(function(r){ return r.text(); }).then(function(s){
          statusDiv.innerText = s;
        });
      }, 400);
    })();
  </script>
</body>
</html>
)rawliteral";

// 处理坦克控制页面
void handleTankPage() {
  server.send(200, "text/html; charset=utf-8", tank_html);
}

// 处理坦克方向控制 (兼容旧版方向按钮，保留)
void handleTankSpeed() {
  if (server.hasArg("d")) {
    int d = server.arg("d").toInt();
    web_tank_speed_val = constrain((float)d, -1.0f, 1.0f);
  }
  server.send(200, "text/plain", "OK");
}

void handleTankTurn() {
  if (server.hasArg("d")) {
    int d = server.arg("d").toInt();
    web_tank_turn_val = constrain((float)d, -1.0f, 1.0f);
  }
  server.send(200, "text/plain", "OK");
}

// 处理坦克虚拟摇杆输入 (全向模拟值)
void handleTankJoy() {
  if (server.hasArg("x")) {
    web_tank_turn_val = constrain(server.arg("x").toFloat(), -1.0f, 1.0f);
  }
  if (server.hasArg("y")) {
    web_tank_speed_val = constrain(server.arg("y").toFloat(), -1.0f, 1.0f);
  }
  server.send(200, "text/plain", "OK");
}

// 处理坦克开火
void handleTankFire() {
  web_tank_fire = true;
  server.send(200, "text/plain", "OK");
}

// 处理坦克状态查询
void handleTankStatus() {
  String status = current_ui_status;
  status += "\n---\nJoystick: ";
  if (fabs(web_tank_speed_val) < 0.01f && fabs(web_tank_turn_val) < 0.01f) {
    status += "CENTER";
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), "SPD=%.2f TURN=%.2f", (double)web_tank_speed_val, (double)web_tank_turn_val);
    status += buf;
  }
  server.send(200, "text/plain; charset=utf-8", status);
}

// 乒乓球游戏手柄 HTML 页面 — 横向触摸滑块
const char* pong_html = u8R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: Arial, sans-serif;
      background: #1a1a2e;
      color: #eee;
      text-align: center;
      height: 100dvh;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      touch-action: none;
      user-select: none;
      -webkit-user-select: none;
      overflow: hidden;
    }
    h2 { font-size: 20px; margin-bottom: 12px; letter-spacing: 2px; color: #4ae0a0; }
    #slider-wrap {
      width: 90vmin;
      max-width: 400px;
      margin: 20px auto;
      touch-action: none;
    }
    #slider-track {
      position: relative;
      width: 100%;
      height: 80px;
      background: linear-gradient(180deg, #1e2a4a, #0f1a30);
      border: 2px solid #2a3a6a;
      border-radius: 40px;
      touch-action: none;
      overflow: hidden;
    }
    #slider-fill {
      position: absolute;
      top: 0; left: 0;
      height: 100%;
      background: linear-gradient(180deg, rgba(74,224,160,0.3), rgba(74,224,160,0.1));
      border-radius: 40px;
      pointer-events: none;
      transition: width 0.05s linear;
    }
    #slider-knob {
      position: absolute;
      top: 50%;
      width: 60px; height: 60px;
      transform: translate(-50%, -50%);
      border-radius: 50%;
      background: radial-gradient(circle at 35% 35%, #4ae0a0, #2a8a60);
      box-shadow: 0 0 20px rgba(74,224,160,0.5);
      pointer-events: none;
    }
    #paddle-pos-label {
      font-family: monospace;
      font-size: 14px;
      color: #4ae0a0;
      margin-top: 8px;
    }
    #status {
      padding: 6px 12px;
      background: rgba(255,255,255,0.04);
      border-radius: 8px;
      font-family: monospace;
      font-size: 12px;
      white-space: pre-wrap;
      width: 85vmin;
      max-width: 340px;
      min-height: 30px;
      line-height: 1.4;
      color: #888;
      margin-top: 12px;
    }
    .hint {
      color: rgba(255,255,255,0.25);
      font-size: 12px;
      margin-top: 10px;
      letter-spacing: 1px;
    }
  </style>
</head>
<body>
  <h2>🏓 PONG CONTROLLER</h2>
  <div id="slider-wrap">
    <div id="slider-track">
      <div id="slider-fill"></div>
      <div id="slider-knob"></div>
    </div>
    <div id="paddle-pos-label">PADDLE: 0.50</div>
  </div>
  <div class="hint">◀ SLIDE LEFT / RIGHT ▶</div>
  <div id="status">Move the slider to control your paddle</div>
  <script>
    (function(){
      var track = document.getElementById('slider-track');
      var knob = document.getElementById('slider-knob');
      var fill = document.getElementById('slider-fill');
      var label = document.getElementById('paddle-pos-label');
      var statusDiv = document.getElementById('status');
      var active = false;
      var pos = 0.5; // default center

      function sendPaddle(p) {
        p = Math.max(0, Math.min(1, p));
        fetch('/pong/move?pos=' + p.toFixed(3));
      }

      function updateUI(clientX) {
        var rect = track.getBoundingClientRect();
        var relX = clientX - rect.left;
        pos = relX / rect.width;
        pos = Math.max(0, Math.min(1, pos));
        // Visual update
        knob.style.left = (pos * 100) + '%';
        fill.style.width = (pos * 100) + '%';
        label.innerText = 'PADDLE: ' + pos.toFixed(3);
        sendPaddle(pos);
      }

      // Pointer Events (unified touch + mouse)
      track.addEventListener('pointerdown', function(e) {
        e.preventDefault();
        track.setPointerCapture(e.pointerId);
        active = true;
        updateUI(e.clientX);
      });
      track.addEventListener('pointermove', function(e) {
        if (!active) return;
        e.preventDefault();
        updateUI(e.clientX);
      });
      track.addEventListener('pointerup', function(e) {
        e.preventDefault();
        active = false;
      });
      track.addEventListener('pointerleave', function(e) {
        active = false;
      });

      // Status polling
      setInterval(function(){
        fetch('/pong-status').then(function(r){ return r.text(); }).then(function(s){
          statusDiv.innerText = s;
        });
      }, 500);
    })();
  </script>
</body>
</html>
)rawliteral";

// 处理乒乓球控制页面
void handlePongPage() {
  server.send(200, "text/html; charset=utf-8", pong_html);
}

// 处理乒乓球球拍位置
void handlePongMove() {
  if (server.hasArg("pos")) {
    web_pong_paddle = constrain(server.arg("pos").toFloat(), 0.0f, 1.0f);
    web_pong_active = true;
  }
  server.send(200, "text/plain", "OK");
}

// 处理乒乓球状态查询
void handlePongStatus() {
  String status = current_ui_status;
  status += "\n---\nPaddle: ";
  char buf[32];
  snprintf(buf, sizeof(buf), "%.3f", (double)web_pong_paddle);
  status += buf;
  server.send(200, "text/plain; charset=utf-8", status);
}

// 处理确认按钮
void handleEnter() {
  web_btn_pressed = true;
  server.send(200, "text/plain", "OK");
}

// Web服务器任务函数
void webServerTask(void* pvParameters) {
  // 获取MAC地址并生成SSID
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[32];
  sprintf(ssid, "ESP32_Game_%02X:%02X", mac[4], mac[5]);
  
  // 启动AP模式
  WiFi.softAP(ssid, "12345678");
  IPAddress myIP = WiFi.softAPIP();
  USBSerial.print("AP IP address: ");
  USBSerial.println(myIP);

  // 初始化LittleFS文件系统
  if(!LittleFS.begin(true)){
      USBSerial.println("An Error has occurred while mounting LittleFS");
  } else {
      USBSerial.println("LittleFS mounted successfully");
      // 列出文件用于调试
      File root = LittleFS.open("/");
      File file = root.openNextFile();
      while(file){
          USBSerial.print("FILE: ");
          USBSerial.println(file.name());
          file = root.openNextFile();
      }
  }

  // 注册URL处理函数
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/up", handleUp);
  server.on("/down", handleDown);
  server.on("/left", handleLeft);
  server.on("/right", handleRight);
  server.on("/enter", handleEnter);
  
  // 坦克游戏手柄路由
  server.on("/tank", handleTankPage);
  server.on("/tank/joy", handleTankJoy);
  server.on("/tank/speed", handleTankSpeed);
  server.on("/tank/turn", handleTankTurn);
  server.on("/tank/fire", handleTankFire);
  server.on("/tank-status", handleTankStatus);

  // 乒乓球游戏手柄路由
  server.on("/pong", handlePongPage);
  server.on("/pong/move", handlePongMove);
  server.on("/pong-status", handlePongStatus);

  // 提供背景图片静态文件
  server.serveStatic("/background.jpg", LittleFS, "/background.jpg");

  // 启动服务器
  server.begin();
  USBSerial.println("HTTP server started");

  // 主循环：处理客户端请求
  while (1) {
    server.handleClient();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// 初始化Web服务器
void initWebServer() {
  xTaskCreatePinnedToCore(
    webServerTask,
    "WebServerTask",
    4096,
    NULL,
    1,
    &s_webServerTaskHandle,
    0 // 核心0
  );
}

void suspendWebServer() {
    if (s_webServerTaskHandle != NULL) {
        vTaskSuspend(s_webServerTaskHandle);
    }
}

void resumeWebServer() {
    if (s_webServerTaskHandle != NULL) {
        vTaskResume(s_webServerTaskHandle);
    }
}