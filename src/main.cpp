#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_system.h>

namespace
{
constexpr char kAccessPointName[] = "ESP32-CAM-Car";
constexpr char kAccessPointPassword[] = "esp32cam";

// Four-motor mapping verified during Part 6.
constexpr uint8_t kLeftIn1Pin = 13;
constexpr uint8_t kLeftIn2Pin = 14;
constexpr uint8_t kRightIn1Pin = 12;
constexpr uint8_t kRightIn2Pin = 15;
constexpr uint8_t kFlashLedPin = 4;

// The browser renews an active command every 200 ms. Either safety limit stops
// the motors independently of the browser's release event.
constexpr uint32_t kCommunicationTimeoutMs = 500;
constexpr uint32_t kActionLimitMs = 1500;

// AI Thinker ESP32-CAM camera pin map verified during Part 2.
constexpr int kPinPwdn = 32;
constexpr int kPinReset = -1;
constexpr int kPinXclk = 0;
constexpr int kPinSiod = 26;
constexpr int kPinSioc = 27;
constexpr int kPinY9 = 35;
constexpr int kPinY8 = 34;
constexpr int kPinY7 = 39;
constexpr int kPinY6 = 36;
constexpr int kPinY5 = 21;
constexpr int kPinY4 = 19;
constexpr int kPinY3 = 18;
constexpr int kPinY2 = 5;
constexpr int kPinVsync = 25;
constexpr int kPinHref = 23;
constexpr int kPinPclk = 22;

enum class SideDirection : uint8_t
{
  stop,
  directionA,
  directionB,
};

enum class Motion : uint8_t
{
  stop,
  bothA,
  bothB,
  pivotA,
  pivotB,
};

httpd_handle_t pageServer = nullptr;
httpd_handle_t streamServer = nullptr;

portMUX_TYPE driveMux = portMUX_INITIALIZER_UNLOCKED;
Motion activeMotion = Motion::stop;
bool driveRunning = false;
bool safetyStopLatched = false;
uint32_t driveStartedAt = 0;
uint32_t lastCommandAt = 0;

constexpr char kIndexPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
  <title>ESP32-CAM 四轮小车</title>
  <style>
    :root { color-scheme: dark; }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: #111827;
      color: #f9fafb;
      font-family: system-ui, sans-serif;
    }
    main {
      width: min(94vw, 720px);
      margin: 0 auto;
      padding: 18px 0 28px;
      text-align: center;
    }
    h1 { margin: 0 0 12px; font-size: 1.45rem; }
    #camera {
      display: block;
      width: 100%;
      min-height: 220px;
      object-fit: contain;
      background: #000;
      border-radius: 12px;
    }
    #status {
      min-height: 1.5em;
      margin: 12px 0;
      color: #86efac;
      font-weight: 650;
    }
    .controls {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 12px;
    }
    button {
      min-height: 72px;
      border: 1px solid #475569;
      border-radius: 12px;
      background: #1e293b;
      color: #f8fafc;
      font: inherit;
      font-weight: 700;
      touch-action: none;
      user-select: none;
      -webkit-user-select: none;
    }
    button.active { background: #2563eb; border-color: #60a5fa; }
    .stop {
      grid-column: 1 / -1;
      min-height: 60px;
      background: #991b1b;
      border-color: #ef4444;
    }
    .note {
      margin: 14px 4px 0;
      color: #cbd5e1;
      font-size: .92rem;
      line-height: 1.45;
    }
  </style>
</head>
<body>
  <main>
    <h1>ESP32-CAM 四轮小车</h1>
    <img id="camera" alt="摄像头实时画面">
    <div id="status" role="status">已停止</div>
    <div class="controls">
      <button data-command="both-a">双侧方向 A</button>
      <button data-command="both-b">双侧方向 B</button>
      <button data-command="pivot-a">原地组合 A</button>
      <button data-command="pivot-b">原地组合 B</button>
      <button class="stop" id="stop" type="button">立即停止</button>
    </div>
    <p class="note">按住按钮移动，松手立即停止。一次按住最长 1.5 秒；达到上限后请松开，再重新按住。方向 A/B 的实际车身方向需装轮后验证。</p>
  </main>
  <script>
    const camera = document.getElementById("camera");
    const statusText = document.getElementById("status");
    const motionButtons = [...document.querySelectorAll("[data-command]")];
    let activeCommand = null;
    let activeButton = null;
    let controlWorker = null;

    camera.src = "http://" + location.hostname + ":81/stream";

    const wait = (milliseconds) =>
      new Promise((resolve) => setTimeout(resolve, milliseconds));

    async function sendCommand(command, keepalive = false) {
      const response = await fetch(
        "/control?command=" + encodeURIComponent(command) + "&_=" + Date.now(),
        { cache: "no-store", keepalive }
      );
      if (!response.ok) throw new Error("HTTP " + response.status);
    }

    function showStopped(message = "已停止") {
      statusText.textContent = message;
      statusText.style.color = "#86efac";
      if (activeButton) activeButton.classList.remove("active");
      activeButton = null;
    }

    async function holdCommand(command, button) {
      let safetyMessage = false;
      activeCommand = command;
      activeButton = button;
      button.classList.add("active");
      statusText.textContent = "正在执行：" + button.textContent;
      statusText.style.color = "#93c5fd";

      try {
        while (activeCommand === command) {
          await sendCommand(command);
          // Check the release state every 20 ms while keeping a 200 ms
          // command-renewal interval. This keeps stop behind the last completed
          // movement request instead of racing it on another HTTP connection.
          for (let count = 0; count < 10 && activeCommand === command; ++count) {
            await wait(20);
          }
        }
      } catch (error) {
        activeCommand = null;
        safetyMessage = true;
        statusText.textContent = "安全停车：控制通信中断或动作已到上限";
        statusText.style.color = "#fca5a5";
      }

      try {
        await sendCommand("stop", true);
        if (!safetyMessage) showStopped();
      } catch (error) {
        statusText.textContent = "连接中断：车端将在 0.5 秒内自动停车";
        statusText.style.color = "#fca5a5";
      }

      if (activeButton) activeButton.classList.remove("active");
      activeButton = null;
    }

    function startHolding(command, button, event) {
      event.preventDefault();
      if (controlWorker) return;
      if (button.setPointerCapture && event.pointerId !== undefined) {
        button.setPointerCapture(event.pointerId);
      }
      controlWorker = holdCommand(command, button)
        .finally(() => { controlWorker = null; });
    }

    function releaseControl(event) {
      if (event) event.preventDefault();
      activeCommand = null;
    }

    for (const button of motionButtons) {
      button.addEventListener("pointerdown", (event) =>
        startHolding(button.dataset.command, button, event));
      button.addEventListener("pointerup", releaseControl);
      button.addEventListener("pointercancel", releaseControl);
      button.addEventListener("lostpointercapture", releaseControl);
      button.addEventListener("contextmenu", (event) => event.preventDefault());
    }

    document.getElementById("stop").addEventListener("click", () => {
      releaseControl();
      if (controlWorker) {
        statusText.textContent = "正在停车";
      } else {
        sendCommand("stop", true).then(() => showStopped()).catch(() => {
          statusText.textContent = "连接中断：车端将在 0.5 秒内自动停车";
          statusText.style.color = "#fca5a5";
        });
      }
    });

    window.addEventListener("blur", releaseControl);
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) releaseControl();
    });
    window.addEventListener("pagehide", () => {
      activeCommand = null;
      sendCommand("stop", true).catch(() => {});
    });
  </script>
