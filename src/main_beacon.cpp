/*
  This script demonstrates a BLE beacon

  Seth McNeill
  2026 November 13
*/


#include <Arduino.h>
#include <Arduino_Nesso_N1.h>
#include <NimBLEDevice.h>
#include <NimBLEBeacon.h>

// Custom 128-bit Proximity UUID
#define BEACON_UUID "8ec76ea3-6668-48da-9866-75be8bc86f4d"
#define BEACON_NAME "NessoBeacon3"

// Beacon identifiers & timing
#define BEACON_MAJOR 1
#define MINOR_DEFAULT 100
#define MINOR_PRESSED 101
#define MEASURED_POWER -59       // Calibrated RSSI at 1 meter (in dBm)
#define BATTERY_REFRESH_MS 2000  // How often to poll the BQ27220 fuel gauge (ms)

// Nesso N1 Display & Battery objects
NessoDisplay display;
NessoBattery battery;

static uint8_t advInstance = 0;
unsigned long lastBatteryUpdate = 0;
bool lastButtonState = HIGH;

// Updates the TX indicator circle (Red when KEY1 is pressed/301, Green when default/300)
void updateBeaconDisplay(bool isPressed, uint16_t minorValue) {
  if (isPressed) {
    display.fillCircle(214, 22, 12, TFT_RED);
    display.drawCircle(214, 22, 14, TFT_WHITE);
  } else {
    display.fillCircle(214, 22, 14, TFT_BLACK); // Clear outer ring
    display.fillCircle(214, 22, 12, TFT_GREEN);
  }

  // Update Minor value display
  display.setTextSize(2);
  display.setTextColor(isPressed ? TFT_RED : TFT_CYAN, TFT_BLACK);
  display.setCursor(10, 48);
  display.printf("Major:%u Minor:%u ", BEACON_MAJOR, minorValue);
}

// Reads BQ27220 battery gauge and updates the bottom half of the screen
void updateBatteryDisplay() {
  float voltage = battery.getVoltage();
  uint16_t percentage = battery.getChargeLevel();
  bool isCharging = (battery.getChargeStatus() == NessoBattery::CHARGING);

  uint16_t batColor = TFT_GREEN;
  if (percentage <= 25) {
    batColor = TFT_RED;
  } else if (percentage <= 50) {
    batColor = TFT_YELLOW;
  }

  display.setTextSize(2);
  display.setTextColor(batColor, TFT_BLACK);
  display.setCursor(10, 86);
  display.printf("Bat: %3u%%  %.2fV ", percentage, voltage);

  display.setTextSize(1);
  display.setTextColor(isCharging ? TFT_GREEN : TFT_LIGHTGREY, TFT_BLACK);
  display.setCursor(10, 114);
  display.printf("Power: %s   ", isCharging ? "Charging" : "Battery ");
}

// Draws static UI layout once on boot
void initScreenUI() {
  display.begin();
  display.setRotation(1); // Landscape orientation (240 x 135)
  display.fillScreen(TFT_BLACK);

  // Header text
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setCursor(10, 14);
  display.print("iBeacon Active");

  // Divider lines
  display.drawFastHLine(0, 40, 240, TFT_DARKGREY);
  display.drawFastHLine(0, 76, 240, TFT_DARKGREY);

  updateBeaconDisplay(false, MINOR_DEFAULT);
  updateBatteryDisplay();
}

// Configures and starts continuous legacy iBeacon advertising
void startContinuousBeacon(uint16_t minorValue, bool isPressed) {
  NimBLEExtAdvertising *pAdvertising = NimBLEDevice::getAdvertising();

  if (pAdvertising->isAdvertising()) {
    pAdvertising->stop(advInstance);
  }

  NimBLEBeacon oBeacon;
  oBeacon.setManufacturerId(0x4C00); // Apple Company ID
  oBeacon.setProximityUUID(NimBLEUUID(BEACON_UUID));
  oBeacon.setMajor(BEACON_MAJOR);
  oBeacon.setMinor(minorValue);
  oBeacon.setSignalPower(MEASURED_POWER);

  NimBLEExtAdvertisement oAdvertisementData;
  oAdvertisementData.setLegacyAdvertising(true);
  oAdvertisementData.setConnectable(false);
  oAdvertisementData.setScannable(false);
  oAdvertisementData.setFlags(0x06);

  // Standard Apple iBeacon advertising interval: 100ms (160 * 0.625ms = 100ms)
  oAdvertisementData.setMinInterval(160);
  oAdvertisementData.setMaxInterval(160);

  NimBLEBeacon::BeaconData beaconData = oBeacon.getData();
  oAdvertisementData.setManufacturerData(
      reinterpret_cast<const uint8_t *>(&beaconData),
      sizeof(beaconData)
  );

  pAdvertising->setInstanceData(advInstance, oAdvertisementData);
  pAdvertising->start(advInstance);

  // Update screen status
  updateBeaconDisplay(isPressed, minorValue);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Initialize battery monitoring & enable LiPo charging when USB-C is connected
  battery.begin();
  battery.enableCharge();

  // Initialize KEY1 via Nesso N1 I2C expander
  pinMode(KEY1, INPUT_PULLUP);
  lastButtonState = digitalRead(KEY1);

  // Initialize LCD screen
  initScreenUI();

  // Initialize NimBLE
  NimBLEDevice::init(BEACON_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Start continuous beacon broadcasting
  bool isPressed = (lastButtonState == LOW);
  startContinuousBeacon(isPressed ? MINOR_PRESSED : MINOR_DEFAULT, isPressed);
}

void loop() {
  unsigned long now = millis();
  bool currentButtonState = digitalRead(KEY1); // Active LOW when pressed

  // Update beacon payload only when KEY1 state changes (pressed or released)
  if (currentButtonState != lastButtonState) {
    lastButtonState = currentButtonState;
    bool isPressed = (currentButtonState == LOW);
    startContinuousBeacon(isPressed ? MINOR_PRESSED : MINOR_DEFAULT, isPressed);
    delay(30); // Debounce
  }

  // Periodically refresh battery percentage and voltage on screen
  if (now - lastBatteryUpdate >= BATTERY_REFRESH_MS) {
    lastBatteryUpdate = now;
    updateBatteryDisplay();
  }

  delay(10);
}
