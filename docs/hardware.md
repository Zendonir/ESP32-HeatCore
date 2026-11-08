# Hardware: KinCony KC868-A16 V3 + RF433-Set + SSD1306

## Pinbelegung

Quellen: KinCony-Forum „KC868-A16v3 ESP32-S3 IO pins define“ (tid=7614), die
KinCony-ESPHome-Konfiguration der A16v3 und der KinCony-Schaltplan
`KC868-A16-schematic.pdf`. **Achtung:** Dieser Schaltplan gehört zur
ESP32-WROOM-Ausführung der A16 (LAN8720-Ethernet, 433 MHz an IO2/IO15,
I²C an IO4/IO5). Seine GPIO-Nummern gelten nicht für die V3; die
Peripherieschaltungen (Expander-Adressen, Puffer, Ein-/Ausgangsstufen) sind
aber sehr wahrscheinlich übernommen. Ein Schaltplan der V3 lag nicht vor.
Ergänzend: KinCony-Produktseite der A16v3 mit Blockschaltbild und
Platinenfotos der **KC868-A16-V3.1** (Versorgung 12/24 V DC, A1/A2 0–5 V und
A3/A4 4–20 mA, 3 × 1-Wire mit Pull-ups auf der Platine, SSD1306 und
RF433-Sockel als Ausstattung, freie GPIO39/40/41 ohne Pull-ups, gegenüber der
A16v1 „RF-Empfängerschaltung korrigiert“, Verpolschutz, W5500 statt LAN8720A).
Der Fließtext der Produktseite nennt „4CH analog input DC0-5V“ – das
Blockschaltbild und der A16-Schaltplan zeigen dagegen zwei 4–20-mA-Kanäle;
maßgeblich ist die Beschaltung (siehe unten).
Definiert in `src/platform/Board.h`.

| Funktion | ESP32-S3 | Status |
|---|---|---|
| I²C SDA / SCL | GPIO9 / GPIO10 | dokumentiert |
| PCF8574 Ausgänge 1–8 / 9–16 | 0x24 / 0x25 | dokumentiert + A16-Schaltplan (U16: A2, U32: A0+A2) |
| PCF8574 Eingänge 1–8 / 9–16 | 0x22 / 0x21 | A16-Schaltplan (U14: A1, U31: A0) und KinCony-ESPHome; **Blockschaltbild A16v3 zeigt 0x21 / 0x22** → im Webinterface umschaltbar („Bänke tauschen“) |
| DS3231 RTC / 24C02 EEPROM | 0x68 / 0x50 | dokumentiert |
| SSD1306 OLED | 0x3C (I²C-Steckleiste) | Blockschaltbild; Foto: Modul auf Abstandsbolzen über der Platine |
| RF433 Empfänger | GPIO8 | Blockschaltbild; A16-Schaltplan: Sockel P2 5 V, Daten über U17 (74LVC1G125) auf 3,3 V; V3.1-Foto: U17 + C8 vorhanden, zusätzlich R160 |
| RF433 Sender | GPIO18 | Blockschaltbild; A16-Schaltplan: Sockel P7 (GND, 3,3 V, Daten); V3.1-Foto: 4-poliges Modul mit EN-Pin im 3-poligen Sockel → Belegung **unbestätigt** |
| 1-Wire 1/2/3 | GPIO47 / GPIO48 / GPIO38 | dokumentiert, Pull-ups auf der Platine; eigener Treiber (die Arduino-Bibliothek „OneWire“ bedient auf dem ESP32-S3 nur GPIO0–33 und fand an diesen Pins nie Sensoren – behoben in 0.7.0) |
| Analog A1 / A2 (0–5 V) | GPIO4 / GPIO6 | Blockschaltbild; Teiler 5,1 kΩ/10 kΩ laut A16-Schaltplan (Faktor 1,51) |
| Analog A3 / A4 (4–20 mA) | GPIO7 / GPIO5 | Blockschaltbild; Shunt 150 Ω direkt am ADC-Pin laut A16-Schaltplan (20 mA → 3,0 V) |
| RS485 RX / TX | GPIO17 / GPIO16 | dokumentiert; A16-Schaltplan: MAX13487E mit automatischer Richtungsumschaltung |
| I²C-Steckleiste (OLED) | XH2.54-4P | P19: SDA, SCL, GND, 3,3 V (A16-Schaltplan und Bestückungsdruck der V3.1 identisch) |
| W5500 SCK/MOSI/MISO/CS/INT/RST | 42/43/44/15/2/1 | dokumentiert |
| SD-Karte MOSI/SCK/MISO/CS/CD | 12/13/14/11/21 | dokumentiert |
| Taster S1 / S2 | EN (Reset) / GPIO0 (Boot) | Produktseite; **keine Benutzertasten** |
| Freie GPIOs | GPIO39 / 40 / 41 | Blockschaltbild: ohne Pull-up auf der Platine; von der Firmware nicht belegt |
| Versorgung | 12/24 V DC | Produktseite, Verpolschutz (NTC) |