</body>
</html>
)HTML";

constexpr char kStreamContentType[] =
    "multipart/x-mixed-replace;boundary=frame";
constexpr char kStreamBoundary[] = "\r\n--frame\r\n";
constexpr char kStreamPart[] =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void setSide(uint8_t in1Pin, uint8_t in2Pin, SideDirection direction)
{
  switch (direction)
  {
  case SideDirection::directionA:
    digitalWrite(in1Pin, HIGH);
    digitalWrite(in2Pin, LOW);
    break;
  case SideDirection::directionB:
    digitalWrite(in1Pin, LOW);
    digitalWrite(in2Pin, HIGH);
    break;
  case SideDirection::stop:
    digitalWrite(in1Pin, LOW);
    digitalWrite(in2Pin, LOW);
    break;
  }
}

void applyMotion(Motion motion)
{
  switch (motion)
  {
  case Motion::bothA:
    setSide(kLeftIn1Pin, kLeftIn2Pin, SideDirection::directionA);
    setSide(kRightIn1Pin, kRightIn2Pin, SideDirection::directionA);
    break;
  case Motion::bothB:
    setSide(kLeftIn1Pin, kLeftIn2Pin, SideDirection::directionB);
    setSide(kRightIn1Pin, kRightIn2Pin, SideDirection::directionB);
    break;
  case Motion::pivotA:
    setSide(kLeftIn1Pin, kLeftIn2Pin, SideDirection::directionB);
    setSide(kRightIn1Pin, kRightIn2Pin, SideDirection::directionA);
    break;
  case Motion::pivotB:
    setSide(kLeftIn1Pin, kLeftIn2Pin, SideDirection::directionA);
    setSide(kRightIn1Pin, kRightIn2Pin, SideDirection::directionB);
    break;
  case Motion::stop:
    setSide(kLeftIn1Pin, kLeftIn2Pin, SideDirection::stop);
    setSide(kRightIn1Pin, kRightIn2Pin, SideDirection::stop);
    break;
  }
}

