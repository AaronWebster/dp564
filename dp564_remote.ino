/*
 * Dolby DP564 Auto-Discovery Remote for ESP32
 * * Features:
 * 1. Auto-connects to strongest OPEN WiFi.
 * 2. Scans subnet for devices with Open Port 4444.
 * 3. Filters by Dolby MAC Address/OUI.
 * 4. Implements Python control logic.
 */

#include <WiFi.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <esp_wifi.h>

WiFiClient client;

// --- Protocol Constants ---
const byte HANDSHAKE_MSG_1[] = {0x00, 0x00, 0x00, 0x05};
const byte HANDSHAKE_MSG_2[] = {0x03};
const byte PRE_CMD[] = {0x00, 0x00, 0x00, 0x0a};
const byte VOLUME_CMD_PREFIX[] = {0x02, 0x03, 0x12, 0x00, 0x00};
const byte DIM_CMD_PREFIX[]    = {0x02, 0x05, 0x13, 0x00, 0x00};
const byte SOURCE_CMD_PREFIX[] = {0x02, 0x03, 0x01, 0x00, 0x00};
const byte VOLUME_DEVICE_UPDATE_PREFIX[] = {0x00, 0x00, 0x00, 0x0b, 0x04, 0x03, 0x14, 0x01, 0x02, 0x00};
const byte VOLUME_ACK_PREFIX[]           = {0x00, 0x00, 0x00, 0x0b, 0x04, 0x03, 0x12, 0x01, 0x02, 0x00};
const byte DIM_ACK_PREFIX[]              = {0x00, 0x00, 0x00, 0x0b, 0x04, 0x05, 0x13, 0x01, 0x02, 0x00};
const byte SOURCE_ACK_PREFIX[]           = {0x00, 0x00, 0x00, 0x0b, 0x04, 0x03, 0x01, 0x01, 0x02, 0x00};
const byte HEARTBEAT_PACKET[] = {0x00, 0x00, 0x00, 0x05, 0x04};

const unsigned long HEARTBEAT_INTERVAL = 10000;
const uint16_t TARGET_PORT = 4444;

// --- Global State ---
IPAddress dp564Ip;
bool deviceFound = false;
unsigned long lastHeartbeatTime = 0;
float currentVolume = 0.0;
bool isDimmed = false;
String currentSource = "aes1";
bool isConnected = false;
byte rxBuffer[256];

// --- MAC Matching Helper ---
bool isDolbyMac(uint8_t* mac) {
  // 1. 00:12:A6 (Dolby Australia)
  if (mac[0] == 0x00 && mac[1] == 0x12 && mac[2] == 0xA6) return true;
  
  // 2. 00:D0:46 (Dolby Labs)
  if (mac[0] == 0x00 && mac[1] == 0xD0 && mac[2] == 0x46) return true;

  // 3. 70:B3:D5:3F:50:00/36
  // Match first 4 bytes + upper nibble of 5th byte
  if (mac[0] == 0x70 && mac[1] == 0xB3 && mac[2] == 0xD5 && mac[3] == 0x3F) {
     if ((mac[4] & 0xF0) == 0x50) return true; 
  }

  // 4. 70:B3:D5:73:A0:00/36
  // Match first 4 bytes + upper nibble of 5th byte
  if (mac[0] == 0x70 && mac[1] == 0xB3 && mac[2] == 0xD5 && mac[3] == 0x73) {
     if ((mac[4] & 0xF0) == 0xA0) return true; 
  }

  // 5. D4:25:CC:80:00:00/28
  // Match first 3 bytes + upper nibble of 4th byte
  if (mac[0] == 0xD4 && mac[1] == 0x25 && mac[2] == 0xCC) {
    if ((mac[3] & 0xF0) == 0x80) return true;
  }

  return false;
}

// --- Setup ---

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n--- DP564 Auto-Discovery Controller ---");

  connectToStrongestOpenWifi();
  
  if (WiFi.status() == WL_CONNECTED) {
    scanForDolbyDevice();
  }

  if (deviceFound) {
    connectToDevice();
  } else {
    Serial.println("Startup Failed: No matching Dolby device found.");
  }
}

// --- WiFi Logic ---

