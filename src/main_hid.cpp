/*
  This script demonstrates a BLE keyboard/mouse. 

  Seth McNeill
  2026 November 12
*/


#include <Arduino.h>
#include <Arduino_Nesso_N1.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <math.h>

NessoDisplay display;

// BLE HID objects
NimBLEServer* pServer = nullptr;
NimBLEHIDDevice* pHid = nullptr;
NimBLECharacteristic* pInputKeyboard = nullptr;
NimBLECharacteristic* pInputMouse = nullptr;

volatile bool deviceConnected = false;
bool lastConnectedState = false;
bool lastKey1State = HIGH;
bool lastKey2State = HIGH;

// Composite HID Report Descriptor:
// Report ID 1 = Standard 8-byte Keyboard
// Report ID 2 = Standard 4-byte Relative Mouse
static const uint8_t hidReportDescriptor[] = {
    // --- KEYBOARD (Report ID 1) ---
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0 - Left Control)
    0x29, 0xE7,        //   Usage Maximum (0xE7 - Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs) - Modifier byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Const,Var,Abs) - Reserved byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x65,        //   Usage Maximum (0x65)
    0x81, 0x00,        //   Input (Data,Array,Abs) - 6 Keycodes
    0xC0,              // End Collection

    // --- MOUSE (Report ID 2) ---
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (0x01)
    0x29, 0x03,        //     Usage Maximum (0x03)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs) - 3 Buttons
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x03,        //     Input (Const,Var,Abs) - 5-bit Padding
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel) - X, Y, Wheel
    0xC0,              //   End Collection
    0xC0               // End Collection
};

// NimBLE 2.5.x Server Callbacks
class HidServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        deviceConnected = true;
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        NimBLEDevice::startAdvertising(0); // Instance 0 for ESP32-C6 Extended Advertising
    }
};

// ASCII-to-HID keycode helper
bool asciiToHid(char c, uint8_t& modifier, uint8_t& keycode) {
    modifier = 0;
    keycode = 0;

    if (c >= 'a' && c <= 'z') {
        keycode = 0x04 + (c - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        modifier = 0x02; // Left Shift
        keycode = 0x04 + (c - 'A');
        return true;
    }
    if (c >= '1' && c <= '9') {
        keycode = 0x1E + (c - '1');
        return true;
    }
    if (c == '0') {
        keycode = 0x27;
        return true;
    }
    switch (c) {
        case ' ':  keycode = 0x2C; return true;
        case '\n': keycode = 0x28; return true; // Enter
        case '!':  modifier = 0x02; keycode = 0x1E; return true;
        case '.':  keycode = 0x37; return true;
        case ',':  keycode = 0x36; return true;
        case '-':  keycode = 0x2D; return true;
        default:   return false;
    }
}

void sendKeystroke(uint8_t modifier, uint8_t keycode) {
    uint8_t report[8] = { modifier, 0x00, keycode, 0, 0, 0, 0, 0 };
    pInputKeyboard->setValue(report, sizeof(report));
    pInputKeyboard->notify();
    delay(12);

    uint8_t release[8] = { 0 };
    pInputKeyboard->setValue(release, sizeof(release));
    pInputKeyboard->notify();
    delay(12);
}

void typePhrase(const char* text) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (!deviceConnected) break;
        uint8_t mod = 0, code = 0;
        if (asciiToHid(text[i], mod, code)) {
            sendKeystroke(mod, code);
        }
    }
}

void sendMouseMove(int8_t dx, int8_t dy) {
    uint8_t report[4] = { 0x00, (uint8_t)dx, (uint8_t)dy, 0x00 };
    pInputMouse->setValue(report, sizeof(report));
    pInputMouse->notify();
}

void moveMouseInCircle(float radius = 60.0f, int steps = 72) {
    int prevX = (int)roundf(radius);
    int prevY = 0;

    for (int i = 1; i <= steps; i++) {
        if (!deviceConnected) break;

        float angle = (2.0f * PI * i) / steps;
        int targetX = (int)roundf(cosf(angle) * radius);
        int targetY = (int)roundf(sinf(angle) * radius);

        int8_t dx = (int8_t)(targetX - prevX);
        int8_t dy = (int8_t)(targetY - prevY);

        sendMouseMove(dx, dy);

        prevX = targetX;
        prevY = targetY;
        delay(12);
    }
}

