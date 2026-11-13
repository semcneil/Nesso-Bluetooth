/*
  This script demonstrates a BLE peripheral. It is based on the NimBLE_Secure_Server
  that is part of the NimBLE-Arduino library.

  Seth McNeill
  2026 November 12
*/


#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Arduino_Nesso_N1.h>

// Custom Service & Characteristics
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

// Timing constants
constexpr unsigned long BAT_NOTIFY_DELAY = 3000;
constexpr unsigned long BTN_DEBOUNCE_MS  = 15;

NessoDisplay display;
NessoBattery battery;

NimBLECharacteristic* pButtonChar         = nullptr;
NimBLECharacteristic* pRxDataChar         = nullptr;
NimBLECharacteristic* pBatteryLevelChar   = nullptr;
NimBLECharacteristic* pBatteryStateChar   = nullptr;
NimBLECharacteristic* pBatteryVoltageChar = nullptr;

// State tracking
unsigned long lastBatteryUpdateMs = 0;
unsigned long lastKey1ChangeMs    = 0;
unsigned long lastKey2ChangeMs    = 0;
uint8_t       lastKey1State       = 0;
uint8_t       lastKey2State       = 0;
uint8_t       receivedByte        = 0;

// Cached battery telemetry for display
uint8_t       currentBatPct       = 0;
float         currentBatVolts     = 0.0f;
bool          isCharging          = false;

// Granular UI dirty flags
volatile bool fullRedrawDirty     = false;
volatile bool ioDirty             = false;
volatile bool batteryDirty        = false;

bool          isConnected         = false;
char          peerIdStr[24]       = "None";

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        String addr = String(connInfo.getAddress().toString().c_str());
        addr.toUpperCase();
        strncpy(peerIdStr, addr.c_str(), sizeof(peerIdStr) - 1);
        peerIdStr[sizeof(peerIdStr) - 1] = '\0';

        // Match Central's 30ms - 45ms connection interval
        pServer->updateConnParams(connInfo.getConnHandle(), 24, 36, 0, 200);

        isConnected     = true;
        fullRedrawDirty = true;
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        strncpy(peerIdStr, "None", sizeof(peerIdStr));
        isConnected     = false;
        fullRedrawDirty = true;
        NimBLEDevice::getAdvertising()->start(0);
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue val = pCharacteristic->getValue();
        if (val.size() > 0) {
            receivedByte = val[0];
            ioDirty      = true;
            pCharacteristic->setValue(&receivedByte, 1);
        }
    }
};

static ServerCallbacks serverCallbacks;
static RxCallbacks     rxCallbacks;

void renderBootScreen() {
    display.fillScreen(TFT_BLACK);
    display.fillRect(0, 0, display.width(), 28, TFT_PURPLE);
    display.setTextColor(TFT_WHITE, TFT_PURPLE);
    display.setTextSize(1);
    display.setCursor(10, 10);
    display.print("NESSO N1 BLE PERIPHERAL");

    display.setTextColor(TFT_MAGENTA, TFT_BLACK);
    display.setTextSize(2);
    display.setCursor(20, (display.height() / 2) - 12);
    display.print("Booting...");
}

// Fast partial update for KEY1 TX and RX 0x1237 values (< 1 ms)
void updateIoRegion() {
    display.setTextSize(2);
    display.setTextColor(lastKey1State ? TFT_CYAN : TFT_LIGHTGREY, TFT_BLACK);
    display.setCursor(8, 78);
    display.print(lastKey1State ? "PRESSED " : "RELEASED");

    display.setTextColor(receivedByte ? TFT_YELLOW : TFT_LIGHTGREY, TFT_BLACK);
    display.setCursor(128, 78);
    display.printf("0x%02X (%u) ", receivedByte, receivedByte);
}

// Fast partial update for footer battery readout
void updateBatteryRegion() {
    display.setTextSize(1);
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.setCursor(8, 119);
    const char* chgTag = isCharging ? "CHG" : "BAT";
    display.printf("[PERIPH] BAT: %3u%% (%.2fV) [%s] ", currentBatPct, currentBatVolts, chgTag);
}

