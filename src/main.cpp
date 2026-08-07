#include <Arduino.h>

namespace
{
// AI Thinker ESP32-CAM pins that are not used by the camera in this test.
constexpr uint8_t kMotorIn1Pin = 13;
constexpr uint8_t kMotorIn2Pin = 14;
constexpr uint8_t kStatusLedPin = 4;
constexpr uint32_t kRunLimitMs = 2000;
constexpr uint32_t kStandaloneWaitMs = 25000;

bool motorRunning = false;
uint32_t motorStartedAt = 0;

void stopMotor()
{
  digitalWrite(kMotorIn1Pin, LOW);
  digitalWrite(kMotorIn2Pin, LOW);
  motorRunning = false;
  Serial.println("Motor stopped.");
}

void runMotor(bool forward)
{
  digitalWrite(kMotorIn1Pin, forward ? HIGH : LOW);
  digitalWrite(kMotorIn2Pin, forward ? LOW : HIGH);
  motorStartedAt = millis();
  motorRunning = true;

  Serial.printf("Motor command: %s (automatic stop in %lu ms)\n",
                forward ? "forward" : "reverse",
                static_cast<unsigned long>(kRunLimitMs));
}

void printHelp()
{
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  f = forward for 2 seconds");
  Serial.println("  r = reverse for 2 seconds");
  Serial.println("  s = stop immediately");
  Serial.println("  h = show this help");
}

void runStandaloneTest()
{
  Serial.println();
  Serial.println("Standalone test starts in 30 seconds.");
  Serial.println("The flash LED will blink during the final 5 seconds.");
  delay(kStandaloneWaitMs);

  for (uint8_t count = 0; count < 5; ++count)
  {
    digitalWrite(kStatusLedPin, HIGH);
    delay(200);
    digitalWrite(kStatusLedPin, LOW);
    delay(800);
  }

  runMotor(true);
  delay(kRunLimitMs);
  stopMotor();

  delay(3000);

  runMotor(false);
  delay(kRunLimitMs);
  stopMotor();

  Serial.println("Standalone test complete. Motor output remains disabled.");
}
} // namespace

void setup()
{
  // Load LOW into both GPIO output latches before enabling output mode.
  digitalWrite(kMotorIn1Pin, LOW);
  digitalWrite(kMotorIn2Pin, LOW);
  digitalWrite(kStatusLedPin, LOW);
  pinMode(kMotorIn1Pin, OUTPUT);
  pinMode(kMotorIn2Pin, OUTPUT);
  pinMode(kStatusLedPin, OUTPUT);

  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=== Day 3: TC1508A single motor test ===");
  Serial.printf("IN1 control pin: GPIO %u\n", kMotorIn1Pin);
  Serial.printf("IN2 control pin: GPIO %u\n", kMotorIn2Pin);
  Serial.println("Motor output is disabled after every 2-second run.");
  printHelp();
  runStandaloneTest();
}

void loop()
{
  if (motorRunning && millis() - motorStartedAt >= kRunLimitMs)
  {
    stopMotor();
  }

  if (Serial.available() == 0)
  {
    delay(10);
    return;
  }

  const char command = static_cast<char>(Serial.read());

  switch (command)
  {
  case 'f':
  case 'F':
    runMotor(true);
    break;

  case 'r':
  case 'R':
    runMotor(false);
    break;

  case 's':
  case 'S':
    stopMotor();
    break;

  case 'h':
  case 'H':
    printHelp();
    break;

  case '\r':
  case '\n':
  case ' ':
    break;

  default:
    Serial.printf("Unknown command: %c\n", command);
    printHelp();
    break;
  }
}
