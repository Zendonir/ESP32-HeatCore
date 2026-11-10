# EMS-ESP (Buderus, Junkers, Nefit)

[EMS-ESP](https://emsesp.org) (Gateway von BBQKees) verbindet den EMS-Bus
der Heizung mit MQTT. HeatCore liest darüber Kessel, Thermostat und Mischer
und kann einen Heizkreis für die [Wärmeanforderung](waermeanforderung.md)
schalten. Zu finden unter **Einrichtung → EMS-ESP (Kessel)**.

## Voraussetzungen

- EMS-ESP und HeatCore am selben MQTT-Broker (HeatCore: System → MQTT).
- EMS-ESP: MQTT-Format **verschachtelt** (Standard). Sprache beliebig –
  HeatCore versteht deutsche und englische Texte, Schaltwerte als
  „an/aus“, „on/off“, true/false oder 1/0 und Auswahlwerte auch als Zahl.
- Steuern: Thermostat mit den Betriebsarten Tag/Nacht/Automatik (RC30,
  RC35). Lesen geht mit jedem Thermostat.

## Einstellungen

| Feld | Bedeutung |
|---|---|
| Aktiv | Anbindung ein/aus |
| Basisthema | wie in EMS-ESP unter MQTT (Standard `ems-esp`) |
| Heizkreis | 1–4: den HeatCore schalten darf und dessen Werte angezeigt werden |
| Als Sensoren | Werte als Temperatursensoren anbieten |

## Was HeatCore liest

| Thema | Werte |
|---|---|
| `<basis>/boiler_data` | Außentemperatur, Vorlauf (Ist/Soll), Rücklauf, Brenner, Leistung, Heizbetrieb, Warmwasser, Anlagendruck, Status- und Fehlercode |
| `<basis>/thermostat_data` | für den gewählten Heizkreis: Betriebsart, Absenkart, Raumsoll, berechneter Vorlauf, Sommer/Winter, Heizsystem; gedämpfte Außentemperatur, Fehlercode |
| `<basis>/mixer_data` | für den gewählten Heizkreis: Vorlauf, Mischerstellung, Pumpe |
| `<basis>/status` | online/offline |

Die Seite zeigt alle Werte mit ihrem Alter. Daten älter als 90 s gelten als
veraltet und werden nicht für Entscheidungen benutzt.

## Sensoren

Mit „Als Sensoren“ erscheinen unter Temperatursensoren → „Neu gefunden“:

| Sensor | Wert |
|---|---|
| `ems:outdoor` | Außentemperatur am Kessel |
| `ems:boilerflow` | Vorlauf am Kessel |
| `ems:hcflow` | Vorlauf des Heizkreises (Mischer) |
| `ems:dhw` | Warmwasser |

Der Knopf **„Werte als Sensoren übernehmen“** übernimmt alle vier auf einmal.
Die Außentemperatur bekommt dabei die Rolle „Außen“ und fließt in Statistik
und Raummodell ein. Die Vorläufe bekommen bewusst **nicht** die Rolle
„Vorlauf“: diese Rolle steuert die Sicherheitsabschaltung, und die soll
nicht ohne eigene Entscheidung an einem Netzwerkwert hängen.

## Heizkreis schalten

Siehe [Wärmeanforderung](waermeanforderung.md#buderusjunkers-mit-rc35-und-ems-esp-bbqkees).
HeatCore sendet Befehle an die Themen von EMS-ESP:

| Thema | Nachricht |
|---|---|
| `<basis>/thermostat/hc<n>/mode` | `day`, `night`, `auto` (bzw. die gemerkte Betriebsart) |
| `<basis>/thermostat/hc<n>/reducemode` | `nofrost` beim Übernehmen, die gemerkte Absenkart bei der Rückgabe |

Die Anbindung lässt sich nicht abschalten, solange die Wärmeanforderung sie
benutzt. Wechselt das Basisthema oder der Heizkreis, bekommt der alte
Heizkreis zuerst seine Einstellungen zurück.

## REST-API

| Methode und Pfad | Zweck |
|---|---|
| `GET /api/emsesp` | Einstellungen und alle gelesenen Werte |
| `PUT /api/emsesp` | `{"enabled":true,"base":"ems-esp","hc":2,"sensors":true}` |
| `POST /api/emsesp/sensors` | Werte als Sensoren übernehmen |
