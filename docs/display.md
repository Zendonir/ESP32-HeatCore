# SSD1306-OLED

128×64 Pixel, I²C 0x3C. Optional: Fehlt oder versagt das Display, wird alle
30 s eine Neuinitialisierung versucht; die Regelung ist unabhängig davon.

## Seiten

1. **System** – Gerätename, Uhrzeit, IP, Netzwerkart, Datum, aktive Heizkreise,
   Sensoren ok/gesamt, Fehlerzahl
2. **Räume** – Name, Ist, Soll, Kennzeichen (Flamme = Ventil offen, `+` =
   Anforderung, `F` = Fenster, `!` = Fehler). Modi: Liste, Groß (ein Raum),
   Kompakt
3. **Heizkreise** – 16 Felder: gefüllt = offen, `+` Anforderung, `X` Fehler,
   Strich = nicht belegt
4. **Diagnose** – Last der HeatCore-Tasks (bezogen auf beide Kerne), freier RAM
   und PSRAM, Laufzeit, Firmware, SD, MQTT, Netzwerk
5. **Lernmodell** – Raum, Status, Aufheiztrend, Totzeit ± Unsicherheit,
   Prognose 1 h ± Unsicherheit, Heizleistung

Alarme ab der eingestellten Priorität erscheinen als Balken in der letzten
Zeile; neue Alarme können das Display wecken.

## Einstellungen (Webinterface, sofort wirksam, persistent)

Aktivierung, Kontrast 0–255, Rotation 0°/180° (der SSD1306 kann nativ keine
90°), automatischer Seitenwechsel und Intervall, Seiten einzeln, angezeigte
Räume, Timeout (0 = immer an), Anzeigemodus, Alarmpriorität, Alarm weckt,
Sprache Deutsch/Englisch.

Die **Live-Vorschau** zeigt exakt den Bildspeicher, der an das Panel geht, und
kann jede Seite auch ohne angeschlossenes Display rendern.

## Bedienung

Navigation über Digitaleingänge, Funktasten, MQTT/Home Assistant und das
Webinterface mit denselben Aktionen: nächste/vorherige Seite, Seite wählen,
wecken, nächster Raum, Sollwert des angezeigten Raums ±. Der erste Tastendruck
bei dunklem Display weckt nur.
