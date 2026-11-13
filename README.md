# ESP32-HeatCore

Lokal konfigurierbare, selbstständig regelnde und lernfähige Heizungssteuerung
für das **KinCony KC868-A16 V3** (ESP32-S3). 16 Ausgänge, 16 Digitaleingänge,
4 Analogeingänge, drei 1-Wire-Busse, RS485, **433-MHz-Funk (Sender und
Empfänger)** und **SSD1306-OLED** werden über eine einheitliche
Softwarearchitektur verwaltet. Alles funktioniert ohne Internet, Cloud, MQTT
oder Home Assistant; die gesamte Konfiguration erfolgt im eigenen
Webinterface.

## Funktionen

* **Bedienung:** Webinterface nach Aufgaben gegliedert (Heizung, Einrichtung,
  Diagnose, System) mit einem Satz Erklärung je Seite, Checkliste „Erste
  Schritte“ für die Einrichtung und Raumkarten mit Zustand im Klartext.
  Fach- und Diagnoseseiten blendet die „Expertenansicht“ ein – keine
  Funktion fehlt, sie ist nur nicht im Weg.
* **Heizung:** Räume mit Zeitplänen, Komfort/Absenkung/Frostschutz,
  Raumgruppen, globale Betriebsarten (Absenkbetrieb, Abwesend, Urlaub mit
  Ablauf), 16 Heizkreise mit Mindestlaufzeiten, NC/NO-Ventile; je Raum
  Zweipunkt- oder PWM-Taktregelung (PI) für thermische Stellantriebe;
  Räume mit mehreren Heizkreisen teilen die Wärme über einen Anteil je Kreis
  auf (z. B. 100 % am Fenster, 60 % an der Innenwand).
* **Ventile von Hand:** jedes Ventil dauerhaft auf, zu oder mit festem
  Öffnungsanteil (z. B. 30 % je 20-min-Zyklus) – unabhängig von Fühler und
  Zeitplan, bleibt über Neustarts erhalten; Schutzfunktionen bleiben aktiv.
* **Sicherheit:** Vorlauf- und Raum-Übertemperatur, Sollwertgrenzen, begrenzter
  Handbetrieb, Schaltratenbegrenzung, Fühlerausfall-Strategie, Task- und
  Ausgangs-Watchdog, sicherer Zustand bei Reset/OTA. Nicht durch Automationen,
  Funk oder Home Assistant übersteuerbar.
* **433 MHz Empfang:** EV1527/PT2262 mit automatischer Pulslängenerkennung,
  Lernmodus im Webinterface, Wiederholungsfilter, Entprellung, Langdruck,
  Zustandskontakte mit Überwachungs-Timeout, Signalqualität, Statistik,
  unbekannte/ignorierte Codes, protokollunabhängige Pulsaufzeichnung.
* **433 MHz Senden:** Sendegeräte, Befehle, Gruppen, Testsenden,
  Wiederholungen, Timeout, Duty-Cycle-Budget, Loopback-Selbsttest.
* **Funk-Temperatursensoren:** Decoder-Plug-in-Schnittstelle (Nexus-TH,
  unbestätigt) und rtl_433-JSON über RS485 für externe Empfänger.
* **Bluetooth-Thermometer:** Thermometer in Reichweite finden und koppeln, z. B.:
  - Xiaomi/Qingping mit pvvx-Firmware oder Originalfirmware (mit Schlüssel);
  - Shelly BLU H&T (BTHome);
  - Govee und SwitchBot.

  Gekoppelte Thermometer arbeiten als normale Fühler und gehen mit Feuchte
  und Batteriestand an Home Assistant.
* **OLED:** 5 Seiten, Live-Vorschau im Browser, alle Einstellungen dynamisch,
  Navigation über Digitaleingänge, Funk, MQTT oder Web.
* **Automationen:** gleichberechtigte Signalquellen, Trigger/Bedingungen/
  Aktionen, Prioritäten, Validierung, Testmodus, Schleifenschutz,
  Ereignisprotokoll.
* **Lernmodell:** RLS-Raummodell mit Totzeit, Klassifizierung von
  Lüftungs-, Stör-, manuellen und unvollständigen Perioden, Prognosen mit
  Unsicherheit, begrenzte Beratung der Hysterese-Regelung mit Rückfallebene.