void connectToStrongestOpenWifi() {
  Serial.println("Scanning for open networks...");
  int n = WiFi.scanNetworks();
  
  if (n == 0) {
    Serial.println("No networks found.");
    return;
  }

  int bestNet = -1;
  int bestRssi = -1000;

  for (int i = 0; i < n; ++i) {
    // Check if OPEN (No encryption)
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
      if (WiFi.RSSI(i) > bestRssi) {
        bestRssi = WiFi.RSSI(i);
        bestNet = i;
      }
    }
    delay(10);
  }

  if (bestNet >= 0) {
    String ssid = WiFi.SSID(bestNet);
    Serial.printf("Connecting to OPEN network: %s (RSSI: %d)\n", ssid.c_str(), bestRssi);
    WiFi.begin(ssid.c_str());
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi Connected!");
      Serial.print("Local IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\nFailed to connect.");
    }
  } else {
    Serial.println("No open networks found.");
  }
}

// --- Discovery Logic ---

void scanForDolbyDevice() {
  Serial.println("Scanning subnet for Dolby devices on port 4444...");
  
  IPAddress local = WiFi.localIP();
  IPAddress subnet = WiFi.subnetMask();
  
  // Calculate valid subnet range (Assuming Class C /24 for simplicity, or iterating last octet)
  // To be robust, we iterate 1 to 254 on the local subnet.
  
  IPAddress target;
  target = local; // Copy structure
  
  // We use a temporary client for scanning
  WiFiClient scanClient;
  scanClient.setTimeout(1); // Very short timeout for scanning

  for (int i = 1; i < 255; i++) {
    target[3] = i;
    
    // Skip ourselves
    if (target == local) continue;

    // Optional: Print progress dot every 10 IPs
    if (i % 10 == 0) Serial.print(".");

    // 1. Try to connect to Port 4444
    // We increase timeout slightly to ensure we catch it, but keep it snappy.
    // 150ms is usually enough for local LAN.
    if (scanClient.connect(target, TARGET_PORT, 150)) {
      Serial.printf("\nFound open port 4444 at %s. Checking MAC...\n", target.toString().c_str());
      
      // 2. ARP Lookup
      // Since we just established a TCP connection, the ARP cache MUST contain the MAC.
      uint8_t mac[6];
      if (getMacFromIp(target, mac)) {
        Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X - ", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
        if (isDolbyMac(mac)) {
          Serial.println("MATCH! Dolby Device Found.");
          dp564Ip = target;
          deviceFound = true;
          scanClient.stop();
          return; // Stop scanning
        } else {
          Serial.println("No Match.");
        }
      } else {
        Serial.println("Could not resolve MAC.");
      }
      scanClient.stop();
    }
  }
  Serial.println("\nScan complete.");
}

// Helper to read internal ARP table
bool getMacFromIp(IPAddress ip, uint8_t* macAddr) {
  ip4_addr_t target_ip;
  IP4_ADDR(&target_ip, ip[0], ip[1], ip[2], ip[3]);
  
  struct eth_addr *ret_eth_addr;
  const ip4_addr_t *ret_ip_addr;
  
  // Note: This accesses LwIP internals directly.
  // netif_default is the default network interface (STA)
  ssize_t idx = etharp_find_addr(netif_default, &target_ip, &ret_eth_addr, &ret_ip_addr);
  
  if (idx > -1) {
    memcpy(macAddr, ret_eth_addr->addr, 6);
    return true;
  }
  return false;
}

// --- Main Application Logic (Unchanged) ---

void loop() {
  if (!deviceFound) return;

  if (!client.connected() && isConnected) {
    Serial.println("[Error] Connection lost.");
    isConnected = false;
    client.stop();
  }
  
  if (!isConnected) return; // Wait for manual reset or logic expansion

  if (millis() - lastHeartbeatTime > HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  }

  processIncomingPackets();

  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) handleUserCommand(input);
  }
}

void connectToDevice() {
  Serial.print("Connecting to DP564 at ");
  Serial.println(dp564Ip);

  if (client.connect(dp564Ip, TARGET_PORT)) {
    Serial.println("Socket connected. Performing Handshake...");
    client.write(HANDSHAKE_MSG_1, sizeof(HANDSHAKE_MSG_1));
    delay(100); 
    client.write(HANDSHAKE_MSG_2, sizeof(HANDSHAKE_MSG_2));
    
    unsigned long startWait = millis();
    while(millis() - startWait < 500) {
      while(client.available()) client.read();
    }
    
    isConnected = true;
    lastHeartbeatTime = millis();
    Serial.println("Handshake complete. Ready.");
    printHelp();
  } else {
    Serial.println("Connection Failed.");
    isConnected = false;
  }
}

