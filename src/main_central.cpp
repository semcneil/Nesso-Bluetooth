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

// Passkey & Timing Constants
constexpr uint32_t      SERVER_PASSKEY    = 123456;
constexpr unsigned long BTN_DEBOUNCE_MS   = 15;
constexpr unsigned long LOCAL_BAT_POLL_MS = 3000;
constexpr unsigned long RSSI_POLL_MS      = 1000;

NessoDisplay display;
NessoBattery battery;

// Client state flags
static NimBLEAdvertisedDevice* advDevice   = nullptr;
static NimBLEClient*           pClient     = nullptr;
static bool                    doConnect   = false;
static bool                    isConnected = false;
static bool                    isScanning  = false;

// Remote characteristic pointer for KEY1 writes
static NimBLERemoteCharacteristic* pRemoteRxChar = nullptr;

// Local button & battery state tracking
uint8_t       lastKey1State      = 0;
uint8_t       lastKey2State      = 0;
unsigned long lastKey1Change     = 0;
unsigned long lastKey2Change     = 0;
unsigned long lastLocalBatPollMs = 0;
unsigned long lastRssiPollMs     = 0;

uint8_t       localBatPct        = 0;
float         localBatVolts      = 0.0f;
bool          localCharging      = false;
int           currentRssi        = 0;

// Granular UI dirty flags
volatile bool fullRedrawDirty    = false;
volatile bool buttonsDirty       = false;
volatile bool batteryDirty       = false;
volatile bool rssiDirty          = false;

uint8_t       serverBatPct       = 0;
float         serverBatVolts     = 0.0f;
bool          serverCharging     = false;
uint8_t       serverKey1State    = 0;
char          serverAddrStr[24]  = "None";

uint8_t readKey1() { return (digitalRead(KEY1) == LOW) ? 1 : 0; }
uint8_t readKey2() { return (digitalRead(KEY2) == LOW) ? 1 : 0; }

void updateLocalBattery() {
    uint16_t voltageMv = battery.getVoltage();
    uint8_t percentage = battery.getChargeLevel();
    auto status        = battery.getChargeStatus();

    if (percentage > 100) percentage = 100;

    localBatPct   = percentage;
    localBatVolts = voltageMv / 1000.0f;
    localCharging = (status == NessoBattery::CHARGING);
    batteryDirty  = true;
}

void notifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic,
                    uint8_t* pData, size_t length, bool isNotify) {
    NimBLEUUID uuid = pRemoteCharacteristic->getUUID();

    if (uuid.equals(NimBLEUUID(BUTTON_CHAR_UUID)) && length >= 1) {
        serverKey1State = pData[0];
        buttonsDirty    = true;
    } 
    else if (uuid.equals(NimBLEUUID(BATTERY_LEVEL_CHAR_UUID)) && length >= 1) {
        serverBatPct = pData[0];
        batteryDirty = true;
    } 
    else if (uuid.equals(NimBLEUUID(BATTERY_POWER_STATE_CHAR_UUID)) && length >= 1) {
        serverCharging = ((pData[0] >> 4) & 0x03) == 0x03;
        batteryDirty   = true;
    } 
    else if (uuid.equals(NimBLEUUID(BATTERY_VOLTAGE_CHAR_UUID)) && length >= 2) {
        uint16_t voltageMv = pData[0] | (pData[1] << 8);
        serverBatVolts     = voltageMv / 1000.0f;
        batteryDirty       = true;
    }
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        String addr = String(client->getPeerAddress().toString().c_str());
        addr.toUpperCase();
        strncpy(serverAddrStr, addr.c_str(), sizeof(serverAddrStr) - 1);
        serverAddrStr[sizeof(serverAddrStr) - 1] = '\0';

        isConnected     = true;
        isScanning      = false;
        fullRedrawDirty = true;
    }

    void onDisconnect(NimBLEClient* client, int reason) override {
        isConnected     = false;
        isScanning      = false;
        pRemoteRxChar   = nullptr;
        serverKey1State = 0;
        currentRssi     = 0;
        strncpy(serverAddrStr, "None", sizeof(serverAddrStr));
        fullRedrawDirty = true;
    }

    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        NimBLEDevice::injectPassKey(connInfo, SERVER_PASSKEY);
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (!connInfo.isEncrypted()) {
            NimBLEDevice::getClientByHandle(connInfo.getConnHandle())->disconnect();
        }
    }
};

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        // if (advertisedDevice->getName() == TARGET_DEVICE_NAME ||
        //     advertisedDevice->isAdvertisingService(NimBLEUUID(CUSTOM_SERVICE_UUID))) {
        if (advertisedDevice->getName() == TARGET_DEVICE_NAME) {
            NimBLEDevice::getScan()->stop();
            isScanning = false;

            if (advDevice != nullptr) {
                delete advDevice;
                advDevice = nullptr;
            }
            advDevice       = new NimBLEAdvertisedDevice(*advertisedDevice);
            currentRssi     = advertisedDevice->getRSSI();
            doConnect       = true;
            fullRedrawDirty = true;
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
}

