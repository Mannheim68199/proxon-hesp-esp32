# ESP32 Proxon P1 HESP-RS485 Sniffer to MQTT

Dieses System implementiert ein effizientes Kommunikations-Gateway für den **HESP-Bus** (Proxon P1 Heizungs- und Lüftungsanlagen). 
Basierend auf einem **ESP32 Dev Module** liest (snifft) das System zyklische Bus-Telegramme via RS485, wertet diese in Echtzeit aus und stellt die Daten über **MQTT** sowie ein **Live-Webinterface (WebSockets)** bereit.

Zusätzlich erlaubt die Firmware das gezielte **Injizieren von Steuerbefehlen (SET/Query)** exakt in einem kritischen Sende-Zeitfenster direkt auf den Bus, um Parameter der Lüftungsanlage zu verändern. ( was aber (noch?) nicht funktioniert !!! )

---

## 🗺️ Systemarchitektur & Funktionsweise

Das System nutzt konsequent die **Dual-Core-Architektur** des ESP32, um zeitkritische Bus-Operationen vollständig von netzwerkbedingten Latenzen zu trennen:

### Ultrafast Binary Queues (Ringpuffer)
Zur thread-sicheren Kopplung der beiden CPU-Kerne kommen zwei unabhängige, binäre Ringpuffer (`hespBinQueue` und `hespRespQueue`) mit einer festen Größe von `QUEUE_SIZE = 31` zum Einsatz. Core 1 legt die empfangenen Rohdaten oder Antworten blitzschnell binär im RAM ab und inkrementiert die Zeiger (`qIn` / `qRespIn`). Core 0 holt diese im Haupt-Thread asynchron ab (`qOut` / `qRespOut`), wandelt sie performant in HEX-Strings um und sendet sie ins Netzwerk.

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

### Verwendete Bibliotheken
Die Firmware setzt auf bewährte Standard-Bibliotheken des ESP32-Arduino-Ökosystems:
* `WiFi.h` (Integriert, zur WLAN-Verbindung)
* `WebServer.h` & `WebSocketsServer.h` (Für das Web-Dashboard und Live-Streaming)
* `PubSubClient.h` (Für die MQTT-Kommunikation – Puffer wird im Code dynamisch auf 2048 Bytes erweitert)
* `ArduinoJson.h` (Ab Version 6/7, zur Verarbeitung komplexer Laufzeitparameter)
* `ArduinoOTA.h` (Integrierte Update-Schnittstelle)

---

## 📡 MQTT-Schnittstelle & API-Spezifikation

Das Gateway kommuniziert ereignisbasiert und bidirektional über drei dedizierte MQTT-Topics.

### 1. Gesendete Daten (Publish)
* **`rs485/sniffer/raw`**: Core 0 konvertiert die aus dem Ringpuffer gelesenen Rohdaten in performant vorreservierte (`hexPayload.reserve()`) HEX-Strings und pusht sie auf dieses Topic.
* **`rs485/sniffer/response`**: Hierhin sendet das System die dedizierten Antworten der Anlage auf abgesetzte Befehle.

### 2. Empfangene Steuerbefehle (Subscribe)
* **`rs485/sniffer/send`**: Dieses Topic verarbeitet zwei unterschiedliche Payload-Typen (wird im `mqttCallback` automatisch unterschieden):

#### Typ A: Befehls-Injektion (HEX-String)
Wird ein reiner HEX-String empfangen, konvertiert die Firmware diesen Byte für Byte über die Hilfsfunktion `hexCharToByte` und sichert das Ergebnis im globalen `txBuffer`. Das Flag `readyToSend = true` signalisiert Core 1, dass das Paket in der nächsten Buspause injiziert werden soll.

#### Typ B: Parameter-Konfiguration via JSON
Beginnt die Nachricht mit einer geschweiften Klammer `{` (ASCII 123), wird sie als JSON geparst. Folgende Parameter lassen sich zur Laufzeit verändern:

```json
{
  "sendTimeBegin": 40,
  "sendTimeEnd": 55,
  "SET_REPEAT_TIME": 1800
}
```

* **`sendTimeBegin`** / **`sendTimeEnd`**: Definiert das erlaubte Sendezeitfenster in Millisekunden nach der erkannten Bus-Stille.
* **`SET_REPEAT_TIME`**: Bestimmt die Dauer in Millisekunden, über die ein SET-Befehl zyklisch wiederholt an den Bus gefeuert wird.
* **`setDefaultValues`**: Übergibt man diesen Key im JSON, setzt das Gateway alle Timing-Variablen sofort auf die sicheren Standardwerte zurück (`35ms` / `50ms` / `1500ms`).

