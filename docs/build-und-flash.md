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
Artefakt `heatcore-firmware` am Workflow-Lauf (`dist/heatcore-ota.bin`,
`dist/heatcore-factory.bin`, Prüfsummen).

Versionsnummer: Release-Builds übernehmen sie aus dem Git-Tag (`v0.5.0` →
`0.5.0`), Entwicklungs-Builds heißen `<VERSION>-dev+<commit>` (Datei
`VERSION`, `tools/version.py`). Die Version steht im Webinterface, auf dem
Display und in Home Assistant.

## Flashen direkt über GitHub (Web-Flasher, ohne Installation)

Voraussetzung: einmalig GitHub Pages aktivieren und ein Release erstellen
(siehe „Release veröffentlichen“).

1. Die Pages-Seite des Projekts öffnen:
   **https://zendonir.github.io/ESP32-HeatCore/** – mit Chrome oder Edge auf
   einem Desktop-Rechner (Web Serial; Firefox/Safari und Smartphones können
   das nicht).
2. Platine per USB-C-Datenkabel anschließen, „Per USB installieren“ klicken,
   Port „USB JTAG/serial debug unit“ wählen.
3. Erstinstallation: „Gerät löschen“ bestätigen. Bei einer bestehenden
   HeatCore-Installation nicht löschen – die Einstellungen bleiben erhalten.
4. Erscheint kein Port: S2 (Download) halten, S1 (Reset) kurz drücken, S2
   loslassen, erneut versuchen. Nach dem Flashen einmal S1 drücken.

Der Web-Flasher (ESP Web Tools) schreibt `heatcore-factory.bin` (Bootloader,
Partitionstabelle, Firmware) an Adresse 0x0 – dasselbe wie
`esptool.py --chip esp32s3 write_flash 0x0 heatcore-factory.bin`.

## Release veröffentlichen

Einmalig: GitHub → Settings → Pages → Build and deployment → Source:
**GitHub Actions**.

* Actions → **Release** → „Run workflow“, Version eingeben (z. B. `v0.5.0`;
  Vorabversion `v0.5.0-rc1`), oder
* einen Tag pushen: `git tag v0.5.0 && git push origin v0.5.0`.

Der Workflow führt die Host-Tests aus, baut die Firmware mit dieser Version,
prüft beide Images mit denselben Regeln wie das Gerät (`tools/make_release.py`:
ESP32-S3-Anwendungs-Image, HeatCore-Kennung, Version, Factory-Image-Aufbau)
und veröffentlicht ein GitHub-Release mit `heatcore-ota.bin`,
`heatcore-factory.bin`, `release.json`, `manifest.json` und `SHA256SUMS`.
Danach aktualisiert der Workflow **Pages** automatisch den Web-Flasher und die
Update-Quelle (`/firmware/release.json`, `/firmware/heatcore-ota.bin`).
Beide Workflows müssen dafür auf dem Standard-Branch (`main`) liegen.

## Erstes Flashen (PlatformIO / esptool)

1. Board stromlos machen, alle Lasten/Ventile abklemmen (siehe
   `docs/hardware.md`, Testaufbau).
2. USB-C anschließen. Falls der Port nicht erscheint: S2 (Boot) gedrückt
   halten, kurz S1 (Reset) drücken, S2 loslassen → Download-Modus.
3. `pio run -e kc868_a16v3 -t upload`
   oder das zusammengeführte Image an Adresse 0x0 schreiben:
   `esptool.py --chip esp32s3 write_flash 0x0 firmware.factory.bin`
4. `pio device monitor` – Bootmeldungen erscheinen über USB-CDC.

## Ersteinrichtung

1. **Mit LAN-Kabel:** DHCP, Webinterface unter `http://heatcore.local`
   (oder die IP aus dem Router/Display). **Ohne LAN:** nach ca. 20 s erscheint
   der Zugangspunkt `HeatCore-xxxx` (Passwort `heatcore123`, bitte ändern).
   Mit dem Handy verbinden – die Einrichtungsseite öffnet sich meist von selbst
   (sonst `http://192.168.4.1/`). Unter System → Netzwerk „Netze suchen“, das
   eigene WLAN wählen, Passwort eingeben, speichern: Die Seite zeigt die neue
   Adresse an, der Zugangspunkt geht danach aus. Ablauf siehe unten.
2. System → Allgemein: Gerätename, Zeitzone, **Web-Passwort setzen**.
3. Hardware → Selbsttest: I²C-Adressen, RTC, 1-Wire, RF, Display prüfen.
4. Räume & Heizkreise anlegen, Ausgänge als „Heizkreis“ belassen.
5. Sensoren übernehmen und Räumen zuordnen; Vorlauffühler mit Rolle „Vorlauf“.
6. System → Sicherheit prüfen (Vorlaufgrenze für die Anlage!).
7. Funkgeräte anlernen, Display einstellen, Regeln anlegen.

