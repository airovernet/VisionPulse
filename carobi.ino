#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp32cam.h>
#include "variables.h"
#include "webpage.h"

// Wi-Fi credentials
const char* WIFI_SSID = "SEON";
const char* WIFI_PASS = "asd123567";

// I2C pins for OLED
#define I2C_SDA 15
#define I2C_SCL 14
TwoWire I2Cbus = TwoWire(0);

// Servo pins
Servo headServo;   // GPIO12
Servo leftServo;   // GPIO13
Servo rightServo;  // GPIO2
#define HEAD_SERVO_PIN 12
#define LEFT_SERVO_PIN 13
#define RIGHT_SERVO_PIN 2

// Servo position limits and tracking
#define HEAD_MIN_ANGLE 0
#define HEAD_MAX_ANGLE 180
int currentHeadAngle = 30;  // Starting at middle position

// Display defines
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define SCREEN_ADDRESS  0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2Cbus, OLED_RESET);

// Web server
WebServer server(80);

// Camera resolution
static auto hiRes = esp32cam::Resolution::find(800, 600);

// Emoji bitmaps from variables.h
const int bitmaps_len = 4;
const unsigned char* bitmaps[bitmaps_len] = {
    neutral,
    neutral_right,
    neutral_left,
    blink_low
};

// All bitmaps from variables.h
const int all_bitmaps_len = 24;
const unsigned char* all_bitmaps[all_bitmaps_len] = {
    neutral,
    blink_low,
    angry,
    annoyed,
    awe,
    focused,
    frustrated,
    furious,
    glee,
    happy,
    neutral_left,
    neutral_right,
    sad_down,
    sad_up,
    scaret,
    skeptic,
    sleepy,
    squint,
    surprised,
    suspicious,
    unimpressed,
    wink_left,
    wink_right,
    worried
};

bool isMoving = false;
unsigned long moveStartTime = 0;
int moveduration = 300;
volatile bool streamActive = false;
bool debugEnabled = false;  // Debug status tracking
String debugStatus = "Debug: Off";  // Initial debug status

int oledIndex = 1;
String oledText = "text";

static unsigned long lastUpdate = 0;
int intervalUpdate = 1000;

// MJPEG stream task
void streamTask(void *pvParameters) {
  WiFiClient client = *(WiFiClient*)pvParameters;
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  client.print(response);

  while (client.connected() && streamActive) {
    auto frame = esp32cam::capture();
    if (frame == nullptr) {
      Serial.println("STREAM CAPTURE FAIL");
      continue;
    }

    String header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ";
    header += frame->size();
    header += "\r\n\r\n";
    client.print(header);
    client.write(frame->data(), frame->size());
    client.print("\r\n");
    delay(50); // ~20 FPS, adjust as needed
  }
  streamActive = false;
  client.stop();
  vTaskDelete(NULL);
}

void startStream() {
  if (!streamActive) {
    streamActive = true;
    WiFiClient client = server.client();
    xTaskCreate(streamTask, "StreamTask", 8192, (void*)&client, 1, NULL);
    debugStatus = "Stream: On";
    if (debugEnabled) updateOledDebug();
  }
}

void stopStream() {
  streamActive = false;
  debugStatus = "Stream: Off";
  if (debugEnabled) updateOledDebug();
}

void handleSnapshot() {
  auto frame = esp32cam::capture();
  if (frame == nullptr) {
    Serial.println("SNAPSHOT CAPTURE FAIL");
    server.send(500, "text/plain", "Failed to capture image");
    return;
  }

  WiFiClient client = server.client();
  if (!client.connected()) {
    Serial.println("Client disconnected before sending snapshot");
    return;
  }

  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: image/jpeg\r\n");
  client.print("Content-Length: ");
  client.println(frame->size());
  client.print("Content-Disposition: inline; filename=\"snapshot.jpg\"\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");

  size_t bytesWritten = client.write(frame->data(), frame->size());
  if (bytesWritten != frame->size()) {
    Serial.printf("Error: Only wrote %d of %d bytes\n", bytesWritten, frame->size());
  } else {
    Serial.println("Snapshot captured and sent successfully");
    debugStatus = "Snapshot Taken";
    if (debugEnabled) updateOledDebug();
  }
  client.flush();
  client.stop();
}

