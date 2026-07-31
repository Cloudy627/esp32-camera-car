#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_system.h>

namespace
{
constexpr char kAccessPointName[] = "ESP32-CAM-Car";
constexpr char kAccessPointPassword[] = "esp32cam";

// AI Thinker ESP32-CAM camera pin map.
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

httpd_handle_t pageServer = nullptr;
httpd_handle_t streamServer = nullptr;

constexpr char kIndexPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-CAM 小车</title>
  <style>
    body {
      margin: 0;
      min-height: 100vh;
      display: grid;
      place-items: center;
      background: #111827;
      color: #f9fafb;
      font-family: system-ui, sans-serif;
    }
    main {
      width: min(92vw, 720px);
      text-align: center;
    }
    img {
      display: block;
      width: 100%;
      min-height: 220px;
      margin-top: 18px;
      object-fit: contain;
      background: #000;
      border-radius: 12px;
    }
    p { color: #cbd5e1; }
  </style>
</head>
<body>
  <main>
    <h1>ESP32-CAM 实时画面</h1>
    <p>看到连续画面，就表示第 2 天测试成功。</p>
    <img id="camera" alt="摄像头实时画面">
  </main>
  <script>
    document.getElementById("camera").src =
      "http://" + location.hostname + ":81/stream";
  </script>
</body>
</html>
)HTML";

constexpr char kStreamContentType[] =
    "multipart/x-mixed-replace;boundary=frame";
constexpr char kStreamBoundary[] = "\r\n--frame\r\n";
constexpr char kStreamPart[] =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

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
  if (result != ESP_OK)
  {
    return result;
  }

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
    {
      result = httpd_resp_send_chunk(request, partHeader, headerLength);
    }
    if (result == ESP_OK)
    {
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char *>(frame->buf), frame->len);
    }

    esp_camera_fb_return(frame);

    if (result != ESP_OK)
    {
      break;
    }

    delay(1);
  }

  return result;
}

bool startWebServers()
{
  const httpd_uri_t indexUri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = handleIndex,
      .user_ctx = nullptr,
  };

  const httpd_uri_t streamUri = {
      .uri = "/stream",
      .method = HTTP_GET,
      .handler = handleStream,
      .user_ctx = nullptr,
  };

  httpd_config_t pageConfig = HTTPD_DEFAULT_CONFIG();
  pageConfig.server_port = 80;

  if (httpd_start(&pageServer, &pageConfig) != ESP_OK)
  {
    Serial.println("Failed to start the page server.");
    return false;
  }

  if (httpd_register_uri_handler(pageServer, &indexUri) != ESP_OK)
  {
    Serial.println("Failed to register the page handler.");
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
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=== ESP32-CAM OV3660 stream test ===");
  Serial.printf("Chip model : %s\n", ESP.getChipModel());
  Serial.printf("PSRAM      : %s\n", psramFound() ? "available" : "not found");
  Serial.printf("Reset code : %d\n", esp_reset_reason());

  if (!startCamera())
  {
    Serial.println("Test stopped because the camera could not start.");
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
    Serial.println("Test stopped because the web server could not start.");
    return;
  }

  Serial.println();
  Serial.println("Camera is ready.");
  Serial.printf("Wi-Fi name : %s\n", kAccessPointName);
  Serial.printf("Password   : %s\n", kAccessPointPassword);
  Serial.printf("Open       : http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void loop()
{
  static uint32_t lastHeartbeat = 0;

  if (millis() - lastHeartbeat >= 5000)
  {
    lastHeartbeat = millis();
    Serial.printf("Camera heartbeat | clients %u | free heap %u\n",
                  WiFi.softAPgetStationNum(),
                  ESP.getFreeHeap());
  }

  delay(20);
}
