# ESP32 Proxon P1 HESP-RS485 Sniffer & Controller to MQTT

Dieses System implementiert ein effizientes Kommunikations-Gateway für den **HESP-Bus** (Proxon P1 Heizungs- und Lüftungsanlagen). 
Basierend auf einem **ESP32 Dev Module** liest (snifft) das System zyklische Bus-Telegramme (Frage+Antwort) via RS485 mit **19200 Baud**, wertet diese in Echtzeit aus und stellt die Daten über **MQTT** sowie ein **Live-Webinterface (WebSockets)** bereit.

Zusätzlich erlaubt die Firmware den aktiven **Controller-Modus**: Wird der Bus länger als 250 ms still, übernimmt das ESP32 die Busmeisterschaft und stellt selbst periodische Queries im 100-ms-Takt an die Anlage. So können Parameter der Lüftungsanlage abgefraget **und** verändert werden – ohne dass ein externes Bedienpanel den Wert wieder überschreibt.

Ich nutze eine MariaDB zum Speichern der Daten und Node-Red zum Darstellen der Parameter und deren zeitlichen Verlauf.
Wie man mit den Daten in MQTT weiter arbeitet, ist aber nicht Thema dieses Projektes. 

---

## 🗺️ Systemarchitektur & Funktionsweise

Das System nutzt konsequent die **Dual-Core-Architektur** des ESP32, um zeitkritische Bus-Operationen vollständig von netzwerkbedingten Latenzen zu trennen:

### Betriebsmodi (BusMode)

Die Firmware unterscheidet zwei automatische Betriebszustände auf Core 1:

| Modus | Auslöser | Verhalten |
| :--- | :--- | :--- |
| **SNIFFER** | Bus aktiv (Daten < 250 ms) | Pasives Mithören von Frage/Antwort-Paaren; optionale Injektion von QUERY/SET im Sendezeitfenster |
| **CONTROLLER** | Bus still > 250 ms | Aktive Busmeisterschaft: 48 periodische Queries im exakten 100-ms-Takt, präzises Antwort-Einlesen, Kollisionserkennung |

Der Übergang zwischen den Modi erfolgt **vollautomatisch**:
- **SNIFFER → CONTROLLER**: Wenn seit der letzten Bus-Aktivität > 250 ms keine Daten empfangen wurden, wechselt der Task in den Controller-Modus und beginnt bei Query-Index 0.
- **CONTROLLER → SNIFFER**: Wenn innerhalb der 100-ms-Periode fremde Daten auf dem Bus erkannt werden (Kollision), wird sofort in den Sniffer-Modus zurückgefallen.

### Ultrafast Binary Queues (Ringpuffer)

Zur thread-sicheren Kopplung der beiden CPU-Kerne kommt ein kombinierter binärer Ringpuffer (`hespCombinedQueue`) mit einer festen Größe von `QUEUE_SIZE = 31` zum Einsatz. Jeder Frame trägt eine `HespMessageFrame`-Struktur:

| Feld | Beschreibung |
| :--- | :--- |
| `requestData` / `requestLength` | Die gestellte Frage (oder im Sniffer-Modus: das mitgehörte Telegramm) |
| `responseData` / `responseLength` | Die gekoppelte Antwort (im Sniffer-Modus: `-1` = keine Kopplung) |
| `isControllerMode` | `true` = Frage+Antwort sind ein synchrones Paar (Controller); `false` = passives Sniffer-Fragment |
| `timestamp` | Zeitpunkt des Ereignisses (ms) |
| `requestToResponseMs` | Latenz zwischen Frage-Ende und erstem Antwort-Byte (nur Controller-Modus) |

Core 1 legt die fertigen Frames binär im RAM ab und inkrementiert `qCombinedIn`. Core 0 holt diese asynchron ab (`qCombinedOut`), wandelt sie in HEX-Strings um und publiziert sie auf MQTT.

### Query-Speicher (LittleFS)

Die 48 periodischen Controller-Queries werden in einem strukturierten RAM-Spiegel (`ramBuffer[NUM_CONTROLLER_QUERIES]`) gehalten und persistent im **LittleFS** unter `/queries.bin` gesichert. Jede Zeile (`ControllerQueriesRow`) enthält:

