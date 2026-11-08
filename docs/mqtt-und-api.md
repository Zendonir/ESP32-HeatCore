# MQTT / Home Assistant und REST-API

Alles funktioniert ohne MQTT und Home Assistant. MQTT ist optional
(System → MQTT).

## Themen (Basis `heatcore/<geräte-id>`, konfigurierbar)

| Thema | Richtung | Inhalt |
|---|---|---|
| `<basis>/status` | aus | `online`/`offline` (LWT) |
| `<basis>/room/<id>/state` | aus | JSON: temp, setpoint, ha_mode, preset, action, window, demand, reason, manual, duty (nur PWM-Regelung, %) |
| `<basis>/room/<id>/setpoint/set` | ein | Sollwert (auf Sicherheitsgrenzen begrenzt) |
| `<basis>/room/<id>/mode/set` / `preset/set` | ein | `auto`/`heat`/`off` bzw. `comfort`/`eco`/`frost`/`none` |
| `<basis>/mode/state`, `/mode/set` | aus/ein | `normal`/`eco`/`away`/`vacation` |
| `<basis>/rf/<id>/state`, `/avail`, `/event`, `/diag` | aus | Zustand, Funkverfügbarkeit, Tastenereignis, Empfangsqualität |
| `<basis>/rf/stats`, `<basis>/rf/tx/state` | aus | Empfangsstatistik, Senderstatus |
| `<basis>/rf/tx/<dev>/<cmd>/send` | ein | nur für Befehle mit Freigabe „Home Assistant“ |
| `<basis>/sensor/<id>/state`, `/avail` | aus | t, h, bat_low, status |
| `<basis>/display/state`, `/display/{enabled,contrast,page}/set` | aus/ein | Display |
| `<basis>/output/<n>/state`, `/set` | aus/ein | nur freigegebene allgemeine Ausgänge, zeitlich begrenzt |
| `<basis>/circuit/<n>/state` | aus | Ventil offen, Störung |
| `<basis>/alarm/state`, `<basis>/notify` | aus | Alarme, Benachrichtigungen |
| `<basis>/rule/<id>/run` | ein | Regeln mit manuellem Trigger |
| `<basis>/in/<thema>` / `<basis>/out/<thema>` | ein/aus | frei für Automationen |

MQTT-Discovery legt an: Klima-Entität je Raum, Fenster-Sensor je Raum,
Lernmodellstatus, Funkgeräte (binary_sensor + event + Qualität),
Temperatur-/Feuchte-/Batteriesensoren, Funkstatistik, Senderstatus,
Sende-Buttons, Display (Schalter, Kontrast, Seite, Störung), freigegebene
Ausgänge, Ventile und Störungen je Heizkreis, Sammelstörung, Regel-Buttons.
Entfernte Geräte werden in Home Assistant gelöscht. Nach einem HA-Neustart
(`homeassistant/status` = online) wird alles neu veröffentlicht.

**Home Assistant kann nicht:** Sicherheitsgrenzen ändern, Übertemperaturschutz
deaktivieren, Heizkreis-Ausgänge schalten, Ausgänge unbegrenzt einschalten oder
die lokale Regelung blockieren. Alle Befehle laufen durch denselben
Aktionsausführer mit Quelle „mqtt“.

### Raumwerte für Anzeigen (z. B. Button+)

Zusätzlich zu `<basis>/room/<id>/state` (JSON) werden – abschaltbar unter
System → MQTT („Raumwerte als einzelne Themen“) – einfache Werte veröffentlicht
(retained):

| Thema | Beispiel |
|---|---|
| `<basis>/room/<id>/temp` | `21.3` |
| `<basis>/room/<id>/setpoint` | `21.5` |
| `<basis>/room/<id>/heating` | `1` / `0` |
| `<basis>/room/<id>/window` | `1` / `0` |
| `<basis>/room/<id>/mode_text` | `Komfort`, `Absenkung`, `Frostschutz`, `Hand`, `Fenster offen`, `Aus` |
| `<basis>/room/<id>/humidity` | `48.0` (wenn der Raumfühler Feuchte liefert) |
| `<basis>/room/<id>/text` | `21,3° / 21,5° heizt` |

