# Bluetooth-Thermometer

HeatCore kann Bluetooth-LE-Thermometer in Reichweite empfangen und wie jeden
anderen Fühler verwenden: als Raumfühler, mit Korrektur und Abgleich, als
Ersatzfühler und in Automationen. Gekoppelte Thermometer gibt HeatCore
automatisch an Home Assistant weiter (MQTT-Discovery). Dort erscheinen sie
mit Temperatur, Luftfeuchte und Batteriestand.

## Einrichtung

1. **Einrichtung → Bluetooth-Thermometer** öffnen und die Bluetooth-Suche
   einschalten.
2. Innerhalb von etwa einer Minute erscheinen die Thermometer in Reichweite
   mit Messwert, Batterie und Empfangsstärke.
3. Für das eigene Thermometer einen Namen eingeben, einen Raum wählen und
   auf **Koppeln** klicken. Bei mehreren gleichen Thermometern hilft die
   MAC-Adresse auf dem Etikett oder in der App. Alternativ das Thermometer
   kurz in der Hand anwärmen.
4. Das Thermometer steht danach auch unter **Temperatursensoren** (Korrektur,
   Abgleich, Ersatzfühler, Priorität).

Zum Entkoppeln den Fühler unter „Temperatursensoren“ entfernen.

„Koppeln“ heißt hier nicht Bluetooth-Pairing. HeatCore hört passiv zu,
verbindet sich nicht und sendet nichts an das Thermometer. Die Batterie wird
dadurch nicht zusätzlich belastet, und Apps können das Thermometer weiter
nutzen. Thermometer der Nachbarn tauchen deshalb auch in der Liste auf.
Verwendet werden nur die gekoppelten.

## Unterstützte Thermometer

| Gerät / Firmware | Format | Schlüssel |
|---|---|---|
| Xiaomi LYWSD03MMC, MHO-C401, Qingping CGG1 … mit **pvvx**-Firmware | BTHome v2, „Custom“ (pvvx), ATC1441 | nein (BTHome optional verschlüsselt) |
| **Shelly BLU H&T** und andere BTHome-v2-Geräte | BTHome v2 | nur wenn in der Shelly-App verschlüsselt |
| Xiaomi LYWSD03MMC, LYWSDCGQ … mit **Originalfirmware** | MiBeacon v4/v5 | ja (Bindkey) |
| Govee H5072, H5075, H5074, H5051 | Herstellerdaten | nein |
| SwitchBot Meter, Meter Plus | SwitchBot | nein |

**Empfehlung:** Xiaomi-Thermometer mit der pvvx-Firmware im Format BTHome.
Die Firmware lässt sich ohne Werkzeug über den Browser aufspielen
(pvvx „TelinkMiFlasher“). Danach ist kein Schlüssel nötig, und die Batterie
hält lange.

**Verschlüsselte Geräte:** Die Liste zeigt „verschlüsselt – Schlüssel nötig“.
Dort den 32-stelligen Schlüssel eintragen:
- Xiaomi: den Bindkey liefert z. B. der „Xiaomi Cloud Tokens Extractor“.
- Shelly: den Schlüssel zeigt die Shelly-App.

Sobald das nächste Funkpaket ankommt, erscheinen die Werte. Ein falscher
Schlüssel wird als „Schlüssel falsch“ angezeigt. Die Weboberfläche und der
Konfigurationsexport geben gespeicherte Schlüssel nie aus.

Die Formate sind nach den veröffentlichten Spezifikationen und den
quelloffenen Decodern umgesetzt: BTHome, pvvx sowie Home Assistant
`xiaomi-ble`, `govee-ble` und `switchbot`. Geprüft ist das mit Testvektoren,
nicht mit jedem einzelnen Gerät. Die ältere Xiaomi-Verschlüsselung (v2/v3)
wird nicht unterstützt; sie wird als „nicht auswertbar“ angezeigt.

## Verhalten

- Messwerte gehen höchstens alle 30 s je Thermometer an die Regelung und an
  Home Assistant. Wiederholt gesendete Pakete zählen nicht.
- Ein gekoppeltes Thermometer gilt nach 15 min ohne Empfang als ausgefallen
  (einstellbar unter „Temperatursensoren → ⚙“). Dann greift wie bei jedem
  Fühler der Ersatzfühler bzw. das Sensor-Ausfallverhalten des Raums.
- Batterie unter 15 % meldet „Batterie schwach“.
- Home Assistant bekommt je Thermometer Temperatur, Luftfeuchte,
  Batteriestand (%) und „Batterie schwach“. Voraussetzung: MQTT ist
  eingerichtet (System → MQTT / Home Assistant).

## Funk, Reichweite und Speicher

- Bluetooth und WLAN teilen sich im ESP32-S3 Funkteil und Antenne. HeatCore
  hört nur etwa 30 % der Zeit zu. Thermometer senden alle paar Sekunden, das
  genügt. Mit LAN ist das unkritisch. Bei schwachem WLAN kann die
  Weboberfläche etwas langsamer reagieren. Die externe Antenne am
  IPEX-Anschluss verbessert beides.
- Reichweite: in der Wohnung typischerweise ein bis zwei Räume durch Wände.
  Der Schaltschrank aus Metall dämpft stark. Für Thermometer weiter weg eine
  externe Antenne nutzen.
- Der Bluetooth-Stack startet erst beim Einschalten der Suche und belegt dann
  etwa 40–60 KB internen Arbeitsspeicher. Ausschalten stoppt die Suche sofort,
  der Speicher wird aber erst beim nächsten Neustart frei.
- Sind beim Einschalten weniger als 55 KB frei, startet Bluetooth nicht, und
  die Seite zeigt den Grund. Seit 0.11.3 liegen die Stacks der Tasks, die
  nie in den Flash schreiben, im 8-MB-PSRAM. Das spart rund 46 KB internen
  RAM. Der Selbsttest zeigt unter „Task-Speicher“ für jede Task, wie viel
  Stack frei ist, wo er liegt und wie viel internes RAM frei ist. War der letzte Neustart durch Speichermangel
  ausgelöst, bleibt Bluetooth bis zum nächsten Neustart aus. Damit kann keine
  Neustartschleife entstehen.
- Der Selbsttest zeigt den Zustand unter „Bluetooth-Thermometer“.

## REST-API

| Methode und Pfad | Zweck |
|---|---|
| `GET /api/ble` | Zustand, Statistik, Thermometer in Reichweite |
| `PUT /api/ble` `{"enabled":true}` | Suche ein/aus |
| `PUT /api/ble/key` `{"mac":"A4:C1:38:…","key":"<32 hex>"}` | Schlüssel speichern (leer = löschen) |
| `POST /api/sensors/adopt` `{"id":"ble:A4C138…","name":"Bad","room":"bad"}` | Thermometer koppeln |

Die Einstellungen liegen im Konfigurationsabschnitt `sensors` unter `ble`.