GPIO43/44 (UART0) sind mit dem W5500 belegt. Serielle Ausgaben laufen deshalb
über die native USB-Schnittstelle (`ARDUINO_USB_CDC_ON_BOOT=1`).

## Erfahrungen vom ersten Test auf der Platine

* Expander 0x24/0x25/0x22/0x21, SSD1306 (0x3C), DS3231 (Uhrzeit, Temperatur),
  SD-Karte, PSRAM, 433-MHz-Empfänger und -Sender (Loopback 6/6 Rahmen)
  funktionieren.
* Der I²C-Scan des Arduino-Cores 3.3 (leerer Schreibzugriff,
  `i2c_master_probe`) meldete auf der Platine Dutzende Geisteradressen und
  verfehlte DS3231/24C02. Die Firmware prüft Adressen deshalb per
  1-Byte-Lesezugriff (`I2cBus::probe`). Der Scan sperrt den Bus nur pro
  Adresse; vorher blockierte er ihn über eine Sekunde, was als Lese-/
  Schreibfehler der Expander zählte und den Ausgangsalarm auslösen konnte.
* WLAN mit −95 dBm: Der ESP32-S3-WROOM-1U braucht eine externe Antenne am
  IPEX-Anschluss; der Selbsttest weist unter −80 dBm darauf hin.
* Ein üblicher 433-MHz-Superregenerativempfänger liefert im Ruhezustand
  mehrere tausend Rauschflanken pro Sekunde – das ist normal und wird von der
  Dekodierung herausgefiltert.

## Elektrische Kompatibilität – vor dem ersten Einschalten prüfen

1. **433-MHz-Empfänger (GPIO8).** Der ESP32-S3 ist nicht 5-V-tolerant. Im
   A16-Schaltplan wird der Empfängersockel mit 5 V versorgt und die Datenleitung
   über einen 74LVC1G125 (5-V-toleranter Eingang, 3,3-V-Ausgang) gepuffert –
   typische 5-V-Set-Empfänger (XY-MK-5V, MX-RM-5V) sind dort also unkritisch.
   Laut Produktseite wurde bei der V3 ein „Fehler der RF-Empfängerschaltung“
   der A16v1 behoben. Das Platinenfoto der V3.1 zeigt am Empfängersockel P2
   weiterhin U17 im SOT-23-5-Gehäuse (im A16-Schaltplan der 74LVC1G125) mit
   C8 sowie einen zusätzlichen R160 – der Puffer ist also sehr wahrscheinlich
   vorhanden, aber ohne V3-Schaltplan nicht bewiesen (**unbestätigt (HW)**).
   Für die V3 einmal prüfen: Neben dem RF-Sockel sollte ein 5-poliger
   SOT-23-5-Baustein (74LVC1G125) sitzen, oder am Pin GPIO8 mit dem Multimeter
   messen (Empfänger rauscht → Pegel springt, Maximum ≤ 3,3 V). Wird ein Modul
   frei verdrahtet statt gesteckt, gilt der Puffer nicht.
2. **433-MHz-Sender (GPIO18).** Im A16-Schaltplan ist der Sendersockel P7
   dreipolig (GND, 3,3 V, Daten). Das KinCony-Foto der V3.1 zeigt darin ein
   vierpoliges Sendermodul (Beschriftung u. a. „+“, „DAT“, „EN“), das nur mit
   drei Pins im Sockel steckt. Die Sockelbelegung der V3.1 ist damit nicht
   eindeutig: das mitgelieferte KinCony-Modul so stecken wie geliefert; bei
   einem anderen Modul vorher GND/VCC/Daten am Sockel nachmessen (Daten =
   GPIO18, im Ruhezustand Low). Hat das Modul einen EN-Pin, muss dieser High
   sein, sonst wird nichts abgestrahlt – der Loopback-Selbsttest zeigt das.
   Im A16-Schaltplan wird der Sendersockel mit **3,3 V** versorgt. FS1000A-artige Sender funktionieren damit, senden aber
   mit deutlich geringerer Leistung als bei 5–12 V (Reichweite eher Raum als
   Haus). Mehr Leistung nur mit externer Versorgung des Moduls und unter
   Einhaltung der SRD-Grenze (10 mW ERP, 433,05–434,79 MHz).
   Antenne: λ/4 ≈ 17,3 cm Draht.
