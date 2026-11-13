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

// Beacon identifiers & timing
#define BEACON_MAJOR 1
#define MINOR_DEFAULT 100
#define MINOR_PRESSED 101
#define MEASURED_POWER -59       // Calibrated RSSI at 1 meter (in dBm)
#define BEACON_INTERVAL 2000     // Periodic broadcast interval (ms)
#define ADV_BURST_MS 150         // Duration of each BLE burst & red flash (ms)
#define BATTERY_REFRESH_MS 2000  // How often to poll the BQ27220 fuel gauge (ms)

// Nesso N1 Display & Battery objects
NessoDisplay display;
NessoBattery battery;

static uint8_t advInstance = 0;
unsigned long lastPeriodicBeacon = 0;
unsigned long lastBatteryUpdate = 0;
bool lastButtonState = HIGH;

// Draws the red/dark-gray TX indicator circle and current Minor value
void drawTxIndicator(bool transmitting, uint16_t minorValue) {
  // Indicator circle on the right side of the header
  if (transmitting) {
    display.fillCircle(214, 22, 12, TFT_RED);
    display.drawCircle(214, 22, 14, TFT_WHITE);
  } else {
    display.fillCircle(214, 22, 14, TFT_BLACK); // Clear outer ring
    display.fillCircle(214, 22, 12, TFT_DARKGREY);
  }

  // Update Minor value display
  display.setTextSize(2);
  display.setTextColor(transmitting ? TFT_RED : TFT_CYAN, TFT_BLACK);
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

  drawTxIndicator(false, MINOR_DEFAULT);
  updateBatteryDisplay();
}

// Builds and transmits a short legacy iBeacon burst while flashing the red indicator
void sendBeaconBurst(uint16_t minorValue) {
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

  // Fast packet interval (~20ms) during the burst
  oAdvertisementData.setMinInterval(32);
  oAdvertisementData.setMaxInterval(48);

  NimBLEBeacon::BeaconData beaconData = oBeacon.getData();
  oAdvertisementData.setManufacturerData(
      reinterpret_cast<const uint8_t *>(&beaconData),
      sizeof(beaconData)
  );

  pAdvertising->setInstanceData(advInstance, oAdvertisementData);
  pAdvertising->start(advInstance);

  // Flash red indicator on screen while radio is transmitting
  drawTxIndicator(true, minorValue);
  delay(ADV_BURST_MS);

  pAdvertising->stop(advInstance);
  drawTxIndicator(false, minorValue);
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
  NimBLEDevice::init("Nesso-N1-Beacon");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Transmit initial beacon
  lastPeriodicBeacon = millis();
  sendBeaconBurst(lastButtonState == LOW ? MINOR_PRESSED : MINOR_DEFAULT);
}

void loop() {
  unsigned long now = millis();
  bool currentButtonState = digitalRead(KEY1); // Active LOW when pressed

  // 1. Immediate trigger when KEY1 is pressed (Minor = 101)
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    sendBeaconBurst(MINOR_PRESSED);
    lastPeriodicBeacon = millis();
    delay(30); // Debounce
  }
  lastButtonState = currentButtonState;

  // 2. Periodic trigger every BEACON_INTERVAL ms (Minor = 100 unless KEY1 held)
  if (now - lastPeriodicBeacon >= BEACON_INTERVAL) {
    lastPeriodicBeacon = now;
    uint16_t minorToSend = (currentButtonState == LOW) ? MINOR_PRESSED : MINOR_DEFAULT;
    sendBeaconBurst(minorToSend);
  }

  // 3. Periodically refresh battery percentage and voltage on screen
  if (now - lastBatteryUpdate >= BATTERY_REFRESH_MS) {
    lastBatteryUpdate = now;
    updateBatteryDisplay();
  }

  delay(10);
}
