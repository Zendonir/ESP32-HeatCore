#pragma once
// KinCony KC868-A16 V3 (ESP32-S3-WROOM-1U N16R8) pin map.
//
// Sources: KinCony forum "KC868-A16v3 ESP32-S3 IO pins define" (tid=7614)
// and KinCony's ESPHome configuration for the A16v3. Items marked
// UNCONFIRMED could not be checked against a schematic of the concrete
// board revision; the self-test (I2C scan, 1-Wire scan, RF loopback) shows
// what is actually present. See docs/hardware.md.
#include <cstdint>

namespace board {

// ---- I2C bus (shared by IO expanders, RTC, EEPROM and the OLED!)
constexpr int kI2cSda = 9;
constexpr int kI2cScl = 10;
constexpr uint32_t kI2cHz = 100000;  // conservative: long OLED cable on the same bus

// PCF8574 IO expanders (outputs and inputs are active LOW on the A16 family:
// PCF8574 powers up with all pins HIGH = outputs off, inputs released)
constexpr uint8_t kPcfOut1 = 0x24;  // outputs 1..8
constexpr uint8_t kPcfOut2 = 0x25;  // outputs 9..16
constexpr uint8_t kPcfIn1 = 0x22;   // inputs 1..8   (UNCONFIRMED order 0x22/0x21)
constexpr uint8_t kPcfIn2 = 0x21;   // inputs 9..16
constexpr bool kOutputsActiveLow = true;
constexpr bool kInputsActiveLow = true;

constexpr uint8_t kRtcAddr = 0x68;     // DS3231
constexpr uint8_t kEepromAddr = 0x50;  // 24C02
constexpr uint8_t kOledAddr = 0x3C;    // SSD1306 (external module on the I2C header)

// ---- RF433 sockets
constexpr int kRf433Rx = 8;   // receiver data -> ESP32 (must be 3.3 V level!)
constexpr int kRf433Tx = 18;  // ESP32 -> transmitter data

// ---- 1-Wire buses (pull-ups on board)
constexpr int kOneWire[3] = {47, 48, 38};

// ---- Analog inputs A1..A4 (0..5 V at the terminal, divided on the board)
// UNCONFIRMED: mapping A1=GPIO4, A2=GPIO6, A3=GPIO7, A4=GPIO5 from the forum.
constexpr int kAnalog[4] = {4, 6, 7, 5};

// ---- RS485 (transceiver with automatic direction control assumed: no DE pin
// is listed for the A16v3 - UNCONFIRMED)
constexpr int kRs485Rx = 17;
constexpr int kRs485Tx = 16;

// ---- Ethernet W5500 (SPI)
constexpr int kEthSck = 42;
constexpr int kEthMosi = 43;
constexpr int kEthMiso = 44;
constexpr int kEthCs = 15;
constexpr int kEthInt = 2;
constexpr int kEthRst = 1;

// ---- SD card (SPI)
constexpr int kSdMosi = 12;
constexpr int kSdSck = 13;
constexpr int kSdMiso = 14;
constexpr int kSdCs = 11;
constexpr int kSdDetect = 21;

// ---- Buttons: S1 = EN/RESET, S2 = BOOT (GPIO0, strapping pin). Neither is
// usable as a normal user button: navigation uses digital inputs, RF
// remotes or the web interface instead.
constexpr int kBootButton = 0;

}  // namespace board