void serveHtml() {
  server.send(200, "text/html", htmlPage);
}

void updateOledDebug() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(debugStatus);
  display.display();
}

void handleOled() {
  display.clearDisplay();
  display.drawBitmap(0, 0, all_bitmaps[oledIndex], SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.display();
  debugStatus = "Emoji: " + String(oledIndex);
  if (debugEnabled) updateOledDebug();
}

void handleOled1() {
  lastUpdate = millis();
  intervalUpdate = 2000;
  oledIndex = 2;
  handleOled();
  debugStatus = "Emoji: Angry";
  if (debugEnabled) updateOledDebug();
  server.send(200, "text/plain", "OLED set to Angry");
}

void handleOled2() {
  lastUpdate = millis();
  intervalUpdate = 2000;
  oledIndex = 9;
  handleOled();
  debugStatus = "Emoji: Happy";
  if (debugEnabled) updateOledDebug();
  server.send(200, "text/plain", "OLED set to Happy");
}

void handleText() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(oledText);
  display.display();
  debugStatus = "Text: " + oledText;
  if (debugEnabled) updateOledDebug();
  Serial.println(oledText + " Command Received");
  server.send(200, "text/plain", "OLED updated: " + oledText);
}

void handleSendText() {
  if (server.method() == HTTP_POST) {
    String text = server.arg("text");
    if (text.length() > 0) {
      oledText = text;
      handleText();
    } else {
      server.send(400, "text/plain", "No text provided");
    }
  } else {
    server.send(405, "text/plain", "Method not allowed");
  }
}

void handleToggleStream() {
  if (server.method() == HTTP_POST) {
    String state = server.arg("state");
    if (state == "on") {
      startStream();
      server.send(200, "text/plain", "Stream started");
    } else {
      stopStream();
      server.send(200, "text/plain", "Stream stopped");
    }
  } else {
    server.send(405, "text/plain", "Method not allowed");
  }
}

void handleToggleDebug() {
  if (server.method() == HTTP_POST) {
    String state = server.arg("state");
    debugEnabled = (state == "on");
    debugStatus = debugEnabled ? "Debug: On" : "Debug: Off";
    if (debugEnabled) {
      updateOledDebug();
    } else {
      handleOled(); // Revert to default OLED display
    }
    server.send(200, "text/plain", "Debug " + String(debugEnabled ? "enabled" : "disabled"));
  } else {
    server.send(405, "text/plain", "Method not allowed");
  }
}

void handleHeadUp() {
  int newAngle = currentHeadAngle + 10;
  if (newAngle <= HEAD_MAX_ANGLE) {
    currentHeadAngle = newAngle;
    headServo.write(currentHeadAngle);
    Serial.printf("Head Up to %d degrees\n", currentHeadAngle);
    debugStatus = "Head: " + String(currentHeadAngle) + " deg";
    if (debugEnabled) updateOledDebug();
    server.send(200, "text/plain", "Head tilted up to " + String(currentHeadAngle) + " degrees");
  } else {
    Serial.println("Head at maximum angle");
    debugStatus = "Head: Max Angle";
    if (debugEnabled) updateOledDebug();
    server.send(200, "text/plain", "Head at maximum angle (" + String(HEAD_MAX_ANGLE) + " degrees)");
  }
}

void handleHeadDown() {
  int newAngle = currentHeadAngle - 10;
  if (newAngle >= HEAD_MIN_ANGLE) {
    currentHeadAngle = newAngle;
    headServo.write(currentHeadAngle);
    Serial.printf("Head Down to %d degrees\n", currentHeadAngle);
    debugStatus = "Head: " + String(currentHeadAngle) + " deg";
    if (debugEnabled) updateOledDebug();
    server.send(200, "text/plain", "Head tilted down to " + String(currentHeadAngle) + " degrees");
  } else {
    Serial.println("Head at minimum angle");
    debugStatus = "Head: Min Angle";
    if (debugEnabled) updateOledDebug();
    server.send(200, "text/plain", "Head at minimum angle (" + String(HEAD_MIN_ANGLE) + " degrees)");
  }
}

