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
#define CUSTOM_SERVICE_UUID         "ABCD"
#define NON_SECURE_CHAR_UUID        "1234"
#define SECURE_CHAR_UUID            "1235"
#define BUTTON_CHAR_UUID            "1236"
#define RX_DATA_CHAR_UUID           "1237"

// Standard Bluetooth SIG Battery Service & Characteristics
#define BATTERY_SERVICE_UUID        "180F"
#define BATTERY_LEVEL_CHAR_UUID     "2A19"
#define BATTERY_VOLTAGE_CHAR_UUID   "2B18"

// Timing constants
constexpr unsigned long BAT_NOTIFY_DELAY = 3000;
constexpr unsigned long BTN_DEBOUNCE_MS  = 20;

NessoBattery battery;

NimBLECharacteristic* pButtonChar         = nullptr;
NimBLECharacteristic* pRxDataChar         = nullptr;
NimBLECharacteristic* pBatteryLevelChar   = nullptr;
NimBLECharacteristic* pBatteryVoltageChar = nullptr;

// State tracking
unsigned long lastBatteryUpdateMs = 0;
unsigned long lastButtonChangeMs  = 0;
uint8_t       lastButtonState     = 0;
uint8_t       receivedByte        = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.printf("Client connected: %s\n", connInfo.getAddress().toString().c_str());
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        Serial.printf("Client disconnected (reason: %d). Restarting advertising...\n", reason);
        NimBLEDevice::getAdvertising()->start(0);
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue val = pCharacteristic->getValue();
        if (val.size() > 0) {
            receivedByte = val[0];
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

uint8_t readButtonState() {
    // KEY1 is active-low on the Arduino Nesso N1
    return (digitalRead(KEY1) == LOW) ? 1 : 0;
}

void updateBatteryMetrics() {
    uint16_t voltageMv = battery.getVoltage();       // Millivolts (e.g., 3850)
    uint8_t percentage = battery.getChargeLevel();   // 0 - 100%

    if (percentage > 100) percentage = 100;
    pBatteryLevelChar->setValue(&percentage, 1);

    // Millivolts scaled by 10^-3 via NimBLE2904
    pBatteryVoltageChar->setValue(reinterpret_cast<uint8_t*>(&voltageMv), sizeof(voltageMv));

    Serial.printf("Battery Updated -> %.3f V (%u%%)\n", voltageMv / 1000.0f, percentage);
}

void setup() {
    Serial.begin(115200);
    delay(4000);
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

    // Optional User Description descriptor (0x2901) so LightBlue displays "Receive Data"
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
}

void loop() {
    unsigned long now = millis();

    // 1. Rapid Button Polling & Change Notification
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

    // 2. Periodic Battery Update & Notification (every BAT_NOTIFY_DELAY ms)
    if ((now - lastBatteryUpdateMs) >= BAT_NOTIFY_DELAY) {
        lastBatteryUpdateMs = now;
        updateBatteryMetrics();

        if (NimBLEDevice::getServer()->getConnectedCount() > 0) {
            pBatteryLevelChar->notify();
            pBatteryVoltageChar->notify();
        }
    }

    delay(5);
}