void setVolume(float db) {
  if (db > 0.0 || db < -95.0) {
    Serial.println("Error: Volume must be between -95.0 and 0.0");
    return;
  }
  int val = 192 + (int)(db * 2);
  byte payload = (byte)val;
  Serial.printf("Sending Volume: %.1f dB\n", db);
  client.write(PRE_CMD, sizeof(PRE_CMD));
  delay(100); 
  client.write(VOLUME_CMD_PREFIX, sizeof(VOLUME_CMD_PREFIX));
  client.write(payload);
}

void setDim(bool state) {
  byte payload = state ? 0x01 : 0x00;
  client.write(PRE_CMD, sizeof(PRE_CMD));
  delay(100);
  client.write(DIM_CMD_PREFIX, sizeof(DIM_CMD_PREFIX));
  client.write(payload);
}

void setSource(String src) {
  byte payload;
  src.toLowerCase();
  if (src == "aes1") payload = 0x00;
  else if (src == "aes2") payload = 0x01;
  else if (src == "optical") payload = 0x02;
  else if (src == "streaming") payload = 0x03;
  else { Serial.println("Invalid source."); return; }
  
  client.write(PRE_CMD, sizeof(PRE_CMD));
  delay(100);
  client.write(SOURCE_CMD_PREFIX, sizeof(SOURCE_CMD_PREFIX));
  client.write(payload);
}

void sendHeartbeat() {
  if(client.connected()) client.write(HEARTBEAT_PACKET, sizeof(HEARTBEAT_PACKET));
}

void processIncomingPackets() {
  while (client.available()) {
    int len = client.read(rxBuffer, sizeof(rxBuffer));
    if (len <= 0) return;
    if (memcmp(rxBuffer, HEARTBEAT_PACKET, sizeof(HEARTBEAT_PACKET)) == 0) return;

    if (len > sizeof(VOLUME_DEVICE_UPDATE_PREFIX) && memcmp(rxBuffer, VOLUME_DEVICE_UPDATE_PREFIX, sizeof(VOLUME_DEVICE_UPDATE_PREFIX)) == 0) {
      currentVolume = (rxBuffer[len - 1] - 192) / 2.0;
      Serial.printf("\n[Device] Vol: %.1f dB\n", currentVolume);
    }
    else if (len > sizeof(VOLUME_ACK_PREFIX) && memcmp(rxBuffer, VOLUME_ACK_PREFIX, sizeof(VOLUME_ACK_PREFIX)) == 0) {
      currentVolume = (rxBuffer[len - 1] - 192) / 2.0;
      Serial.printf("[ACK] Vol: %.1f dB\n", currentVolume);
    }
    else if (len > sizeof(DIM_ACK_PREFIX) && memcmp(rxBuffer, DIM_ACK_PREFIX, sizeof(DIM_ACK_PREFIX)) == 0) {
      isDimmed = (rxBuffer[len - 1] == 0x01);
      Serial.printf("[ACK] Dim: %s\n", isDimmed ? "ON" : "OFF");
    }
    else if (len > sizeof(SOURCE_ACK_PREFIX) && memcmp(rxBuffer, SOURCE_ACK_PREFIX, sizeof(SOURCE_ACK_PREFIX)) == 0) {
      byte val = rxBuffer[len - 1];
      if(val==0x00) currentSource="aes1";
      if(val==0x01) currentSource="aes2";
      if(val==0x02) currentSource="optical";
      if(val==0x03) currentSource="streaming";
      Serial.printf("[ACK] Src: %s\n", currentSource.c_str());
    }
  }
}

void handleUserCommand(String cmdLine) {
  int spaceIndex = cmdLine.indexOf(' ');
  String cmd = (spaceIndex == -1) ? cmdLine : cmdLine.substring(0, spaceIndex);
  String arg = (spaceIndex == -1) ? "" : cmdLine.substring(spaceIndex + 1);
  cmd.toLowerCase();
  
  if (cmd == "volume") setVolume(arg.toFloat());
  else if (cmd == "dim") {
    if (arg == "on") setDim(true); else setDim(false);
  }
  else if (cmd == "source") setSource(arg);
  else if (cmd == "status") {
    Serial.printf("Vol: %.1f dB | Src: %s | Dim: %s\n", currentVolume, currentSource.c_str(), isDimmed ? "ON" : "OFF");
  }
  else printHelp();
}

void printHelp() { Serial.println("Cmds: volume <db>, dim <on/off>, source <name>, status"); }
