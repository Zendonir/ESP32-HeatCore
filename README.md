# ESP32-HeatCore

Lokal konfigurierbare, selbstständig regelnde und lernfähige Heizungssteuerung
für das **KinCony KC868-A16 V3** (ESP32-S3). 16 Ausgänge, 16 Digitaleingänge,
4 Analogeingänge, drei 1-Wire-Busse, RS485, **433-MHz-Funk (Sender und
Empfänger)** und **SSD1306-OLED** werden über eine einheitliche
Softwarearchitektur verwaltet. Alles funktioniert ohne Internet, Cloud, MQTT
oder Home Assistant; die gesamte Konfiguration erfolgt im eigenen
Webinterface.

## Funktionen

* **Heizung:** Räume mit Zeitplänen, Komfort/Absenkung/Frostschutz,
  Raumgruppen, globale Betriebsarten (Absenkbetrieb, Abwesend, Urlaub mit
  Ablauf), 16 Heizkreise mit Mindestlaufzeiten, NC/NO-Ventile.
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
* **Home Assistant (optional):** MQTT-Discovery für alle Komponenten,
  eingeschränkte Rechte.

## Status

* Firmware kompiliert für ESP32-S3 (CI), Host-Testsuite grün (86 Tests),
  Webinterface per End-to-End-Test im Simulator geprüft.
* **Auf der echten Platine noch nicht getestet.** Hardwareabhängige Punkte sind
  in `docs/tests-und-abnahme.md` als „unbestätigt (HW)“ markiert. Vor dem
  Anschluss echter Ventile den Testaufbau in `docs/hardware.md` durchführen.
  Die Ausgänge sind laut Schaltplan Low-Side-MOSFET-Schalter (nur DC-Lasten;
  230-V-/24-V-AC-Stellantriebe über Koppelrelais).

## Dokumentation

* [Hardware, Pinbelegung, elektrische Prüfung](docs/hardware.md)
* [433-MHz-Funk, Sicherheit, Funkrecht, Temperatursensoren](docs/rf433.md)
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