3. **Analogeingänge.** A1/A2 sind 0–5-V-Eingänge (Teiler 1,51), A3/A4
   4–20-mA-Eingänge: ein 150-Ω-Shunt liegt **ohne Teiler direkt am
   ESP32-Pin**. Dort niemals eine Spannungsquelle anschließen – 5 V würden den
   ESP32-S3 (max. 3,3 V) schädigen. Die Betriebsart ist pro Kanal im
   Webinterface einstellbar (Hardware → Analogeingänge) und muss zur
   Beschaltung passen; Voreinstellung wie im Blockschaltbild. Im
   4–20-mA-Betrieb meldet die Firmware Ströme unter 3,6 mA bzw. über 21 mA als
   Drahtbruch/Störung (NAMUR NE 43); als Sensor verwendet, gilt der Kanal dann
   als gestört und die Ersatzsensor-Logik greift.
4. **SSD1306 (I²C).** Die 4-polige I²C-Steckleiste (A16-Schaltplan P19:
   SDA, SCL, GND, 3,3 V) passt direkt zu üblichen 4-Pin-OLED-Modulen –
   **Pinreihenfolge vergleichen**, viele Module haben GND–VCC–SCL–SDA. Das Modul
   wird mit 3,3 V versorgt. Kabel kurz halten (< 30 cm); Bus läuft mit 100 kHz.
5. **Geteilter I²C-Bus.** Das Display hängt am selben Bus wie die
   Ausgangs-Expander. Jede Display-Übertragung ist auf 17 Byte begrenzt, nutzt
   ein Mutex mit Timeout und eine Bus-Recovery (9 SCL-Takte + STOP). Ein
   Display, das SDA dauerhaft auf Low zieht, würde jedoch auch die
   Ausgangsansteuerung stören: Dann greift der Ausgangs-Watchdog
   (Alarm `hw.outputs`, Heizkreise als gestört markiert). Bei Verdacht das
   Display abstecken.

## Klemmen der KC868-A16-V3.1 (Beschriftung laut KinCony-Anschlussbild)

| Klemmenblock | Belegung |
|---|---|
| Digital Inputs 1–8 / 9–16 | je 8 Eingänge + GND (potenzialfreie Kontakte gegen GND) |
| RS485 | A, B |
| Analog Inputs | GND, 1, 2, 3, 4 – im Anschlussbild als „DC 0–5V“ beschriftet; das Blockschaltbild nennt A3/A4 als 4–20 mA. Vor Gebrauch mit bekannter Spannung prüfen (Betriebsart je Kanal im Webinterface) |
| GPIO's 1 2 3 + GND + 3.3V | **1-Wire-Busse 1–3** (GPIO47/48/38, Pull-ups auf der Platine): DS18B20 an GND, Daten an 1/2/3, VDD an 3.3V |
| Digital Outputs 1–8 / 9–16 | je 8 Ausgänge + rot markierte Klemme „+12/24V DC Input“ für die Lastversorgung des Blocks |
| POWER | 12/24 V DC (– / +) |

## Ausgangs- und Eingangsbeschaltung

* **Ausgänge (KC868-A16-V3.1): High-Side-Schalter.** Auf der Platine sitzen
  P-Kanal-MOSFETs **NCE60P10K** (Platinenfoto); KinCony gibt **max. 4 A je
  Kanal** an. Jeder Ausgangsblock (1–8 und 9–16) hat eine eigene, rot
  markierte Klemme **„+12/24V DC Input“**: Dort die Versorgung der Lasten
  anschließen (beide Blöcke!). Ein eingeschalteter Ausgang führt diese
  Spannung; die Last liegt **zwischen Ausgangsklemme und GND** (Minus des
  Netzteils, z. B. über eine Klemmleiste).

  ```
  +24 V ──► rote Klemme „+12/24V DC Input“ (Block 1–8 und Block 9–16)
  Ausgang n ──► Stellantrieb ──► GND (Minus Netzteil)
  ```

  Nur **Gleichspannung**. Stellantriebe für **230 V AC oder 24 V AC** nicht
  direkt anschließen – dafür Koppelrelais (Spule 24 V DC zwischen Ausgang und
  GND) oder Halbleiterrelais. Am einfachsten versorgt ein 24-V-DC-Netzteil die
  Platine (POWER) und beide Ausgangsblöcke; bei getrennten Netzteilen die
  Minuspole verbinden.
