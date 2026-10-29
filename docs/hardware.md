# Hardware: KinCony KC868-A16 V3 + RF433-Set + SSD1306

## Pinbelegung

Quellen: KinCony-Forum „KC868-A16v3 ESP32-S3 IO pins define“ (tid=7614) und die
KinCony-ESPHome-Konfiguration der A16v3. Definiert in `src/platform/Board.h`.

| Funktion | ESP32-S3 | Status |
|---|---|---|
| I²C SDA / SCL | GPIO9 / GPIO10 | dokumentiert |
| PCF8574 Ausgänge 1–8 / 9–16 | 0x24 / 0x25 | dokumentiert |
| PCF8574 Eingänge 1–8 / 9–16 | 0x22 / 0x21 | Reihenfolge **unbestätigt** (Selbsttest zeigt Adressen) |
| DS3231 RTC / 24C02 EEPROM | 0x68 / 0x50 | dokumentiert |
| SSD1306 OLED | 0x3C (I²C-Steckleiste) | Modulabhängig (manche Module 0x3D) |
| RF433 Empfänger | GPIO8 | dokumentiert |
| RF433 Sender | GPIO18 | dokumentiert |
| 1-Wire 1/2/3 | GPIO47 / GPIO48 / GPIO38 | dokumentiert, Pull-ups auf der Platine |
| Analog A1–A4 (0–5 V) | GPIO4 / 6 / 7 / 5 | Zuordnung **unbestätigt** |
| RS485 RX / TX | GPIO17 / GPIO16 | dokumentiert; automatische Richtungsumschaltung **unbestätigt** |
| W5500 SCK/MOSI/MISO/CS/INT/RST | 42/43/44/15/2/1 | dokumentiert |
| SD-Karte MOSI/SCK/MISO/CS/CD | 12/13/14/11/21 | dokumentiert |
| Taster S1 / S2 | EN (Reset) / GPIO0 (Boot) | **keine Benutzertasten** |

GPIO43/44 (UART0) sind mit dem W5500 belegt. Serielle Ausgaben laufen deshalb
über die native USB-Schnittstelle (`ARDUINO_USB_CDC_ON_BOOT=1`).

## Elektrische Kompatibilität – vor dem ersten Einschalten prüfen

1. **433-MHz-Empfänger (GPIO8).** Der ESP32-S3 ist **nicht 5-V-tolerant**.
   Typische Set-Empfänger (XY-MK-5V, MX-RM-5V) werden mit 5 V versorgt und
   liefern 5-V-Pegel. Prüfen, mit welcher Spannung der RF-Sockel der A16 V3
   versorgt wird und ob die Platine den Pegel anpasst (mit dem Multimeter am
   Datenpin messen, Empfänger empfängt Rauschen → Pegel springt):
   * Datenpegel ≤ 3,3 V: direkt verwendbar.
   * Datenpegel 5 V: Spannungsteiler (z. B. 10 kΩ / 20 kΩ) oder ein
     3,3-V-tauglicher Superhet-Empfänger (z. B. RXB6, 3,3–5,5 V).
   Die passenden KinCony-Module sind für den Sockel vorgesehen.
2. **433-MHz-Sender (GPIO18).** FS1000A-artige Sender akzeptieren 3,3-V-Logik.
   Höhere Versorgungsspannung erhöht die Sendeleistung – die
   SRD-Grenze (10 mW ERP im Band 433,05–434,79 MHz) einhalten.
   Antenne: λ/4 ≈ 17,3 cm Draht.
3. **SSD1306 (I²C).** 4-poliges Modul (VCC, GND, SCL, SDA). Die meisten Module
   vertragen 3,3–5 V; VCC an 3,3 V anschließen, damit die Pull-ups auf dem
   Modul nicht auf 5 V ziehen. Kabel kurz halten (< 30 cm). Bus läuft mit
   100 kHz.
4. **Geteilter I²C-Bus.** Das Display hängt am selben Bus wie die
   Ausgangs-Expander. Jede Display-Übertragung ist auf 17 Byte begrenzt, nutzt
   ein Mutex mit Timeout und eine Bus-Recovery (9 SCL-Takte + STOP). Ein
   Display, das SDA dauerhaft auf Low zieht, würde jedoch auch die
   Ausgangsansteuerung stören: Dann greift der Ausgangs-Watchdog
   (Alarm `hw.outputs`, Heizkreise als gestört markiert). Bei Verdacht das
   Display abstecken.

## Verhalten bei Reset, OTA und Stromausfall

* Der PCF8574 behält seinen Zustand bei einem **ESP-Reset** bei. Die Firmware
  schreibt deshalb als allererste Aktion in `setup()` alle Ausgänge AUS und nach
  dem Laden der Konfiguration den „sicheren Zustand“ (Ventile zu).
* Vor einem **OTA-Neustart** werden alle Heizkreise in den sicheren Zustand
  gebracht (`g_shutdown`), erst dann erfolgt `ESP.restart()`.
* Nach **Stromausfall** startet der PCF8574 mit allen Pins HIGH = Ausgänge aus.
* Stromlos offene Ventile (NO) sind konfigurierbar; bei ihnen bedeutet „sicher“
  = Ausgang EIN (Ventil zu). Bei Stromausfall öffnen NO-Ventile physikalisch –
  das kann keine Firmware verhindern.

## Bedienelemente

S1 (Reset) und S2 (Boot, GPIO0 = Strapping-Pin) werden **nicht** als
Benutzertasten verwendet. Display-Navigation und Raumfunktionen erfolgen über:

* Digitaleingänge (Aktionen „Display: nächste Seite“, „nächster Raum“,
  „Sollwert des angezeigten Raums ±0,5“),
* Funkfernbedienungen (gleiche Aktionen),
* das Webinterface.

## Testaufbau vor dem Anschluss echter Ventile

1. Ausgänge mit LEDs/Glühlampen (oder Relais ohne Last) statt Stellantrieben
   betreiben.
2. Raumfühler (DS18B20) zunächst offen auf dem Tisch; mit Hand/Eiswasser
   Temperaturänderungen erzeugen.
3. Vorlauffühler mit Rolle „Vorlauf“ anlegen und durch Erwärmen den
   Vorlaufschutz auslösen → alle Heizkreise müssen schließen.
4. Fensterkontakt (Draht oder Funk) betätigen → betroffener Raum geht auf
   Frostschutz, Heizkreis schließt nach der Mindestlaufzeit.
5. Netzwerkkabel ziehen / MQTT-Broker stoppen → Regelung läuft weiter.
6. Display abziehen → Regelung läuft weiter, Status „Display Störung“.
7. OTA-Update einspielen → Ausgänge vor dem Neustart aus, danach neue
   Entscheidung der Regelung.
