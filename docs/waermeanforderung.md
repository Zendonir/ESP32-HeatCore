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
- **Wiederholen:** Der aktuelle Zustand wird regelmäßig erneut gesendet,
  voreingestellt alle 5 min. Das deckt Änderungen am Bedienteil und
  Neustarts des Gateways ab.
- **Startverzögerung:** Nach einem Start wartet die Funktion 1 min, bis die
  Fühler Werte liefern.
- **Raumauswahl:** Es zählen nur die angehakten Räume. Ist keiner angehakt,
  zählen alle.
- **Rückgabe:** Wird die Funktion abgeschaltet oder HeatCore kontrolliert neu
  gestartet (Update, Neustart über das Webinterface), sendet HeatCore die
  Nachricht „Rückgabe“. Der Heizkreis regelt dann wieder selbst.
- **Unerwarteter Ausfall:** Fällt HeatCore unerwartet aus, bleibt der zuletzt
  gesendete Zustand stehen. Für diesen Fall empfiehlt sich eine
  Home-Assistant-Automation: „HeatCore nicht erreichbar → Heizkreis 2 auf
  Automatik“. HeatCore meldet seine Erreichbarkeit per MQTT, die
  Wärmeanforderung erscheint als Binärsensor „Wärmeanforderung“.

## Buderus/Junkers mit RC35 und EMS-ESP (BBQKees)

Beispiel: Heizkreis 1 versorgt die Heizkörper und bleibt beim RC35.
Heizkreis 2 ist die Fußbodenheizung, deren Räume HeatCore regelt.

1. EMS-ESP und HeatCore mit demselben MQTT-Broker verbinden
   (HeatCore: System → MQTT).
2. Vorlage **„EMS-ESP, Heizkreis 2 am RC35“** einsetzen:
   - Thema `ems-esp/thermostat`
   - „Wärme an“ `{"cmd":"mode","data":"day","hc":2}`
   - „Wärme aus“ `{"cmd":"mode","data":"night","hc":2}`
   - „Rückgabe“ `{"cmd":"mode","data":"auto","hc":2}`

   Ein anderes Basisthema in EMS-ESP muss im Thema angepasst werden.
3. Am RC35 für Heizkreis 2 die **Absenkart „Abschaltbetrieb“** wählen
   (EMS-ESP: `reducemode` = `nofrost`). „Nacht“ schaltet Pumpe und Mischer
   dann wirklich ab. Bei „reduziert“ läuft der Heizkreis abgesenkt weiter.
4. Mit **„An“ senden** und **„Aus“ senden** prüfen, ob der RC35 bzw. das
   EMS-ESP-Protokoll umschaltet.

Die Vorlauftemperatur von Heizkreis 2 bestimmt weiterhin die Heizkurve des
RC35. Die einzelnen Räume regelt HeatCore über die Ventile.

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
| `POST /api/heatdemand/test` `{"send":"on"\|"off"\|"fallback"}` | Nachricht sofort senden |