* **Netzteilgröße:** Thermische Stellantriebe brauchen warm nur wenig
  (Danfoss Thermot 088H3216 / TWA-A 24 V: 2 W ≈ 0,08 A), ziehen kalt aber
  deutlich mehr (Anlaufstrom bis 350 mA). Wie lange der Anlaufstrom fließt,
  nennt Danfoss nicht; HeatCore rechnet vorsichtig mit dem ganzen Hub von
  etwa 3 min (Einschaltphase 180 s). Mindestlaufzeit und -pause der
  Heizkreise für diese Antriebe auf 180 s stellen – kürzere Pulse öffnen das
  Ventil kaum. Faustformel ohne Lastbegrenzung:
  Anzahl × Einschaltstrom + 0,25 A Platine + 20 % Reserve – für 6 Ventile
  24 V / 2,5 A, für 16 Ventile 24 V / 5 A. Mit der **Netzteil-Lastgrenze**
  (System → Sicherheit → „Netzteil der Ventile“) staffelt HeatCore das
  Einschalten so, dass das eingetragene Netzteil nie überlastet wird; dann
  genügt auch ein kleineres.
  *Hinweis:* Der Schaltplan der älteren KC868-A16 (v1, ESP32-WROOM) zeigt
  Low-Side-N-MOSFETs (60 V/10 A) – das gilt **nicht** für die V3.1.
* Ansteuerung: PCF-Pin LOW → Optokoppler-LED an → Ausgang an. 10-kΩ-Pull-ups
  halten freigegebene Pins HIGH = aus. Das entspricht `kOutputsActiveLow`
  (A16-Schaltplan; auf der V3.1 mit einer Testlampe und den Status-LEDs der
  Ausgänge bestätigen).
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

## Stabilität und Neustartdiagnose

* Alle zyklischen Aufgaben (Regelung, Ein-/Ausgänge, Funk, Sensoren, Display,
  Netzwerk, Speicher, externe Quellen) werden vom Hardware-Task-Watchdog
  überwacht. Hängt eine Aufgabe länger als 30 s, startet der ESP32 neu; die
  Ausgänge gehen dabei in den sicheren Zustand.
* Der Grund des letzten Neustarts steht unter **Hardware → Selbsttest &
  Status → Letzter Neustart**, z. B. „Task-Watchdog (Aufgabe hing) nach
  3 h 12 min; zuletzt hängend: net (seit 31 s)“ oder „Unterspannung
  (Brownout) – Netzteil prüfen“. Die Angabe überlebt Neustarts, nicht aber
  einen Stromausfall.
* Selbstheilung: War die Steuerung online und hat danach 30 min lang weder
  LAN noch WLAN (und niemand ist am Einrichtungs-Zugangspunkt), startet sie
  einmal neu. Ist der interne Arbeitsspeicher eine Minute lang erschöpft,
  ebenfalls.
* Arbeitsspeicher: Speicherblöcke ab 512 Byte liegen im 8-MB-PSRAM, der
  knappe interne RAM bleibt WLAN, Netzwerk und Treibern vorbehalten. Unter
  „Freier RAM“ sollte das Minimum deutlich über 20 KB liegen. Ein sehr
  schwaches WLAN (unter −80 dBm, ohne externe Antenne am IPEX-Anschluss)
  lässt Sendepuffer anwachsen und ist die häufigste Ursache für „Gerät nicht
  mehr erreichbar“ – Antenne anschließen oder LAN verwenden.
* Das Display schaltet sich nach der eingestellten Zeit (Standard 5 min)
  ab; ein Tastendruck weckt es. „Display aus“ allein ist kein Fehler.

## Bedienelemente

S1 (Reset) und S2 (Boot, GPIO0 = Strapping-Pin) werden **nicht** als
Benutzertasten verwendet. Display-Navigation und Raumfunktionen erfolgen über:

* Digitaleingänge (Aktionen „Display: nächste Seite“, „nächster Raum“,
  „Sollwert des angezeigten Raums ±0,5“),
* Funkfernbedienungen (gleiche Aktionen),
* das Webinterface.

Für spätere lokale Bedientaster stehen laut Produktseite GPIO39/40/41 frei zur
Verfügung (`board::kFreeGpio`, ohne Pull-up auf der Platine – Taster bräuchten
den internen oder einen externen Pull-up); die Firmware verwendet sie derzeit
nicht.

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