void updateDisplay(bool connected, const char* actionMsg = "Ready") {
    display.fillScreen(BLACK);

    // Header bar
    display.fillRect(0, 0, 240, 28, NAVY);
    display.setTextSize(2);
    display.setTextColor(WHITE, NAVY);
    display.setCursor(14, 6);
    display.print("BLE Keyboard/Mouse");

    // Pairing / Connection Status
    if (connected) {
        display.setTextSize(2);
        display.setTextColor(GREEN, BLACK);
        display.setCursor(36, 44);
        display.print("Status: PAIRED");
    } else {
        display.setTextSize(2);
        display.setTextColor(YELLOW, BLACK);
        display.setCursor(18, 40);
        display.print("Status: PAIRING...");

        display.setTextSize(1);
        display.setTextColor(LIGHTGREY, BLACK);
        display.setCursor(28, 64);
        display.print("Connect to 'Nesso N1 HID' on PC");
    }

    // Divider & Button Legend
    display.drawFastHLine(10, 78, 220, DARKGREY);
    display.setTextSize(1);
    display.setTextColor(CYAN, BLACK);
    display.setCursor(12, 88);
    display.print("KEY1: Type Phrase | KEY2: Circle Mouse");

    // Live Action Status
    display.setTextSize(2);
    display.setTextColor(WHITE, BLACK);
    display.setCursor(24, 110);
    display.print(actionMsg);
}

void setupBleHid() {
    NimBLEDevice::init("Nesso N1 HID");

    // Enable bonding & Just-Works encryption (required by Windows HID class driver)
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new HidServerCallbacks());

    pHid = new NimBLEHIDDevice(pServer);
    pInputKeyboard = pHid->getInputReport(1); // Report ID 1 = Keyboard
    pInputMouse    = pHid->getInputReport(2); // Report ID 2 = Mouse

    pHid->setManufacturer("Arduino");
    pHid->setPnp(0x02, 0x2341, 0x0076, 0x0100);
    pHid->setHidInfo(0x00, 0x01);
    pHid->setReportMap((uint8_t*)hidReportDescriptor, sizeof(hidReportDescriptor));
    pHid->setBatteryLevel(100);

    pServer->start();

    // 1. Primary Advertisement (max 31 bytes): MUST contain Flags + HID UUID + Appearance
    NimBLEExtAdvertisement extAdv;
    extAdv.setLegacyAdvertising(true);
    extAdv.setScannable(true);
    extAdv.setConnectable(true);
    extAdv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP); // 0x06: Required by Windows!
    extAdv.setAppearance(0x03C1); // 0x03C1 = HID Keyboard (Windows recognizes 0x03C1 faster than 0x03C0)
    extAdv.setCompleteServices16({ NimBLEUUID((uint16_t)0x1812) }); // HID Service UUID

    // 2. Scan Response (max 31 bytes): Holds the full device name so primary packet isn't truncated
    NimBLEExtAdvertisement scanResp;
    scanResp.setLegacyAdvertising(true);
    scanResp.setName("Nesso N1 HID");

    NimBLEExtAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setInstanceData(0, extAdv);
    pAdvertising->setScanResponseData(0, scanResp);
    pAdvertising->start(0);
}

void setup() {
    Serial.begin(115200);

    display.begin();
    display.setRotation(1);

    pinMode(KEY1, INPUT_PULLUP);
    pinMode(KEY2, INPUT_PULLUP);

    setupBleHid();
    updateDisplay(false, "Waiting for host...");
}

void loop() {
    if (deviceConnected != lastConnectedState) {
        lastConnectedState = deviceConnected;
        updateDisplay(deviceConnected, deviceConnected ? "Ready" : "Disconnected");
    }

    bool currentKey1 = digitalRead(KEY1);
    bool currentKey2 = digitalRead(KEY2);

    // KEY1 pressed -> Type phrase
    if (lastKey1State == HIGH && currentKey1 == LOW) {
        if (deviceConnected) {
            updateDisplay(true, "Typing phrase...");
            typePhrase("Hello from Nesso N1!\n");
            delay(250);
            updateDisplay(true, "Ready");
        } else {
            updateDisplay(false, "Pair PC first!");
        }
    }

    // KEY2 pressed -> Move mouse in a circle
    if (lastKey2State == HIGH && currentKey2 == LOW) {
        if (deviceConnected) {
            updateDisplay(true, "Moving mouse...");
            moveMouseInCircle(60.0f, 72);
            updateDisplay(true, "Ready");
        } else {
            updateDisplay(false, "Pair PC first!");
        }
    }

    lastKey1State = currentKey1;
    lastKey2State = currentKey2;

    delay(15);
}