void renderFullScreen() {
    display.fillScreen(TFT_BLACK);

    uint16_t bannerColor = isConnected ? TFT_PURPLE : TFT_DARKCYAN;

    display.fillRect(0, 0, display.width(), 32, bannerColor);
    display.setTextColor(TFT_WHITE, bannerColor);
    display.setTextSize(2);
    display.setCursor(8, 8);
    display.print(isConnected ? "CONNECTED" : "READY");

    display.setTextSize(1);
    display.setCursor(142, 6);
    display.print("PERIPHERAL");
    display.setCursor(142, 18);
    display.print(isConnected ? "KEY2: Disc" : "Advertising");

    display.setTextColor(TFT_MAGENTA, TFT_BLACK);
    display.setCursor(8, 40);
    display.print("CLIENT:");

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(56, 40);
    display.print(isConnected ? peerIdStr : "Waiting for central...");

    display.drawFastHLine(8, 54, display.width() - 16, TFT_DARKGREY);

    display.setTextColor(TFT_MAGENTA, TFT_BLACK);
    display.setCursor(8, 62);
    display.print("LOCAL KEY1 (TX)");

    display.drawFastVLine(120, 60, 48, TFT_DARKGREY);

    display.setCursor(128, 62);
    display.print("RX VALUE (0x1237)");

    display.drawFastHLine(8, 112, display.width() - 16, TFT_DARKGREY);

    updateIoRegion();
    updateBatteryRegion();
}

uint8_t readKey1() { return (digitalRead(KEY1) == LOW) ? 1 : 0; }
uint8_t readKey2() { return (digitalRead(KEY2) == LOW) ? 1 : 0; }

void disconnectAllClients() {
    NimBLEServer* pServer = NimBLEDevice::getServer();
    for (uint16_t handle : pServer->getPeerDevices()) {
        pServer->disconnect(handle);
    }
}

uint8_t encodeBatteryPowerState(NessoBattery::ChargeStatus status, uint8_t percentage) {
    bool charging = (status == NessoBattery::CHARGING);
    uint8_t presentField     = 0x03;
    uint8_t dischargingField = charging ? 0x03 : 0x02;
    uint8_t chargingField    = charging ? 0x03 : 0x02;
    uint8_t levelField       = (percentage <= 10) ? 0x03 : 0x02;
    return (levelField << 6) | (chargingField << 4) | (dischargingField << 2) | presentField;
}

void updateBatteryMetrics() {
    uint16_t voltageMv = battery.getVoltage();
    uint8_t percentage = battery.getChargeLevel();
    auto status        = battery.getChargeStatus();

    if (percentage > 100) percentage = 100;

    currentBatPct   = percentage;
    currentBatVolts = voltageMv / 1000.0f;
    isCharging      = (status == NessoBattery::CHARGING);

    uint8_t powerStateByte = encodeBatteryPowerState(status, percentage);

    pBatteryLevelChar->setValue(&percentage, 1);
    pBatteryStateChar->setValue(&powerStateByte, 1);
    pBatteryVoltageChar->setValue(reinterpret_cast<uint8_t*>(&voltageMv), sizeof(voltageMv));

    batteryDirty = true;
}