- `length`: Byte-Länge der Query (12 oder 14 Bytes)
- `data[MAX_HESP_QUERY_LEN]`: Die HESP-Frame-Daten (Header, DP-ID, Payload, CRC)
- `changed`: Flag für Änderungen seit dem letzten Flash-Write

Über MQTT lassen sich einzelne Zeilen zur Laufzeit modifizieren (`updateRowByID`) – die Änderung wird sofort im Flash persistiert und im nächsten 100-ms-Zyklus wirksam.

---

## 🔌 Hardware-Verdrahtung und Anschlüsse

Das System benötigt ein **RS485-zu-TTL-Modul mit automatischer Flusssteuerung / Sende-Empfangs-Umschaltung** (rote Platine, voll kompatibel mit 3,3 V Logikpegeln), um Schäden am ESP32 zu vermeiden.

### Pinbelegung

| ESP32 Dev Module Pin | RS485-zu-TTL Modul Pin | Proxon X6 Klemme | Kabelfarbe (Standard) | Beschreibung |
| :--- | :--- | :--- | :--- | :--- |
| **Pin 22 (RX2)** | TXD | - | - | Empfangsleitung UART2 |
| **Pin 23 (TX2)** | RXD | - | - | Sendeleitung UART2 |
| **GND** | GND | X6-1 | Weiß | Gemeinsames Bezugspotenzial |
| **3.3V** | VCC | - | - | Spannungsversorgung Modul |
| - | D- / A | X6-4 | Braun | RS485 Datenleitung A |
| - | D+ / B | X6-3 | Blau | RS485 Datenleitung B |
| - | - | X6-2 | - | +12V (Anlage) *Nicht mit 3.3V verbinden!* |

> ⚠️ **WICHTIGER HARDWARE-HINWEIS:** Die +12V-Leitung der Proxon X6 Klemme (X6-2) darf **niemals** direkt an den 3,3V- oder VIN-Pin des ESP32 angeschlossen werden! Zur Versorgung des ESP32 direkt über die Anlage ist ein geeigneter DC-DC-Abwärtsregler (12V auf 5V an den `VIN`/`5V`-Pin) zwingend erforderlich.

| Hardware | Bezeichnung / Version |
| :--- | :--- |
| Gerätetyp | **P 1-L** installiert Ende 2013 |
| Hauptplatine | **LT-ZIM V1.7** von Hermes electronic GmbH Version 1.7 01.2018 (Austausch Anfang 2026) |
| PTC-Platine: | **PCT-Modul 4x V1.2** |
| Panel: | **BDE Comfort Version V03.7.9B00** |

---

## 🛠️ Software-Konfiguration

Vor dem Kompilieren müssen die Netzwerk- und MQTT-Zugangsdaten direkt im Quellcode angepasst werden:

```cpp
// --- WLAN Parameter ---
const char* ssid = "DEINE_SSID";
const char* password = "DEIN_WLAN_PASSWORT";

// --- MQTT Broker Parameter ---
const char* mqtt_server = "DEINE MQTT_SERVER_ID";
const int   mqtt_port = 1883;
```


## Verwendete Bibliotheken

Die Firmware setzt auf bewährte Standard-Bibliotheken des ESP32-Arduino-Ökosystems:
```
    WiFi.h (Integriert, zur WLAN-Verbindung)
    WebServer.h & WebSocketsServer.h (Für das Web-Dashboard und Live-Streaming)
    PubSubClient.h (Für die MQTT-Kommunikation – Puffer wird im Code dynamisch auf 2048 Bytes erweitert)
    ArduinoJson.h (Ab Version 6/7, zur Verarbeitung komplexer Laufzeitparameter)
    ArduinoOTA.h (Integrierte Update-Schnittstelle)
    LittleFS.h (Persistenter Query-Speicher und Log-Datei auf dem Flash)
```

## 📡 MQTT-Schnittstelle & API-Spezifikation

Das Gateway kommuniziert ereignisbasiert und bidirektional über vier dedizierte MQTT-Topics.

1. Gesendete Daten (Publish)
   
|Topic |	Inhalt|
| :--- | :--- |
|rs485/sniffer/raw |	HEX-Strings der Bus-Telegramme. Im Controller-Modus: Frage + Antwort concatenated (vollständiges Paar). Im Sniffer-Modus: einzelne mitgehörte Fragmente.|
|rs485/sniffer/response |	Dedizierte Antworten der Anlage auf injizierte Befehle (Sniffer-Modus, Sub-Bus-Antwort).|
|rs485/sniffer/log |	Textbasierte System-Logs (Diagnose, Moduswechsel, Fehlermeldungen, Statistik).|

