# Hydraulischer Abgleich (Heizung → Hydraulischer Abgleich)

HeatCore misst keinen Durchfluss. Es zeichnet aber alle 15 Minuten auf, wie
lange das Ventil jedes Raums offen war und wie nah der Raum am Sollwert lag.
Daraus lässt sich ablesen, welcher Heizkreis zu wenig oder zu viel Wasser
bekommt.

**Grundidee:** In einer gut abgeglichenen Anlage brauchen alle Räume ungefähr
gleich viel Ventilzeit.

* Ein Raum, dessen Ventil fast immer offen ist und der trotzdem zu kalt
  bleibt, bekommt zu wenig Durchfluss.
* Ein Raum, der den Sollwert mit kurzen Öffnungszeiten hält oder sogar
  überschwingt, bekommt mehr, als er braucht. Dieses Wasser fehlt den anderen
  Kreisen.

Verstellt wird am Heizkreisverteiler, am Durchflussmesser (l/min) bzw. am
voreinstellbaren Ventileinsatz.

## Welche Daten zählen

Nur ruhige Komfortzeiten werden ausgewertet. Dafür muss alles Folgende
zutreffen:

* Der Zeitplan steht auf Komfort und der Sollwert ist mindestens 15 °C.
* Das Fenster war zu.
* Es gab keinen Handbetrieb, weder für den Sollwert noch für das Ventil.
* Es gab keine Störung und keinen Ersatzwert für einen ausgefallenen Fühler.
* Das Wärmeangebot war da: Die Ventile wurden nicht wegen eines kalten
  Kessels zugehalten.
* Es war draußen höchstens 15 °C warm (sofern ein Außenfühler vorhanden ist).
* Es lag nicht in den ersten 2 Stunden nach einer Sollwerterhöhung
  (Aufheizphase).

Ein Raum wird erst ab **12 Stunden** solcher Daten bewertet. Der
Auswertezeitraum ist einstellbar: 3 bis 14 Tage, Vorgabe 7 Tage.

## Empfehlungen je Raum

Die Werte sind Mittelwerte über den Auswertezeitraum:

* **Ventil offen Ø:** Anteil der Zeit, in der das Ventil offen war.
* **Abweichung Ø:** Ist-Temperatur minus Soll-Temperatur.

| Empfehlung | Bedingung | Vorschlag |
|---|---|---|
| **Durchfluss erhöhen** | Ventil ≥ 80 % offen und Abweichung ≤ −0,4 K | etwa +0,5 l/min bzw. eine halbe Umdrehung weiter öffnen |
| **etwas erhöhen** | Ventil ≥ 80 % offen, Sollwert wird knapp gehalten – oder Ventil ≥ 60 % und mindestens 30 Prozentpunkte über dem Median aller Räume | etwa +0,25 l/min |
| **Durchfluss reduzieren** | Sollwert gehalten (Abweichung ≥ −0,2 K) mit ≤ 20 % Ventilzeit, oder ≥ 25 % der Zeit über 0,5 K zu warm bei ≤ 45 % Ventilzeit | etwa −0,25 l/min |
| **passt** | alles andere | nichts ändern |
| **zu wenig Daten** | weniger als 12 h geeignete Zeit | warten |

**„Fremdwärme?“** erscheint bei einem Raum, der trotz seltener
Ventilöffnung zu warm ist, zum Beispiel durch Sonne, Herd oder Ofen. Der
Durchfluss darf dort reduziert werden. Die Bewertung ist für diesen Raum
aber weniger aussagekräftig.

**Räume mit mehreren Heizkreisen** haben nur einen Raumfühler. Sie werden
deshalb gemeinsam bewertet. Die Empfehlung gilt für alle Kreise des Raums:
gleichmäßig verstellen.

## Gesamtbild

* **Vorlauf zu niedrig:** Mindestens die Hälfte der bewerteten Räume ist trotz
  offener Ventile zu kalt, und kein Raum hat Wärme übrig. Mehr Durchfluss an
  einem Kreis nimmt ihn dann nur den anderen weg. Deshalb zuerst die
  Vorlauftemperatur bzw. Heizkurve anheben (z. B. am RC35 die Auslegungs- oder
  Raumtemperatur) und danach erneut auswerten.
* **Vorlauf könnte sinken:** Alle Räume halten den Sollwert mit höchstens
  35 % Ventilzeit. Eine niedrigere Vorlauftemperatur spart Energie. Zum
  Reduzieren werden dann nur noch deutliche Ausreißer vorgeschlagen (weniger
  als die Hälfte des Medians).

## Vorgehen

1. Einige kalte Tage abwarten, bis alle Räume bewertet sind.
2. Bei „Vorlauf zu niedrig“ zuerst die Heizkurve anheben.
3. Sonst nur einen oder wenige Kreise in kleinen Schritten verstellen.
4. Jede Verstellung im **Änderungsprotokoll** eintragen, z. B.
   „Kreis 3: 1,5 → 2,0 l/min“, für einen Raum oder für alle Räume.
5. Der betroffene Raum wird danach **24 Stunden** nicht bewertet.
   Fußbodenheizungen reagieren so träge. Danach zählen für ihn nur die neuen
   Daten.
6. Wiederholen, bis alle Räume „passt“ zeigen.

Das Änderungsprotokoll fasst bis zu 40 Einträge. Es wird mit den
Heizungseinstellungen gespeichert und ist in der Datensicherung enthalten.

Die Auswertung ersetzt keinen rechnerischen Abgleich nach Heizlast
(Verfahren B). Sie hilft aber, die Anlage im Betrieb nachzujustieren.

## Home Assistant, REST und KI

* **Home Assistant:** Je Raum mit Heizkreis gibt es einen Sensor
  „<Raum> Abgleich“. Sein Zustand ist passt, Durchfluss erhöhen, etwas
  erhöhen, Durchfluss reduzieren oder zu wenig Daten. Die Attribute sind
  Empfehlung, Ventilzeit (%), Abweichung (K) und Stunden.
* **REST:**
  * `GET /api/balance`
  * `PUT /api/balance` mit `{"days":7}`
  * `POST /api/balance/notes` mit `{"room":"flur","text":"…"}` (leerer Raum
    = alle Räume)
  * `DELETE /api/balance/notes/<id>`
* **MCP:** Das Werkzeug `get_balance` liefert dieselbe Auswertung an
  KI-Assistenten.
