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

## Wärmeangebot: Ventile zu, wenn die Heizung keine Wärme liefert

Liefert der Kessel bzw. Heizkreis gerade keine Wärme (Nachtabsenkung am RC35,
Sommerbetrieb, Pumpe aus), bringt ein offenes Ventil nichts: Die Stellantriebe
ziehen Strom, und das Lernmodell sieht „Ventil offen, Raum wird nicht warm“.
Mit der Prüfung des Wärmeangebots bleiben die Ventile der automatisch
geregelten Heizkreise dann zu.

| Prüfen über | Wärme vorhanden, wenn … |
|---|---|
| `emsesp` | der vom Thermostat **berechnete Vorlauf** des EMS-ESP-Heizkreises mindestens den Mindest-Vorlauf erreicht **und** die Heizkreispumpe läuft. Funktioniert auch, wenn alle Ventile zu sind. |
| `sensor` | ein Temperaturfühler (z. B. Kesselvorlauf, `ems:boilerflow`, ein Fühler aus Home Assistant) mindestens den Mindest-Vorlauf misst. Der Fühler muss die Temperatur auch ohne Durchfluss durch die Fußbodenkreise sehen – also nicht am Verteiler. |

* Fehlt die Wärme länger als die **Verzögerung** (Standard 5 min), schließen
  die Ventile. Kommt Wärme zurück, öffnen sie sofort wieder (Mindest-Pausenzeit
  des Heizkreises beachtet).
* Die Räume behalten ihren Wärmebedarf; der Grund zeigt
  „kein Waermeangebot, Ventile zu“. Die Wärmeanforderung an den Kessel läuft
  normal weiter – sie holt die Wärme ja gerade.
* Fordert HeatCore selbst Wärme an, bleiben die Ventile die ersten 15 min
  offen, damit der Kessel anlaufen und der Fühler warm werden kann.
* Liegen keine aktuellen Daten vor (EMS-ESP aus, Fühler ausgefallen),
  arbeiten die Ventile normal – kein Aussperren durch einen Fehler.
* Ventile von Hand (Heizkreis dauerhaft offen/zu/Prozent) sind nicht
  betroffen.
* Bei stromlos offenen Ventilen heißt „zu“: Der Stellantrieb wird bestromt.
* Home Assistant: Binärsensor „Waermeangebot“ (Grund als Attribut); die
  Heizleistung der Räume ist dann 0 %.

## REST-API

| Methode und Pfad | Zweck |
|---|---|
| `GET /api/heatdemand` | Einstellungen und Zustand |
| `PUT /api/heatdemand` | Einstellungen speichern |
| `POST /api/heatdemand/test` `{"send":"on"\|"off"\|"fallback"}` | sofort senden |

Einstellungen: `enabled`, `target` (`emsesp` oder `mqtt`), `rooms`,
`runOnSec`, `minOnSec`, `minOffSec`, `repeatSec`, `output`, bei `mqtt`
zusätzlich `topic`, `payloadOn`, `payloadOff`, `payloadFallback`, `retain`.
Wärmeangebot (unabhängig von `enabled`): `supply` (`off`, `emsesp`,
`sensor`), `supplySensor`, `supplyMinTemp` (15..70 °C, Standard 25),
`supplyDelaySec` (0..3600, Standard 300). Der Zustand steht unter
`status.supply` (`ok`, `value`, `text`, `lowSec`).