const char *motionLabel(Motion motion)
{
  switch (motion)
  {
  case Motion::bothA:
    return "both sides direction A";
  case Motion::bothB:
    return "both sides direction B";
  case Motion::pivotA:
    return "pivot combination A";
  case Motion::pivotB:
    return "pivot combination B";
  case Motion::stop:
    return "stop";
  }
  return "unknown";
}

bool requestMotion(Motion requestedMotion)
{
  const uint32_t now = millis();
  bool accepted = true;
  bool stateChanged = false;

  portENTER_CRITICAL(&driveMux);
  if (requestedMotion == Motion::stop)
  {
    applyMotion(Motion::stop);
    activeMotion = Motion::stop;
    driveRunning = false;
    safetyStopLatched = false;
    stateChanged = true;
  }
  else if (safetyStopLatched)
  {
    accepted = false;
  }
  else if (driveRunning && requestedMotion != activeMotion)
  {
    // A direction change must pass through an explicit stopped state. This
    // prevents alternating requests from resetting the hard action limit.
    applyMotion(Motion::stop);
    activeMotion = Motion::stop;
    driveRunning = false;
    safetyStopLatched = true;
    accepted = false;
  }
  else
  {
    if (!driveRunning)
    {
      applyMotion(requestedMotion);
      activeMotion = requestedMotion;
      driveStartedAt = now;
      driveRunning = true;
      stateChanged = true;
    }
    lastCommandAt = now;
  }
  portEXIT_CRITICAL(&driveMux);

  if (accepted && stateChanged)
  {
    Serial.printf("Drive command: %s\n", motionLabel(requestedMotion));
  }
  return accepted;
}

void checkDriveSafety()
{
  const uint32_t now = millis();
  bool communicationExpired = false;
  bool actionExpired = false;

  portENTER_CRITICAL(&driveMux);
  if (driveRunning)
  {
    communicationExpired = now - lastCommandAt >= kCommunicationTimeoutMs;
    actionExpired = now - driveStartedAt >= kActionLimitMs;

    if (communicationExpired || actionExpired)
    {
      applyMotion(Motion::stop);
      activeMotion = Motion::stop;
      driveRunning = false;
      safetyStopLatched = true;
    }
  }
  portEXIT_CRITICAL(&driveMux);

  if (communicationExpired)
  {
    Serial.println("Safety stop: control communication timeout.");
  }
  else if (actionExpired)
  {
    Serial.println("Safety stop: action time limit reached.");
  }
}

