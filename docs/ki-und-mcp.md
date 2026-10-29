# Statistik und KI-Optimierung (MCP)

HeatCore zeichnet den Betrieb jedes Raums auf und stellt diese Daten über das
**Model Context Protocol (MCP)** einem KI-Assistenten wie Claude zur Verfügung.
Die KI kann daraus Optimierungen ableiten und – je nach Freigabe – als
Vorschlag einreichen oder in engen Grenzen direkt anwenden. Die Regelung selbst
bleibt vollständig lokal; die Steuerung braucht dafür keinen Internetzugang.

## Aufzeichnung (Statistik)

* Je Raum **15-Minuten-Werte der letzten 14 Tage** (1344 Werte, 16 Byte je
  Wert): mittlere Ist-Temperatur, mittlerer wirksamer Sollwert,
  Außentemperatur, Ventil-Einschaltdauer (%), Fensterminuten,
  Ventilschaltungen und Merker (Handsollwert, Handbetrieb, Störung,
  Ersatzfühler, Komfortzeit, Modellberatung aktiv).
* Zusätzlich die Reihe `_system`: Vorlauf- und Außentemperatur, Anteil
  geöffneter Heizkreise.
* Gesichert im Flash (`/hist.bin`, stündlich und vor jedem Neustart/Update),
  wird beim Start wiederhergestellt; Werkseinstellungen löschen sie.
* Kennzahlen je Tag und Zeitraum (Fenster, Handbetrieb und Störungen sind aus
  den Komfortkennzahlen ausgeklammert):

| Kennzahl | Bedeutung |
|---|---|
| `comfortMeanDevK` / `comfortMeanAbsDevK` | mittlere (Betrags-)Abweichung Ist − Soll in Komfortzeiten |
| `belowSetpointHours` | Stunden mehr als 0,5 K unter Soll |
| `aboveSetpointHours` | Stunden mehr als 0,5 K über Soll bei geschlossenem Ventil |
| `maxOvershootK` | höchstes Überschwingen bis 2 h nach Heizende |
| `warmupMeanMin` / `warmupMaxMin` | Zeit von einem Sollwertsprung (≥ 1 K) bis 0,3 K unter dem neuen Soll |
| `heatingHours`, `switches` | Ventil-Offenzeit, Ventil-Einschaltungen |
| `windowHours`, `faultHours`, `manualHours` | ausgeklammerte Zeiten |

Webinterface: **Übersicht → Statistik** (Verlauf mit Ist, Soll, Ventil und
Fensterzeiten, Kennzahlen je Tag, Tabellenansicht).
REST: `GET /api/stats?days=7`, `GET /api/stats/<raum>?hours=48&res=15`.

## MCP-Schnittstelle

* Endpunkt `POST http://<steuerung>/mcp` (MCP „Streamable HTTP“, JSON-Antworten,
  Protokollrevisionen 2024-11-05 bis 2025-11-25 über den
  `initialize`-Handshake). Neuere Clients, die zuerst `server/discover` probieren,
  fallen wie vorgesehen auf den Handshake zurück – geprüft mit dem offiziellen
  MCP-Python-SDK 2.3.0 (`tools/sim/mcp_client_test.py`).
* Anmeldung: `Authorization: Bearer <token>`. Das Token wird im Webinterface
  erzeugt und danach nur noch verdeckt angezeigt. Anfragen von fremden
  Web-Seiten (abweichender `Origin`) werden abgewiesen.

### Zugriffsstufen (System → KI / MCP)

| Stufe | Wirkung |
|---|---|
| aus | Endpunkt antwortet nicht (Voreinstellung) |
| nur lesen | Statistik, Einstellungen, Lernmodell, Ereignisse |
| Vorschläge | zusätzlich `change_settings`: Vorschläge erscheinen im Webinterface und werden erst nach **Freigabe durch einen Menschen** angewendet (empfohlen) |
| direkt | Änderungen werden sofort angewendet; Tageslimit (Standard 10) |

### Werkzeuge

