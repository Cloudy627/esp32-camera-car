#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <new>

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

// The browser uses one persistent WebSocket and sends a heartbeat while a
// control is held. Both limits are enforced here, independently of the page.
constexpr uint32_t kCommunicationTimeoutMs = 500;
constexpr uint32_t kActionLimitMs = 5000;
constexpr size_t kMaximumWebSocketMessageLength = 31;

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

enum class SafetyStopReason : uint8_t
{
  none,
  communicationTimeout,
  actionLimit,
  directionChange,
};

struct ControlSession
{
  int socket;
};

httpd_handle_t pageServer = nullptr;
httpd_handle_t streamServer = nullptr;

portMUX_TYPE driveMux = portMUX_INITIALIZER_UNLOCKED;
Motion activeMotion = Motion::stop;
bool driveRunning = false;
bool safetyStopLatched = false;
SafetyStopReason safetyStopReason = SafetyStopReason::none;
uint32_t driveStartedAt = 0;
uint32_t lastCommandAt = 0;
int activeControlSocket = -1;

portMUX_TYPE streamMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t streamFrameCount = 0;
uint32_t streamStartCount = 0;
uint32_t streamEndCount = 0;
uint32_t streamCaptureFailureCount = 0;
uint16_t activeStreamCount = 0;
uint32_t bootId = 0;

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
    #control-status {
      min-height: 1.5em;
      margin: 12px 0;
      color: #86efac;
      font-weight: 650;
    }
    .telemetry {
      margin: 5px 0;
      color: #cbd5e1;
      font-size: .88rem;
      line-height: 1.35;
    }
    #diagnostic { color: #fcd34d; min-height: 2.4em; }
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
    button:disabled { opacity: .5; }
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
    <div id="control-status" role="status">正在连接控制通道</div>
    <p class="telemetry" id="device-status">车端状态：等待连接</p>
    <p class="telemetry" id="video-status">视频状态：正在连接</p>
    <p class="telemetry" id="diagnostic"></p>
    <div class="controls">
      <button data-command="both-a" disabled>双侧方向 A</button>
      <button data-command="both-b" disabled>双侧方向 B</button>
      <button data-command="pivot-a" disabled>原地组合 A</button>
      <button data-command="pivot-b" disabled>原地组合 B</button>
      <button class="stop" id="stop" type="button">立即停止</button>
    </div>
    <p class="note">按住按钮移动，松手立即停止。一次按住最长 5 秒；达到上限后必须松开，再重新按住。方向 A/B 的实际车身方向需装轮后验证。</p>
    <p class="note">启动约束：ESP32-CAM 启动时必须关闭电机电源；驱动板供电期间不得重启或复位 ESP32-CAM。</p>
  </main>
  <script>
    const camera = document.getElementById("camera");
    const controlStatus = document.getElementById("control-status");
    const deviceStatus = document.getElementById("device-status");
    const videoStatus = document.getElementById("video-status");
    const diagnostic = document.getElementById("diagnostic");
    const motionButtons = [...document.querySelectorAll("[data-command]")];

    let socket = null;
    let socketReconnectTimer = null;
    let socketReconnectDelay = 500;
    let pageClosing = false;
    let activeCommand = null;
    let activeButton = null;
    let pointerHeld = false;
    let releaseRequired = false;
    let lastBootId = sessionStorage.getItem("esp32-cam-boot-id");

    let videoReconnectTimer = null;
    let videoReconnectDelay = 1000;
    let videoNeedsReconnect = false;
    let lastStreamFrames = null;
    let lastVideoProgressAt = Date.now();
    let lastVideoAttemptAt = 0;

    function setControlStatus(message, color = "#86efac") {
      controlStatus.textContent = message;
      controlStatus.style.color = color;
    }

    function clearActiveButton() {
      if (activeButton) activeButton.classList.remove("active");
      activeButton = null;
    }

    function setControlsEnabled(enabled) {
      for (const button of motionButtons) button.disabled = !enabled;
    }

    function socketIsOpen() {
      return socket && socket.readyState === WebSocket.OPEN;
    }

    function sendSocket(message) {
      if (!socketIsOpen()) return false;
      socket.send(message);
      return true;
    }

    function beginMotion(command, button, event) {
      event.preventDefault();
      if (!socketIsOpen()) {
        setControlStatus("控制通道未连接，无法启动电机", "#fca5a5");
        return;
      }
      if (pointerHeld || activeCommand || releaseRequired) return;
      if (button.setPointerCapture && event.pointerId !== undefined) {
        button.setPointerCapture(event.pointerId);
      }

      pointerHeld = true;
      activeCommand = command;
      activeButton = button;
      button.classList.add("active");
      setControlStatus("正在执行：" + button.textContent, "#93c5fd");
      if (!sendSocket("move:" + command)) {
        pointerHeld = false;
        activeCommand = null;
        clearActiveButton();
      }
    }

    function releaseControl(event) {
      if (event) event.preventDefault();
      const shouldSendStop = pointerHeld || activeCommand || releaseRequired;
      pointerHeld = false;
      activeCommand = null;
      releaseRequired = false;
      clearActiveButton();
      if (shouldSendStop && sendSocket("stop")) {
        setControlStatus("已停止");
      } else if (shouldSendStop) {
        setControlStatus("控制连接中断，车端将在约 0.5 秒内自动停车", "#fca5a5");
      }
    }

    for (const button of motionButtons) {
      button.addEventListener("pointerdown", (event) =>
        beginMotion(button.dataset.command, button, event));
      button.addEventListener("pointerup", releaseControl);
      button.addEventListener("pointercancel", releaseControl);
      button.addEventListener("lostpointercapture", releaseControl);
      button.addEventListener("contextmenu", (event) => event.preventDefault());
    }

    document.getElementById("stop").addEventListener("click", () => {
      const wasActive = pointerHeld || activeCommand || releaseRequired;
      pointerHeld = false;
      activeCommand = null;
      releaseRequired = false;
      clearActiveButton();
      if (sendSocket("stop")) {
        setControlStatus("已停止");
      } else {
        setControlStatus(
          wasActive ? "控制连接中断，车端将在约 0.5 秒内自动停车" : "控制通道未连接",
          "#fca5a5"
        );
      }
    });

    setInterval(() => {
      if (pointerHeld && activeCommand) sendSocket("heartbeat");
    }, 200);

    setInterval(() => {
      if (socketIsOpen()) sendSocket("status");
    }, 2000);

    function handleDeviceStatus(message) {
      const uptimeSeconds = Math.floor(message.uptimeMs / 1000);
      deviceStatus.textContent =
        "车端在线 · 启动标识 " + message.boot + " · 已运行 " + uptimeSeconds + " 秒";

      if (lastBootId && lastBootId !== message.boot) {
        diagnostic.textContent =
          "检测到启动标识变化：ESP32 在本页面会话期间发生过重启；请结合串口 Reset code 排查原因。";
      }
      lastBootId = message.boot;
      sessionStorage.setItem("esp32-cam-boot-id", message.boot);

      const now = Date.now();
      if (lastStreamFrames === null) {
        lastStreamFrames = message.streamFrames;
        lastVideoProgressAt = now;
      } else if (message.streamFrames !== lastStreamFrames) {
        lastStreamFrames = message.streamFrames;
        lastVideoProgressAt = now;
        videoNeedsReconnect = false;
        if (videoReconnectTimer) {
          clearTimeout(videoReconnectTimer);
          videoReconnectTimer = null;
        }
        videoReconnectDelay = 1000;
        videoStatus.textContent = "视频状态：帧持续更新";
        if (diagnostic.textContent.includes("视频流单独中断")) {
          diagnostic.textContent = "视频已自动恢复；本次记录不能单独确定是网络、HTTP 负载、电源波动还是电机干扰。";
        }
      } else if (message.streamActive === 0 && now - lastVideoAttemptAt > 1500) {
        scheduleVideoReconnect("车端没有活动的视频流");
      } else if (now - lastVideoProgressAt > 6500) {
        scheduleVideoReconnect("超过 6.5 秒未检测到新视频帧");
      }
    }

    function handleSocketMessage(event) {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch (error) {
        setControlStatus("收到无法识别的车端状态", "#fca5a5");
        return;
      }

      if (message.type === "status") {
        handleDeviceStatus(message);
      } else if (message.type === "accepted" && activeCommand) {
        setControlStatus("正在执行：" + activeButton.textContent, "#93c5fd");
      } else if (message.type === "stopped" && !activeCommand) {
        setControlStatus("已停止");
      } else if (message.type === "locked") {
        activeCommand = null;
        releaseRequired = true;
        clearActiveButton();
        const detail = message.reason === "action-limit"
          ? "已达到 5 秒动作上限"
          : "控制心跳中断或方向切换被阻止";
        setControlStatus("安全停车：" + detail + "；必须松开后再按", "#fca5a5");
      } else if (message.type === "error") {
        setControlStatus("控制命令被拒绝", "#fca5a5");
      }
    }

    function scheduleSocketReconnect() {
      if (pageClosing || socketReconnectTimer) return;
      const delay = socketReconnectDelay;
      socketReconnectDelay = Math.min(socketReconnectDelay * 2, 8000);
      socketReconnectTimer = setTimeout(() => {
        socketReconnectTimer = null;
        connectSocket();
      }, delay);
    }

    function connectSocket() {
      if (pageClosing || document.hidden ||
          (socket && (socket.readyState === WebSocket.CONNECTING || socketIsOpen()))) return;
      const newSocket = new WebSocket("ws://" + location.host + "/ws");
      socket = newSocket;
      newSocket.addEventListener("open", () => {
        if (socket !== newSocket) return;
        socketReconnectDelay = 500;
        setControlsEnabled(true);
        setControlStatus("控制通道已连接，电机保持停止");
        sendSocket("status");
      });
      newSocket.addEventListener("message", handleSocketMessage);
      newSocket.addEventListener("close", () => {
        if (socket !== newSocket) return;
        socket = null;
        setControlsEnabled(false);
        pointerHeld = false;
        activeCommand = null;
        releaseRequired = false;
        clearActiveButton();
        deviceStatus.textContent = "车端状态：控制连接已断开，正在重连";
        setControlStatus("控制连接中断，车端将立即或在约 0.5 秒内停车", "#fca5a5");
        diagnostic.textContent =
          "控制与视频若同时中断，可能是 ESP32 重启、热点/供电中断或手机链路中断；请查看热点是否仍存在及串口日志。";
        scheduleSocketReconnect();
      });
      newSocket.addEventListener("error", () => newSocket.close());
    }

    function scheduleVideoReconnect(reason) {
      if (pageClosing || videoReconnectTimer) return;
      videoNeedsReconnect = true;
      const delay = videoReconnectDelay + Math.floor(Math.random() * 250);
      videoReconnectDelay = Math.min(videoReconnectDelay * 2, 10000);
      videoStatus.textContent =
        "视频状态：" + reason + "，将在 " + (delay / 1000).toFixed(1) + " 秒后重连";
      diagnostic.textContent = socketIsOpen()
        ? "控制通道和热点仍在线，当前检测到视频流单独中断；自动重连不能证明电机干扰或电源波动已经解决。"
        : "控制与视频都已中断；需要结合热点是否消失、启动标识和串口 Reset code 区分原因。";
      videoReconnectTimer = setTimeout(() => {
        videoReconnectTimer = null;
        startVideo();
      }, delay);
    }

    function startVideo() {
      if (pageClosing || document.hidden) return;
      videoNeedsReconnect = false;
      lastVideoAttemptAt = Date.now();
      lastVideoProgressAt = lastVideoAttemptAt;
      videoStatus.textContent = "视频状态：正在连接";
      camera.src =
        "http://" + location.hostname + ":81/stream?attempt=" + lastVideoAttemptAt;
    }

    camera.crossOrigin = "anonymous";
    camera.addEventListener("load", () => {
      videoStatus.textContent = "视频状态：连接已建立，正在确认帧更新";
    });
    camera.addEventListener("error", () => scheduleVideoReconnect("视频连接失败或断流"));

    window.addEventListener("blur", releaseControl);
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) {
        releaseControl();
      } else {
        connectSocket();
        if (videoNeedsReconnect || !camera.currentSrc) startVideo();
      }
    });
    window.addEventListener("pagehide", () => {
      pageClosing = true;
      if (socketReconnectTimer) clearTimeout(socketReconnectTimer);
      if (videoReconnectTimer) clearTimeout(videoReconnectTimer);
      sendSocket("stop");
      if (socket) socket.close();
    });

    connectSocket();
    startVideo();
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
    stateChanged = driveRunning || safetyStopLatched;
    if (stateChanged) applyMotion(Motion::stop);
    activeMotion = Motion::stop;
    driveRunning = false;
    safetyStopLatched = false;
    safetyStopReason = SafetyStopReason::none;
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
    safetyStopReason = SafetyStopReason::directionChange;
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

