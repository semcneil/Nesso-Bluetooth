/*
  This script demonstrates a BLE mesh

  Seth McNeill
  2026 November 15
*/

#include "secrets.h"

#include <Arduino.h>
#include <Arduino_Nesso_N1.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>

// ==========================================
// 0. OPTIONAL DEVICE NAME CONFIGURATION
// ==========================================
// Leave as "" to flash the EXACT same code to all boards (uses unique MAC hex ID).
// Or set up to 4 characters (e.g., "N-01", "BASE", "UNIT") before flashing each board.
// #define NODE_CUSTOM_NAME  ""

// ==========================================
// 1. NESSO N1 DISPLAY INSTANCE
// ==========================================
NessoDisplay display;

// ==========================================
// 2. MESH PROTOCOL DEFINITIONS
// ==========================================
#define MAX_NODES         15
#define MESH_MAGIC_1      0x4E // 'N'
#define MESH_MAGIC_2      0x31 // '1'
#define INITIAL_TTL       5
#define NODE_TIMEOUT_MS   15000 // Drop node from list if silent for 15s
#define BEACON_INTERVAL   1200  // Broadcast own presence every 1.2s

struct __attribute__((packed)) MeshPacket {
  uint8_t  magic1;
  uint8_t  magic2;
  uint16_t originId;   // Unique hardware ID derived from ESP32-C6 MAC
  char     label[5];   // 4-char display name + null terminator
  uint16_t seqNum;     // Rolling packet sequence number
  uint8_t  ttl;        // Remaining hops (5 = Direct, 4 = M1, 3 = M2, etc.)
};

struct PeerNode {
  bool     active;
  uint16_t id;
  char     label[5];
  uint16_t lastSeq;
  bool     isDirect;
  uint8_t  hops;         // 0 = Direct, 1..4 = Relay hops away
  int8_t   rssi;
  uint32_t lastSeenMs;
  uint32_t lastDirectMs;
};

PeerNode peers[MAX_NODES];
uint16_t myNodeId = 0;
char     myNodeLabel[5] = {0};
uint16_t mySeqNum = 0;

// ESP32-C6 uses NimBLE Extended Advertising by default in NimBLE v2.x
NimBLEExtAdvertising* pAdvertising = nullptr;
NimBLEScan* pScan = nullptr;

// Thread-safe relay handoff between NimBLE callback and loop()
volatile bool hasRelayPacket = false;
MeshPacket relayPacket;
portMUX_TYPE meshMux = portMUX_INITIALIZER_UNLOCKED;

// ==========================================
// 3. MESH LOGIC & HOP-COUNT TRACKING
// ==========================================
void processIncomingPacket(const MeshPacket& pkt, int8_t rssi) {
  if (pkt.magic1 != MESH_MAGIC_1 || pkt.magic2 != MESH_MAGIC_2) return;
  if (pkt.originId == myNodeId) return; // Ignore own echoed packets
  if (pkt.ttl == 0 || pkt.ttl > INITIAL_TTL) return;

  uint32_t now = millis();
  uint8_t packetHops = INITIAL_TTL - pkt.ttl; // 0 = Direct, 1 = M1, 2 = M2...
  bool isDirectHit = (packetHops == 0);

  int existingIdx = -1;
  int emptyIdx = -1;

  for (int i = 0; i < MAX_NODES; i++) {
    if (peers[i].active && peers[i].id == pkt.originId) {
      existingIdx = i;
      break;
    }
    if (!peers[i].active && emptyIdx == -1) {
      emptyIdx = i;
    }
  }

  // Duplicate check: Ignore older or already-processed sequence numbers,
  // EXCEPT if a shorter/direct path arrives for the same sequence number.
  if (existingIdx != -1) {
    int16_t seqDiff = (int16_t)(pkt.seqNum - peers[existingIdx].lastSeq);
    if (seqDiff < 0) {
      return; // Stale out-of-order packet
    }
    if (seqDiff == 0) {
      if (isDirectHit) {
        peers[existingIdx].isDirect = true;
        peers[existingIdx].hops = 0;
        peers[existingIdx].lastDirectMs = now;
        peers[existingIdx].rssi = rssi;
      } else if (!peers[existingIdx].isDirect && packetHops < peers[existingIdx].hops) {
        // Found a shorter relay path for the same broadcast
        peers[existingIdx].hops = packetHops;
        peers[existingIdx].rssi = rssi;
      }
      return;
    }
  }

  int targetIdx = (existingIdx != -1) ? existingIdx : emptyIdx;
  if (targetIdx == -1) return; // Peer table full (15 devices)

  peers[targetIdx].active = true;
  peers[targetIdx].id = pkt.originId;
  memcpy(peers[targetIdx].label, pkt.label, 4);
  peers[targetIdx].label[4] = '\0';
  peers[targetIdx].lastSeq = pkt.seqNum;
  peers[targetIdx].rssi = rssi;
  peers[targetIdx].lastSeenMs = now;

  if (isDirectHit) {
    peers[targetIdx].isDirect = true;
    peers[targetIdx].hops = 0;
    peers[targetIdx].lastDirectMs = now;
  } else {
    // Only downgrade from Direct to Mesh if direct signal hasn't been heard in 4s
    if (now - peers[targetIdx].lastDirectMs > 4000) {
      peers[targetIdx].isDirect = false;
      peers[targetIdx].hops = packetHops;
    }
  }

  // Queue packet for rebroadcast (managed flooding) if hops remain
  if (pkt.ttl > 1) {
    portENTER_CRITICAL(&meshMux);
    relayPacket = pkt;
    relayPacket.ttl = pkt.ttl - 1;
    hasRelayPacket = true;
    portEXIT_CRITICAL(&meshMux);
  }
}

