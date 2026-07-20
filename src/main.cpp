#include <Arduino.h>
#include <esp_system.h>

void setup()
{
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=== ESP32-CAM minimum system test ===");
  Serial.printf("Chip model : %s\n", ESP.getChipModel());
  Serial.printf("CPU cores  : %d\n", ESP.getChipCores());
  Serial.printf("CPU freq   : %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash size : %u bytes\n", ESP.getFlashChipSize());
  Serial.printf("Free heap  : %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Reset reason code: %d\n", esp_reset_reason());
  Serial.println("System started successfully.");
}

void loop()
{
  static uint32_t heartbeat = 0;

  Serial.printf("Heartbeat %lu | uptime %lu ms | free heap %u\n",
                static_cast<unsigned long>(heartbeat++),
                static_cast<unsigned long>(millis()),
                ESP.getFreeHeap());

  delay(2000);
}
