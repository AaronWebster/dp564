// Dolby DP564 Remote control.

#include <ESP8266WiFi.h>
#include <WiFiClient.h>

#include "ESPRotary.h";

constexpr char kApSsid[] = "FBI Party Van";
constexpr char kApPassword[] = "";

IPAddress kHost(192, 168, 4, 91);
constexpr uint16_t kPort = 4444;

constexpr int kKnobPinClk = 4;
constexpr int kKnobPinDt = 5;
constexpr int kKnobPinBtn = 2;
constexpr int kKnobMinValue = 0x02;
constexpr int kKnobMaxValue = 0xbf;
constexpr int kKnobClicksPerStep = 4;

ESPRotary knob = ESPRotary(kKnobPinClk, kKnobPinDt, kKnobClicksPerStep,
                           kKnobMinValue, kKnobMaxValue, 0);
int knob_position;

WiFiClient client;

// PROBABLY WRONG:
//
// 0x00: Heartbeat packet. This packet is used to maintain the connection
// between the device and the controller. It typically contains no payload.
//
// 0x01: Handshake request packet. This packet is used to initiate a new session
// with the device. The payload of the packet should contain the string "Dolby".
//
// 0x02: Handshake response packet. This packet is sent in response to a
// handshake request packet, and confirms that the connection has been
// established successfully. The payload of the packet should contain the string
// "ACK ".
//
// 0x03: Handshake completion packet. This packet is sent to indicate that the
// handshake process has completed successfully. It typically contains no
// payload.
//
// 0x04: Heartbeat request packet. This packet is used to request a heartbeat
// response from the device. It typically contains no payload.
//
// 0x05: Heartbeat response packet. This packet is sent in response to a
// heartbeat request packet. It typically contains no payload.
//
// 0x0a: Control packet. This packet is used to send control commands to the
// device, such as changing the volume or routing signals. The format of the
// packet varies depending on the specific control command being sent.
//
// 0x0b: Control response packet. This packet is sent in response to a control
// packet to acknowledge the command and provide any relevant status or
// information.

void setup() {
  Serial.begin(115200);

  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(kApSsid);

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

  // Is this really a header?
  // Byte 1: 0x3a (start of packet marker)
  // Byte 2: 0x0a (packet type - control packet)
  // Byte 3-4: 0x000c (packet length, including header and payload)
  // Byte 5: 0x00 (flags - none)
  // Byte 6-16: 0x00 (reserved)

  if (!client.connected()) {
    if (client.connect(kHost, kPort)) {
      Serial.print("Connected to ");
      Serial.print(kHost);
      Serial.print(":");
      Serial.println(kPort);

      // Byte 1: 0x00 (reserved)
      // Byte 2: 0x00 (reserved)
      // Byte 3: 0x00 (reserved)
      // Byte 4: 0x05 (length of the payload)
      // Byte 5: 0x03 (payload - handshake request)
      uint8_t hello1[] = {0x00, 0x00, 0x00, 0x05};
      client.write(hello1, sizeof(hello1), 0x18);  // Set PSH=1, ACK=1
      client.flush();
      uint8_t hello2[] = {0x03};
      client.write(hello2, sizeof(hello2), 0x18);  // Set PSH=1, ACK=1
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
    // Start of new control command, 0x0a represents that the command has 10
    // bytes total.
    uint8_t control[] = {0x00, 0x00, 0x00, 0x0a};
    // 6-byte payload
    // Byte 1-2: 0x0205 (device ID - fixed value)
    // Byte 3-4: 0x1100 (control command ID - fixed value)
    // Byte 5: 0x00 (parameter count - always zero)
    // Byte 6: volume setting (0-255)
    uint8_t value[] = {0x02, 0x03, 0x12, 0x00, 0x00, knob_position};
    client.write(command, sizeof(command));
    client.flush();
    while (client.available()) client.read();
    Serial.print("Knob value set to: ");
    Serial.println(knob_position);
  }
}