## Sensor-Abgleich (Sensoren → „Abgleich“)

Jeder Sensor hat eine Korrektur (Offset, ±10 K), die auf jeden Messwert
addiert wird. Der Abgleich setzt sie für mehrere Sensoren auf einmal:
Sensoren mindestens 30 min dicht nebeneinander an einen zugluftfreien Ort
legen, auswählen, Bezugswert wählen – **Median** (empfohlen, unempfindlich
gegen einen Ausreißer), **Mittelwert**, ein **Bezugssensor** (behält seine
Korrektur) oder ein **Referenzthermometer** (Wert eingeben, dann genügt auch
ein einzelner Sensor). Alle Sensoren brauchen einen aktuellen Messwert; würde
eine Korrektur ±10 K überschreiten, wird nichts geändert („liegt der Sensor
wirklich am gleichen Ort?“). „Korrekturen zurücksetzen“ setzt sie auf 0.

`POST /api/sensors/calibrate` mit
`{"ids":["ow1:…","ow1:…"],"reference":"median"|"mean"|"value"|"reset"|"<sensor-id>","value":21.3}`
liefert `target` und je Sensor `raw`, `oldOffset`, `newOffset`.

## Externe Quellen (Übersicht → Externe Quellen)

Messwerte und Tasten anderer Geräte – ohne Firmwareänderung konfigurierbar:

* **MQTT-Abo** auf ein beliebiges Thema (ohne Platzhalter) oder **HTTP-Abfrage**
  einer lokalen `http://`-Adresse (Intervall 10–3600 s, eigener Task, Timeout 3 s).
* Inhalt: reine Zahl (`21.3`, `21,3 °C`) oder JSON mit Feldpfad
  (`value`, `params.bthomesensor:200.value`, `a.0.b`).
* Arten: **Temperatur** → Sensor `ext:<name>` (unter Sensoren → neu gefunden
  einem Raum zuordnen; Timeout, Korrektur, Ersatzfühler wie bei jedem Sensor),
  **Luftfeuchte** / **Batterie %** → ergänzen eine Temperaturquelle,
  **Taste/Auslöser** → Regel-Auslöser „MQTT“ mit Thema `<name>`.
* Unplausible Werte (Temperatur außerhalb −50…120 °C) werden verworfen und
  angezeigt. Fällt eine Quelle aus, wird der Sensor nach seinem Timeout als
  gestört behandelt; die Regelung nutzt den Ersatzfühler bzw. die
  Fühlerausfall-Strategie.

Beispiele:

| Gerät | Quelle | Thema / URL | Pfad |
|---|---|---|---|
| Shelly BLU H&T über Shelly Gen2/Gen3 („Generic status update over MQTT“) | MQTT | `shellyplus1-xxxx/status/bthomesensor:200` | `value` |
| dito, Gen3 gesammelte Meldungen | MQTT | `shelly1g3-xxxx/events/rpc` | `params.bthomesensor:200.value` |
| Shelly ohne Broker | HTTP | `http://<ip>/rpc/BTHomeSensor.GetStatus?id=200` | `value` |
| Zigbee2MQTT-Fühler | MQTT | `zigbee2mqtt/<name>` | `temperature` |
| Button+ Taste | MQTT | Tastenthema aus der Button+-Konfiguration | leer |

Die Nummern der `bthomesensor`-Komponenten und die Button+-Themen stehen in der
jeweiligen Geräteoberfläche bzw. lassen sich mit MQTT Explorer ablesen.
REST: `GET/PUT /api/external`.

## REST-API (Auszug)

