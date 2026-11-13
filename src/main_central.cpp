#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Arduino_Nesso_N1.h>

// Target Server Name & Services
#define TARGET_DEVICE_NAME              "NimBLE-Secure-Server"
#define CUSTOM_SERVICE_UUID             "ABCD"
#define NON_SECURE_CHAR_UUID            "1234"
#define SECURE_CHAR_UUID                "1235"
#define BUTTON_CHAR_UUID                "1236"
#define RX_DATA_CHAR_UUID               "1237"

// Standard Bluetooth SIG Battery Service & Characteristics
#define BATTERY_SERVICE_UUID            "180F"
#define BATTERY_LEVEL_CHAR_UUID         "2A19"
#define BATTERY_POWER_STATE_CHAR_UUID   "2A1A"
#define BATTERY_VOLTAGE_CHAR_UUID       "2B18"

// Passkey & Debounce Timing
constexpr uint32_t      SERVER_PASSKEY  = 123456;
constexpr unsigned long BTN_DEBOUNCE_MS = 20;

NessoDisplay display;

// Client state flags
static NimBLEAdvertisedDevice* advDevice   = nullptr;
static NimBLEClient*           pClient     = nullptr;
static bool                    doConnect   = false;
static bool                    isConnected = false;
static bool                    isScanning  = false;

// Remote characteristic pointer for KEY1 writes
static NimBLERemoteCharacteristic* pRemoteRxChar = nullptr;

// Local button state tracking
uint8_t       lastKey1State  = 0;
uint8_t       lastKey2State  = 0;
unsigned long lastKey1Change = 0;
unsigned long lastKey2Change = 0;

// Remote server telemetry for display (shared between BLE callbacks & loop)
volatile bool displayDirty     = false;
uint8_t       serverBatPct     = 0;
float         serverBatVolts   = 0.0f;
bool          serverCharging   = false;
uint8_t       serverKey1State  = 0;
char          serverAddrStr[24] = "None";

// Forward declarations
void renderStatusScreen();

// Helper: Reads active-low buttons on Arduino Nesso N1 (1 = pressed, 0 = released)
uint8_t readKey1() { return (digitalRead(KEY1) == LOW) ? 1 : 0; }
uint8_t readKey2() { return (digitalRead(KEY2) == LOW) ? 1 : 0; }

// Notification callback for subscribed characteristics
void notifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic,
                    uint8_t* pData, size_t length, bool isNotify) {
    NimBLEUUID uuid = pRemoteCharacteristic->getUUID();

    if (uuid.equals(NimBLEUUID(BUTTON_CHAR_UUID)) && length >= 1) {
        serverKey1State = pData[0];
        displayDirty    = true;
        Serial.printf("[NOTIFY] Server KEY1 -> %s (%u)\n", serverKey1State ? "PRESSED" : "RELEASED", serverKey1State);
    } 
    else if (uuid.equals(NimBLEUUID(RX_DATA_CHAR_UUID)) && length >= 1) {
        Serial.printf("[NOTIFY] Receive Data (0x1237) Echo -> 0x%02X (%u)\n", pData[0], pData[0]);
    } 
    else if (uuid.equals(NimBLEUUID(BATTERY_LEVEL_CHAR_UUID)) && length >= 1) {
        serverBatPct = pData[0];
        displayDirty = true;
        Serial.printf("[NOTIFY] Server Battery Level -> %u%%\n", serverBatPct);
    } 
    else if (uuid.equals(NimBLEUUID(BATTERY_POWER_STATE_CHAR_UUID)) && length >= 1) {
        uint8_t state  = pData[0];
        serverCharging = ((state >> 4) & 0x03) == 0x03;
        displayDirty   = true;
        Serial.printf("[NOTIFY] Server Power State -> 0x%02X (%s)\n", state, serverCharging ? "CHARGING" : "ON BATTERY");
    } 
    else if (uuid.equals(NimBLEUUID(BATTERY_VOLTAGE_CHAR_UUID)) && length >= 2) {
        uint16_t voltageMv = pData[0] | (pData[1] << 8);
        serverBatVolts     = voltageMv / 1000.0f;
        displayDirty       = true;
        Serial.printf("[NOTIFY] Server Battery Voltage -> %.3f V (%u mV)\n", serverBatVolts, voltageMv);
    }
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        String addr = String(client->getPeerAddress().toString().c_str());
        addr.toUpperCase();
        strncpy(serverAddrStr, addr.c_str(), sizeof(serverAddrStr) - 1);
        serverAddrStr[sizeof(serverAddrStr) - 1] = '\0';

        Serial.printf("Connected to server: %s\n", serverAddrStr);
        isConnected  = true;
        isScanning   = false;
        displayDirty = true;
    }

    void onDisconnect(NimBLEClient* client, int reason) override {
        Serial.printf("Disconnected (reason: %d). Press KEY2 to scan/reconnect.\n", reason);
        isConnected     = false;
        isScanning      = false;
        pRemoteRxChar   = nullptr;
        serverKey1State = 0;
        strncpy(serverAddrStr, "None", sizeof(serverAddrStr));
        displayDirty    = true;
    }

    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        Serial.printf("Supplying passkey: %lu\n", SERVER_PASSKEY);
        NimBLEDevice::injectPassKey(connInfo, SERVER_PASSKEY);
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (!connInfo.isEncrypted()) {
            Serial.println("Authentication failed!");
            NimBLEDevice::getClientByHandle(connInfo.getConnHandle())->disconnect();
            return;
        }
        Serial.println("Pairing & encryption succeeded!");
    }
};

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        if (advertisedDevice->getName() == TARGET_DEVICE_NAME ||
            advertisedDevice->isAdvertisingService(NimBLEUUID(CUSTOM_SERVICE_UUID))) {
            Serial.printf("Found target server: %s\n", advertisedDevice->toString().c_str());
            NimBLEDevice::getScan()->stop();
            isScanning = false;

            if (advDevice != nullptr) {
                delete advDevice;
                advDevice = nullptr;
            }
            advDevice    = new NimBLEAdvertisedDevice(*advertisedDevice);
            doConnect    = true;
            displayDirty = true;
        }
    }
};