2. Empfangene Steuerbefehle (Subscribe)

|Topic |	Inhalt|
| :--- | :--- |
|rs485/sniffer/send| Dieses Topic verarbeitet zwei unterschiedliche Payload-Typen (wird im mqttCallback automatisch unterschieden)|

### Typ A: Befehls-Injektion (HEX-String)

Wird ein reiner HEX-String empfangen, konvertiert die Firmware diesen Byte für Byte und sichert das Ergebnis im txBuffer und im ramBuffer (Query-Speicher).

Verhalten je nach aktuellem BusModus:
Modus 	Aktion
SNIFFER 	Das Paket wird in der nächsten Buspause injiziert (QUERY einmalig, SET wiederholt bis SET_REPEAT_TIME abgelaufen). Die DP-ID wird zur Laufzeit im Query-Buffer aktualisiert (updateRowByID), sodass der Controller-Modus den neuen Wert im nächsten Zyklus übernimmt.
CONTROLLER 	Die entsprechende Zeile im ramBuffer wird direkt überschrieben und im Flash gesichert. Ab dem nächsten 100-ms-Takt wird die modifizierte Query aktiv gestellt.

    Hinweis: Bei SET-Befehlen (Header 0x11) prüft der Sniffer-Modus optional, ob der neue Wert tatsächlich vom gespeicherten abweicht, bevor die Injektion startet (Stichwort: Bedienpanel überschreibt nicht mehr).

### Typ B: Parameter-Konfiguration via JSON