// ==========================================
// 4. NIMBLE v2.x SCAN CALLBACK
// ==========================================
class MeshScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (!advertisedDevice->haveManufacturerData()) return;

    std::string mfgData = advertisedDevice->getManufacturerData();
    if (mfgData.length() == sizeof(MeshPacket) + 2) {
      const uint8_t* raw = (const uint8_t*)mfgData.data();
      if (raw[0] == 0xFF && raw[1] == 0xFF) {
        MeshPacket pkt;
        memcpy(&pkt, raw + 2, sizeof(MeshPacket));
        processIncomingPacket(pkt, advertisedDevice->getRSSI());
      }
    }
  }
};

// ==========================================
// 5. BROADCAST HELPER (ESP32-C6 EXTENDED ADVERTISING)
// ==========================================
void broadcastPacket(const MeshPacket& pkt) {
  pAdvertising->stop(0);

  // Use legacy-compatible 1M PHY PDU for reliable broad scanning
  NimBLEExtAdvertisement extAdv(BLE_HCI_LE_PHY_1M, BLE_HCI_LE_PHY_1M);
  extAdv.setLegacyAdvertising(true);
  extAdv.setScannable(false);
  extAdv.setConnectable(false);
  extAdv.setMinInterval(32); // 20ms interval (in 0.625ms units)
  extAdv.setMaxInterval(64); // 40ms interval (in 0.625ms units)
  extAdv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);

  // Build Manufacturer Data: 2-byte Company ID (0xFFFF) + MeshPacket
  uint8_t payload[sizeof(MeshPacket) + 2];
  payload[0] = 0xFF;
  payload[1] = 0xFF;
  memcpy(payload + 2, &pkt, sizeof(MeshPacket));

  extAdv.setManufacturerData(payload, sizeof(payload));
  pAdvertising->setInstanceData(0, extAdv);
  pAdvertising->start(0);
}

// ==========================================
// 6. UI RENDERING (NESSO N1 240x135 LANDSCAPE 2-COLUMN GRID)
// ==========================================
void drawUI() {
  int w = display.width();   // 240px in landscape

  // Top Header Bar
  display.fillRect(0, 0, w, 18, TFT_NAVY);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, TFT_NAVY);
  display.setCursor(4, 5);
  display.printf("ME:%-4s", myNodeLabel);

  // Legend in Header
  display.fillCircle(78, 9, 3, TFT_GREEN);
  display.setCursor(84, 5);
  display.print("DIR");

  display.fillCircle(114, 9, 3, TFT_ORANGE);
  display.setCursor(120, 5);
  display.print("M1-4");

  int activeCount = 0;
  for (int i = 0; i < MAX_NODES; i++) {
    if (peers[i].active) activeCount++;
  }
  display.setCursor(188, 5);
  display.printf("%2d/15", activeCount);

  // 2-Column x 8-Row Grid
  int startY = 21;
  int colW   = w / 2; // 120px
  int rowH   = 14;

  for (int i = 0; i < MAX_NODES; i++) {
    int col = i % 2;
    int row = i / 2;
    int x   = col * colW;
    int y   = startY + (row * rowH);

    display.fillRect(x, y, colW - 1, rowH - 1, TFT_BLACK);

    if (peers[i].active) {
      // Status Badge: Green [DIR] for Direct, Orange [M1..M4] for Mesh Hops
      if (peers[i].isDirect) {
        display.fillRoundRect(x + 2, y + 1, 22, 11, 2, TFT_DARKGREEN);
        display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
        display.setCursor(x + 4, y + 3);
        display.print("DIR");
      } else {
        display.fillRoundRect(x + 2, y + 1, 22, 11, 2, TFT_ORANGE);
        display.setTextColor(TFT_BLACK, TFT_ORANGE);
        display.setCursor(x + 7, y + 3);
        display.printf("M%u", peers[i].hops);
      }

      // Node Label (Custom Name or MAC Hex ID)
      display.setTextColor(TFT_WHITE, TFT_BLACK);
      display.setCursor(x + 27, y + 3);
      display.printf("%-4s", peers[i].label);

      // Signal Strength (RSSI)
      display.setTextColor(TFT_CYAN, TFT_BLACK);
      display.setCursor(x + 56, y + 3);
      display.printf("%4ddB", peers[i].rssi);

    } else {
      // Empty Slot Placeholder
      display.setTextColor(0x39E7, TFT_BLACK);
      display.setCursor(x + 4, y + 3);
      display.printf("%2d. --", i + 1);
    }
  }

  // 16th Grid Cell (Bottom-Right)
  int lastX = colW;
  int lastY = startY + (7 * rowH);
  display.fillRect(lastX, lastY, colW - 1, rowH - 1, TFT_BLACK);
  display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  display.setCursor(lastX + 4, lastY + 3);
  display.print("MAX 4 RELAY HOPS");
}