---

## 💻 Webinterface & Sicherer OTA-Prozess

Das System stellt eine minimalistische, aber hochperformante Web-Oberfläche zur Verfügung. Sie ist vollständig im Flash-Speicher (`PROGMEM`) hinterlegt.

### Hauptseite (`http://<esp32-ip>/`)
* **Live-Log:** Zeigt den Bus-Verkehr hexadezimal in Echtzeit über eine permanente WebSocket-Verbindung (Port 81).
* **Traffic Control:** Bietet Buttons zum Pausieren (`Log: AUS`) oder Leeren der Ansicht, um den Browser-RAM bei hoher Bus-Last zu entlasten.

### Zweistufiger Firmware-Update-Prozess (`http://<esp32-ip>/update`)
Um unvollständige Flash-Vorgänge oder hängende Browser-Anfragen nach einem Reboot zu verhindern, nutzt der Webserver eine ausgeklügelte dreistufige Logik beim Einspielen einer neuen `.bin`-Datei:
1. **Browser-Beruhigung:** Nach erfolgreichem Empfang der Firmware sendet der ESP32 sofort ein sauberes HTML-Dokument mit einem HTTP-Refresh-Header (`content='5;url=/'`) an den Client zurück.
2. **Verbindungs-Abbau:** Der ESP32 wartet kurz per `delay(500)` und schließt die TCP-Client-Verbindung mittels `server.client().stop()` aktiv von sich aus. Dadurch weiß der Browser, dass die Transaktion beendet ist.
3. **Hard Reboot:** Erst nachdem die Netzwerk-Pakete sicher den Netzwerkstack verlassen haben, triggert das Gateway über `ESP.restart()` den kontrollierten Neustart der Hardware.

---

## ⏱️ Zeitkritische Logik & Core-1 Bus Task

Die Kommunikation auf dem HESP-Bus erfordert exakte zeitliche Koordination. Der Sende-Task auf Core 1 arbeitet autark in einer permanenten Schleife (`for(;;)`):

```text
Bus-Aktivität (Master/Slave) ===[ DATA FRAME ]===|
                                                 |
                                                 v
                                        Stille detektiert (>10ms Initial / >12ms PACKET_TIMEOUT)
                                                 |
                                                 v
                                        Sende-Fenster öffnet (z.B. +35ms / sendTimeBegin)
                                                 |---> [COLLISION / VETO CHECK via digitalRead]
                                                 |---> [EIGENES PARAMETER-SET INJIZIEREN]
                                                 v
                                        Sende-Fenster schließt (z.B. +50ms / sendTimeEnd)
```

### Der Bus-Zyklus im Detail

1. **Schritt 1: Starr Bus-Empfang & Zyklus-Erkennung**
   Der Task überwacht `Serial2`. Erkennt er nach einer Bus-Ruhepause von mehr als `10 ms` das erste eintreffende Byte, speichert er den exakten Zeitstempel in `cycleStartTime` und schaltet den Sende-Timer scharf (`timerArmed = true`).
2. **Schritt 2: Normales Sammel-Ende**
   Verstreichen nach dem letzten Byte mehr als `PACKET_TIMEOUT` (12 ms) oder überschreitet die Zyklusdauer `95 ms`, gilt das Telegramm als beendet. Es wird atomar in den Ringpuffer `hespBinQueue` kopiert und für den MQTT-Versand auf Core 0 freigegeben.
3. **Schritt 3: Kollisionsschutz & Veto-Logik**
   Bevor das Gateway im Zeitfenster (`sendTimeBegin` bis `sendTimeEnd`) Daten sendet, wird ein hardwarenaher **Belegungs-Check** durchgeführt:
   ```cpp
   if (digitalRead(RX2_PIN) == LOW || Serial2.available() > 0)
   ```
   Zieht eine andere Komponente (z.B. die Anlage für eine "lange Antwort") den RX-Pin genau in diesem Moment auf `LOW` (Startbit) oder befinden sich bereits ungelesene Bytes im Puffer, legt das Gateway ein **Veto** ein. Das Senden wird für diesen 100ms-Takt sofort blockiert, um Bus-Kollisionen mathematisch auszuschließen.
