#include <Arduino.h>
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <esp_task_wdt.h>

// Coil output pins
#define CoilUp     13
#define CoilDown   27
#define CoilLeft   26
#define CoilRight  25
#define StatusLED  14

// nRF24L01 pins
#define CE_PIN     4
#define CSN_PIN    5

RF24 radio(CE_PIN, CSN_PIN);
const byte address[6] = "00001";

// Wi-Fi credentials
const char* ssid     = "GTI-GUDANG";
const char* password = "Pipitq1006";

IPAddress local_IP(192, 168, 8, 141);
IPAddress gateway(192, 168, 8, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

struct ControlPacket {
  bool liftUp;
  bool liftDown;
  bool trolleyLeft;
  bool trolleyRight;
};

ControlPacket lastPacket = {false, false, false, false};
SemaphoreHandle_t packetMutex;

bool webOverrideActive  = false;
unsigned long webOverrideTimeout = 0;
const unsigned long WEB_OVERRIDE_DURATION = 5000;

unsigned long lastWSMessage = 0;
const unsigned long WS_SAFETY_TIMEOUT = 2000;

void stopAllCoils() {
  digitalWrite(CoilUp,    LOW);
  digitalWrite(CoilDown,  LOW);
  digitalWrite(CoilLeft,  LOW);
  digitalWrite(CoilRight, LOW);
}

void applyControl(ControlPacket packet) {
  digitalWrite(CoilUp,     packet.liftUp      ? HIGH : LOW);
  digitalWrite(CoilDown,   packet.liftDown    ? HIGH : LOW);
  digitalWrite(CoilLeft,   packet.trolleyLeft ? HIGH : LOW);
  digitalWrite(CoilRight,  packet.trolleyRight? HIGH : LOW);
  digitalWrite(StatusLED,  webOverrideActive  ? HIGH : LOW);
}

void broadcastStatus() {
  if (ws.count() == 0) return;
  String json = "{";
  json += "\"liftUp\":"    + String(lastPacket.liftUp      ? "true" : "false") + ",";
  json += "\"liftDown\":"  + String(lastPacket.liftDown    ? "true" : "false") + ",";
  json += "\"trolleyLeft\":"  + String(lastPacket.trolleyLeft  ? "true" : "false") + ",";
  json += "\"trolleyRight\":" + String(lastPacket.trolleyRight ? "true" : "false") + ",";
  json += "\"override\":"  + String(webOverrideActive ? "true" : "false") + ",";
  json += "\"connected\":true";
  json += "}";
  ws.textAll(json);
}

void setupWebServer() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed - RF control only");
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  ws.onEvent([](AsyncWebSocket *srv, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      Serial.println("WS client connected");
      broadcastStatus();
    } else if (type == WS_EVT_DATA) {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->opcode == WS_TEXT && len > 0) {
        lastWSMessage = millis();

        String msg = "";
        for (size_t i = 0; i < len; i++) msg += (char)data[i];

        ControlPacket pkt;
        if (xSemaphoreTake(packetMutex, portMAX_DELAY)) {
          if (msg == "liftUp") {
            pkt = {true, false, false, false};
          } else if (msg == "liftDown") {
            pkt = {false, true, false, false};
          } else if (msg == "trolleyLeft") {
            pkt = {false, false, true, false};
          } else if (msg == "trolleyRight") {
            pkt = {false, false, false, true};
          } else if (msg == "stop") {
            pkt = {false, false, false, false};
          } else {
            xSemaphoreGive(packetMutex);
            return;
          }
          lastPacket = pkt;
          xSemaphoreGive(packetMutex);
        }

        applyControl(lastPacket);
        webOverrideActive = true;
        webOverrideTimeout = millis();
        broadcastStatus();
      }
    }
  });
  server.addHandler(&ws);
  server.begin();
  Serial.println("Async Web server + WebSocket started");
}

void allCoilsSafetyOff() {
  stopAllCoils();
  digitalWrite(StatusLED, LOW);
}

void setup() {
  Serial.begin(115200);

  pinMode(CoilUp, OUTPUT);
  pinMode(CoilDown, OUTPUT);
  pinMode(CoilLeft, OUTPUT);
  pinMode(CoilRight, OUTPUT);
  pinMode(StatusLED, OUTPUT);

  allCoilsSafetyOff();

  packetMutex = xSemaphoreCreateMutex();

  if (!radio.begin()) {
    Serial.println("Radio hardware not responding!");
    while (1) {
      delay(1000);
    }
  }

  radio.setChannel(108);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_LOW);
  radio.setRetries(5, 15);
  radio.openReadingPipe(1, address);
  radio.startListening();

  setupWebServer();

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();

  if (radio.available()) {
    ControlPacket incoming;
    radio.read(&incoming, sizeof(incoming));

    if (!webOverrideActive) {
      if (xSemaphoreTake(packetMutex, portMAX_DELAY)) {
        lastPacket = incoming;
        xSemaphoreGive(packetMutex);
      }
      applyControl(lastPacket);
      broadcastStatus();
    }
  }

  if (webOverrideActive) {
    if (millis() - webOverrideTimeout > WEB_OVERRIDE_DURATION) {
      webOverrideActive = false;
      if (xSemaphoreTake(packetMutex, portMAX_DELAY)) {
        lastPacket = {false, false, false, false};
        xSemaphoreGive(packetMutex);
      }
      allCoilsSafetyOff();
      broadcastStatus();
      Serial.println("Web override expired, RF control resumed");
    }

    if (millis() - lastWSMessage > WS_SAFETY_TIMEOUT && lastWSMessage > 0) {
      webOverrideActive = false;
      if (xSemaphoreTake(packetMutex, portMAX_DELAY)) {
        lastPacket = {false, false, false, false};
        xSemaphoreGive(packetMutex);
      }
      allCoilsSafetyOff();
      broadcastStatus();
      lastWSMessage = 0;
      Serial.println("WS safety timeout - coils stopped");
    }
  }

  ws.cleanupClients();
  delay(5);
}
