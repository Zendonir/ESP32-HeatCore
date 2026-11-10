# Wärmeanforderung an Kessel und Heizkreis

Die Funktion meldet dem Wärmeerzeuger, wann HeatCore-Räume Wärme brauchen.
Zu finden ist sie unter **Einrichtung → Wärmeanforderung (Kessel)**. Damit
läuft der Heizkreis nicht, wenn alle Ventile zu sind, und taktet nicht
unnötig.

## Ablauf

- **Wärme an:** Ein Raum braucht Wärme. Bei Hysterese-Regelung ist das der
  Heizbedarf. Bei PWM-Regelung zählt jeder Anteil über 0 %, auch in der
  Pause des Takts. HeatCore sendet dann „Wärme an“ und schaltet den
  optionalen Ausgang ein, etwa die Pumpe des Fußbodenverteilers.
- **Wärme aus:** Ist kein Bedarf mehr da, sendet HeatCore nach dem Nachlauf
  „Wärme aus“. Voreinstellung: 10 min Nachlauf.
- **Mindestlaufzeit und Mindestpause** verhindern kurzes Takten.
- **Wiederholen** (eigene MQTT-Nachricht): Der aktuelle Zustand wird
  regelmäßig erneut gesendet, voreingestellt alle 5 min. Das deckt
  Änderungen am Bedienteil und Neustarts des Gateways ab. Mit EMS-ESP
  ersetzt die Rückmeldung des Thermostats das Wiederholen.
- **Startverzögerung:** Nach einem Start wartet die Funktion 1 min, bis die
  Fühler Werte liefern.
- **Raumauswahl:** Es zählen nur die angehakten Räume. Ist keiner angehakt,
  zählen alle.
- **Rückgabe:** Wird die Funktion abgeschaltet oder HeatCore kontrolliert neu
  gestartet (Update, Neustart über das Webinterface), gibt HeatCore den
  Heizkreis zurück. Er regelt dann wieder selbst.
- **Unerwarteter Ausfall:** Fällt HeatCore unerwartet aus, bleibt der zuletzt
  gesendete Zustand stehen. Für diesen Fall empfiehlt sich eine
  Home-Assistant-Automation: „HeatCore nicht erreichbar → Heizkreis 2 auf
  Automatik“. HeatCore meldet seine Erreichbarkeit per MQTT, die
  Wärmeanforderung erscheint als Binärsensor „Wärmeanforderung“.

## Ziel

- **Heizkreis am Thermostat über EMS-ESP (direkt):** HeatCore schaltet
  einen Heizkreis eines RC30/RC35 über die
  [EMS-ESP-Anbindung](emsesp.md). Empfohlen für Buderus/Junkers mit
  EMS-ESP (BBQKees), siehe unten.
- **Eigene MQTT-Nachricht:** frei wählbares Thema und Nachrichten für
  „Wärme an“, „Wärme aus“ und „Rückgabe“, z. B. für andere Gateways.

## Buderus/Junkers mit RC35 und EMS-ESP (BBQKees)

Beispiel: Heizkreis 1 versorgt die Heizkörper und bleibt beim RC35.
Heizkreis 2 ist die Fußbodenheizung (Mischer MM10), deren Räume HeatCore
regelt.

1. EMS-ESP und HeatCore mit demselben MQTT-Broker verbinden
   (HeatCore: System → MQTT).
2. **Einrichtung → EMS-ESP:** einschalten, Basisthema (Standard `ems-esp`)
   und **Heizkreis 2** wählen. Die Seite zeigt danach Kessel, Thermostat
   und Mischer – so ist sofort zu sehen, ob die Daten ankommen.
3. **Einrichtung → Wärmeanforderung:** Ziel „Heizkreis am Thermostat über
   EMS-ESP“, die Räume der Fußbodenheizung anhaken, speichern.

Am RC35 muss nichts umgestellt werden. HeatCore macht beim Übernehmen
des Heizkreises:

- Es merkt sich **Betriebsart** (z. B. Automatik) und **Absenkart**
  (z. B. Außenhaltebetrieb) des Heizkreises. Die Werte werden gespeichert und
  überstehen einen Neustart.
- Es stellt die Absenkart auf **Abschaltbetrieb** (EMS-ESP `nofrost`, in Home
  Assistant „Frostschutz“). Erst damit schaltet „Nacht“ Pumpe und Mischer
  wirklich ab – mit „Außenhaltebetrieb“ würde der Heizkreis unterhalb der
  eingestellten Außentemperatur abgesenkt weiterheizen. Der Frostschutz des
  RC35 bleibt aktiv.
- Danach: Wärme gebraucht → Betriebsart **Tag**, kein Bedarf (nach dem
  Nachlauf) → **Nacht**.

Das Thermostat bestätigt jeden Befehl über die Daten von EMS-ESP (alle
10 s). Solange sie aktuell sind, sendet HeatCore nur, wenn das Thermostat
etwas anderes meldet – etwa weil jemand am RC35 umgestellt hat – und dann
höchstens einmal pro Minute. Jeder Befehl wird im RC35 gespeichert, deshalb
gibt es kein blindes Wiederholen. Ohne Daten vom Thermostat übernimmt
HeatCore den Heizkreis nicht.

**Rückgabe:** Beim Abschalten der Funktion, beim Wechsel des Ziels oder des
Heizkreises und bei einem kontrollierten Neustart bekommt der Heizkreis
Betriebsart und Absenkart zurück, die er vorher hatte.

Die Vorlauftemperatur von Heizkreis 2 bestimmt weiterhin die Heizkurve des
RC35. Die einzelnen Räume regelt HeatCore über die Ventile.

Mit **„Wärme an“ senden** / **„Wärme aus“ senden** lässt sich das sofort
prüfen; der Zustand zeigt, was das Thermostat zurückmeldet
(„bestätigt“).

## Ausgang

Der Ausgang ist optional. In Frage kommt nur ein Ausgang mit der Funktion
„Allgemein“, nie ein Heizkreis. Er folgt der Wärmeanforderung und lässt sich
mit oder ohne MQTT nutzen, etwa für eine Pumpe oder einen potentialfreien
Kesselkontakt über ein Koppelrelais.

## REST-API

| Methode und Pfad | Zweck |
|---|---|
| `GET /api/heatdemand` | Einstellungen und Zustand |
| `PUT /api/heatdemand` | Einstellungen speichern |
| `POST /api/heatdemand/test` `{"send":"on"\|"off"\|"fallback"}` | sofort senden |

Einstellungen: `enabled`, `target` (`emsesp` oder `mqtt`), `rooms`,
`runOnSec`, `minOnSec`, `minOffSec`, `repeatSec`, `output`, bei `mqtt`
zusätzlich `topic`, `payloadOn`, `payloadOff`, `payloadFallback`, `retain`.
