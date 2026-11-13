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
constexpr unsigned long BTN_DEBOUNCE_MS  = 20;

NessoDisplay display;
NessoBattery battery;

NimBLECharacteristic* pButtonChar         = nullptr;
NimBLECharacteristic* pRxDataChar         = nullptr;
NimBLECharacteristic* pBatteryLevelChar   = nullptr;
NimBLECharacteristic* pBatteryStateChar   = nullptr;
NimBLECharacteristic* pBatteryVoltageChar = nullptr;

// State tracking
unsigned long lastBatteryUpdateMs = 0;
unsigned long lastButtonChangeMs  = 0;
uint8_t       lastButtonState     = 0;
uint8_t       receivedByte        = 0;

// Cached battery telemetry for display
uint8_t       currentBatPct       = 0;
float         currentBatVolts     = 0.0f;
bool          isCharging          = false;

// Thread-safe UI state shared between NimBLE callbacks and loop()
volatile bool displayDirty  = false;
bool          isConnected   = false;
char          peerIdStr[24] = "None";

// Forward declaration
void renderStatusScreen();

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        String addr = String(connInfo.getAddress().toString().c_str());
        addr.toUpperCase();
        strncpy(peerIdStr, addr.c_str(), sizeof(peerIdStr) - 1);
        peerIdStr[sizeof(peerIdStr) - 1] = '\0';

        isConnected  = true;
        displayDirty = true;
        Serial.printf("Client connected: %s\n", peerIdStr);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        strncpy(peerIdStr, "None", sizeof(peerIdStr));
        isConnected  = false;
        displayDirty = true;

        Serial.printf("Client disconnected (reason: %d). Restarting advertising...\n", reason);
        NimBLEDevice::getAdvertising()->start(0);
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue val = pCharacteristic->getValue();
        if (val.size() > 0) {
            receivedByte = val[0];
            displayDirty = true;
            Serial.printf("Received byte: 0x%02X (%u)\n", receivedByte, receivedByte);

            // Normalize stored value to 1 byte (uint8_t) and notify subscribed clients
            pCharacteristic->setValue(&receivedByte, 1);
            if (NimBLEDevice::getServer()->getConnectedCount() > 0) {
                pCharacteristic->notify();
            }
        }
    }
};

static ServerCallbacks serverCallbacks;
static RxCallbacks     rxCallbacks;

void renderBootScreen() {
    display.fillScreen(TFT_BLACK);

    display.fillRect(0, 0, display.width(), 28, TFT_NAVY);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.setTextSize(1);
    display.setCursor(10, 10);
    display.print("NESSO N1 BLE SERVER");

    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.setTextSize(2);
    display.setCursor(20, (display.height() / 2) - 12);
    display.print("Booting...");

    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.setTextSize(1);
    display.setCursor(20, (display.height() / 2) + 16);
    display.print("Initializing fuel gauge & radio");
}

void renderStatusScreen() {
    display.fillScreen(TFT_BLACK);

    // 1. Top Header Banner (Green = Connected, Orange = Ready)
    uint16_t bannerColor = isConnected ? TFT_DARKGREEN : TFT_ORANGE;
    uint16_t textColor   = isConnected ? TFT_WHITE : TFT_BLACK;

    display.fillRect(0, 0, display.width(), 32, bannerColor);
    display.setTextColor(textColor, bannerColor);

    display.setTextSize(2);
    display.setCursor(8, 8);
    display.print(isConnected ? "CONNECTED" : "READY");

    // Live Battery & Charging readout on the right side of the header
    display.setTextSize(1);
    const char* chgTag = isCharging ? "CHG" : "BAT";
    display.setCursor(142, 6);
    display.printf("%3u%% [%s]", currentBatPct, chgTag);

    display.setCursor(142, 18);
    display.printf("%.2fV", currentBatVolts);

    // 2. Peer / Client ID Section
    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(8, 42);
    display.print("CLIENT ID:");

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(8, 56);
    display.print(isConnected ? peerIdStr : "Waiting for connection...");

    // Divider line
    display.drawFastHLine(8, 74, display.width() - 16, TFT_DARKGREY);

    // 3. Receive Characteristic (0x1237) Value Section
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(8, 84);
    display.print("RECEIVE VALUE (0x1237):");

    display.setTextSize(2);
    display.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    display.setCursor(8, 100);
    display.printf("0x%02X  (%u)", receivedByte, receivedByte);
}

uint8_t readButtonState() {
    // KEY1 is active-low on the Arduino Nesso N1
    return (digitalRead(KEY1) == LOW) ? 1 : 0;
}

// Encodes NessoBattery::ChargeStatus into the standard GATT 0x2A1A bitfield
uint8_t encodeBatteryPowerState(NessoBattery::ChargeStatus status, uint8_t percentage) {
    bool charging = (status == NessoBattery::CHARGING);

    uint8_t presentField     = 0x03;              // 11: Battery Present
    uint8_t dischargingField = charging ? 0x03 : 0x02; // 11: Not Discharging, 10: Discharging
    uint8_t chargingField    = charging ? 0x03 : 0x02; // 11: Charging, 10: Not Charging
    uint8_t levelField       = (percentage <= 10) ? 0x03 : 0x02; // 11: Critically Low, 10: Good Level

    // return (levelField << 6) | (chargingField << 4) | (dischargingField << 2) | presentField;
    return((uint8_t)charging);
}

