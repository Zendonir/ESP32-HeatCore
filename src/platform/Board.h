#pragma once
// KinCony KC868-A16 V3 (ESP32-S3-WROOM-1U N16R8) pin map.
//
// Sources: KinCony forum "KC868-A16v3 ESP32-S3 IO pins define" (tid=7614),
// KinCony's ESPHome configuration for the A16v3 and the KinCony schematic
// of the KC868-A16 (ESP32-WROOM version, KC868-A16-schematic.pdf). The
// schematic confirms the peripheral circuits (expander address straps,
// buffers, output/input stages); its ESP32 GPIO numbers do NOT apply to the
// V3. Items marked UNCONFIRMED are not covered by a V3 schematic; the
// self-test (I2C scan, 1-Wire scan, RF loopback) shows what is present.
// See docs/hardware.md.
#include <cstdint>

namespace board {

// ---- I2C bus (shared by IO expanders, RTC, EEPROM and the OLED!)
constexpr int kI2cSda = 9;
constexpr int kI2cScl = 10;
constexpr uint32_t kI2cHz = 100000;  // conservative: long OLED cable on the same bus

// PCF8574 IO expanders. Address straps per A16 schematic:
//   U16 outputs 1..8  A2=1        -> 0x24     U32 outputs 9..16 A0=A2=1 -> 0x25
//   U14 inputs 1..8   A1=1        -> 0x22     U31 inputs 9..16  A0=1    -> 0x21
// Outputs: PCF pin LOW drives the TLP181 opto LED (anode at 3.3 V, 1 kOhm),
// which switches a low-side N-MOSFET (60 V/10 A). 10 kOhm pull-ups to 3.3 V
// keep released pins HIGH = off; the PCF8574 powers up HIGH -> all off.
// Inputs: EL357 optocouplers pull the PCF pin LOW when the input is active.
constexpr uint8_t kPcfOut1 = 0x24;  // outputs 1..8
constexpr uint8_t kPcfOut2 = 0x25;  // outputs 9..16
constexpr uint8_t kPcfIn1 = 0x22;   // inputs 1..8
constexpr uint8_t kPcfIn2 = 0x21;   // inputs 9..16
constexpr bool kOutputsActiveLow = true;
constexpr bool kInputsActiveLow = true;

constexpr uint8_t kRtcAddr = 0x68;     // DS3231
constexpr uint8_t kEepromAddr = 0x50;  // 24C02
constexpr uint8_t kOledAddr = 0x3C;    // SSD1306 (external module on the I2C header)

// ---- RF433 sockets. A16 schematic: receiver socket P2 is supplied with 5 V,
// its data line is buffered by a 74LVC1G125 at 3.3 V (5 V tolerant input),
// so 5 V receivers are safe. The transmitter socket P7 is supplied with 3.3 V
// (low transmit power, short range). Same circuit on the V3: UNCONFIRMED.
constexpr int kRf433Rx = 8;   // receiver data (buffered) -> ESP32
constexpr int kRf433Tx = 18;  // ESP32 -> transmitter data

// ---- 1-Wire buses (pull-ups on board)
constexpr int kOneWire[3] = {47, 48, 38};

// ---- Analog inputs A1..A4 (0..5 V at the terminal). A16 schematic: 0-5 V
// channels use a 5.1 kOhm / 10 kOhm divider (gain 1.51, 5 V -> 3.31 V).
// UNCONFIRMED: mapping A1=GPIO4, A2=GPIO6, A3=GPIO7, A4=GPIO5 from the forum.
// (default divider gain 1.51 is set in hc::IoManager, calibratable in the UI)
constexpr int kAnalog[4] = {4, 6, 7, 5};

// ---- RS485. A16 schematic: MAX13487E with automatic direction control
// (no DE pin needed), receive line buffered by a 74LVC1G125, 120 Ohm
// termination and TVS diodes on board. Same transceiver on the V3: UNCONFIRMED.
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
