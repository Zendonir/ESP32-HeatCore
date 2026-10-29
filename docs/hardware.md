# Hardware: KinCony KC868-A16 V3 + RF433-Set + SSD1306

## Pinbelegung

Quellen: KinCony-Forum „KC868-A16v3 ESP32-S3 IO pins define“ (tid=7614), die
KinCony-ESPHome-Konfiguration der A16v3 und der KinCony-Schaltplan
`KC868-A16-schematic.pdf`. **Achtung:** Dieser Schaltplan gehört zur
ESP32-WROOM-Ausführung der A16 (LAN8720-Ethernet, 433 MHz an IO2/IO15,
I²C an IO4/IO5). Seine GPIO-Nummern gelten nicht für die V3; die
Peripherieschaltungen (Expander-Adressen, Puffer, Ein-/Ausgangsstufen) sind
aber sehr wahrscheinlich übernommen. Ein Schaltplan der V3 lag nicht vor.
Ergänzend: KinCony-Produktseite der A16v3 (Versorgung 12/24 V DC, 4 Analog-
eingänge 0–5 V, 3 × 1-Wire mit Pull-ups auf der Platine, SSD1306 und
RF433-Sockel als Ausstattung, freie GPIO39/40/41, gegenüber der A16v1
„RF-Empfängerschaltung korrigiert“, Verpolschutz, W5500 statt LAN8720A).
Definiert in `src/platform/Board.h`.

| Funktion | ESP32-S3 | Status |
|---|---|---|
| I²C SDA / SCL | GPIO9 / GPIO10 | dokumentiert |
| PCF8574 Ausgänge 1–8 / 9–16 | 0x24 / 0x25 | dokumentiert + A16-Schaltplan (U16: A2, U32: A0+A2) |
| PCF8574 Eingänge 1–8 / 9–16 | 0x22 / 0x21 | Adress-Brücken im A16-Schaltplan (U14: A1, U31: A0) |
| DS3231 RTC / 24C02 EEPROM | 0x68 / 0x50 | dokumentiert |
| SSD1306 OLED | 0x3C (I²C-Steckleiste) | Modulabhängig (manche Module 0x3D) |
| RF433 Empfänger | GPIO8 | dokumentiert; A16-Schaltplan: Sockel 5 V, Daten über 74LVC1G125 auf 3,3 V gepuffert |
| RF433 Sender | GPIO18 | dokumentiert; A16-Schaltplan: Sockel mit 3,3 V versorgt |
| 1-Wire 1/2/3 | GPIO47 / GPIO48 / GPIO38 | dokumentiert, Pull-ups auf der Platine |
| Analog A1–A4 (0–5 V) | GPIO4 / 6 / 7 / 5 | Bereich 0–5 V laut Produktseite; Zuordnung **unbestätigt**; Teiler 5,1 kΩ/10 kΩ laut A16-Schaltplan (Faktor 1,51) |
| RS485 RX / TX | GPIO17 / GPIO16 | dokumentiert; A16-Schaltplan: MAX13487E mit automatischer Richtungsumschaltung |
| I²C-Steckleiste (OLED) | XH2.54-4P | A16-Schaltplan P19: 1 SDA, 2 SCL, 3 GND, 4 3,3 V |
| W5500 SCK/MOSI/MISO/CS/INT/RST | 42/43/44/15/2/1 | dokumentiert |
| SD-Karte MOSI/SCK/MISO/CS/CD | 12/13/14/11/21 | dokumentiert |
| Taster S1 / S2 | EN (Reset) / GPIO0 (Boot) | Produktseite; **keine Benutzertasten** |
| Freie GPIOs | GPIO39 / 40 / 41 | Produktseite; von der Firmware nicht belegt |
| Versorgung | 12/24 V DC | Produktseite, Verpolschutz (NTC) |

GPIO43/44 (UART0) sind mit dem W5500 belegt. Serielle Ausgaben laufen deshalb
über die native USB-Schnittstelle (`ARDUINO_USB_CDC_ON_BOOT=1`).

## Elektrische Kompatibilität – vor dem ersten Einschalten prüfen