// ==========================================
// 7. SETUP & MAIN LOOP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Initialize Nesso N1 Display
  display.begin();
  display.setRotation(1); // Landscape 240x135
  display.fillScreen(TFT_BLACK);

  // Initialize Peer Table
  memset(peers, 0, sizeof(peers));

  // Derive unique 16-bit hardware ID from factory BLE MAC address
  uint8_t baseMac[6] = {0};
  esp_read_mac(baseMac, ESP_MAC_BT);
  myNodeId = (baseMac[4] << 8) | baseMac[5];

  // Configure Display Label: Use NODE_CUSTOM_NAME if set, otherwise fallback to MAC hex
  if (strlen(NODE_CUSTOM_NAME) > 0) {
    strncpy(myNodeLabel, NODE_CUSTOM_NAME, 4);
    myNodeLabel[4] = '\0';
  } else {
    snprintf(myNodeLabel, sizeof(myNodeLabel), "%04X", myNodeId);
  }

  // Initialize NimBLE v2.x
  NimBLEDevice::init("NessoN1");
  NimBLEDevice::setPower(9); // Max TX power (+9dBm)

  // Configure Scanner (NimBLE v2.x API)
  pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(new MeshScanCallbacks(), true);
  pScan->setActiveScan(false);
  pScan->setInterval(45);
  pScan->setWindow(45);
  pScan->start(0, false, true);

  // Grab Extended Advertiser pointer
  pAdvertising = NimBLEDevice::getAdvertising();

  drawUI();
}

void loop() {
  static uint32_t lastBeaconMs = 0;
  static uint32_t lastUiMs = 0;
  uint32_t now = millis();

  // 1. Relay queued packets from other nodes
  bool doRelay = false;
  MeshPacket pktToForward;
  portENTER_CRITICAL(&meshMux);
  if (hasRelayPacket) {
    pktToForward = relayPacket;
    hasRelayPacket = false;
    doRelay = true;
  }
  portEXIT_CRITICAL(&meshMux);

  if (doRelay) {
    broadcastPacket(pktToForward);
    delay(35);
  }

  // 2. Broadcast own heartbeat periodically
  if (now - lastBeaconMs >= BEACON_INTERVAL) {
    lastBeaconMs = now;
    mySeqNum++;

    MeshPacket selfPkt;
    selfPkt.magic1   = MESH_MAGIC_1;
    selfPkt.magic2   = MESH_MAGIC_2;
    selfPkt.originId = myNodeId;
    memcpy(selfPkt.label, myNodeLabel, sizeof(myNodeLabel));
    selfPkt.seqNum   = mySeqNum;
    selfPkt.ttl      = INITIAL_TTL;

    broadcastPacket(selfPkt);
  }

  // 3. Expire stale nodes & refresh screen every 500ms
  if (now - lastUiMs >= 500) {
    lastUiMs = now;
    for (int i = 0; i < MAX_NODES; i++) {
      if (peers[i].active && (now - peers[i].lastSeenMs > NODE_TIMEOUT_MS)) {
        peers[i].active = false;
      }
    }
    drawUI();
  }
}