static ClientCallbacks clientCallbacks;
static ScanCallbacks   scanCallbacks;

void renderBootScreen() {
    display.fillScreen(TFT_BLACK);

    display.fillRect(0, 0, display.width(), 28, TFT_NAVY);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.setTextSize(1);
    display.setCursor(10, 10);
    display.print("NESSO N1 BLE CENTRAL");

    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.setTextSize(2);
    display.setCursor(20, (display.height() / 2) - 12);
    display.print("Booting...");

    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.setTextSize(1);
    display.setCursor(20, (display.height() / 2) + 16);
    display.print("Initializing BLE scanner & UI");
}

void renderStatusScreen() {
    display.fillScreen(TFT_BLACK);

    // 1. Top Header Banner (Green = CONNECTED, Blue = SCANNING, Dark Grey = IDLE)
    uint16_t bannerColor = TFT_DARKGREY;
    uint16_t textColor   = TFT_WHITE;
    const char* stateLabel = "IDLE";

    if (isConnected) {
        bannerColor = TFT_DARKGREEN;
        textColor   = TFT_WHITE;
        stateLabel  = "CONNECTED";
    } else if (isScanning || doConnect) {
        bannerColor = TFT_BLUE;
        textColor   = TFT_WHITE;
        stateLabel  = "SCANNING";
    }

    display.fillRect(0, 0, display.width(), 32, bannerColor);
    display.setTextColor(textColor, bannerColor);
    display.setTextSize(2);
    display.setCursor(8, 8);
    display.print(stateLabel);

    // Server Battery Telemetry on right side of header when connected
    display.setTextSize(1);
    if (isConnected) {
        const char* chgTag = serverCharging ? "CHG" : "BAT";
        display.setCursor(138, 6);
        display.printf("SRV:%3u%%[%s]", serverBatPct, chgTag);

        display.setCursor(138, 18);
        display.printf("    %.2fV", serverBatVolts);
    } else {
        display.setCursor(142, 12);
        display.print("KEY2: Toggle");
    }

    // 2. Server Address / Status Line
    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(8, 40);
    display.print("SERVER:");

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(56, 40);
    if (isConnected) {
        display.print(serverAddrStr);
    } else if (isScanning || doConnect) {
        display.print("Searching for server...");
    } else {
        display.print("Press KEY2 to scan");
    }

    display.drawFastHLine(8, 55, display.width() - 16, TFT_DARKGREY);

    // 3. Two-Column Button Status Section (Local KEY1 TX vs Remote Server KEY1 RX)
    // Left Column: Local KEY1 (Sent to 0x1237)
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(8, 64);
    display.print("LOCAL KEY1 (TX)");

    display.setTextSize(2);
    display.setTextColor(lastKey1State ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
    display.setCursor(8, 80);
    display.print(lastKey1State ? "PRESSED" : "RELEASED");

    // Vertical divider
    display.drawFastVLine(120, 62, 62, TFT_DARKGREY);

    // Right Column: Server KEY1 (Received from 0x1236)
    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(128, 64);
    display.print("SERVER KEY1 (RX)");

    display.setTextSize(2);
    if (!isConnected) {
        display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        display.setCursor(128, 80);
        display.print("---");
    } else {
        display.setTextColor(serverKey1State ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
        display.setCursor(128, 80);
        display.print(serverKey1State ? "PRESSED" : "RELEASED");
    }

    // Footer hint
    display.setTextSize(1);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.setCursor(8, 118);
    display.print("KEY1: Send 1/0   KEY2: Connect/Disc");
}

void startScanning() {
    if (!isScanning && !isConnected) {
        Serial.println("Scanning for NimBLE-Secure-Server...");
        NimBLEDevice::getScan()->start(0, false);
        isScanning   = true;
        displayDirty = true;
    }
}

void stopAndDisconnect() {
    doConnect = false;
    if (isScanning) {
        NimBLEDevice::getScan()->stop();
        isScanning = false;
        Serial.println("Scanning stopped (IDLE).");
    }
    if (pClient != nullptr && pClient->isConnected()) {
        Serial.println("Disconnecting from server...");
        pClient->disconnect();
    }
    isConnected     = false;
    pRemoteRxChar   = nullptr;
    serverKey1State = 0;
    strncpy(serverAddrStr, "None", sizeof(serverAddrStr));
    displayDirty    = true;
}

bool connectAndDiscover() {
    if (pClient == nullptr) {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(&clientCallbacks, false);
        pClient->setConnectionParams(12, 12, 0, 150);
        pClient->setConnectTimeout(5000);
    }

    if (!pClient->connect(advDevice)) {
        Serial.println("Failed to connect.");
        displayDirty = true;
        return false;
    }

    // 1. Discover Custom Service ("ABCD")
    NimBLERemoteService* pCustomSvc = pClient->getService(CUSTOM_SERVICE_UUID);
    if (pCustomSvc) {
        NimBLERemoteCharacteristic* pNonSec = pCustomSvc->getCharacteristic(NON_SECURE_CHAR_UUID);
        if (pNonSec && pNonSec->canRead()) {
            Serial.printf("[READ] 1234 (Non-Secure): %s\n", pNonSec->readValue().c_str());
        }

        NimBLERemoteCharacteristic* pBtn = pCustomSvc->getCharacteristic(BUTTON_CHAR_UUID);
        if (pBtn) {
            if (pBtn->canRead()) {
                serverKey1State = pBtn->readValue<uint8_t>();
                Serial.printf("[READ] 1236 (Initial Server KEY1): %u\n", serverKey1State);
            }
            if (pBtn->canNotify()) {
                pBtn->subscribe(true, notifyCallback);
            }
        }

        pRemoteRxChar = pCustomSvc->getCharacteristic(RX_DATA_CHAR_UUID);
        if (pRemoteRxChar) {
            if (pRemoteRxChar->canRead()) {
                Serial.printf("[READ] 1237 (Initial RX): 0x%02X\n", pRemoteRxChar->readValue<uint8_t>());
            }
            if (pRemoteRxChar->canNotify()) {
                pRemoteRxChar->subscribe(true, notifyCallback);
            }
            // Immediately sync current local KEY1 state upon connecting
            uint8_t initialKey1 = readKey1();
            if (pRemoteRxChar->canWrite()) {
                pRemoteRxChar->writeValue(&initialKey1, 1, true);
            }
        }

        NimBLERemoteCharacteristic* pSec = pCustomSvc->getCharacteristic(SECURE_CHAR_UUID);
        if (pSec && pSec->canRead()) {
            Serial.println("Reading secure characteristic 1235 (initiating passkey pairing)...");
            if (pClient->secureConnection()) {
                Serial.printf("[READ] 1235 (Secure): %s\n", pSec->readValue().c_str());
            }
        }
    }

    // 2. Discover Standard Battery Service ("180F")
    NimBLERemoteService* pBatSvc = pClient->getService(BATTERY_SERVICE_UUID);
    if (pBatSvc) {
        NimBLERemoteCharacteristic* pBatLvl = pBatSvc->getCharacteristic(BATTERY_LEVEL_CHAR_UUID);
        if (pBatLvl) {
            if (pBatLvl->canRead()) {
                serverBatPct = pBatLvl->readValue<uint8_t>();
                Serial.printf("[READ] Server Battery Level: %u%%\n", serverBatPct);
            }
            if (pBatLvl->canNotify()) {
                pBatLvl->subscribe(true, notifyCallback);
            }
        }

        NimBLERemoteCharacteristic* pBatState = pBatSvc->getCharacteristic(BATTERY_POWER_STATE_CHAR_UUID);
        if (pBatState) {
            if (pBatState->canRead()) {
                uint8_t state  = pBatState->readValue<uint8_t>();
                serverCharging = ((state >> 4) & 0x03) == 0x03;
                Serial.printf("[READ] Server Power State: 0x%02X\n", state);
            }
            if (pBatState->canNotify()) {
                pBatState->subscribe(true, notifyCallback);
            }
        }

        NimBLERemoteCharacteristic* pBatVolt = pBatSvc->getCharacteristic(BATTERY_VOLTAGE_CHAR_UUID);
        if (pBatVolt) {
            if (pBatVolt->canRead()) {
                uint16_t mv    = pBatVolt->readValue<uint16_t>();
                serverBatVolts = mv / 1000.0f;
                Serial.printf("[READ] Server Battery Voltage: %.3f V (%u mV)\n", serverBatVolts, mv);
            }
            if (pBatVolt->canNotify()) {
                pBatVolt->subscribe(true, notifyCallback);
            }
        }
    }

    displayDirty = true;
    return true;
}

void setup() {
    Serial.begin(115200);

    // Initialize display immediately and show Booting screen
    display.begin();
    display.setRotation(1); // Landscape orientation (240x135)
    renderBootScreen();

    delay(1500);
    Serial.println("Starting NimBLE Central Client...");

    // Initialize KEY1 and KEY2 buttons
    pinMode(KEY1, INPUT_PULLUP);
    pinMode(KEY2, INPUT_PULLUP);
    lastKey1State = readKey1();
    lastKey2State = readKey2();

    NimBLEDevice::init("NimBLE-Central");
    NimBLEDevice::setPower(3);

    NimBLEDevice::setSecurityAuth(true, true, false);
    NimBLEDevice::setSecurityPasskey(SERVER_PASSKEY);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(&scanCallbacks, false);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->setActiveScan(true);

    // Start scanning on boot (KEY2 toggles off/on anytime)
    startScanning();

    // Switch from Booting screen to active Status screen
    displayDirty = false;
    renderStatusScreen();
}

void loop() {
    unsigned long now = millis();

    // 1. Refresh display safely on the main loop task
    if (displayDirty) {
        displayDirty = false;
        renderStatusScreen();
    }

    // 2. Handle KEY2 Press: Toggle Scan/Connect vs. Disconnect/Idle
    uint8_t currentKey2 = readKey2();
    if (currentKey2 != lastKey2State && (now - lastKey2Change) >= BTN_DEBOUNCE_MS) {
        lastKey2Change = now;
        lastKey2State  = currentKey2;

        if (currentKey2 == 1) {
            if (isConnected || isScanning || doConnect) {
                stopAndDisconnect();
            } else {
                startScanning();
            }
        }
    }

    // 3. Complete pending connection triggered by scan result
    if (doConnect) {
        doConnect = false;
        if (connectAndDiscover()) {
            Serial.println("Discovery & subscriptions complete!");
        } else {
            Serial.println("Connection failed. Press KEY2 to retry.");
            displayDirty = true;
        }
    }

    // 4. Handle KEY1 State Change: Write 1 (pressed) or 0 (released) to 0x1237
    uint8_t currentKey1 = readKey1();
    if (currentKey1 != lastKey1State && (now - lastKey1Change) >= BTN_DEBOUNCE_MS) {
        lastKey1Change = now;
        lastKey1State  = currentKey1;
        displayDirty   = true;

        if (isConnected && pRemoteRxChar != nullptr && pRemoteRxChar->canWrite()) {
            pRemoteRxChar->writeValue(&lastKey1State, 1, true);
            Serial.printf("[WRITE] KEY1 -> 0x1237 = %u\n", lastKey1State);
        } else {
            Serial.printf("KEY1 -> %u (not connected)\n", lastKey1State);
        }
    }

    delay(5);
}