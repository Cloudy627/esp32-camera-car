#include <Arduino.h>

namespace
{
constexpr uint8_t kLeftIn1Pin = 13;
constexpr uint8_t kLeftIn2Pin = 14;
constexpr uint8_t kRightIn1Pin = 12;
constexpr uint8_t kRightIn2Pin = 15;
constexpr uint8_t kStatusLedPin = 4;

constexpr uint32_t kRunLimitMs = 1500;
constexpr uint32_t kPauseMs = 2000;
constexpr uint32_t kStandaloneWaitMs = 25000;

enum class Direction : int8_t
{
  reverse = -1,
  stop = 0,
  forward = 1,
};

bool driveRunning = false;
uint32_t driveStartedAt = 0;

void setSide(uint8_t in1Pin, uint8_t in2Pin, Direction direction)
{
  switch (direction)
  {
  case Direction::forward:
    digitalWrite(in1Pin, HIGH);
    digitalWrite(in2Pin, LOW);
    break;
  case Direction::reverse:
    digitalWrite(in1Pin, LOW);
    digitalWrite(in2Pin, HIGH);
    break;
  case Direction::stop:
    digitalWrite(in1Pin, LOW);
    digitalWrite(in2Pin, LOW);
    break;
  }
}

void stopDrive()
{
  setSide(kLeftIn1Pin, kLeftIn2Pin, Direction::stop);
  setSide(kRightIn1Pin, kRightIn2Pin, Direction::stop);
  driveRunning = false;
  Serial.println("Drive stopped.");
}

void runDrive(Direction left, Direction right, const char *label)
{
  setSide(kLeftIn1Pin, kLeftIn2Pin, left);
  setSide(kRightIn1Pin, kRightIn2Pin, right);
  driveStartedAt = millis();
  driveRunning = true;
  Serial.printf("Drive command: %s (automatic stop in %lu ms)\n",
                label,
                static_cast<unsigned long>(kRunLimitMs));
}

void runTestStep(Direction left, Direction right, const char *label)
{
  runDrive(left, right, label);
  delay(kRunLimitMs);
  stopDrive();
  delay(kPauseMs);
}

void printHelp()
{
  Serial.println();
  Serial.println("Commands (each movement stops automatically):");
  Serial.println("  w = both sides direction A");
  Serial.println("  x = both sides direction B");
  Serial.println("  a = pivot combination A");
  Serial.println("  d = pivot combination B");
  Serial.println("  s = stop immediately");
  Serial.println("  h = show this help");
}

void runStandaloneTest()
{
  Serial.println();
  Serial.println("Standalone test starts in 30 seconds.");
  Serial.println("Keep motor power off until the ESP32 has started.");
  Serial.println("The flash LED will blink during the final 5 seconds.");
  delay(kStandaloneWaitMs);

  for (uint8_t count = 0; count < 5; ++count)
  {
    digitalWrite(kStatusLedPin, HIGH);
    delay(200);
    digitalWrite(kStatusLedPin, LOW);
    delay(800);
  }

  runTestStep(Direction::forward, Direction::stop, "left side A");
  runTestStep(Direction::reverse, Direction::stop, "left side B");
  runTestStep(Direction::stop, Direction::forward, "right side A");
  runTestStep(Direction::stop, Direction::reverse, "right side B");
  runTestStep(Direction::forward, Direction::forward, "both sides A");
  runTestStep(Direction::reverse, Direction::reverse, "both sides B");
  runTestStep(Direction::reverse, Direction::forward, "pivot A");
  runTestStep(Direction::forward, Direction::reverse, "pivot B");

  stopDrive();
  Serial.println("Standalone test complete. All motor outputs remain disabled.");
}
} // namespace

void setup()
{
  // Preload LOW before enabling outputs so every driver input starts disabled.
  digitalWrite(kLeftIn1Pin, LOW);
  digitalWrite(kLeftIn2Pin, LOW);
  digitalWrite(kRightIn1Pin, LOW);
  digitalWrite(kRightIn2Pin, LOW);
  digitalWrite(kStatusLedPin, LOW);

  pinMode(kLeftIn1Pin, OUTPUT);
  pinMode(kLeftIn2Pin, OUTPUT);
  pinMode(kRightIn1Pin, OUTPUT);
  pinMode(kRightIn2Pin, OUTPUT);
  pinMode(kStatusLedPin, OUTPUT);

  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=== Part 4: four-motor drive test ===");
  Serial.printf("Left driver: GPIO %u / GPIO %u\n", kLeftIn1Pin, kLeftIn2Pin);
  Serial.printf("Right driver: GPIO %u / GPIO %u\n", kRightIn1Pin, kRightIn2Pin);
  Serial.println("Every movement is limited to 1.5 seconds.");
  printHelp();
  runStandaloneTest();
}

void loop()
{
  if (driveRunning && millis() - driveStartedAt >= kRunLimitMs)
  {
    stopDrive();
  }

  if (Serial.available() == 0)
  {
    delay(10);
    return;
  }

  const char command = static_cast<char>(Serial.read());

  switch (command)
  {
  case 'w':
  case 'W':
    runDrive(Direction::forward, Direction::forward, "both sides A");
    break;
  case 'x':
  case 'X':
    runDrive(Direction::reverse, Direction::reverse, "both sides B");
    break;
  case 'a':
  case 'A':
    runDrive(Direction::reverse, Direction::forward, "pivot A");
    break;
  case 'd':
  case 'D':
    runDrive(Direction::forward, Direction::reverse, "pivot B");
    break;
  case 's':
  case 'S':
    stopDrive();
    break;
  case 'h':
  case 'H':
    printHelp();
    break;
  case '\r':
  case '\n':
    break;
  default:
    Serial.printf("Unknown command: %c\n", command);
    printHelp();
    break;
  }
}