void updateBatteryMetrics() {
    uint16_t voltageMv = battery.getVoltage();       // Millivolts (e.g., 3850)
    uint8_t percentage = battery.getChargeLevel();   // 0 - 100%
    auto status        = battery.getChargeStatus();  // CHARGING or NOT_CHARGING

    if (percentage > 100) percentage = 100;

    currentBatPct   = percentage;
    currentBatVolts = voltageMv / 1000.0f;
    isCharging      = (status == NessoBattery::CHARGING);

    uint8_t powerStateByte = encodeBatteryPowerState(status, percentage);

    // Update BLE characteristics
    pBatteryLevelChar->setValue(&percentage, 1);
    pBatteryStateChar->setValue(&powerStateByte, 1);
    pBatteryVoltageChar->setValue(reinterpret_cast<uint8_t*>(&voltageMv), sizeof(voltageMv));

    // Trigger screen refresh
    displayDirty = true;

    Serial.printf("Battery Updated -> %.3f V (%u%%) [%s] (0x2A1A=0x%02X)\n",
                  currentBatVolts,
                  currentBatPct,
                  isCharging ? "CHARGING" : "NOT_CHARGING",
                  powerStateByte);
}

void setup() {
    Serial.begin(115200);

    // Initialize display immediately and show Booting screen
    display.begin();
    display.setRotation(1); // Landscape orientation (240x135)
    renderBootScreen();

    delay(1500);
    Serial.println("Starting NimBLE Server");

    pinMode(KEY1, INPUT_PULLUP);
    lastButtonState = readButtonState();

    battery.begin();

    NimBLEDevice::init("NimBLE");
    NimBLEDevice::setPower(3);

    NimBLEDevice::setSecurityAuth(true, true, false);
    NimBLEDevice::setSecurityPasskey(123456);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

    NimBLEServer* pServer = NimBLEDevice::createServer();

    // --- Custom Service ("ABCD") ---
    NimBLEService*        pService                 = pServer->createService(CUSTOM_SERVICE_UUID);
    NimBLECharacteristic* pNonSecureCharacteristic = pService->createCharacteristic(NON_SECURE_CHAR_UUID, NIMBLE_PROPERTY::READ);
    NimBLECharacteristic* pSecureCharacteristic    =
        pService->createCharacteristic(SECURE_CHAR_UUID,
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);

    pButtonChar = pService->createCharacteristic(
        BUTTON_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Read / Write / Notify uint8_t receiving characteristic
    pRxDataChar = pService->createCharacteristic(
        RX_DATA_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
    );
    pRxDataChar->setValue(&receivedByte, 1);
    pRxDataChar->setCallbacks(&rxCallbacks);

    NimBLEDescriptor* pRxDesc = pRxDataChar->createDescriptor("2901", NIMBLE_PROPERTY::READ);
    pRxDesc->setValue("Receive Data");

    pNonSecureCharacteristic->setValue("Hello Non Secure BLE");
    pSecureCharacteristic->setValue("Hello Secure BLE");
    pButtonChar->setValue(&lastButtonState, 1);

    // --- Standard Battery Service ("180F") ---
    NimBLEService* pBatteryService = pServer->createService(BATTERY_SERVICE_UUID);

    pBatteryLevelChar = pBatteryService->createCharacteristic(
        BATTERY_LEVEL_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pBatteryStateChar = pBatteryService->createCharacteristic(
        BATTERY_POWER_STATE_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pBatteryVoltageChar = pBatteryService->createCharacteristic(
        BATTERY_VOLTAGE_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    NimBLE2904* pFormat2904 = static_cast<NimBLE2904*>(
        pBatteryVoltageChar->createDescriptor("2904", NIMBLE_PROPERTY::READ)
    );
    pFormat2904->setFormat(NimBLE2904::FORMAT_UINT16);
    pFormat2904->setExponent(-3);
    pFormat2904->setUnit(0x2728);
    pFormat2904->setNamespace(1);

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

    // Switch from Booting screen to Ready status screen
    displayDirty = false;
    renderStatusScreen();
}

void loop() {
    unsigned long now = millis();

    // 1. Refresh display on the main task whenever BLE state, RX value, or battery updates
    if (displayDirty) {
        displayDirty = false;
        renderStatusScreen();
    }

    // 2. Rapid Button Polling & Change Notification
    uint8_t currentButtonState = readButtonState();
    if (currentButtonState != lastButtonState && (now - lastButtonChangeMs) >= BTN_DEBOUNCE_MS) {
        lastButtonChangeMs = now;
        lastButtonState    = currentButtonState;

        pButtonChar->setValue(&lastButtonState, 1);
        if (NimBLEDevice::getServer()->getConnectedCount() > 0) {
            pButtonChar->notify();
        }
        Serial.printf("KEY1 State -> %u\n", lastButtonState);
    }

    // 3. Periodic Battery Update & Notification (every BAT_NOTIFY_DELAY ms)
    if ((now - lastBatteryUpdateMs) >= BAT_NOTIFY_DELAY) {
        lastBatteryUpdateMs = now;
        updateBatteryMetrics();

        if (NimBLEDevice::getServer()->getConnectedCount() > 0) {
            pBatteryLevelChar->notify();
            pBatteryStateChar->notify();  // need to only notify if it has changed
            pBatteryVoltageChar->notify();
        }
    }

    delay(5);
}