#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

// Coil output pins
#define CoilUp     13
#define CoilDown   27
#define CoilLeft   26
#define CoilRight  25
#define StatusLED  14  // Web override indicator

// nRF24L01 pins
#define CE_PIN     4
#define CSN_PIN    5

RF24 radio(CE_PIN, CSN_PIN);
const byte address[6] = "00001";

// Wi-Fi credentials
const char* ssid = "GTI-GUDANG";
const char* password = "Pipitq1006";

// Static IP configuration
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
bool webOverrideActive = false;
unsigned long webOverrideTimeout = 0;

void applyControl(ControlPacket packet) {
  digitalWrite(CoilUp,     packet.liftUp      ? HIGH : LOW);
  digitalWrite(CoilDown,   packet.liftDown    ? HIGH : LOW);
  digitalWrite(CoilLeft,   packet.trolleyLeft ? HIGH : LOW);
  digitalWrite(CoilRight,  packet.trolleyRight? HIGH : LOW);
  digitalWrite(StatusLED,  webOverrideActive  ? HIGH : LOW);
  broadcastStatus();
}

void broadcastStatus() {
  String json = "{";
  json += "\"liftUp\":" + String(lastPacket.liftUp ? "true" : "false") + ",";
  json += "\"liftDown\":" + String(lastPacket.liftDown ? "true" : "false") + ",";
  json += "\"trolleyLeft\":" + String(lastPacket.trolleyLeft ? "true" : "false") + ",";
  json += "\"trolleyRight\":" + String(lastPacket.trolleyRight ? "true" : "false");
  json += "}";
  ws.textAll(json);
}

void setupWebServer() {
  if (!LittleFS.begin()) {
    Serial.println("❌ LittleFS mount failed");
    return;
  }

  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n✅ Connected: " + WiFi.localIP().toString());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/control", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("cmd")) {
      request->send(400, "text/plain", "Missing command");
      return;
    }

    String cmd = request->getParam("cmd")->value();

    if (cmd == "liftUp") {
      lastPacket.liftUp = true;
      lastPacket.liftDown = false;
    } else if (cmd == "liftDown") {
      lastPacket.liftDown = true;
      lastPacket.liftUp = false;
    } else if (cmd == "trolleyLeft") {
      lastPacket.trolleyLeft = true;
      lastPacket.trolleyRight = false;
    } else if (cmd == "trolleyRight") {
      lastPacket.trolleyRight = true;
      lastPacket.trolleyLeft = false;
    } else if (cmd == "stop") {
      lastPacket = {false, false, false, false};
    } else {
      request->send(400, "text/plain", "Unknown command");
      return;
    }

    applyControl(lastPacket);
    webOverrideActive = true;
    webOverrideTimeout = millis();
    request->send(200, "text/plain", "OK");
  });

  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      Serial.println("🟢 WebSocket client connected");
    }
  });
  server.addHandler(&ws);

  server.begin();
  Serial.println("✅ Async Web server + WebSocket started");
}

void setup() {
  Serial.begin(115200);

  pinMode(CoilUp, OUTPUT);
  pinMode(CoilDown, OUTPUT);
  pinMode(CoilLeft, OUTPUT);
  pinMode(CoilRight, OUTPUT);
  pinMode(StatusLED, OUTPUT);

  if (!radio.begin()) {
    Serial.println("❌ Radio hardware not responding!");
    while (1);
  }

  radio.setChannel(108);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_LOW);
  radio.setRetries(5, 15);
  radio.openReadingPipe(1, address);
  radio.startListening();

  setupWebServer();
}

void loop() {
  if (radio.available() && !webOverrideActive) {
    radio.read(&lastPacket, sizeof(lastPacket));
    applyControl(lastPacket);
  }

  if (webOverrideActive && millis() - webOverrideTimeout > 5000) {
    webOverrideActive = false;
    Serial.println("⏱️ Web override expired, returning to RF control");
  }

  delay(10);
}