void handleMoveForward() {
  moveduration = 200;
  isMoving = true;
  moveStartTime = millis();
  leftServo.attach(LEFT_SERVO_PIN);
  rightServo.attach(RIGHT_SERVO_PIN);
  leftServo.write(0);
  rightServo.write(180);
  Serial.println("Move Forward Command Received");
  debugStatus = "Moving Forward";
  if (debugEnabled) updateOledDebug();
  server.send(200, "text/plain", "Moving forward");
}

void handleMoveBackward() {
  moveduration = 200;
  isMoving = true;
  moveStartTime = millis();
  leftServo.attach(LEFT_SERVO_PIN);
  rightServo.attach(RIGHT_SERVO_PIN);
  leftServo.write(180);
  rightServo.write(0);
  Serial.println("Move Backward Command Received");
  debugStatus = "Moving Backward";
  if (debugEnabled) updateOledDebug();
  server.send(200, "text/plain", "Moving backward");
}

void handleRotateLeft() {
  moveduration = 30;
  isMoving = true;
  moveStartTime = millis();
  leftServo.attach(LEFT_SERVO_PIN);
  rightServo.attach(RIGHT_SERVO_PIN);
  leftServo.write(0);
  rightServo.write(0);
  Serial.println("Rotate Left Command Received");
  debugStatus = "Rotating Left";
  if (debugEnabled) updateOledDebug();
  server.send(200, "text/plain", "Rotating left");
}

void handleRotateRight() {
  moveduration = 30;
  isMoving = true;
  moveStartTime = millis();
  leftServo.attach(LEFT_SERVO_PIN);
  rightServo.attach(RIGHT_SERVO_PIN);
  leftServo.write(180);
  rightServo.write(180);
  Serial.println("Rotate Right Command Received");
  debugStatus = "Rotating Right";
  if (debugEnabled) updateOledDebug();
  server.send(200, "text/plain", "Rotating right");
}

void setup() {
  Serial.begin(115200);

  // Initialize I2C
  I2Cbus.begin(I2C_SDA, I2C_SCL, 100000);

  // Initialize OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 OLED display failed to initialize.");
    while (true);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Starting...");
  display.display();

  // Camera initialization
  {
    using namespace esp32cam;
    Config cfg;
    cfg.setPins(pins::AiThinker);
    cfg.setResolution(hiRes);
    cfg.setBufferCount(2);
    cfg.setJpeg(80);
    bool ok = Camera.begin(cfg);
    Serial.println(ok ? "CAMERA OK" : "CAMERA FAIL");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.print("Camera: ");
    display.println(ok ? "CAMERA OK" : "CAMERA FAIL");
    display.display();
  }

  // Wi-Fi initialization
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // Display IP address
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.display();
  delay(3000);

  Serial.print("http://");
  Serial.println(WiFi.localIP());

  // Web server routes
  server.on("/", serveHtml);
  server.on("/stream", startStream);
  server.on("/snapshot", handleSnapshot);
  server.on("/oled1", handleOled1);
  server.on("/oled2", handleOled2);
  server.on("/sendText", HTTP_POST, handleSendText);
  server.on("/up", handleHeadUp);
  server.on("/down", handleHeadDown);
  server.on("/movef", handleMoveForward);
  server.on("/moveb", handleMoveBackward);
  server.on("/movel", handleRotateLeft);
  server.on("/mover", handleRotateRight);
  server.on("/toggleStream", HTTP_POST, handleToggleStream);
  server.on("/toggleDebug", HTTP_POST, handleToggleDebug);

  server.begin();

  // Servo initialization
  headServo.attach(HEAD_SERVO_PIN);
  leftServo.attach(LEFT_SERVO_PIN);
  rightServo.attach(RIGHT_SERVO_PIN);
  
  // Set initial head position
  headServo.write(currentHeadAngle);
}

void loop() {
  if (millis() - lastUpdate > intervalUpdate && !debugEnabled) {
    lastUpdate = millis();
    display.clearDisplay();
    int randomIndex = random(bitmaps_len);
    display.drawBitmap(0, 0, bitmaps[randomIndex], SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    display.display();
    intervalUpdate = 1000;
  }

  server.handleClient();

  if (isMoving && millis() - moveStartTime >= moveduration) {
    leftServo.detach();
    rightServo.detach();
    isMoving = false;
    debugStatus = "Movement Stopped";
    if (debugEnabled) updateOledDebug();
  }
}