/*
  This script demonstrates a BLE peripheral

  Seth McNeill
  2026 August 17
*/

#include <Arduino.h>
#include <Arduino_Nesso_N1.h>
#include "time.h"
#include <esp_crt_bundle.h>
#include <esp_mac.h>
#include <ArduinoJson.h>

#include "secrets.h"

#include <NimBLEDevice.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

class RandomDataCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        uint32_t randomVal = random(0, 100000);
        pCharacteristic->setValue((uint8_t*)&randomVal, sizeof(randomVal));
        
        Serial.printf("Client (%s) read data. Sent random value: %u\n", 
                      connInfo.getAddress().toString().c_str(), 
                      randomVal);
    }
};

void setup() {
    Serial.begin(115200);
    delay(3000);
    while (!Serial) { delay(10); }

    Serial.println("Starting NimBLE BLE5 Peripheral...");
    randomSeed(analogRead(0));

    // 1. Initialize NimBLE Device
    NimBLEDevice::init("drseth");

    NimBLEAddress macAddress = NimBLEDevice::getAddress();
    Serial.printf("Device BLE MAC Address: %s\n", macAddress.toString().c_str());

    // 2. Create Server & Service
    NimBLEServer* pServer = NimBLEDevice::createServer();
    NimBLEService* pService = pServer->createService(SERVICE_UUID);

    // 3. Create Characteristic with READ property
    NimBLECharacteristic* pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::READ
    );
    pCharacteristic->setCallbacks(new RandomDataCallbacks());

    uint32_t initialVal = 0;
    pCharacteristic->setValue((uint8_t*)&initialVal, sizeof(initialVal));

    // 4. Start Server
    pServer->start();

    // 5. Build Advertising Data using NimBLEExtAdvertisement
    NimBLEExtAdvertisement advData;
    advData.setName("drseth");
    advData.setCompleteServices(NimBLEUUID(SERVICE_UUID));

    // 6. Register payload to advertising set 0
    NimBLEExtAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setInstanceData(0, advData);

    // 7. Start advertising set 0
    pAdvertising->start(0);
    
    Serial.println("BLE Advertising started. Waiting for queries/reads...");
}

void loop() {
    delay(1000);
}