4. **Schritt 4: Injektion & Echo-Löschung**
   Ist der Bus frei, wird der Puffer über `Serial2.write()` physisch übertragen. Nach einem `Serial2.flush()` wartet das System `300 Mikrosekunden` auf die Richtungsumschaltung des RS485-Transceivers und löscht über eine `while`-Schleife die lokalen Echos (*Tx-Self-Hear*), damit sich das Gateway nicht selbst snifft.
5. **Schritt 5: Sub-Bus-Antwortmodus**
   Nach dem Senden schaltet das System in den Modus `warteAufAntwort = true`. Eintreffende Bytes werden nun isoliert im separaten `rxSubBuffer` gesammelt, um die direkte Antwort der Anlage präzise herauszufiltern.
6. **Schritt 6: Automatische Timer-Abschaltung**
   Handelt es sich bei die Injektion um einen `SET`-Befehl (`txType == 1`), prüft das System die abgelaufene Zeit seit dem ersten Senden. Nach Erreichen der `SET_REPEAT_TIME` (z.B. 1500 ms) wird die Injektions-Schleife vollautomatisch deaktiviert, um den Bus wieder freizugeben.

---

## 📈 Status- und Performance-Metriken

Jede minute (`60000 ms`) friert Core 0 die Zähler kurzzeitig ein, berechnet wichtige Systemmetriken und gibt eine strukturierte Diagnose im USB-Terminal (`Serial0`) aus. Dies dient der langfristigen Stabilitätskontrolle im Feld:

```text
========================================
   HESP-BUS STATISTIK (Letzte Minute)
----------------------------------------
 Gelesene Nachrichten (RX): 602
 Gesendete Nachrichten MQTT: 602
 Geschriebene Nachrichten (TX): 4
 Gesendete Antworten (RX): 4
 Bus-Frequenz gesamt:        ~10.10 Hz
 Bus-Qualität gesamt:        ~100.00 %
========================================
```

* **Bus-Frequenz:** Errechnet die durchschnittliche Paketanzahl pro Sekunde (erwartet sind ca. 10 bis 20 Hz, je nach Anlagenkonfiguration).
* **Bus-Qualität:** Setzt die erfolgreich per MQTT abgesetzten Pakete in Relation zu den gelesenen Telegrammen. Ein Wert von 100 % signalisiert eine fehlerfreie, latenzfreie Verarbeitung auf Core 0 ohne Queue-Überläufe.

---

## 🚀 Inbetriebnahme & Troubleshooting

1. **Kompilierung**:
   
     Stelle in der Arduino IDE sicher, dass das ESP32 Dev Module ausgewählt ist und der serielle Monitor auf 115200 Baud steht.
   
3. **Diagnose beim Booten**:
   
   Achte auf die Meldung >>> ERFOLG: RS485-Task erfolgreich auf Core 1 gestartet! <<<.
   Erscheint stattdessen ein Fehlercode, war der Heap-Speicher für den 8KB Task-Stack unzureichend.
   
5. **Fehlersuche bei fehlendem Traffic**:
   
     1. Falls das Live-Log im Webinterface oder Terminal komplett leer bleibt, tausche die A- und B-Leitungen des RS485-Moduls (häufigste Ursache bei RS485-Inbetriebnahmen).
     2. Überprüfe mit einem Multimeter, ob der GND des ESP32 eine stabile, niederohmige Verbindung zum GND der Proxon-Platine (X6-1) aufweist.
   Ohne gemeinsamen GND führen Gleichtaktstörungen zu korrupten Datenpaketen.


## Referenzen 

1. Das gesamte Projekt basiert auf der Arbeit von Markus Mauch und seinen Beschreibungen unter:

- [Markus Mauch's HESP documentation](https://markusmauch.github.io/proxon-hesp/)
- [Documentation repository](https://github.com/markusmauch/proxon-hesp)

2. Der Text und der Code wurden zu ca 95% von Google erstellt


## Geltungsbereich

Alle Angaben stammen aus Messungen an einer einzelnen Anlage, erhoben ohne Beteiligung
des Herstellers. Nachbau auf eigene Verantwortung; Eingriffe können Gewährleistung oder
Garantie berühren. Es werden keine geschützten Hersteller-Unterlagen wiedergegeben.

## Lizenz & Marken

Inhalte unter [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/deed.de) (siehe
[`LICENSE`](LICENSE)). Unabhängiges Projekt, **nicht** mit Zimmermann Lüftungs- und
Wärmesysteme GmbH & Co. KG affiliiert; „PROXON", „Zimmermann" und weitere genannte Namen
sind Marken ihrer jeweiligen Inhaber und werden beschreibend verwendet.