bool parseMotion(httpd_req_t *request, Motion &motion)
{
  char query[64];
  char command[16];
  const size_t queryLength = httpd_req_get_url_query_len(request);

  if (queryLength == 0 || queryLength >= sizeof(query) ||
      httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "command", command, sizeof(command)) != ESP_OK)
  {
    return false;
  }

  if (strcmp(command, "both-a") == 0)
    motion = Motion::bothA;
  else if (strcmp(command, "both-b") == 0)
    motion = Motion::bothB;
  else if (strcmp(command, "pivot-a") == 0)
    motion = Motion::pivotA;
  else if (strcmp(command, "pivot-b") == 0)
    motion = Motion::pivotB;
  else if (strcmp(command, "stop") == 0)
    motion = Motion::stop;
  else
    return false;

  return true;
}

bool startCamera()
{
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = kPinY2;
  config.pin_d1 = kPinY3;
  config.pin_d2 = kPinY4;
  config.pin_d3 = kPinY5;
  config.pin_d4 = kPinY6;
  config.pin_d5 = kPinY7;
  config.pin_d6 = kPinY8;
  config.pin_d7 = kPinY9;
  config.pin_xclk = kPinXclk;
  config.pin_pclk = kPinPclk;
  config.pin_vsync = kPinVsync;
  config.pin_href = kPinHref;
  config.pin_sccb_sda = kPinSiod;
  config.pin_sccb_scl = kPinSioc;
  config.pin_pwdn = kPinPwdn;
  config.pin_reset = kPinReset;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode =
      psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location =
      psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK)
  {
    Serial.printf("Camera init failed: 0x%x\n", result);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  Serial.printf("Camera sensor PID: 0x%04x\n", sensor->id.PID);

  if (sensor->id.PID == OV3660_PID)
  {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -2);
    Serial.println("OV3660 detected and image correction applied.");
  }
  else
  {
    Serial.println("Warning: the detected sensor is not OV3660.");
  }

  return true;
}

esp_err_t handleIndex(httpd_req_t *request)
{
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, kIndexPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handleControl(httpd_req_t *request)
{
  Motion requestedMotion = Motion::stop;
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");

  if (!parseMotion(request, requestedMotion))
  {
    httpd_resp_set_status(request, "400 Bad Request");
    return httpd_resp_sendstr(
        request, "{\"ok\":false,\"error\":\"invalid command\"}");
  }

  if (!requestMotion(requestedMotion))
  {
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_sendstr(
        request, "{\"ok\":false,\"error\":\"safety stop latched\"}");
  }

  return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t handleStream(httpd_req_t *request)
{
  esp_err_t result = httpd_resp_set_type(request, kStreamContentType);
  if (result != ESP_OK) return result;

  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  char partHeader[64];

  while (true)
  {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == nullptr)
    {
      Serial.println("Camera frame capture failed.");
      return ESP_FAIL;
    }

    const size_t headerLength =
        snprintf(partHeader, sizeof(partHeader), kStreamPart, frame->len);
    result = httpd_resp_send_chunk(
        request, kStreamBoundary, strlen(kStreamBoundary));
    if (result == ESP_OK)
      result = httpd_resp_send_chunk(request, partHeader, headerLength);
    if (result == ESP_OK)
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char *>(frame->buf), frame->len);

    esp_camera_fb_return(frame);
    if (result != ESP_OK) break;
    delay(1);
  }

  return result;
}

