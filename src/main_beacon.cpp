/*
  This script demonstrates a BLE beacon

  Seth McNeill
  2026 November 13
*/


#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEBeacon.h>

// Custom 128-bit Proximity UUID (Generate your own unique UUID for production)
#define BEACON_UUID "8ec76ea3-6668-48da-9866-75be8bc86f4d"

// Beacon identifiers
#define BEACON_MAJOR 1
#define BEACON_MINOR 101
#define MEASURED_POWER -59 // Calibrated RSSI at 1 meter (in dBm)

// Advertising instance ID (0 is standard for single-beacon setups)
static uint8_t advInstance = 0;

void initBeacon() {
  NimBLEBeacon oBeacon = NimBLEBeacon();

  // 0x4C00 is Apple's Company Identifier (required for iBeacon recognition)
  oBeacon.setManufacturerId(0x4C00);
  oBeacon.setProximityUUID(NimBLEUUID(BEACON_UUID));
  oBeacon.setMajor(BEACON_MAJOR);
  oBeacon.setMinor(BEACON_MINOR);
  oBeacon.setSignalPower(MEASURED_POWER);

  // Use NimBLEExtAdvertisement for ESP32-C6 BLE 5.0 controller
  NimBLEExtAdvertisement oAdvertisementData;

  // Enable legacy PDU mode so standard iOS/Android iBeacon scanners detect it
  oAdvertisementData.setLegacyAdvertising(true);
  oAdvertisementData.setConnectable(false);
  oAdvertisementData.setScannable(false);

  // 0x06 = General Discoverable Mode (0x02) + BR/EDR Not Supported (0x04)
  oAdvertisementData.setFlags(0x06);

  // Pass the BeaconData struct directly as Manufacturer Specific Data
  NimBLEBeacon::BeaconData beaconData = oBeacon.getData();
  oAdvertisementData.setManufacturerData(
      reinterpret_cast<const uint8_t *>(&beaconData),
      sizeof(beaconData)
  );

  // Configure and start extended advertising on instance 0
  NimBLEExtAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setInstanceData(advInstance, oAdvertisementData);
  pAdvertising->start(advInstance);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting NimBLE ESP32-C6 iBeacon on Arduino Nesso N1...");

  // Initialize NimBLE
  NimBLEDevice::init("Nesso-N1-Beacon");

  // Optional: Set TX power (+9 dBm)
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  initBeacon();
  Serial.println("NimBLE iBeacon advertising started.");
}

void loop() {
  delay(2000);
}
