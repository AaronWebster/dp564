/*
    This sketch establishes a TCP connection to a "quote of the day" service.
    It sends a "hello" message, and then prints received data.
*/

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include "ESPRotary.h";

constexpr char kApSsid[] = "";
constexpr char kApPassword[] = "";

IPAddress kHost(192, 168, 4, 91);
constexpr uint16_t kPort = 4444;

constexpr int kKnobPinClk = 4;
constexpr int kKnobPinDt = 5;
constexpr int kKnobPinBtn = 2;
constexpr int kKnobMinValue = 0x02;
constexpr int kKnobMaxValue = 0xbf;
constexpr int kKnobClicksPerStep = 4;

ESPRotary knob = ESPRotary(kKnobPinClk, kKnobPinDt, kKnobClicksPerStep, kKnobMinValue, kKnobMaxValue, 0);
int knob_position;

WiFiClient client;

void setup() {
  Serial.begin(115200);

  // We start by connecting to a WiFi network

  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(kApSsid);

  /* Explicitly set the ESP8266 to be a WiFi-client, otherwise, it by default,
     would try to act as both a client and an access-point and could cause
     network-issues with your other WiFi-devices on your WiFi-network. */
  WiFi.mode(WIFI_STA);
  WiFi.begin(kApSsid, kApPassword);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  knob_position = knob.getPosition();
}

void loop() {
  knob.loop();
  while (client.available()) client.read();

  if (!client.connected()) {
    if (client.connect(kHost, kPort)) {
      Serial.print("Connected to ");
      Serial.print(kHost);
      Serial.print(":");
      Serial.println(kPort);
      uint8_t hello1[] = {0x00, 0x00, 0x00, 0x05};
      client.write(hello1, sizeof(hello1));
      client.flush();
      uint8_t hello2[] = {0x03};
      client.write(hello2, sizeof(hello2));
      client.flush();
    } else {
      Serial.println("connection failed");
      delay(5000);
      return;
    }
  }

  while (client.available()) client.read();

  if (knob_position != knob.getPosition()) {
    knob_position = knob.getPosition();
    uint8_t command[] = {0x00, 0x00, 0x00, 0x0a, 0x02, 0x03, 0x12, 0x00, 0x00, knob_position};
    client.write(command, sizeof(command));
    client.flush();
    while (client.available()) client.read();
    Serial.print("Knob value set to: ");
    Serial.println(knob_position);
  }
}