void setup() {
    Serial.begin(115200);

    display.begin();
    display.setRotation(1);
    renderBootScreen();

    delay(1000);

    pinMode(KEY1, INPUT_PULLUP);
    pinMode(KEY2, INPUT_PULLUP);
    lastKey1State = readKey1();
    lastKey2State = readKey2();

    battery.begin();

    NimBLEDevice::init("NimBLE");
    NimBLEDevice::setPower(9); // +9 dBm TX power

    NimBLEDevice::setSecurityAuth(true, true, false);
    NimBLEDevice::setSecurityPasskey(123456);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

    NimBLEServer* pServer = NimBLEDevice::createServer();

    NimBLEService*        pService                 = pServer->createService(CUSTOM_SERVICE_UUID);
    NimBLECharacteristic* pNonSecureCharacteristic = pService->createCharacteristic(NON_SECURE_CHAR_UUID, NIMBLE_PROPERTY::READ);
    NimBLECharacteristic* pSecureCharacteristic    =
        pService->createCharacteristic(SECURE_CHAR_UUID,
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);

    pButtonChar = pService->createCharacteristic(
        BUTTON_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Include WRITE_NR so Central can send fast unacknowledged writes
    pRxDataChar = pService->createCharacteristic(
        RX_DATA_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
    );
    pRxDataChar->setValue(&receivedByte, 1);
    pRxDataChar->setCallbacks(&rxCallbacks);

    NimBLEDescriptor* pRxDesc = pRxDataChar->createDescriptor("2901", NIMBLE_PROPERTY::READ);
    pRxDesc->setValue("Receive Data");

    pNonSecureCharacteristic->setValue("Hello Non Secure BLE");
    pSecureCharacteristic->setValue("Hello Secure BLE");
    pButtonChar->setValue(&lastKey1State, 1);
    pService->start();

    NimBLEService* pBatteryService = pServer->createService(BATTERY_SERVICE_UUID);
    pBatteryLevelChar   = pBatteryService->createCharacteristic(BATTERY_LEVEL_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    pBatteryStateChar   = pBatteryService->createCharacteristic(BATTERY_POWER_STATE_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    pBatteryVoltageChar = pBatteryService->createCharacteristic(BATTERY_VOLTAGE_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    NimBLE2904* pFormat2904 = static_cast<NimBLE2904*>(pBatteryVoltageChar->createDescriptor("2904", NIMBLE_PROPERTY::READ));
    pFormat2904->setFormat(NimBLE2904::FORMAT_UINT16);
    pFormat2904->setExponent(-3);
    pFormat2904->setUnit(0x2728);
    pFormat2904->setNamespace(1);

    pBatteryService->start();

    updateBatteryMetrics();
    lastBatteryUpdateMs = millis();

    pServer->setCallbacks(&serverCallbacks);

    NimBLEExtAdvertisement extAdv;
    extAdv.setLegacyAdvertising(true);
    extAdv.setScannable(true);
    extAdv.setConnectable(true);
    extAdv.setName("NimBLE-Secure-Server");
    extAdv.setCompleteServices(NimBLEUUID(CUSTOM_SERVICE_UUID));
    extAdv.addServiceUUID(NimBLEUUID(BATTERY_SERVICE_UUID));

    NimBLEExtAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (pAdvertising->setInstanceData(0, extAdv)) {
        pAdvertising->start(0);
    }

    fullRedrawDirty = false;
    ioDirty         = false;
    batteryDirty    = false;
    renderFullScreen();
}

void loop() {
    unsigned long now = millis();

    // 1. Handle KEY1 FIRST for immediate notification
    uint8_t currentKey1 = readKey1();
    if (currentKey1 != lastKey1State && (now - lastKey1ChangeMs) >= BTN_DEBOUNCE_MS) {
        lastKey1ChangeMs = now;
        lastKey1State    = currentKey1;
        ioDirty          = true;

        pButtonChar->setValue(&lastKey1State, 1);
        if (NimBLEDevice::getServer()->getConnectedCount() > 0) {
            pButtonChar->notify();
        }
    }

    // 2. Handle KEY2 disconnect
    uint8_t currentKey2 = readKey2();
    if (currentKey2 != lastKey2State && (now - lastKey2ChangeMs) >= BTN_DEBOUNCE_MS) {
        lastKey2ChangeMs = now;
        lastKey2State    = currentKey2;

        if (currentKey2 == 1 && isConnected) {
            disconnectAllClients();
        }
    }

    // 3. Granular Display Updates
    if (fullRedrawDirty) {
        fullRedrawDirty = false;
        ioDirty         = false;
        batteryDirty    = false;
        renderFullScreen();
    } else {
        if (ioDirty) {
            ioDirty = false;
            updateIoRegion();
        }
        if (batteryDirty) {
            batteryDirty = false;
            updateBatteryRegion();
        }
    }

    // 4. Periodic Battery Update & Notification
    if ((now - lastBatteryUpdateMs) >= BAT_NOTIFY_DELAY) {
        lastBatteryUpdateMs = now;
        updateBatteryMetrics();

        if (NimBLEDevice::getServer()->getConnectedCount() > 0) {
            pBatteryLevelChar->notify();
            pBatteryStateChar->notify();
            pBatteryVoltageChar->notify();
        }
    }

    delay(1);
}