Alle Antworten JSON, Fehler als `{"error": "..."}` mit HTTP-Status.

| Methode und Pfad | Zweck |
|---|---|
| `GET /api/status`, `/api/alarms`, `/api/events?since=` | Übersicht, Alarme, Ereignisprotokoll |
| `GET /api/hardware`, `POST /api/hardware/selftest` | Hardwarestatus, Selbsttest |
| `GET /api/stats?days=`, `GET /api/stats/<raum>?hours=&res=` | Statistik: Kennzahlen je Tag, Verlauf (15-Minuten-Werte, verdichtet) |
| `GET/PUT /api/ai`, `POST /api/ai/changes/<id>/accept`, `/reject`, `/revert` | KI-Zugriff, Token, Journal; Vorschläge freigeben/ablehnen/zurücknehmen |
| `POST /mcp` (Bearer-Token) | MCP-Server für KI-Assistenten, siehe `docs/ki-und-mcp.md` |
| `GET /api/firmware`, `POST /api/firmware/rollback` | Firmware-Version, Speicherbereich, Probelauf, letztes Update-Ergebnis; Rückkehr zur vorherigen Firmware |
| `POST /api/ota?size=&sha256=&source=` (multipart) | Firmware-Update; nur HeatCore-Images dieser Platine, optional mit Größe/SHA-256 aus dem Release |
| `GET/PUT /api/io`, `POST/DELETE /api/outputs/<n>/manual`, `POST /api/outputs/<n>/request` | Ein-/Ausgänge |
| `GET/PUT /api/heating`, `POST /api/rooms/<id>/{setpoint,adjust,mode}`, `POST /api/mode` | Räume, Heizkreise |
| `GET /api/sensors`, `PUT/DELETE /api/sensors/<id>`, `POST /api/sensors/adopt` | Sensor-Manager |
| `GET/PUT /api/safety` | Sicherheit (nur lokal) |
| `GET /api/rf/status`, `PUT /api/rf/config` | Funkstatus und -einstellungen |
| `GET/POST /api/rf/devices`, `PUT/DELETE /api/rf/devices/<id>` | Empfangsgeräte |
| `POST /api/rf/learn/{start,stop}`, `GET /api/rf/learn` | Lernmodus |
| `GET /api/rf/monitor?since=`, `GET/DELETE /api/rf/unknown`, `GET/POST /api/rf/ignored`, `DELETE /api/rf/ignored/<code>` | Monitor, unbekannte/ignorierte Codes |
| `GET /api/rf/raw`, `POST /api/rf/raw/capture` | Pulsaufzeichnung |
| `GET/POST /api/rf/tx/devices`, `PUT/DELETE /api/rf/tx/devices/<id>`, `POST /api/rf/tx/send`, `GET /api/rf/tx/history`, `POST /api/rf/tx/loopback` | Sender |
| `GET /api/rf/sensors`, `POST /api/rf/rtl433` | Funksensoren, externer rtl_433-Eingang |
| `GET /api/display`, `PUT /api/display/config`, `GET /api/display/framebuffer?page=`, `POST /api/display/page` | Display |
| `GET/POST /api/rules`, `PUT/DELETE /api/rules/<id>`, `POST /api/rules/<id>/{enable,test,run}`, `POST /api/rules/validate` | Regeln |
| `GET /api/automation/catalog`, `GET /api/automation/log?since=` | Editor-Katalog, Ausführungsprotokoll |
| `GET /api/model`, `PUT /api/model/config`, `POST /api/model/<raum>/reset` | Lernmodell |
| `GET/PUT /api/config/{system,network,mqtt}`, `GET /api/config/export`, `POST /api/config/import` | Konfiguration (Passwörter werden nie ausgegeben) |
| `POST /api/system/{reboot,factory_reset,onewire_rescan}`, `POST /api/ota` | System |

WebSocket `/ws`: Live-Ereignisse (`{"t":"event",...}`).
