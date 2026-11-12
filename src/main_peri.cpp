/*
  This script demonstrates a BLE peripheral

  Seth McNeill
  2026 August 17
*/


/**
 * NimBLE_Secure_Server Demo:
 *
 * This example demonstrates the secure passkey protected conenction and communication between an esp32 server and an
 * esp32 client. Please note that esp32 stores auth info in nvs memory. After a successful connection it is possible
 * that a passkey change will be ineffective. To avoid this clear the memory of the esp32's between security testings.
 * esptool.py is capable of this, example: esptool.py --port /dev/ttyUSB0 erase_flash.
 *
 *  Created: on Jan 08 2021
 *      Author: mblasee
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.printf("Client connected: %s\n", connInfo.getAddress().toString().c_str());
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        Serial.printf("Client disconnected (reason: %d). Restarting advertising...\n", reason);
        NimBLEDevice::getAdvertising()->start(0);
    }
};

static ServerCallbacks serverCallbacks;

void setup() {
    Serial.begin(115200);
    delay(4000);
    Serial.println("Starting NimBLE Server");
    NimBLEDevice::init("NimBLE");
    NimBLEDevice::setPower(3); /** +3db */

    NimBLEDevice::setSecurityAuth(true, true, false); /** bonding, MITM, don't need BLE secure connections as we are using passkey pairing */
    NimBLEDevice::setSecurityPasskey(123456);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY); /** Display only passkey */
    NimBLEServer*         pServer                  = NimBLEDevice::createServer();
    NimBLEService*        pService                 = pServer->createService("ABCD");
    NimBLECharacteristic* pNonSecureCharacteristic = pService->createCharacteristic("1234", NIMBLE_PROPERTY::READ);
    NimBLECharacteristic* pSecureCharacteristic =
        pService->createCharacteristic("1235",
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);

    pNonSecureCharacteristic->setValue("Hello Non Secure BLE");
    pSecureCharacteristic->setValue("Hello Secure BLE");
    
    // Add callbacks to server
    pServer->setCallbacks(&serverCallbacks);

    // Create a legacy-compatible advertisement instance
    NimBLEExtAdvertisement extAdv;
    extAdv.setLegacyAdvertising(true); // Broadcasts standard BLE 4.x packets
    extAdv.setScannable(true);
    extAdv.setConnectable(true);
    extAdv.setName("NimBLE-Secure-Server");
    extAdv.setCompleteServices(NimBLEUUID("ABCD"));

    // Register and start instance 0
    NimBLEExtAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (pAdvertising->setInstanceData(0, extAdv)) {
        pAdvertising->start(0);
    }
}

void loop() {
    delay(3000);
}