bool refreshActiveMotion()
{
  bool refreshed = false;
  portENTER_CRITICAL(&driveMux);
  if (driveRunning && !safetyStopLatched)
  {
    lastCommandAt = millis();
    refreshed = true;
  }
  portEXIT_CRITICAL(&driveMux);
  return refreshed;
}

SafetyStopReason getSafetyStopReason()
{
  portENTER_CRITICAL(&driveMux);
  const SafetyStopReason reason = safetyStopReason;
  portEXIT_CRITICAL(&driveMux);
  return reason;
}

const char *safetyStopReasonLabel(SafetyStopReason reason)
{
  switch (reason)
  {
  case SafetyStopReason::communicationTimeout:
    return "communication-timeout";
  case SafetyStopReason::actionLimit:
    return "action-limit";
  case SafetyStopReason::directionChange:
    return "direction-change";
  case SafetyStopReason::none:
    return "not-running";
  }
  return "unknown";
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
      safetyStopReason = communicationExpired
                             ? SafetyStopReason::communicationTimeout
                             : SafetyStopReason::actionLimit;
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

bool parseMotion(const char *command, Motion &motion)
{
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

bool isActiveControlSocket(int socket)
{
  portENTER_CRITICAL(&driveMux);
  const bool active = activeControlSocket == socket;
  portEXIT_CRITICAL(&driveMux);
  return active;
}

int replaceControlSocket(int socket)
{
  portENTER_CRITICAL(&driveMux);
  const int previousSocket = activeControlSocket;
  applyMotion(Motion::stop);
  activeMotion = Motion::stop;
  driveRunning = false;
  safetyStopLatched = false;
  safetyStopReason = SafetyStopReason::none;
  activeControlSocket = socket;
  portEXIT_CRITICAL(&driveMux);
  return previousSocket;
}

void controlSessionClosed(void *context)
{
  ControlSession *session = static_cast<ControlSession *>(context);
  bool stoppedActiveControl = false;

  portENTER_CRITICAL(&driveMux);
  if (session != nullptr && activeControlSocket == session->socket)
  {
    applyMotion(Motion::stop);
    activeMotion = Motion::stop;
    driveRunning = false;
    safetyStopLatched = false;
    safetyStopReason = SafetyStopReason::none;
    activeControlSocket = -1;
    stoppedActiveControl = true;
  }
  portEXIT_CRITICAL(&driveMux);

  if (stoppedActiveControl)
  {
    Serial.println("Control WebSocket disconnected; motor outputs stopped.");
  }
  delete session;
}

esp_err_t sendWebSocketText(httpd_req_t *request, const char *text)
{
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(text));
  frame.len = strlen(text);
  return httpd_ws_send_frame(request, &frame);
}

esp_err_t sendControlStatus(httpd_req_t *request)
{
  uint32_t frames;
  uint32_t starts;
  uint32_t ends;
  uint32_t captureFailures;
  uint16_t activeStreams;

  portENTER_CRITICAL(&streamMux);
  frames = streamFrameCount;
  starts = streamStartCount;
  ends = streamEndCount;
  captureFailures = streamCaptureFailureCount;
  activeStreams = activeStreamCount;
  portEXIT_CRITICAL(&streamMux);

  char response[256];
  snprintf(response, sizeof(response),
           "{\"type\":\"status\",\"boot\":\"%08lx\",\"uptimeMs\":%lu,"
           "\"streamActive\":%u,\"streamFrames\":%lu,\"streamStarts\":%lu,"
           "\"streamEnds\":%lu,\"streamCaptureFailures\":%lu}",
           static_cast<unsigned long>(bootId),
           static_cast<unsigned long>(millis()), activeStreams,
           static_cast<unsigned long>(frames),
           static_cast<unsigned long>(starts),
           static_cast<unsigned long>(ends),
           static_cast<unsigned long>(captureFailures));
  return sendWebSocketText(request, response);
}

esp_err_t sendSafetyLocked(httpd_req_t *request)
{
  char response[96];
  snprintf(response, sizeof(response),
           "{\"type\":\"locked\",\"reason\":\"%s\"}",
           safetyStopReasonLabel(getSafetyStopReason()));
  return sendWebSocketText(request, response);
}

esp_err_t handleWebSocket(httpd_req_t *request)
{
  const int socket = httpd_req_to_sockfd(request);
  if (request->method == HTTP_GET)
  {
    ControlSession *session = new (std::nothrow) ControlSession{socket};
    if (session == nullptr) return ESP_ERR_NO_MEM;

    request->sess_ctx = session;
    request->free_ctx = controlSessionClosed;
    const int previousSocket = replaceControlSocket(socket);
    if (previousSocket >= 0 && previousSocket != socket)
    {
      httpd_sess_trigger_close(pageServer, previousSocket);
    }
    Serial.printf("Control WebSocket connected on socket %d; motor outputs stopped.\n",
                  socket);
    return ESP_OK;
  }

  if (!isActiveControlSocket(socket)) return ESP_FAIL;

  httpd_ws_frame_t frame = {};
  esp_err_t result = httpd_ws_recv_frame(request, &frame, 0);
  if (result != ESP_OK) return result;
  if (frame.type != HTTPD_WS_TYPE_TEXT || frame.fragmented ||
      frame.len > kMaximumWebSocketMessageLength)
  {
    sendWebSocketText(request, "{\"type\":\"error\",\"error\":\"invalid-frame\"}");
    return ESP_FAIL;
  }

  char message[kMaximumWebSocketMessageLength + 1] = {};
  frame.payload = reinterpret_cast<uint8_t *>(message);
  result = httpd_ws_recv_frame(request, &frame, kMaximumWebSocketMessageLength);
  if (result != ESP_OK) return result;
  message[frame.len] = '\0';

  if (strcmp(message, "status") == 0)
  {
    return sendControlStatus(request);
  }
  if (strcmp(message, "heartbeat") == 0)
  {
    return refreshActiveMotion() ? ESP_OK : sendSafetyLocked(request);
  }
  if (strcmp(message, "stop") == 0)
  {
    requestMotion(Motion::stop);
    return sendWebSocketText(request, "{\"type\":\"stopped\"}");
  }
  if (strncmp(message, "move:", 5) == 0)
  {
    Motion requestedMotion = Motion::stop;
    if (!parseMotion(message + 5, requestedMotion))
    {
      return sendWebSocketText(
          request, "{\"type\":\"error\",\"error\":\"invalid-command\"}");
    }
    if (!requestMotion(requestedMotion)) return sendSafetyLocked(request);
    return sendWebSocketText(request, "{\"type\":\"accepted\"}");
  }

  return sendWebSocketText(
      request, "{\"type\":\"error\",\"error\":\"unknown-message\"}");
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

esp_err_t handleStream(httpd_req_t *request)
{
  esp_err_t result = httpd_resp_set_type(request, kStreamContentType);
  if (result != ESP_OK) return result;

  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  char partHeader[64];

  portENTER_CRITICAL(&streamMux);
  ++streamStartCount;
  ++activeStreamCount;
  const uint32_t thisStart = streamStartCount;
  portEXIT_CRITICAL(&streamMux);
  Serial.printf("Video stream %lu connected.\n",
                static_cast<unsigned long>(thisStart));

  while (true)
  {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == nullptr)
    {
      portENTER_CRITICAL(&streamMux);
      ++streamCaptureFailureCount;
      portEXIT_CRITICAL(&streamMux);
      Serial.println("Camera frame capture failed.");
      result = ESP_FAIL;
      break;
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
    portENTER_CRITICAL(&streamMux);
    ++streamFrameCount;
    portEXIT_CRITICAL(&streamMux);
    delay(1);
  }

  portENTER_CRITICAL(&streamMux);
  if (activeStreamCount > 0) --activeStreamCount;
  ++streamEndCount;
  const uint32_t thisEnd = streamEndCount;
  portEXIT_CRITICAL(&streamMux);
  Serial.printf("Video stream ended (total ends %lu, result 0x%x).\n",
                static_cast<unsigned long>(thisEnd), result);

  return result;
}

bool startWebServers()
{
  const httpd_uri_t indexUri = {
      .uri = "/", .method = HTTP_GET, .handler = handleIndex, .user_ctx = nullptr};
  const httpd_uri_t websocketUri = {
      .uri = "/ws",
      .method = HTTP_GET,
      .handler = handleWebSocket,
      .user_ctx = nullptr,
      .is_websocket = true,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};
  const httpd_uri_t streamUri = {
      .uri = "/stream", .method = HTTP_GET, .handler = handleStream, .user_ctx = nullptr};

  httpd_config_t pageConfig = HTTPD_DEFAULT_CONFIG();
  pageConfig.server_port = 80;
  pageConfig.lru_purge_enable = true;
  if (httpd_start(&pageServer, &pageConfig) != ESP_OK)
  {
    Serial.println("Failed to start the page server.");
    return false;
  }
  if (httpd_register_uri_handler(pageServer, &indexUri) != ESP_OK ||
      httpd_register_uri_handler(pageServer, &websocketUri) != ESP_OK)
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
  bootId = esp_random();

  Serial.println();
  Serial.println("=== ESP32-CAM four-motor web controller ===");
  Serial.printf("Chip model : %s\n", ESP.getChipModel());
  Serial.printf("PSRAM      : %s\n", psramFound() ? "available" : "not found");
  Serial.printf("Reset code : %d\n", esp_reset_reason());
  Serial.printf("Boot ID    : %08lx\n", static_cast<unsigned long>(bootId));
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
  Serial.println("Camera and WebSocket control page are ready. Motor outputs are stopped.");
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
    int controlSocket;
    uint16_t streams;
    uint32_t frames;
    portENTER_CRITICAL(&driveMux);
    controlSocket = activeControlSocket;
    portEXIT_CRITICAL(&driveMux);
    portENTER_CRITICAL(&streamMux);
    streams = activeStreamCount;
    frames = streamFrameCount;
    portEXIT_CRITICAL(&streamMux);
    Serial.printf(
        "System heartbeat | boot %08lx | uptime %lu ms | clients %u | "
        "control ws %s | streams %u | frames %lu | free heap %u\n",
        static_cast<unsigned long>(bootId), static_cast<unsigned long>(millis()),
        WiFi.softAPgetStationNum(), controlSocket >= 0 ? "connected" : "none",
        streams, static_cast<unsigned long>(frames), ESP.getFreeHeap());
  }

  delay(10);
}