* **Statistik und KI-Optimierung:** 15-Minuten-Aufzeichnung je Raum über
  14 Tage mit Kennzahlen (Aufheizdauer, Überschwingen, Komfortabweichung,
  Heizstunden), MCP-Server für KI-Assistenten wie Claude – Vorschläge mit
  Freigabe im Webinterface, enge Grenzen, Journal mit Rücknahme.
* **Firmware:** Flashen direkt im Browser über GitHub (Web-Flasher), Updates
  im Webinterface – online aus den GitHub-Releases oder per Datei – mit
  Prüfsumme, Probelauf und automatischer Rückkehr zur vorherigen Version.
* **Externe Quellen:** Temperaturen, Luftfeuchte, Batterie und Tasten anderer
  Geräte per MQTT oder HTTP (z. B. Shelly BLU über einen Shelly, Button+,
  Zigbee2MQTT); Raumwerte als einfache MQTT-Themen für Anzeigen.
* **Wärmeanforderung:** meldet dem Kessel bzw. einem Heizkreis per MQTT
  (z. B. Heizkreis 2 am RC35 über EMS-ESP) und/oder über einen Ausgang
  (Pumpe), wann Räume Wärme brauchen. Mit Nachlauf, Mindestzeiten und
  Rückgabe an die eigene Regelung.
* **EMS-ESP (Buderus/Junkers):** liest Kessel, Thermostat und Mischer über
  das EMS-ESP-Gateway, bietet Außentemperatur, Vorläufe und Warmwasser als
  Sensoren an und schaltet einen RC35-Heizkreis direkt – mit Rückmeldung
  des Thermostats und Rückgabe der ursprünglichen Einstellungen.
* **Home Assistant (optional):** MQTT-Discovery für alle Komponenten,
  eingeschränkte Rechte. Je Raum Heizleistung (%), Solltemperatur, Heizbedarf
  und Betrieb als eigene Entitäten, dazu Heizleistung gesamt und
  Gerätediagnose – für Verläufe und Statistiken im Dashboard.
* **Wärmeangebot:** Liefert die Heizung keine Wärme (Nachtabsenkung,
  Pumpe aus – erkannt über EMS-ESP oder einen Vorlauffühler), bleiben die
  Ventile zu und öffnen sofort, wenn wieder Wärme kommt.

## Status

* Firmware kompiliert für ESP32-S3 (CI), Host-Testsuite grün (91 Tests),
  Webinterface per End-to-End-Test im Simulator geprüft.
* **Auf der echten Platine noch nicht getestet.** Hardwareabhängige Punkte sind
  in `docs/tests-und-abnahme.md` als „unbestätigt (HW)“ markiert. Vor dem
  Anschluss echter Ventile den Testaufbau in `docs/hardware.md` durchführen.
  Die Ausgänge der V3.1 sind High-Side-Schalter (P-MOSFET, max. 4 A je Kanal,
  nur Gleichspannung): +12/24 V an die rote Klemme jedes Ausgangsblocks, Last
  zwischen Ausgang und GND; 230-V-/24-V-AC-Stellantriebe über Koppelrelais.

## Dokumentation

* [Hardware, Pinbelegung, elektrische Prüfung](docs/hardware.md)
* [433-MHz-Funk, Sicherheit, Funkrecht, Temperatursensoren](docs/rf433.md)
* [Bluetooth-Thermometer](docs/bluetooth-thermometer.md)
* [Wärmeanforderung an Kessel/Heizkreis (z. B. RC35 über EMS-ESP)](docs/waermeanforderung.md)
* [EMS-ESP: Kessel, Thermostat und Mischer lesen](docs/emsesp.md)
* [OLED-Display](docs/display.md)
* [Automationen, Sicherheit, Lernmodell](docs/automation-und-lernmodell.md)
* [MQTT / Home Assistant und REST-API](docs/mqtt-und-api.md)
* [Statistik und KI-Optimierung (MCP)](docs/ki-und-mcp.md)
* [Build-, Flash- und Simulator-Anleitung](docs/build-und-flash.md)
* [Tests und Abnahmekriterien](docs/tests-und-abnahme.md)

## Projektstruktur

```
lib/heatcore/src/hc/   plattformunabhängiger Kern (auch auf dem PC testbar)
  core/  rf/  sensors/  heating/  io/  automation/  model/  display/
  system/  mqtt/  api/  stats/  ai/  mcp/
src/                   ESP32-S3: main.cpp (Tasks), platform/ (Treiber)
web/index.html         Webinterface (wird eingebettet)
test/host/             Unit- und Integrationstests
tools/                 Web-Einbettung, Host-Simulator, UI-Test
```