## Netzwerk: LAN hat Vorrang

| Situation | LAN | WLAN | Zugangspunkt |
|---|---|---|---|
| LAN verbunden (Link + IP) | aktiv | **aus** | aus |
| kein LAN, WLAN eingerichtet und verbunden | – | aktiv | aus |
| kein LAN, WLAN eingerichtet, aber nicht erreichbar (60 s) | – | versucht es weiter (alle 3 min) | **an** |
| kein LAN, kein WLAN eingerichtet | – | aus | **an** |
| LAN kommt zurück (15 s stabil) | aktiv | wird abgeschaltet | aus |

* Kurze LAN-Unterbrechungen (< 10 s) lösen keinen Wechsel aus; ein LAN-Kabel
  ohne DHCP-Antwort gilt als „kein LAN“.
* Der Zugangspunkt bleibt nach einer erfolgreichen Verbindung noch 1 min an
  (5 min, solange noch ein Gerät damit verbunden ist), damit die neue Adresse
  abgelesen werden kann. Solange er aktiv ist, beantwortet ein DNS-Server alle
  Namen mit 192.168.4.1 (Captive Portal).
* Neue WLAN-Zugangsdaten werden sofort ausprobiert.
* Die Regelung arbeitet in jedem Zustand unabhängig vom Netzwerk weiter.
* Logik: `lib/heatcore/src/hc/net/NetPolicy.*` (Host-Tests `net_*`),
  Umsetzung `src/platform/Net.cpp`.

## Updates im Webinterface (OTA)

System → **Firmware-Update**:

* **Online-Update von GitHub:** „Nach Updates suchen“ liest
  `release.json` von der Update-Quelle (Standard: GitHub-Pages-Seite des
  Projekts), zeigt Version und Änderungen und installiert auf Knopfdruck. Der
  **Browser** lädt die Datei und überträgt sie ins Gerät – die Steuerung
  selbst braucht keinen Internetzugang. Größe und SHA-256 des Releases werden
  im Gerät geprüft; eine beschädigte Übertragung wird nie aktiviert.
* **Datei hochladen:** `heatcore-ota.bin` aus einem Release (optional mit
  SHA-256) oder `.pio/build/kc868_a16v3/firmware.bin`.
* **Vorherige Firmware starten:** die alte Version liegt im zweiten
  OTA-Speicherbereich und kann jederzeit wieder gestartet werden.

Prüfungen im Gerät (schon während der Übertragung): ESP32-S3-Anwendungs-Image
(kein Factory-Image), HeatCore-Kennung dieser Platine (keine fremde Firmware,
keine Firmware für andere Platinen), Größe und Prüfsumme, Image-Prüfung des
Bootloaders. Bricht die Verbindung ab, wird das Teil-Image verworfen.

Ablauf und Sicherheit:

1. Während der Übertragung regelt die Heizung normal weiter.
2. Vor dem Neustart gehen alle Heizkreise in den sicheren Zustand.
3. Die neue Firmware startet **auf Probe** (Bootloader-Rollback aktiv). Sie
   wird erst bestätigt, wenn Regelungs- und Netzwerk-Task mindestens 60 s
   fehlerfrei laufen (frühestens 2 min nach dem Start). Stürzt sie ab, hängt
   sie oder ist sie nach 10 min nicht gesund, kehrt das Gerät automatisch zur
   vorherigen Version zurück; das Ergebnis steht unter „Letztes Ergebnis“.

Die Konfiguration bleibt bei Updates erhalten (LittleFS, atomare
Schreibvorgänge). Vor größeren Versionssprüngen trotzdem: System → Allgemein
→ Konfiguration exportieren.

## Host-Tests und Simulator (ohne Hardware)

```bash
cmake -S . -B build/host
cmake --build build/host -j
./build/host/heatcore_tests            # 86 Tests
./build/host/heatcore_sim --demo       # http://127.0.0.1:8080/
```

Der Simulator nutzt exakt den Plattform-unabhängigen Firmware-Kern
(`lib/heatcore`) mit simulierten Räumen, Sensoren und einer Funkstrecke und
liefert das echte Webinterface aus. Simulierte Funktasten:
`curl -X POST localhost:8080/sim/rf -d '{"code":"0xA5C3E1"}'`.
End-to-End-Test des Webinterfaces (Playwright):
`node tools/sim/ui_test.js` bei laufendem Simulator (`--port 8099`).
MCP-Interop-Test mit dem offiziellen MCP-Python-SDK (`pip install mcp`):
`python3 tools/sim/mcp_client_test.py` (Simulator mit `--speed 600` starten,
damit Statistikdaten entstehen).