1. **433-MHz-Empfänger (GPIO8).** Der ESP32-S3 ist nicht 5-V-tolerant. Im
   A16-Schaltplan wird der Empfängersockel mit 5 V versorgt und die Datenleitung
   über einen 74LVC1G125 (5-V-toleranter Eingang, 3,3-V-Ausgang) gepuffert –
   typische 5-V-Set-Empfänger (XY-MK-5V, MX-RM-5V) sind dort also unkritisch.
   Laut Produktseite wurde bei der V3 ein „Fehler der RF-Empfängerschaltung“
   der A16v1 behoben – die V3-Empfängerstufe weicht also vom vorliegenden
   Schaltplan ab, deshalb bleibt die Pegelanpassung **unbestätigt (HW)**.
   Für die V3 einmal prüfen: Neben dem RF-Sockel sollte ein 5-poliger
   SOT-23-5-Baustein (74LVC1G125) sitzen, oder am Pin GPIO8 mit dem Multimeter
   messen (Empfänger rauscht → Pegel springt, Maximum ≤ 3,3 V). Wird ein Modul
   frei verdrahtet statt gesteckt, gilt der Puffer nicht.
2. **433-MHz-Sender (GPIO18).** Im A16-Schaltplan wird der Sendersockel mit
   **3,3 V** versorgt. FS1000A-artige Sender funktionieren damit, senden aber
   mit deutlich geringerer Leistung als bei 5–12 V (Reichweite eher Raum als
   Haus). Mehr Leistung nur mit externer Versorgung des Moduls und unter
   Einhaltung der SRD-Grenze (10 mW ERP, 433,05–434,79 MHz).
   Antenne: λ/4 ≈ 17,3 cm Draht.
3. **SSD1306 (I²C).** Die 4-polige I²C-Steckleiste (A16-Schaltplan P19:
   SDA, SCL, GND, 3,3 V) passt direkt zu üblichen 4-Pin-OLED-Modulen –
   **Pinreihenfolge vergleichen**, viele Module haben GND–VCC–SCL–SDA. Das Modul
   wird mit 3,3 V versorgt. Kabel kurz halten (< 30 cm); Bus läuft mit 100 kHz.
4. **Geteilter I²C-Bus.** Das Display hängt am selben Bus wie die
   Ausgangs-Expander. Jede Display-Übertragung ist auf 17 Byte begrenzt, nutzt
   ein Mutex mit Timeout und eine Bus-Recovery (9 SCL-Takte + STOP). Ein
   Display, das SDA dauerhaft auf Low zieht, würde jedoch auch die
   Ausgangsansteuerung stören: Dann greift der Ausgangs-Watchdog
   (Alarm `hw.outputs`, Heizkreise als gestört markiert). Bei Verdacht das
   Display abstecken.

## Ausgangs- und Eingangsbeschaltung (A16-Schaltplan)

* **Ausgänge** sind **Low-Side-Schalter** (N-MOSFET 60 V/10 A hinter
  Optokoppler TLP181), also **nur für Gleichspannung**. Last zwischen
  +12/24 V DC und Klemme Yn, Minus der Lastversorgung an die gemeinsame
  Masse des Ausgangsteils. Stellantriebe für **230 V AC oder 24 V AC** dürfen
  nicht direkt angeschlossen werden – dafür Koppelrelais oder Halbleiterrelais
  verwenden (24-V-DC-Stellantriebe direkt möglich).
* Ansteuerung: PCF-Pin LOW → Optokoppler-LED an → Ausgang an. 10-kΩ-Pull-ups
  halten freigegebene Pins HIGH = aus. Das entspricht `kOutputsActiveLow`.
* **Eingänge**: Optokoppler EL357 mit 2-kΩ-Vorwiderstand; aktiv, wenn der
  Eingang gegen die Masse der Eingangsseite geschaltet wird (potenzialfreie
  Kontakte, Reed-Kontakte).

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

Für spätere lokale Bedientaster stehen laut Produktseite GPIO39/40/41 frei zur
Verfügung (`board::kFreeGpio`); die Firmware verwendet sie derzeit nicht.

## Testaufbau vor dem Anschluss echter Ventile

1. Ausgänge mit Gleichspannungs-Testlasten (LED mit Vorwiderstand,
   12-V-Lämpchen, Relais ohne Last) statt Stellantrieben betreiben.
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