| Werkzeug | Inhalt |
|---|---|
| `get_overview` | aktueller Zustand aller Räume, Außen-/Vorlauftemperatur, Alarme |
| `get_settings` | einstellbare Parameter, Zeitpläne, Heizkreise, Sicherheitsgrenzen (nur lesbar), erlaubte Bereiche |
| `get_statistics` | Kennzahlen je Raum und Tag (1–14 Tage) |
| `get_room_history` | Zeitreihe eines Raums, automatisch auf ≤ ca. 600 Punkte verdichtet |
| `get_model` | Lernmodell: Heizleistung, Verluste, Totzeit, Unsicherheit, Periodenklassen |
| `get_events` | letzte Ereignisse |
| `get_change_log` | KI-Journal |
| `change_settings` | Änderungen (mit `dry_run`), nur bei „Vorschläge“/„direkt“ |
| `revert_change` | angewendete KI-Änderung zurücknehmen |

Prompt `optimize_heating` (Argumente `room`, `focus`): leitet die KI durch die
Auswertung und zu kleinen, begründeten Schritten.

### Was die KI ändern darf – und was nicht

| Bereich | Grenzen |
|---|---|
| Komforttemperatur | 17–24 °C und innerhalb der Sicherheits-Sollwertgrenzen, max. ±1,5 K je Schritt |
| Absenktemperatur | 14–21 °C, max. ±2 K je Schritt |
| Frostschutz | 5–12 °C (Reihenfolge Frost ≤ Absenkung ≤ Komfort) |
| Hysterese | 0,1–1,5 K |
| Komfortzeiten | je Wochentag max. 6 Zeiträume, keine Überschneidung, Tageskomfortzeit max. ±3 h je Schritt |
| Ventil-Mindestlauf-/pausenzeit | 60–1800 s, nur Heizkreise mit Raumzuordnung |
| Lernmodell | ein/aus, max. Beratung 0–0,5 K, Modellberatung je Raum |

**Nicht erreichbar:** Sicherheitsgrenzen (Vorlauf, Übertemperatur,
Sollwertgrenzen, Watchdog), Ausgänge und Handbetrieb, Eingänge, Regeln, Funk,
Netzwerk, MQTT, Passwörter, Firmware. Jede Anfrage ist „alles oder nichts“ und
läuft zusätzlich durch dieselbe Konfigurationsprüfung wie das Webinterface.
Jede Änderung steht mit Begründung, vorherigen Werten und Quelle im Journal
(Ereignisquelle `ai`) und lässt sich zurücknehmen – sofern die Werte seitdem
nicht von Hand geändert wurden.

## Einrichtung

1. System → **KI / MCP**: Zugriff wählen (empfohlen „Vorschläge“), Token
   erzeugen, kopieren, speichern.
2. **Claude Code**:
   ```bash
   claude mcp add --transport http heatcore http://heatcore.local/mcp \
     --header "Authorization: Bearer <TOKEN>"
   ```
3. **Claude Desktop** (lokales Netz, Brücke `mcp-remote`, Node.js nötig) in
   `claude_desktop_config.json`:
   ```json
   {"mcpServers": {"heatcore": {"command": "npx",
     "args": ["-y", "mcp-remote", "http://heatcore.local/mcp", "--allow-http",
              "--header", "Authorization: Bearer <TOKEN>"]}}}
   ```
4. Fragen, z. B. „Werte die Heizungsstatistik der letzten Woche aus und schlage
   Optimierungen vor“, oder den Prompt `optimize_heating` verwenden.
5. Vorschläge unter System → KI / MCP prüfen, freigeben oder ablehnen; nach
   3–7 Tagen erneut auswerten lassen.

Hinweise: Die Verbindung im lokalen Netz ist unverschlüsseltes HTTP – das Token
schützt vor unbeabsichtigtem Zugriff, nicht vor Mitlesen im Netz. Nicht ins
Internet freigeben. Für eine sinnvolle Auswertung sollten einige Tage
Aufzeichnung vorliegen.
