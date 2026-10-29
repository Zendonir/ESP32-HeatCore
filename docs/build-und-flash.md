# Build- und Flash-Anleitung

## Voraussetzungen

* Python 3.10+ und PlatformIO Core (`pip install platformio`) oder VS Code mit
  PlatformIO-Erweiterung
* USB-C-Kabel (native USB-Schnittstelle des ESP32-S3)
* Für Host-Tests: CMake ≥ 3.16, C++17-Compiler (gcc/clang)

## Firmware bauen

```bash
pio run -e kc868_a16v3
```

Plattform: pioarduino `55.03.38` (Arduino-ESP32 3.3.8 / ESP-IDF 5.5), Board
ESP32-S3 N16R8 (16 MB Flash, 8 MB Octal-PSRAM), Partitionstabelle
`partitions/heatcore_16MB.csv` (2 × 4 MB OTA, ~7,9 MB LittleFS). Das
Webinterface (`web/index.html`) wird beim Build automatisch komprimiert und
eingebettet (`tools/embed_web.py`).

Jeder Push baut die Firmware auf GitHub Actions; die Binärdateien liegen als
Artefakt `heatcore-firmware` am Workflow-Lauf.

## Erstes Flashen

1. Board stromlos machen, alle Lasten/Ventile abklemmen (siehe
   `docs/hardware.md`, Testaufbau).
2. USB-C anschließen. Falls der Port nicht erscheint: S2 (Boot) gedrückt
   halten, kurz S1 (Reset) drücken, S2 loslassen → Download-Modus.
3. `pio run -e kc868_a16v3 -t upload`
   oder das zusammengeführte Image an Adresse 0x0 schreiben:
   `esptool.py --chip esp32s3 write_flash 0x0 firmware.factory.bin`
4. `pio device monitor` – Bootmeldungen erscheinen über USB-CDC.

## Ersteinrichtung

1. Ohne Netzwerk startet nach 60 s der Access Point `HeatCore-xxxx`
   (Passwort `heatcore123`, bitte ändern), Webinterface unter
   `http://192.168.4.1/`. Mit Ethernet: DHCP, Name `heatcore.local`.
2. System → Allgemein: Gerätename, Zeitzone, **Web-Passwort setzen**.
3. Hardware → Selbsttest: I²C-Adressen, RTC, 1-Wire, RF, Display prüfen.
4. Räume & Heizkreise anlegen, Ausgänge als „Heizkreis“ belassen.
5. Sensoren übernehmen und Räumen zuordnen; Vorlauffühler mit Rolle „Vorlauf“.
6. System → Sicherheit prüfen (Vorlaufgrenze für die Anlage!).
7. Funkgeräte anlernen, Display einstellen, Regeln anlegen.

## Updates

System → Allgemein → Firmware-Update (`firmware.bin`). Vor dem Neustart gehen
alle Heizkreise in den sicheren Zustand. Konfiguration bleibt erhalten
(LittleFS, atomare Schreibvorgänge). Backup: Konfiguration exportieren.

## Host-Tests und Simulator (ohne Hardware)

```bash
cmake -S . -B build/host
cmake --build build/host -j
./build/host/heatcore_tests            # 63 Tests
./build/host/heatcore_sim --demo       # http://127.0.0.1:8080/
```

Der Simulator nutzt exakt den Plattform-unabhängigen Firmware-Kern
(`lib/heatcore`) mit simulierten Räumen, Sensoren und einer Funkstrecke und
liefert das echte Webinterface aus. Simulierte Funktasten:
`curl -X POST localhost:8080/sim/rf -d '{"code":"0xA5C3E1"}'`.
End-to-End-Test des Webinterfaces (Playwright):
`node tools/sim/ui_test.js` bei laufendem Simulator (`--port 8099`).
