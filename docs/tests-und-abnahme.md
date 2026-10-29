# Tests und Abnahmekriterien

## Testebenen

| Ebene | Werkzeug | Umfang |
|---|---|---|
| Unit- und Integrationstests | `build/host/heatcore_tests` (CMake, ASan/UBSan) | 63 Tests, 590 Prüfungen: Funk-Codec, Empfangskette, Sender, Sensor-Decoder, Sensor-Manager, Heizung, Sicherheit, Automationen, Lernmodell mit simulierter Raumthermik, REST-API, MQTT/Discovery, Konfiguration |
| Firmware-Build | GitHub Actions `pio run` | Kompilieren und Linken für ESP32-S3 inkl. aller Treiber |
| Webinterface End-to-End | `tools/sim/ui_test.js` (Playwright) gegen `heatcore_sim` | alle Seiten ohne JS-Fehler, Anlernen eines Funkgeräts über die Oberfläche, Regel anlegen, Displayvorschau |
| Hardware-Integration | Testaufbau laut `docs/hardware.md` | **noch nicht durchgeführt** |

Gekennzeichnet: **bestätigt** = automatisiert nachgewiesen;
**unbestätigt (HW)** = Logik getestet, auf der realen Platine noch zu prüfen.

## Abnahmekriterien

| # | Kriterium | Nachweis | Status |
|---|---|---|---|
| 1 | RF-Empfänger erkennt kompatible Telegramme zuverlässig | `rf_codec_*` (saubere Signale, T = 120–700 µs, Jitter/AGC-Verzerrung/Glitches, 200 000 Rauschpulse ohne Fehldekodierung), `rf_rx_*` | Logik bestätigt; mit den gelieferten Modulen **unbestätigt (HW)** |
| 2 | Lernmodus ohne Firmwareänderung | `api_rf_learn_add_change_remove_without_firmware_change`, UI-E2E-Test | bestätigt (Simulator); **unbestätigt (HW)** |
| 3 | Bekannte Funkcodes lassen sich Aktionen zuweisen | `system_rf_button_binding_adjusts_setpoint`, `system_di_and_rf_share_automation_trigger` | bestätigt |
| 4 | Unbekannte/fehlerhafte Telegramme lösen nichts aus | `rf_rx_unknown_code_never_triggers_input`, `rf_rx_single_frame_is_rejected`, `rf_rx_disabled_device_and_timing_mismatch`, `rf_codec_noise_produces_no_24bit_frames` | bestätigt |
| 5 | Sender überträgt korrekt | `rf_tx_sends_and_decodes` (erzeugte Wellenform wird dekodiert), `system_rf_loopback_selftest` | Logik bestätigt; Abstrahlung **unbestätigt (HW)** – Loopback-Selbsttest auf der Platine ausführen |
| 6 | Senden/Empfangen blockiert die Regelung nicht | Architektur: eigene Tasks, RMT, kein System-Lock während der Aussendung; ISR nur Ringpuffer | **unbestätigt (HW)**: „Regelzyklus max.“ unter Hardware → Selbsttest beobachten |
| 7 | OLED initialisiert zuverlässig | Treiber mit Probe, Init-Sequenz, Wiederholung alle 30 s | **unbestätigt (HW)** |
| 8 | OLED-Ausfall beeinflusst Heizkreise nicht | `system_display_hw_failure_does_not_affect_control`; Display-Task unabhängig | bestätigt; Einschränkung „SDA dauerhaft low“ siehe `docs/hardware.md` |
| 9 | Displayseiten/-konfiguration bleiben nach Neustart erhalten | `system_config_roundtrip_all_sections` | Logik bestätigt; LittleFS **unbestätigt (HW)** |
| 10 | Funk-Sensorausfall wird erkannt | `system_rf_supervision_failsafe`, `system_sensor_loss_detected_and_circuit_closed`, `system_fallback_sensor_is_used` | bestätigt |
| 11 | Prioritäten und Sicherheitsgrenzen | `system_automation_priority_conflict`, `system_automation_validation_and_safety_limits`, `system_automation_static_loop_detection`, `system_automation_runtime_depth_limit`, `system_flow_overtemp_closes_all_even_manual` | bestätigt |
| 12 | Home Assistant erkennt und bedient Komponenten | `mqtt_discovery_entities_and_permissions`, `mqtt_generic_inbound_triggers_rule` | Nachrichten bestätigt; mit echtem Broker/HA **unbestätigt** |
| 13 | Funktionen ohne Home Assistant | gesamte Testsuite läuft ohne MQTT; `system_runs_without_network_hooks` | bestätigt |
| 14 | Funkgeräte im Webinterface hinzufügen/ändern/entfernen | API-Test (#2) + UI-E2E-Test | bestätigt (Simulator) |
| 15 | Regelungsfähig bei WLAN-/Ethernet-/MQTT-Ausfall | Regelung im Control-Task ohne Netzabhängigkeit; MQTT im Netz-Task mit Warteschlange | Logik bestätigt; **unbestätigt (HW)** |
| 16 | OTA/Neustart schaltet keine Heizkreise ungewollt ein | `system_boot_outputs_safe_and_off`; Bootreihenfolge (sicherer Zustand zuerst), `g_shutdown` vor Neustart | Logik bestätigt; **unbestätigt (HW)** |
| 17 | Modell berücksichtigt Fenster und Störungen | `model_excludes_ventilation_periods`, `model_window_event_in_full_system_classified`, `model_detects_missing_heating_effect`, `model_incomplete_and_manual_periods_not_learned` | bestätigt |
| 18 | Speicher und Echtzeit im ESP32-S3-Rahmen | CI-Build: Programm 1,70 MB von 4 MB (40,6 %), statischer RAM 63,7 KB (19,4 %); Systemobjekt im PSRAM | statisch bestätigt; Laufzeit (Heap, PSRAM, Zykluszeit) **unbestätigt (HW)** |

## Hardware-Integrationstest (Checkliste)

1. Selbsttest: alle I²C-Adressen gefunden, RTC gültig, 1-Wire-Sensoren gezählt.
2. Ausgänge 1–16 im Handbetrieb mit Testlast schalten; Handbetrieb läuft ab.
3. Digitaleingänge: Taster kurz/lang, Kontakt → Ereignisprotokoll.
4. RF-Empfänger: Pegel am GPIO8 messen (≤ 3,3 V!), Störpegel (Flanken/s)
   ablesen, Fernbedienung anlernen, Reichweite prüfen.
5. RF-Sender: Loopback-Test, danach eigene Funksteckdose schalten.
6. Display: alle Seiten, Kontrast, Rotation, Timeout; im Betrieb abziehen und
   wieder anstecken.
7. Vorlaufschutz und Raum-Übertemperatur mit erwärmtem Fühler auslösen.
8. Netzwerk trennen, MQTT-Broker stoppen, OTA-Update einspielen – Ausgänge
   beobachten.
9. 24-h-Dauerlauf: Regelzyklus max., freier Heap, Alarme.
