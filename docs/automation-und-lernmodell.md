# Automationen, Sicherheit und Lernmodell

## Einheitliche Ereignisse

Alle Quellen erzeugen dieselbe `Event`-Struktur (`hc/core/Event.h`):
Digitaleingänge (`di:3`) und Funkgeräte (`rf:5/1`, `rf:5`) liefern die gleichen
Gesten; Sensoren, Räume, Ventile, Zeit, MQTT, Lernmodell, Alarme und Webbefehle
liefern weitere Ereignistypen. Eingangszuordnungen (Aktionen pro Geste an
Digitaleingängen und Funktasten) und Automationsregeln verwenden **denselben
Aktionsausführer** (`HeatCoreSystem::execute`) mit denselben Prüfungen.

## Regeln

Eine Regel hat Name, Aktiv-Schalter, Testmodus, Priorität 0–100, Sperrzeit,
max. Ausführungen/h, beliebig viele Trigger (einer genügt), Bedingungen (alle
müssen gelten, einzeln negierbar) und Aktionen. Ausführungszähler, letzte
Ausführung und letztes Ergebnis werden angezeigt und gespeichert.

* **Trigger:** Eingang (Draht/Funk), Temperaturgrenzwert (mit Hysterese, nur
  bei Überschreitung), Temperaturänderung, Raumzustand, Ventilzustand, Zeit,
  MQTT (`<basis>/in/<thema>`), Sensorfehler, Lernmodell-Ereignis, Betriebsart,
  Alarm, manuell (Web/HA).
* **Bedingungen:** Temperatur, Sollwert, Zeitbereich (auch über Mitternacht),
  Wochentag, Betriebsart, Raummodus, Anwesenheit, Fenster,
  Sensorverfügbarkeit, Eingangszustand, Ausgangszustand, Raumgruppe.
* **Aktionen:** Sollwert setzen/ändern, Raummodus, Betriebsmodus (optional
  befristet), Raumgruppe, allgemeinen Ausgang schalten, Funkbefehl,
  MQTT veröffentlichen (`<basis>/out/<thema>`), Benachrichtigung,
  Displaysteuerung, Alarm registrieren/löschen (`user.*`), Fensterstatus,
  Anwesenheit, Wärmeanforderung.

### Validierung vor Aktivierung

Referenzen (Räume, Sensoren, Eingänge, Gruppen, Funkbefehle), Grenzwerte und
Berechtigungen werden geprüft. Ungültige Regeln werden gespeichert, aber nicht
aktiviert. Eine statische Analyse erkennt Regeln, die sich gegenseitig auslösen
können (z. B. „Sollwert ändert sich → Modus“ und „Modus ändert sich →
Sollwert“), und verweigert die Aktivierung.

### Laufzeitschutz

* Kausaltiefe: Ereignisse aus Regelaktionen tragen Tiefe+1, ab Tiefe 3 keine
  weitere Regelverarbeitung.
* Eine Regel reagiert nie auf ihre eigenen Ereignisse.
* Sperrzeit und Ausführungen pro Stunde je Regel.
* Sturmschutz: > 25 Ausführungen in 10 s pausieren alle Regeln für 60 s
  (Alarm `automation.storm`).
* Prioritäten: Greifen mehrere Regeln im selben Ereignis auf dasselbe Ziel zu
  (z. B. Sollwert eines Raums), gewinnt die höhere Priorität; die anderen
  Aktionen werden als „übersteuert“ protokolliert.
* Testmodus: Regel reagiert normal, Aktionen werden nur simuliert.

## Sicherheitsfunktionen (nicht übersteuerbar)

Die Sicherheitsstufe sitzt direkt vor den Ausgängen
(`hc/heating/Safety.cpp`) und ist nur lokal im Webinterface konfigurierbar:

* Vorlauf-Übertemperatur → alle Heizkreise zu (auch bei Handbetrieb).
* Raum-Übertemperatur → Heizkreise des Raums zu.
* Sollwertgrenzen (Standard 5–28 °C, harte Grenze 30 °C).
* Handbetrieb und Fernschaltungen sind immer zeitlich begrenzt.
* Mindestschaltabstand und Schaltungen/Stunde für allgemeine Ausgänge;
  Mindestlauf-/Stillstandszeiten für Ventile.
* Fühlerausfall → Heizkreis zu (oder optional Notbetrieb mit Taktung).
* Watchdogs: Task-Watchdog (Reset bei hängender Regelung) und
  Ausgangs-Watchdog (sicherer Zustand, wenn die Regelschleife > n s steht).
* Automationen, Funk und Home Assistant können Heizkreis-Ausgänge,
  Sicherheitsgrenzen und Systemalarme nicht ändern.

## Lernmodell

`dT/dt = a·u(t − Totzeit) + b·(T_außen − T)` (mit Außenfühler) bzw.
`a·u + c` (Drift, ohne Außenfühler). Parameterschätzung per rekursiver
Kleinste-Quadrate mit Vergessensfaktor; Steigung aus 10 Minutenwerten
(DS18B20-Auflösung 0,0625 K).

Jedes Intervall wird klassifiziert, nur **normale** Intervalle passen das
Modell an:

| Klasse | Ursache |
|---|---|
| Lüftung | Fenster offen (Draht oder Funk) + Erholzeit (Standard 30 min) |
| Unvollständig | fehlende Werte, Ersatzfühler aktiv |
| Manuell | Hand-Ausgang, manueller Sollwert (konfigurierbar) |
| Heizungsstörung | Übertemperatur, Vorlaufschutz, Fühlerausfall, Ausgangsfehler, Verdacht „keine Erwärmung“ |

Ein offenes Fenster wird damit nie als schlechte Heizleistung gelernt. Die
Totzeit wird aus Heizstarts geschätzt (Mittel ± Streuung). Prognosen enthalten
eine Unsicherheit (Parameterkovarianz + Residuen). Verdacht auf
Heizungsstörung: vertrauenswürdiges Modell, volle Ventilöffnung, aber
deutlich weniger Erwärmung als vorhergesagt über ≥ 1 h → Ereignis
`fault_suspected`, Heizkreis als gestört markiert.

**Rückfallebene:** Das Modell gibt nur Ratschläge (Schwellenverschiebung
≤ 0,5 K), und nur, wenn es „bereit“ und die Prognoseunsicherheit klein ist.
Sonst regelt die konventionelle Hysterese unverändert. Die lernende Regelung
ist pro Raum und global abschaltbar.

Kontextereignisse (Fenster, manuelle Änderungen, Anwesenheit, Moduswechsel)
werden pro Raum protokolliert und im Webinterface angezeigt.
