#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

// Button pins
#define buttonLup     3
#define buttonLdown   4
#define buttonTleft   5
#define buttonTright  6
#define statusLED     2

// RF24 CE and CSN pins
RF24 radio(7, 8);
const byte address[6] = "00001";

// Control packet structure
struct ControlPacket {
  bool liftUp;
  bool liftDown;
  bool trolleyLeft;
  bool trolleyRight;
};

// Debounce settings
const unsigned long debounceDelay = 30;
unsigned long lastDebounceTime[4] = {0, 0, 0, 0};
bool lastButtonState[4] = {HIGH, HIGH, HIGH, HIGH};
bool stableState[4] = {false, false, false, false};

void setup() {
  pinMode(buttonLup, INPUT_PULLUP);
  pinMode(buttonLdown, INPUT_PULLUP);
  pinMode(buttonTleft, INPUT_PULLUP);
  pinMode(buttonTright, INPUT_PULLUP);
  pinMode(statusLED, OUTPUT);

  Serial.begin(115200);
  Serial.println("Transmitter starting...");

  if (!radio.begin()) {
    Serial.println("❌ RF24 module not responding!");
    while (1);
  }

  radio.setChannel(108);               // Match receiver
  radio.setDataRate(RF24_250KBPS);     // Match receiver
  radio.setPALevel(RF24_PA_LOW);       // Stable for short range
  radio.openWritingPipe(address);
  radio.stopListening();

  Serial.println("✅ Transmitter ready (momentary mode)");
}

void loop() {
  int rawStates[4] = {
    digitalRead(buttonLup),
    digitalRead(buttonLdown),
    digitalRead(buttonTleft),
    digitalRead(buttonTright)
  };

  unsigned long currentTime = millis();

  // Debounce logic
  for (int i = 0; i < 4; i++) {
    if (rawStates[i] != lastButtonState[i]) {
      lastDebounceTime[i] = currentTime;
      lastButtonState[i] = rawStates[i];
    }

    if ((currentTime - lastDebounceTime[i]) > debounceDelay) {
      stableState[i] = (rawStates[i] == LOW); // LOW = pressed
    }
  }

  ControlPacket packet;
  packet.liftUp      = stableState[0];
  packet.liftDown    = stableState[1];
  packet.trolleyLeft = stableState[2];
  packet.trolleyRight= stableState[3];

  bool success = radio.write(&packet, sizeof(packet));
  if (success) {
    Serial.print("✅ Sent: ");
    Serial.print(packet.liftUp); Serial.print(",");
    Serial.print(packet.liftDown); Serial.print(",");
    Serial.print(packet.trolleyLeft); Serial.print(",");
    Serial.println(packet.trolleyRight);
  } else {
    Serial.println("❌ Failed to send packet");
  }

  // Blink LED to confirm transmission
  digitalWrite(statusLED, HIGH);
  delay(5);
  digitalWrite(statusLED, LOW);

  delay(20); // Smooth loop
}