// Fast partial update for live RSSI signal readout on y = 40
void updateRssiRegion() {
    if (!isConnected) return;

    uint16_t rssiColor = TFT_GREENYELLOW;
    if (currentRssi < -80) {
        rssiColor = TFT_RED;
    } else if (currentRssi < -65) {
        rssiColor = TFT_YELLOW;
    }

    display.setTextSize(1);
    display.setTextColor(rssiColor, TFT_BLACK);
    display.setCursor(166, 40);
    display.printf("%4d dBm ", currentRssi);
}

// Fast partial update for button states only (< 1 ms)
void updateButtonsRegion() {
    display.setTextSize(2);
    display.setTextColor(lastKey1State ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
    display.setCursor(8, 78);
    display.print(lastKey1State ? "PRESSED " : "RELEASED");

    if (!isConnected) {
        display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        display.setCursor(128, 78);
        display.print("---     ");
    } else {
        display.setTextColor(serverKey1State ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
        display.setCursor(128, 78);
        display.print(serverKey1State ? "PRESSED " : "RELEASED");
    }
}

// Fast partial update for header & footer battery readouts
void updateBatteryRegion() {
    uint16_t bannerColor = isConnected ? TFT_DARKGREEN : ((isScanning || doConnect) ? TFT_BLUE : TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, bannerColor);
    display.setTextSize(1);

    if (isConnected) {
        const char* srvChgTag = serverCharging ? "CHG" : "BAT";
        display.setCursor(138, 6);
        display.printf("SRV:%3u%%[%s]", serverBatPct, srvChgTag);
        display.setCursor(138, 18);
        display.printf("    %.2fV  ", serverBatVolts);
    } else {
        display.setCursor(142, 6);
        display.print("CENTRAL    ");
        display.setCursor(142, 18);
        display.print("KEY2:Toggle");
    }

    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.setCursor(8, 119);
    const char* locChgTag = localCharging ? "CHG" : "BAT";
    display.printf("[CENTRAL] BAT: %3u%% (%.2fV) [%s] ", localBatPct, localBatVolts, locChgTag);
}

// Full layout redraw (only called on state transitions)
void renderFullScreen() {
    display.fillScreen(TFT_BLACK);

    uint16_t bannerColor = TFT_DARKGREY;
    const char* stateLabel = "IDLE";

    if (isConnected) {
        bannerColor = TFT_DARKGREEN;
        stateLabel  = "CONNECTED";
    } else if (isScanning || doConnect) {
        bannerColor = TFT_BLUE;
        stateLabel  = "SCANNING";
    }

    display.fillRect(0, 0, display.width(), 32, bannerColor);
    display.setTextColor(TFT_WHITE, bannerColor);
    display.setTextSize(2);
    display.setCursor(8, 8);
    display.print(stateLabel);

    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(8, 40);
    display.print("SRV:");

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(36, 40);
    if (isConnected) {
        display.print(serverAddrStr);
        updateRssiRegion();
    } else if (isScanning || doConnect) {
        display.print("Searching for server...");
    } else {
        display.print("Press KEY2 to scan");
    }

    display.drawFastHLine(8, 54, display.width() - 16, TFT_DARKGREY);

    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(8, 62);
    display.print("LOCAL KEY1 (TX)");

    display.drawFastVLine(120, 60, 48, TFT_DARKGREY);

    display.setCursor(128, 62);
    display.print("SERVER KEY1 (RX)");

    display.drawFastHLine(8, 112, display.width() - 16, TFT_DARKGREY);

    updateButtonsRegion();
    updateBatteryRegion();
}

void startScanning() {
    if (!isScanning && !isConnected) {
        NimBLEDevice::getScan()->start(0, false);
        isScanning      = true;
        fullRedrawDirty = true;
    }
}

void stopAndDisconnect() {
    doConnect = false;
    if (isScanning) {
        NimBLEDevice::getScan()->stop();
        isScanning = false;
    }
    if (pClient != nullptr && pClient->isConnected()) {
        pClient->disconnect();
    }
    isConnected     = false;
    pRemoteRxChar   = nullptr;
    serverKey1State = 0;
    currentRssi     = 0;
    strncpy(serverAddrStr, "None", sizeof(serverAddrStr));
    fullRedrawDirty = true;
}

bool connectAndDiscover() {
    if (pClient == nullptr) {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(&clientCallbacks, false);
        pClient->setConnectionParams(24, 36, 0, 200);
        pClient->setConnectTimeout(5000);
    }

    if (!pClient->connect(advDevice)) {
        return false;
    }

    currentRssi = pClient->getRssi();

    NimBLERemoteService* pCustomSvc = pClient->getService(CUSTOM_SERVICE_UUID);
    if (pCustomSvc) {
        NimBLERemoteCharacteristic* pBtn = pCustomSvc->getCharacteristic(BUTTON_CHAR_UUID);
        if (pBtn) {
            if (pBtn->canRead()) serverKey1State = pBtn->readValue<uint8_t>();
            if (pBtn->canNotify()) pBtn->subscribe(true, notifyCallback);
        }

        pRemoteRxChar = pCustomSvc->getCharacteristic(RX_DATA_CHAR_UUID);
        if (pRemoteRxChar) {
            uint8_t initialKey1 = readKey1();
            if (pRemoteRxChar->canWriteNoResponse()) {
                pRemoteRxChar->writeValue(&initialKey1, 1, false);
            } else if (pRemoteRxChar->canWrite()) {
                pRemoteRxChar->writeValue(&initialKey1, 1, true);
            }
        }

        NimBLERemoteCharacteristic* pSec = pCustomSvc->getCharacteristic(SECURE_CHAR_UUID);
        if (pSec && pSec->canRead() && pClient->secureConnection()) {
            pSec->readValue();
        }
    }

    NimBLERemoteService* pBatSvc = pClient->getService(BATTERY_SERVICE_UUID);
    if (pBatSvc) {
        NimBLERemoteCharacteristic* pBatLvl = pBatSvc->getCharacteristic(BATTERY_LEVEL_CHAR_UUID);
        if (pBatLvl) {
            if (pBatLvl->canRead()) serverBatPct = pBatLvl->readValue<uint8_t>();
            if (pBatLvl->canNotify()) pBatLvl->subscribe(true, notifyCallback);
        }

        NimBLERemoteCharacteristic* pBatState = pBatSvc->getCharacteristic(BATTERY_POWER_STATE_CHAR_UUID);
        if (pBatState) {
            if (pBatState->canRead()) {
                uint8_t state  = pBatState->readValue<uint8_t>();
                serverCharging = ((state >> 4) & 0x03) == 0x03;
            }
            if (pBatState->canNotify()) pBatState->subscribe(true, notifyCallback);
        }

        NimBLERemoteCharacteristic* pBatVolt = pBatSvc->getCharacteristic(BATTERY_VOLTAGE_CHAR_UUID);
        if (pBatVolt) {
            if (pBatVolt->canRead()) {
                serverBatVolts = pBatVolt->readValue<uint16_t>() / 1000.0f;
            }
            if (pBatVolt->canNotify()) pBatVolt->subscribe(true, notifyCallback);
        }
    }

    fullRedrawDirty = true;
    return true;
}

void setup() {
    Serial.begin(115200);

    display.begin();
    display.setRotation(1);
    renderBootScreen();

    delay(1000);

    battery.begin();
    updateLocalBattery();
    lastLocalBatPollMs = millis();

    pinMode(KEY1, INPUT_PULLUP);
    pinMode(KEY2, INPUT_PULLUP);
    lastKey1State = readKey1();
    lastKey2State = readKey2();

    NimBLEDevice::init("NimBLE-Central");
    NimBLEDevice::setPower(9);

    NimBLEDevice::setSecurityAuth(true, true, false);
    NimBLEDevice::setSecurityPasskey(SERVER_PASSKEY);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(&scanCallbacks, false);
    pScan->setInterval(80);
    pScan->setWindow(40);
    pScan->setActiveScan(true);

    // startScanning();

    fullRedrawDirty = false;
    buttonsDirty    = false;
    batteryDirty    = false;
    rssiDirty       = false;
    renderFullScreen();
}

void loop() {
    unsigned long now = millis();

    // 1. Handle KEY1 FIRST for immediate low-latency transmission
    uint8_t currentKey1 = readKey1();
    if (currentKey1 != lastKey1State && (now - lastKey1Change) >= BTN_DEBOUNCE_MS) {
        lastKey1Change = now;
        lastKey1State  = currentKey1;
        buttonsDirty   = true;

        if (isConnected && pRemoteRxChar != nullptr) {
            if (pRemoteRxChar->canWriteNoResponse()) {
                pRemoteRxChar->writeValue(&lastKey1State, 1, false);
            } else {
                pRemoteRxChar->writeValue(&lastKey1State, 1, true);
            }
        }
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

    // 3. Complete pending connection
    if (doConnect) {
        doConnect = false;
        if (!connectAndDiscover()) {
            fullRedrawDirty = true;
        }
    }

    // 4. Poll live RSSI once per second while connected
    if (isConnected && pClient != nullptr && (now - lastRssiPollMs) >= RSSI_POLL_MS) {
        lastRssiPollMs = now;
        int newRssi = pClient->getRssi();
        if (newRssi != 0 && newRssi != currentRssi) {
            currentRssi = newRssi;
            rssiDirty   = true;
        }
    }

    // 5. Granular Display Updates
    if (fullRedrawDirty) {
        fullRedrawDirty = false;
        buttonsDirty    = false;
        batteryDirty    = false;
        rssiDirty       = false;
        renderFullScreen();
    } else {
        if (buttonsDirty) {
            buttonsDirty = false;
            updateButtonsRegion();
        }
        if (rssiDirty) {
            rssiDirty = false;
            updateRssiRegion();
        }
        if (batteryDirty) {
            batteryDirty = false;
            updateBatteryRegion();
        }
    }

    // 6. Periodic local battery poll
    if ((now - lastLocalBatPollMs) >= LOCAL_BAT_POLL_MS) {
        lastLocalBatPollMs = now;
        updateLocalBattery();
    }

    delay(1);
}