bool startWebServers()
{
  const httpd_uri_t indexUri = {
      .uri = "/", .method = HTTP_GET, .handler = handleIndex, .user_ctx = nullptr};
  const httpd_uri_t controlUri = {
      .uri = "/control", .method = HTTP_GET, .handler = handleControl, .user_ctx = nullptr};
  const httpd_uri_t streamUri = {
      .uri = "/stream", .method = HTTP_GET, .handler = handleStream, .user_ctx = nullptr};

  httpd_config_t pageConfig = HTTPD_DEFAULT_CONFIG();
  pageConfig.server_port = 80;
  if (httpd_start(&pageServer, &pageConfig) != ESP_OK)
  {
    Serial.println("Failed to start the page server.");
    return false;
  }
  if (httpd_register_uri_handler(pageServer, &indexUri) != ESP_OK ||
      httpd_register_uri_handler(pageServer, &controlUri) != ESP_OK)
  {
    Serial.println("Failed to register a page server handler.");
    return false;
  }

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = 81;
  streamConfig.ctrl_port = pageConfig.ctrl_port + 1;
  if (httpd_start(&streamServer, &streamConfig) != ESP_OK)
  {
    Serial.println("Failed to start the stream server.");
    return false;
  }
  if (httpd_register_uri_handler(streamServer, &streamUri) != ESP_OK)
  {
    Serial.println("Failed to register the stream handler.");
    return false;
  }

  return true;
}
} // namespace

void setup()
{
  // Preload LOW before enabling outputs so every driver input starts disabled.
  digitalWrite(kLeftIn1Pin, LOW);
  digitalWrite(kLeftIn2Pin, LOW);
  digitalWrite(kRightIn1Pin, LOW);
  digitalWrite(kRightIn2Pin, LOW);
  digitalWrite(kFlashLedPin, LOW);

  pinMode(kLeftIn1Pin, OUTPUT);
  pinMode(kLeftIn2Pin, OUTPUT);
  pinMode(kRightIn1Pin, OUTPUT);
  pinMode(kRightIn2Pin, OUTPUT);
  pinMode(kFlashLedPin, OUTPUT);
  applyMotion(Motion::stop);

  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=== ESP32-CAM four-motor web controller ===");
  Serial.printf("Chip model : %s\n", ESP.getChipModel());
  Serial.printf("PSRAM      : %s\n", psramFound() ? "available" : "not found");
  Serial.printf("Reset code : %d\n", esp_reset_reason());
  Serial.printf("Left driver: GPIO %u / GPIO %u\n", kLeftIn1Pin, kLeftIn2Pin);
  Serial.printf("Right driver: GPIO %u / GPIO %u\n", kRightIn1Pin, kRightIn2Pin);
  Serial.println("Keep motor power off until this startup has completed.");
  Serial.println("Never restart the ESP32 while the motor drivers are powered.");

  if (!startCamera())
  {
    Serial.println("Startup stopped because the camera could not start.");
    return;
  }

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAP(kAccessPointName, kAccessPointPassword))
  {
    Serial.println("Failed to create the Wi-Fi access point.");
    return;
  }
  if (!startWebServers())
  {
    Serial.println("Startup stopped because a web server could not start.");
    return;
  }

  Serial.println();
  Serial.println("Camera and control page are ready. Motor outputs are stopped.");
  Serial.printf("Wi-Fi name : %s\n", kAccessPointName);
  Serial.printf("Password   : %s\n", kAccessPointPassword);
  Serial.printf("Open       : http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Safety     : %lu ms communication timeout, %lu ms action limit\n",
                static_cast<unsigned long>(kCommunicationTimeoutMs),
                static_cast<unsigned long>(kActionLimitMs));
}

void loop()
{
  static uint32_t lastHeartbeat = 0;
  checkDriveSafety();

  if (millis() - lastHeartbeat >= 5000)
  {
    lastHeartbeat = millis();
    Serial.printf("System heartbeat | clients %u | free heap %u\n",
                  WiFi.softAPgetStationNum(), ESP.getFreeHeap());
  }

  delay(10);
}