Beginnt die Nachricht mit einer geschweiften Klammer { (ASCII 123), wird sie als JSON geparst. Folgende Parameter lassen sich zur Laufzeit verändern:


```{
  "sendTimeBegin": 30,
  "sendTimeEnd": 33,
  "SET_REPEAT_TIME": 100,
  "PACKET_TIMEOUT": 12,
  "LONG_ANSWER_MIN": 64,
  "SEND_AFTER_MATCHING_DP": 1,
  "SHOW_DP_LIST_COUNTER": 5,
  "ShowSETChanges": 3,
  "ShowStatistikMinutes": 1
}
```

| Parameter |	Einheit 	|Beschreibung  |
| :--- | :--- | :--- |
|sendTimeBegin |	ms |	Start des erlaubten Sendezeitfensters nach Zyklus-Start (Sniffer-Modus) |
|sendTimeEnd |	ms |	Ende des Sendezeitfensters|
|SET_REPEAT_TIME |	ms |	Dauer, über die ein SET-Befehl zyklisch wiederholt wird, bevor die Injektion stoppt|
|PACKET_TIMEOUT |	ms |	Bus-Stille, die ein Telegramm-Ende signalisiert. ≥ 12 ms: Frage+Antwort in einem Frame; < 12 ms: getrennt|
|LONG_ANSWER_MIN |	Bytes |	Ab dieser Länge gilt eine Antwort als lang und verhindert eine eigene QUERY/SET im selben Zyklus|
|SEND_AFTER_MATCHING_DP <	0/1 |	1: Injektion nur, wenn die DP-ID (Byte 3+4) des laufenden Bus-Traffic mit der zu sendenden übereinstimmt. 0: Immer senden|
|SHOW_DP_LIST_COUNTER |	int |	Anzahl der Zyklen, in denen die DP-Liste im Log ausgegeben wird (Start bei DP 032e)|
|ShowSETChanges |	int |	Anzahl der SET-Änderungen, die im Log mit altem/neuem Wert angezeigt werden|
|ShowStatistikMinutes |	min 	Intervall der Statistik-Ausgabe (intern × 60000 → ms). 0 deaktiviert|

Typ C: Steuerbefehle (JSON-Aktionen)|

| Key |	Wirkung|
| :--- | :--- |
|setDefaultValues |	Setzt alle Timing- und Display-Variablen auf die sicheren Standardwerte zurück (30/33/100/12/64/1/0/0/1min)|
|getQueries |	Gibt die vollständige Query-Tabelle (48 Zeilen) als HEX-Listing im Log aus|
|removeBufferFile |	Löscht /queries.bin vom LittleFS und initialisiert die Query-Tabelle neu aus der Kompilierzeit-Konstante controllerQueriesData|

## 🎛️ Controller-Modus im Detail
Automatik-Wechsel & 100-ms-Takt



```Bus still > 250 ms ──►  MODE_CONTROLLER
                            │
                            ▼
              ┌──────────────────────────────────┐
              │  100 ms Periode (vTaskDelayUntil)│
              │                                  │
              │  ① Query aus ramBuffer lesen    │
              │  ② Serial2.write() → Bus        │
              │  ③ Antwort einlesen (≤ 95 ms)   │
              │     ├─ First-Byte-Timeout: 22 ms│
              │     └─ Inter-Byte-Timeout: 4 ms │
              │  ④ Frame in Queue (Req+Resp)    │
              │  ⑤ Kollisions-Check (~90 ms)    │
              │  ⑥ Nächster Query-Index         │
              └──────────────────────────────────┘
                            │
                    Kollision erkannt?
                            │── ja ──►  MODE_SNIFFER
                            │
                            └── nein ──►  nächste Periode
```

Timing-Konstanten (Controller)

|Konstante |	Wert |	Bedeutung|
| :--- | :--- | :--- |
|MAX_REPLY_WINDOW |	95 ms |	Maximales Gesamtfenster für die Antwort innerhalb der 100-ms-Periode|
|FIRST_BYTE_TIMEOUT |	22 ms |	Maximales Warten auf das erste Antwort-Byte|
|INTER_BYTE_TIMEOUT |	4 ms |	Stille zwischen Bytes → Telegramm beendet|
|Periode |	100 ms |	Exakt gehalten durch vTaskDelayUntil (kompensiert Rechenzeit automatisch)|

Query-Tabelle (Auszug)

Die 48 Queries sind als controllerQueriesData in tools.h hartkodiert und werden beim ersten Start nach /queries.bin geschrieben. Beispiele:

|Index |	DP-ID |	Typ |	Wert |	Bedeutung|
| :--- | :--- | :--- | :--- | :--- |
|0 |	032e |	QUERY (10) |	2 |	Status/Info|
|1 |	0226 |	SET (11) 	|22.37 °C |	IST-Temperatur|
|2 |	0227 |	SET (11) 	|22.0 °C 	|SOLL-Temperatur|
|3 |	0229 |	SET (11) 	|21.0 °C 	|Soll-Temp (alt)|
|5 |	020A |	SET (11) 	|2 	|Betriebsart|
|10| 	00e1 |	SET (11) 	|3 	|Lüfterstufe [0–4]|
|45| 	0191 |	SET (11) 	|1 	|– (Antwort-Pause nur 4 ms)|

    Die Typen 0x10 (QUERY) und 0x11 (SET) stehen im Low-Nibble des Header-Bytes. Queries mit Typ 0x10 sind read-only (Wert fest), 0x11-Einträge sind schreibbar.

## Antwort-Kopplung & Latenzmessung

Im Controller-Modus wird exakt gemessen, wie viele Millisekunden zwischen dem Ende der gesendeten Frage und dem ersten empfangenen Antwort-Byte liegen (requestToResponseMs). Dieser Wert ist im Frame enthalten und steht Core 0 für das MQTT-Publishing zur Verfügung – ideal zur Überwachung der Bus-Gesundheit.
SET-Wiederholung & Stopp-Logik (Sniffer-Modus)

Im Sniffer-Modus funktioniert die SET-Injektion wie folgt:

    Erster erfolgreicher Sendevorgang startet die Stoppuhr (injektionStartZeit).
    In jedem folgenden 100-ms-Zyklus wird der SET-Befehl erneut injiziert.
    Nach Ablauf von SET_REPEAT_TIME wird die Injektion automatisch beendet.
    Ein neues SET über MQTT während des Feuerns setzt die Stoppuhr zurück.

Diese Logik verhindert, dass ein externes Bedienpanel den Wert zwischen den Wiederholungen überschreibt.

## 💻 Webinterface & Sicherer OTA-Prozess

Das System stellt eine minimalistische, aber hochperformante Web-Oberfläche zur Verfügung. Sie ist vollständig im Flash-Speicher (PROGMEM) hinterlegt.
Hauptseite (http://<esp32-ip>/)

    Live-Log: Zeigt den Bus-Verkehr (Sniffer + Controller) in Echtzeit über eine permanente WebSocket-Verbindung (Port 81).
    Traffic Control: Bietet Buttons zum Pausieren (Log: AUS) oder Leeren der Ansicht, um den Browser-RAM bei hoher Bus-Last zu entlasten.

## Log-Export (http://<esp32-ip>/getLog)

    Gibt den aktuellen Inhalt der Web-Log-Queue als text/plain zurück (nützlich für curl oder Skripte).

Zweistufiger Firmware-Update-Prozess (http://<esp32-ip>/update)

Um unvollständige Flash-Vorgänge oder hängende Browser-Anfragen nach einem Reboot zu verhindern, nutzt der Webserver eine ausgeklügelte dreistufige Logik beim Einspielen einer neuen .bin-Datei:

    Browser-Beruhigung: Nach erfolgreichem Empfang der Firmware sendet der ESP32 sofort ein sauberes HTML-Dokument mit einem HTTP-Refresh-Header (content='5;url=/') an den Client zurück.
    Verbindungs-Abbau: Der ESP32 wartet kurz per delay(500) und schließt die TCP-Client-Verbindung mittels server.client().stop() aktiv von sich aus.
    Hard Reboot: Erst nachdem die Netzwerk-Pakete sicher den Netzwerkstack verlassen haben, triggert das Gateway über ESP.restart() den kontrollierten Neustart der Hardware.

## ⏱️ Zeitkritische Logik & Core-1 Bus-Task

Die Kommunikation auf dem HESP-Bus (19200 Baud, 8N1) erfordert exakte zeitliche Koordination. Der Sende-Task auf Core 1 arbeitet autark in einer permanenten Schleife (for(;;)) und delegiert je nach currentBusMode:
Sniffer-Modus – Sendezeitfenster

```Bus-Aktivität (Master/Slave) ===[ DATA FRAME ]===|
                                                 |
                                                 v
                                        Stille detektiert (> PACKET_TIMEOUT ms)
                                                 |
                                                 v
                                        Sende-Fenster öffnet (sendTimeBegin, z.B. +30ms)
                                                 |---> [COLLISION / VETO CHECK]
                                                 |---> [QUERY / SET INJIZIEREN]
                                                 v
                                        Sende-Fenster schließt (sendTimeEnd, z.B. +33ms)
```

```Der Sniffer-Zyklus im Detail

    Bus-Empfang & Zyklus-Erkennung: Erkennt der Task nach einer Ruhepause > PACKET_TIMEOUT das erste Byte, startet der Zyklus-Timer.
    Sammel-Ende: Nach PACKET_TIMEOUT Stille oder Zyklus-Ende wird das Telegramm in die kombinierte Queue kopiert.
    Kollisionsschutz (Veto): Serial2.available() > 0 → Veto, kein Senden in diesem Takt.
    DP-Matching (optional): Bei SEND_AFTER_MATCHING_DP = 1 wird nur gesendet, wenn Byte 3+4 des laufenden Traffic die gleiche DP-ID tragen.
    Injektion & Echo-Löschung: Serial2.write() → flush() → Tx-Self-Hear löschen.
    Sub-Bus-Antwort: Antwort-Bytes werden isoliert in rxSubBuffer gesammelt (Timeout: 2 ms nach letztem Byte / 20 ms ohne Daten).
    SET-Timer: Wiederholtes Feuern bis SET_REPEAT_TIME abgelaufen.
```

```Controller-Modus – Aktive Busmeisterschaft

Siehe oben unter Controller-Modus im Detail.
```


## 📈 Status- und Performance-Metriken

Core 0 friert in einem konfigurierbaren Intervall (Standard: 1 Minute, via ShowStatistikMinutes) die Zähler kurzzeitig ein und gibt eine strukturierte Diagnose im USB-Terminal (Serial0) sowie per MQTT auf rs485/sniffer/log aus:

            

========================================
 11.11.2026 10:53:00
 HESP-BUS STATISTIK (1 Min)
----------------------------------------
 read/send Msg  (MQTT): 1200/1200
 query/response (TX/RX): 1200/1198
 Bus-Freq./Quality:    ~19.98 Hz / 100 %
========================================
Free storage space: 287 kB

## Metrik 	Beschreibung
read/send Msg (MQTT) 	Gelesene Frames vs. erfolgreich per MQTT publiziert
query/response (TX/RX) 	Eigene gestellte Queries vs. erhaltene Antworten (Controller-Modus)
Bus-Frequenz 	(read + write) / 60 / min – Erwartungswert: ~20 Hz (100-ms-Takt)
Bus-Qualität 	100 × send / read – 100 % = kein Queue-Überlauf, keine MQTT-Fehler
Free storage 	Verbleibender Heap-Speicher in kB

## 📁 Persistenter Speicher (LittleFS)
|Datei |	Zweck|
| :--- | :--- |
|/queries.bin |	48 × ControllerQueriesRow (je 16 Bytes) = 768 Bytes. Enthält die aktuelle Query-Tabelle. Wird bei jeder updateRowByID sofort geschrieben.|
|/log.txt |	Laufendes Text-Log (aktuell als Platzhalter, für zukünftige Erweiterungen).|

    ⚠️ Ein LittleFS.format() löscht beide Dateien. Die Query-Tabelle wird beim nächsten Boot aus der hartkodierten Konstante controllerQueriesData neu initialisiert.


## 🚀 Inbetriebnahme & Troubleshooting

    Kompilierung:

    Stelle in der Arduino IDE sicher, dass das ESP32 Dev Module ausgewählt ist (Board → ESP32SDev Module) und der serielle Monitor auf 115200 Baud steht.

    Erster Start / LittleFS-Initialisierung:

    Beim allerersten Boot wird automatisch /queries.bin mit den 48 Standard-Queries geschrieben. In der Konsole erscheint:

    Keine bestehenden Query-Daten gefunden. Schreibe Initialdaten...

    Diagnose beim Booten:

    Achte auf die Meldung:


    >>> ERFOLG: RS485-Task erfolgreich auf Core 1 gestartet! <<<

    Erscheint stattdessen ein Fehlercode, war der Heap-Speicher für den 8 KB Task-Stack unzureichend.

    Moduswechsel beobachten:

    Im Live-Log (Web oder MQTT rs485/sniffer/log) erscheint bei jedem Wechsel:



    >> Bus still (>250ms) -> Wechsel in den AKTIVEN MASTER-Modus
    >> Fremddaten in der Restperiode erkannt. Zurück zu Sniffer.

    Fehlersuche bei fehlendem Traffic:
        Falls das Live-Log im Webinterface oder Terminal komplett leer bleibt, tausche die A- und B-Leitungen des RS485-Moduls (häufigste Ursache bei RS485-Inbetriebnahmen).
        Überprüfe mit einem Multimeter, ob der GND des ESP32 eine stabile, niederohmige Verbindung zum GND der Proxon-Platine (X6-1) aufweist. Ohne gemeinsamen GND führen Gleichtaktstörungen zu korrupten Datenpaketen.
        Im Controller-Modus: Falls keine Antwort zur Frage: XXXX im Log erscheint, ist die Anlage entweder nicht im Bus oder die DP-ID unbekannt.

    Query-Tabelle zurücksetzen:

    Falls durch fehlerhafte SET-Befehle die Query-Tabelle korrupt ist, sende per MQTT:


    { "removeBufferFile": true }

    Das ESP32 löscht /queries.bin und initialisiert die Standard-Queries neu.

## Referenzen

    Das gesamte Projekt basiert auf der Arbeit von Markus Mauch und seinen Beschreibungen unter:

    Markus Mauch’s HESP documentation
    Documentation repository

    Der Text und der Code wurden zu ca. 95 % von Google erstellt.

    Ein ähnliches Projekt mit Anbindung an Home Assistant ist unter https://github.com/DNier/proxon-hesp-homeassistant zu finden.

## Geltungsbereich

Alle Angaben stammen aus Messungen an einer einzelnen Anlage, erhoben ohne Beteiligung des Herstellers. Nachbau auf eigene Verantwortung; Eingriffe können Gewährleistung oder Garantie berühren. Es werden keine geschützten Hersteller-Unterlagen wiedergegeben.
Lizenz & Marken

Inhalte unter CC BY 4.0 (siehe LICENSE). Unabhängiges Projekt, nicht mit Zimmermann Lüftungs- und Wärmesysteme GmbH & Co. KG affiliiert; „PROXON", „Zimmermann" und weitere genannte Namen sind Marken ihrer jeweiligen Inhaber und werden beschreibend verwendet.


