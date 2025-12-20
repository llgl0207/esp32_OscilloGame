#include <WiFi.h>
#include <WebServer.h>
#include "freertos.h"

WebServer server(80);
String current_ui_status = "{}";

const char* html = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; margin:0px auto; padding-top: 30px; background-color: #222; color: #fff; }
    .button { display: inline-block; padding: 20px 30px; font-size: 24px; cursor: pointer; text-align: center; text-decoration: none; outline: none; color: #fff; background-color: #4CAF50; border: none; border-radius: 15px; box-shadow: 0 9px #999; margin: 10px; -webkit-user-select: none; user-select: none; }
    .button:active { background-color: #3e8e41; box-shadow: 0 5px #666; transform: translateY(4px); }
    .nav-btn { background-color: #008CBA; }
    .enter-btn { background-color: #f44336; }
    .row { display: flex; justify-content: center; align-items: center; }
    #ui-status { margin-top: 20px; padding: 10px; border: 1px solid #555; background: #333; text-align: left; font-family: monospace; white-space: pre-wrap; min-height: 100px; }
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
  <h1>ESP32 Game Control</h1>
  
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

  <div id="ui-status">Loading UI Status...</div>

  <p>Connect to WiFi: ESP32_Game_Controller / 12345678</p>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", html);
}

void handleStatus() {
  server.send(200, "text/plain", current_ui_status);
}

void updateWebUIStatus(String status) {
    current_ui_status = status;
}

void handleUp() {
  web_enc_delta = -1; // Menu Up / Prev
  web_game_dir = 0;   // Snake Up
  server.send(200, "text/plain", "OK");
}

void handleDown() {
  web_enc_delta = 1;  // Menu Down / Next
  web_game_dir = 1;   // Snake Down
  server.send(200, "text/plain", "OK");
}

void handleLeft() {
  web_enc_delta = -5; // Volume Down (Faster)
  web_game_dir = 2;   // Snake Left
  server.send(200, "text/plain", "OK");
}

void handleRight() {
  web_enc_delta = 5;  // Volume Up (Faster)
  web_game_dir = 3;   // Snake Right
  server.send(200, "text/plain", "OK");
}

void handleEnter() {
  web_btn_pressed = true;
  server.send(200, "text/plain", "OK");
}

void webServerTask(void* pvParameters) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[32];
  sprintf(ssid, "ESP32_Game_%02X:%02X", mac[4], mac[5]);
  
  WiFi.softAP(ssid, "12345678");
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/up", handleUp);
  server.on("/down", handleDown);
  server.on("/left", handleLeft);
  server.on("/right", handleRight);
  server.on("/enter", handleEnter);

  server.begin();
  Serial.println("HTTP server started");

  while (1) {
    server.handleClient();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void initWebServer() {
  xTaskCreatePinnedToCore(
    webServerTask,
    "WebServerTask",
    4096,
    NULL,
    1,
    NULL,
    0 // Core 0
  );
}
