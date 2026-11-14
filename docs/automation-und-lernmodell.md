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
* Netzteil-Lastgrenze (System → Sicherheit → „Netzteil der Ventile“): Mit
  eingetragenem Nennstrom schaltet HeatCore Ventile nur ein, solange
  Grundlast + Einschaltströme der startenden Ventile + Dauerströme der
  warmen Ventile in den nutzbaren Anteil (Standard 80 %) passen; weitere
  Ventile öffnen gestaffelt nach der Einschaltphase. Abschalten und das
  Schließen in den sicheren Zustand (NO-Ventile) werden nie verzögert.
* Watchdogs: Task-Watchdog (Reset bei hängender Regelung) und
  Ausgangs-Watchdog (sicherer Zustand, wenn die Regelschleife > n s steht).
* Automationen, Funk und Home Assistant können Heizkreis-Ausgänge,
  Sicherheitsgrenzen und Systemalarme nicht ändern.

## Raumregelung: Zweipunkt oder PWM

Thermische Stellantriebe kennen nur **auf** oder **zu** – eine
Zwischenstellung gibt es nicht. Je Raum wählbar (Räume & Heizkreise →
„Regelung“):

* **Zweipunkt (Hysterese, Standard):** Ventil auf unter Soll − H/2, zu über
  Soll + H/2. Das Lernmodell kann die Schaltschwellen begrenzt verschieben
  (siehe unten). Gut für träge Fußbodenheizungen.
* **PWM-Takt (PI-Regler):** Ein PI-Regler berechnet einen Öffnungsanteil
  0–100 % (`duty`), der als ein Puls je Zyklus ausgegeben wird – z. B. 35 %
  von 20 min = 7 min auf, 13 min zu. Damit wird das Ventil „virtuell
  prozentual“ geöffnet und die Raumtemperatur schwankt weniger. Gut für
  Heizkörper und schnell reagierende Räume.
  * `pwmCycleMin` Zyklus 5–60 min (Fußbodenheizung 20–30, Heizkörper 10–15),
    mindestens 2 × Mindestlauf-/Pausenzeit der zugeordneten Heizkreise
  * `pwmKp` Verstärkung 5–200 %/K: Öffnungsanteil je Kelvin unter Soll
  * `pwmTnMin` Nachstellzeit 0–600 min (0 = nur P-Anteil): wie schnell der
    I-Anteil die bleibende Abweichung ausgleicht (Fußbodenheizung 90–180,
    Heizkörper 30–60)
  * **Kürzester Puls = Mindestlaufzeit der Heizkreise.** Thermische
    Stellantriebe brauchen 3–5 min zum Öffnen; Pulse darunter bewirken nichts.
    Kürzere Pulse werden auf 0 bzw. die Mindestlaufzeit gerundet, eine sehr
    kurze Pause auf 100 %. Mindestlauf-/Pausenzeit deshalb mindestens auf die
    Laufzeit des Stellantriebs setzen (typ. 180–300 s).
  * Ein neuer Zyklus beginnt sofort, wenn sich der Sollwert um ≥ 0,25 K ändert
    (Zeitplan, Handänderung, Fenster).
  * Erreicht die Temperatur Soll + Hysterese, schließt das Ventil sofort
    und der laufende Puls endet (Überschwingschutz). Bei offenem Fenster ruht
    der I-Anteil.
  * Das Lernmodell lernt weiter, verschiebt aber keine Schwellen.
  * Der aktuelle Öffnungsanteil steht in der Übersicht („PWM 35 %“), in
    `/api/status`, `/api/heating` und im MQTT-Raumzustand (`duty`).

Sicherheit, Mindestlaufzeiten, Fühlerausfall-Strategie und Übertemperatur
gelten für beide Regelarten unverändert.

## Vorheizen (Fußbodenheizung)

Ein Estrich braucht Stunden, um wärmer zu werden (gemessen z. B. 0,1–0,3 K/h).
Kurze Komfortzeiten wie „06:00–08:00, 16:00–22:00“ mit 3 K Absenkung
erreichen die Komforttemperatur deshalb nie. Empfehlung für
Fußbodenheizung: **ein Komfortblock pro Tag** und eine **kleine Absenkung
(1–2 K)**. Die Schaltfläche „Vorlage Fußbodenheizung“ im Raum trägt
Mo–Fr 06:00–22:00, Sa–So 07:00–22:00 und 240 min Vorheizen ein.

**Vorheizen** (je Raum, „Vorheizen bis zu (min)“, 0 = aus): Im
Zeitplanbetrieb beginnt HeatCore vor einer Komfortzeit so früh mit der
Komforttemperatur, dass der Raum zu ihrem Beginn warm ist:

```
Vorlauf = Totzeit + (Komfort − Ist) / Aufheizrate × 60 min   (höchstens der eingestellte Wert)
```

Aufheizrate und Totzeit kommen aus dem Lernmodell (Heizleistung abzüglich
Wärmeverlust bei der aktuellen Außentemperatur). Solange noch keine
Aufheizrate gelernt ist, wird die volle eingestellte Zeit verwendet. Hat das
Vorheizen begonnen, läuft es bis zum Beginn der Komfortzeit weiter; ist der
Raum schon warm genug, entfällt es. Handbetrieb, Fenster, Urlaub, Abwesend
und feste Betriebsarten haben Vorrang. Grund und Display zeigen
„Vorheizen fuer Komfort ab HH:MM“, Home Assistant den Betrieb „Vorheizen“.

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
