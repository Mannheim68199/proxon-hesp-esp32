/*

    Modul:  ESP32 DEV Module
            RS485 2 TTL Modul:  Amazon: RS485 to TTL module für 3,3 - 5 V!  automatische Umschaltung Lesen/Schreiben  rote Platine

    Anschlüsse:   ESP32       RS485 /            Proxon  X6
                  Pin17       TXD  /  D-/A       braunes Kabel / A     X6-4
                  Pin16       RXD  /  D+/B       blaues Kabel  / B     X6-3
                  3.3V        VCC                              / +12V  X6-2
                  GND         GND   /  GND       weisses Kabel / GND   X6-1

*/
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

// --- KONFIGURATION ---
const char* ssid = "DEINE_SSID";
const char* password = "DEIN_WLAN_PASSWORT";
const char* mqtt_server = "DEINE_MQTT_SERVER_ID";
const int   mqtt_port = 1883;
const char* topic_raw = "rs485/sniffer/raw";   // Hierhin sendet der D1 mini
const char* topic_send = "rs485/sniffer/send"; // Hierauf hört der D1 mini (HEX-String senden)
const char* topic_response = "rs485/sniffer/response";   // Hierhin sendet der D1 mini  Antworten auf SET oder Query

// ESP32 Standard-Pins für UART2
#define RX2_PIN 22
#define TX2_PIN 23

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// NTP-Server für die Uhrzeit
const char* ntpServer = "pool.ntp.org";
// Zeitzonen-String für Deutschland (inklusive automatischer Sommer-/Winterzeit)
// CET = Central European Time, CEST = Central European Summer Time
const char* timeZone = "CET-1CEST,M3.5.0,M10.5.0/3";


// Ringpuffer für die MQTT-Entkopplung (erhöht auf 8 Frames bei 20Hz)
// === ULTRA-SCHNELLE BINÄRE QUEUE ===
#define QUEUE_SIZE 31
#define BUFFER_SIZE 128
#define MAX_FRAME_LEN 256

struct BinaryFrame {
  byte data[MAX_FRAME_LEN];
  int frameSize;
};

BinaryFrame hespBinQueue[QUEUE_SIZE];
BinaryFrame hespRespQueue[QUEUE_SIZE];

// Vorwärtsdeklaration
void rs485SnifferTask(void * parameter);
void logMsg(String msg, bool newLine = false);
void logMsg(const char* msg, bool newLine = false);
void handleWebLogQueue(); // Neuer Abholer für Core 0

// Ein separater, kleiner Ringpuffer NUR für Texte zur Webseite
const int LOG_QUEUE_SIZE = 60;
String webLogQueue[LOG_QUEUE_SIZE];
volatile int logIn = 0;
volatile int logOut = 0;
volatile bool logToWeb = false;


volatile int qIn = 0;
volatile int qOut = 0;
volatile int qRespIn = 0;
volatile int qRespOut = 0;
unsigned long injektionStartZeit = 0; // Merkt sich, wann das Dauerfeuer für das SET begann
bool injektionLaeuft = false;         // Status, ob die Stoppuhr gerade aktiv ist

// Sende-Variablen
byte txBuffer[BUFFER_SIZE];
unsigned int txLength = 0;
volatile bool readyToSend = false;

// lässt sich über MQTT send ändern
unsigned int sendTimeBegin = 40;  // 35 msec - 50 msec time window to send own message
unsigned int sendTimeEnd = 55;
unsigned int SET_REPEAT_TIME = 1800;  // Definiert die Zeit, die ein SET Befehl wiederholt werden soll

unsigned int msgReadCounter = 0;               // Anzahl der Msg pro Minute;
unsigned int msgWriteCounter = 0;              // Anzahl der Msg pro Minute;
unsigned int msgSendCounter = 0;               // Anzahl der Msg pro Minute;
unsigned int msgAnswerCounter = 0;             // Anzahl der eigenen Antworten pro Minute;
unsigned long lastStatTime = 0;                // Zeit in msec zum Zählen der Msg pro Minute;

const unsigned long PACKET_TIMEOUT = 12;    // 12ms Stille signalisiert das Zyklus-Ende
const unsigned int  LONG_ANSWER_MIN = 64;   // Mindestlänge für eine "lange Antwort"

// Task-Handle für den RS485-Task auf Core 1
TaskHandle_t RS485TaskHandle = NULL;


const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>HESP Bus Monitor</title>
  <style>
    body { font-family: monospace; background: #121212; color: #00ff00; padding: 20px; }
    #log { width: 100%; height: 500px; background: #000; border: 1px solid #333; overflow-y: scroll; padding: 10px; white-space: pre-wrap; }
    button { background: #333; color: #fff; padding: 10px; border: none; cursor: pointer; }
    .active { background: #00ff00; color: #000; }
  </style>
</head>
<body>
  <h2>HESP RS485 Bus Live-Log</h2>
  <button id="toggleBtn" onclick="toggleLog()" class="active">Log: AN</button>
  <button onclick="clearLog()">Loeschen</button>
  <br><br>
  <div id="log"></div>

  <script>
    var ws = new WebSocket('ws://' + window.location.hostname + ':81/');
    var logDiv = document.getElementById('log');
    var loggingActive = true;

    ws.onmessage = function(event) {
      if(loggingActive) {
        logDiv.innerText += event.data;
        logDiv.scrollTop = logDiv.scrollHeight; // Auto-Scroll
      }
    };

    function toggleLog() {
      loggingActive = !loggingActive;
      var btn = document.getElementById('toggleBtn');
      if(loggingActive) {
        btn.innerText = "Log: AN";
        btn.className = "active";
        ws.send("LOG_ON");
      } else {
        btn.innerText = "Log: AUS";
        btn.className = "";
        ws.send("LOG_OFF");
      }
    }
    function clearLog() { logDiv.innerText = ""; }
  </script>
</body>
</html>
)rawliteral";


// HTML-Code für die Update-Seite
const char update_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 HESP Firmware Update</title>
  <style>
    body { font-family: sans-serif; background: #1a1a1a; color: #fff; text-align: center; padding: 50px; }
    .box { background: #2a2a2a; padding: 30px; border-radius: 10px; display: inline-block; box-shadow: 0 4px 10px rgba(0,0,0,0.5); }
    input[type=file] { margin: 20px 0; background: #333; padding: 10px; border-radius: 5px; color: #fff; }
    input[type=submit] { background: #00ff00; color: #000; font-weight: bold; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; }
    #prg { margin-top: 20px; font-weight: bold; color: #00ff00; }
  </style>
</head>
<body>
  <div class="box">
    <h2>HESP Gateway - Firmware Update (.bin)</h2>
    <form method='POST' action='/update' enctype='multipart/form-data' id='upload_form'>
      <input type='file' name='update' accept='.bin'><br>
      <input type='submit' value='Update starten'>
    </form>
    <div id='prg'>Bereit</div>
  </div>
  <script>
    document.getElementById('upload_form').onsubmit = function() {
      document.getElementById('prg').innerText = "Uebertrage Firmware... Bitte warten, Geraet startet gleich neu!";
    };
  </script>
</body>
</html>
)rawliteral";


// Diese Funktion wird aufgerufen, wenn eine MQTT-Nachricht für den D1 mini ankommt
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // leere Nachrichten gleich abfangen
  if (length == 0) return;
  
  // Falls die loop() noch sendet, neuen Befehl verwerfen
  if (readyToSend) return; 

  // Ein JSON beginnt typischerweise mit '{' (Dezimalwert 123 im ASCII-Code)
  if (payload[0] == '{') {
    
    // 1. JSON-VERARBEITUNG
    // Dynamisches Dokument erstellen (Größe an deinen JSON anpassen)
    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
      logMsg("JSON-Fehler: "); logMsg(error.f_str(), true);
      return; // Fehler beim Parsen -> Abbrechen
    }

    logMsg(" ",true);
    logMsg(" Parameteraenderungen: ",true);

    // Beispiel: Internen Parameter "sendTimeBegin" auslesen, falls im JSON enthalten
    if (doc.containsKey("sendTimeBegin")) {
      unsigned int newVal = doc["sendTimeBegin"];
      sendTimeBegin = newVal;
      logMsg("sendTimeBegin per JSON geändert: "); logMsg(String(newVal), true);
    }
    if (doc.containsKey("sendTimeEnd")) {
      unsigned int newVal = doc["sendTimeEnd"];
      sendTimeEnd = newVal;
      logMsg("sendTimeEnd per JSON geändert: "); logMsg(String(newVal), true);
    }

    if (doc.containsKey("SET_REPEAT_TIME")) {
      unsigned int newVal = doc["SET_REPEAT_TIME"];
      SET_REPEAT_TIME = newVal;
      logMsg("SET_REPEAT_TIME per JSON geändert: "); logMsg(String(newVal), true);
    }

    if (doc.containsKey("setDefaultValues")) {
      sendTimeBegin = 35;
      sendTimeEnd = 50;
      SET_REPEAT_TIME = 1500;
      logMsg("defaultWert per JSON geändert: sendTimeBegin=35, sendTimeEnd=55, SET_REPEAT_TIME=1500", true);
    }
  } else {
    txLength = 0;
    // HEX-String in echte Bytes wandeln und im Puffer sichern
    for (unsigned int i = 0; i < length; i += 2) {
      if (i + 1 < length && txLength < sizeof(txBuffer)) {
        byte high = hexCharToByte((char)payload[i]);
        byte low = hexCharToByte((char)payload[i+1]);
        txBuffer[txLength++] = (high << 4) | low;
      }
    }
  
    // Signal an die loop() senden, dass gesendet werden soll
    readyToSend = true;   // Signal an Core 1: Bitte in der nächsten Pause senden!
  }
}


void setup() {

  // Behebt den "GPIO 2 is reserved" Bug in Espressif Core 3.1+ und schaltet das WLAN frei
  WiFi.mode(WIFI_STA);

  WiFi.setHostname("ESP32_Proxon");
  
  // UART0 für den PC-Monitor (USB-Kabel) starten
  Serial.begin(115200);

  // Warte, bis der serielle Port wirklich bereit ist
  while (!Serial) { delay(10);}

  // WICHTIG: Warte 1,5 Sekunden, damit das Linux-Terminal Zeit zum Verbinden hat!
  delay(1500); 

  setup_wifi();
  setupWebServer();
  
  logMsg("\n--- DIAGNOSE-START ---", true);
  logMsg("Schritt 1: Hardware-Serial für PC steht.", true);
  
  // UART2 für die Heizung starten (Echte Hardware-Pins 16 und 17)
  Serial2.begin(19200, SERIAL_8N1, RX2_PIN, TX2_PIN);
  logMsg("Schritt 2: Serial2 (Pins 22/23) erfolgreich initialisiert.", true);
  
  // GANZ AM ENDE VON setup(): Jetzt ist das System stabil und bereit für das Web-Log!
  logToWeb = true; 

  
  // MQTT konfigurieren
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback); // Funktion für eingehende MQTT-Mails verknüpfen
  // ERZWINGEN: Setzt den internen MQTT-Sende-Puffer auf 1024 Bytes hoch!
  mqttClient.setBufferSize(2048); 
  logMsg("Schritt 5: MQTT-Server konfiguriert.", true);

  // Jetzt versuchen wir den Task auf Core 1 zu starten
  logMsg("Schritt 6: Versuche Core 1 Task zu erstellen...", true);
  
  // === DER DUAL-CORE TRICK ===
  // Wir erstellen einen eigenen Thread (Task) und fesseln ihn an CORE 1.
  // WLAN und MQTT nutzen standardmäßig CORE 0. 
  
  BaseType_t taskResult = xTaskCreatePinnedToCore(
    rs485SnifferTask,   // Funktion
    "RS485Task",        // Name
    8192,               // Stack-Größe auf 8KB (Sicherheit vor Crash!)
    NULL,               // Parameter
    2,                  // Priorität
    &RS485TaskHandle,   // Handle
    1                   // Core 1
  );

  if (taskResult == pdPASS) {
    logMsg(">>> ERFOLG: RS485-Task erfolgreich auf Core 1 gestartet! <<<", true);
  } else {
    logMsg("[!] KRITISCHER FEHLER: Task-Erstellung fehlgeschlagen! Fehlercode: "); logMsg(String(taskResult), true);
  }

  logMsg("--- DIAGNOSE-ENDE (setup abgeschlossen) ---\n", true);
}

void setupWebServer() {
  
  // --- HAUPTSEITE (LIVE-LOG) ---
  server.on("/", []() {
    server.send(200, "text/html", index_html);
  });

  // --- OTA-UPDATE INTERFACE ANZEIGEN ---
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html", update_html);
  });

  // --- OTA-UPDATE VERARBEITUNG (DATEI-UPLOAD) ---
  server.on("/update", HTTP_POST, []() {
    //server.sendHeader("Connection", "close");
    // 1. Dem Browser eine saubere Antwort senden, die ihn zurück zur Hauptseite schickt
    // Das verhindert, dass der Browser versucht, den POST-Upload nach dem Reboot zu wiederholen!
    String response = "<html><head><meta http-equiv='refresh' content='5;url=/'></head>";
    response += "<body style='font-family:sans-serif; background:#1a1a1a; color:#fff; text-align:center; padding-top:50px;'>";
    response += "<h2>Update erfolgreich!</h2><p>Ger&auml;t startet neu... Webseite schliessen!</p></body></html>";
    
    server.send(200, "text/plain", response);

    // 2. WICHTIG: Die Netzwerkverbindung aktiv schließen, damit die Datenpakete sicher raus sind
    delay(500);
    server.client().stop();
    delay(500);
    
    // 3. Erst jetzt, wenn der Browser beruhigt ist, hart neu starten
    ESP.restart(); // Nach erfolgreichem Upload neu starten
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      logMsg("Update gestartet. Datei: " + String(upload.filename.c_str()) );
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Startet den Flash-Vorgang
        logMsg("Update Fehler initial: " + String(Update.errorString()) );
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        logMsg("Update Write Error: " + String(Update.errorString() ) );
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { // true = Setzt die Größe auf die geschriebenen Bytes
        logMsg("Update Erfolg: übertragene Größe: " + String(upload.totalSize) + " Bytes." );
        logMsg("ESP32 führt jetzt einen automatischen Neustart durch...", true);
      } else {
        logMsg("Update Abschluss-Fehler: " + String(Update.errorString()), true);
      }
    }
  });
  
  server.begin();
  webSocket.begin();
  
  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    if(type == WStype_TEXT) {
      String msg = String((char*)payload);
      if(msg == "LOG_ON")  logToWeb = true;
      if(msg == "LOG_OFF") logToWeb = false;
    }
  });

  logMsg("Schritt 0:  WebServer aktivieren", true);
}

// =========================================================================
// CORE 0 ARBEITET DIE STANDARD-LOOP AB (WLAN, MQTT-Verbindung & MQTT-Senden)
// =========================================================================
void loop() {
  server.handleClient();
  webSocket.loop();
  
  handleWebLogQueue();   // FUNKT DIE LOGS NUN SICHER AUF CORE 0 INS WLAN!

  
  if (!mqttClient.connected()) {
    reconnect();
  }
  mqttClient.loop();


  // Wenn der RS485-Task auf Core 1 fertige Frames in der Queue abgelegt hat,
  // werden diese hier von Core 0 stressfrei ins WLAN gefunkt.
  if (qIn != qOut) {
    String hexPayload = "";
    int currentLen = hespBinQueue[qOut].frameSize;

    if (currentLen > 0 && currentLen <= MAX_FRAME_LEN) {
      // String-Puffer vorab reservieren, das beschleunigt den ESP32 massiv!
      hexPayload.reserve(currentLen * 2 + 2); 

      for (int i = 0; i < currentLen; i++) {
        if (hespBinQueue[qOut].data[i] < 0x10) hexPayload += "0";
        hexPayload += String(hespBinQueue[qOut].data[i], HEX);
      }
    
      bool publishSuccess = mqttClient.publish(topic_raw, hexPayload.c_str());
      
      if (!publishSuccess) {
        logMsg("[!] MQTT-FEHLER: Konnte langes Paket nicht senden! String-Länge: ");
        logMsg(String(hexPayload.length()));
        logMsg(" Zeichen. (Puffer im setup() zu klein?)", true);
      } else {
        msgSendCounter++;
      }
    }
    qOut = (qOut + 1) % QUEUE_SIZE;
  }
  if (qRespIn != qRespOut) {
    String hexPayload = "";
    int currentLen = hespRespQueue[qRespOut].frameSize;

    if (currentLen > 0 && currentLen <= MAX_FRAME_LEN) {
      // String-Puffer vorab reservieren, das beschleunigt den ESP32 massiv!
      hexPayload.reserve(currentLen * 2 + 2); 

      for (int i = 0; i < currentLen; i++) {
        if (hespRespQueue[qRespOut].data[i] < 0x10) hexPayload += "0";
        hexPayload += String(hespRespQueue[qRespOut].data[i], HEX);
      }
    
      bool publishSuccess = mqttClient.publish(topic_response, hexPayload.c_str());
      
      if (!publishSuccess) {
        logMsg("[!] MQTT-FEHLER Antwort: Konnte langes Paket nicht senden! String-Länge: ");
        logMsg(String(hexPayload.length()));
        logMsg(" Zeichen. (Puffer im setup() zu klein?)", true);
      } else {
        msgSendCounter++;
      }
    }
    qRespOut = (qRespOut + 1) % QUEUE_SIZE;
  }

  // =======================================================================
  // STATISTIK-AUSGABE (ALLE 60 SEKUNDEN IM TERMINAL)
  // =======================================================================
  if (millis() - lastStatTime >= 60000) {
    // Sicheres Auslesen und Zurücksetzen (Zähler kurz einfrieren)
    unsigned long currentRead = msgReadCounter;
    unsigned long currentWrite = msgWriteCounter;
    unsigned long currentSend = msgSendCounter;
    unsigned long currentAnswer = msgAnswerCounter;
    
    msgReadCounter = 0;
    msgWriteCounter = 0;
    msgSendCounter = 0;
    msgAnswerCounter = 0;
    lastStatTime = millis();

    // Ausgabe im seriellen Monitor (USB-Terminal)
    logMsg("\n========================================", true);
    //printLocalTime();
    logMsg("   HESP-BUS STATISTIK (Letzte Minute)\n");
    logMsg("----------------------------------------", true);
    logMsg(" Gelesene Nachrichten (RX): "); logMsg(String(currentRead), true);
    logMsg(" Gesendete Nachrichten MQTT: "); logMsg(String(currentSend), true);
    logMsg(" Geschriebene Nachrichten (TX): "); logMsg(String(currentWrite), true);
    logMsg(" Gesendete Antworten (RX): "); logMsg(String(currentAnswer), true);
    logMsg(" Bus-Frequenz gesamt:        ~"); logMsg(String((currentRead + currentWrite) / 60.0)); logMsg(" Hz", true);
    logMsg(" Bus-Qualität gesamt:        ~"); logMsg(String((currentSend / currentRead) * 100.0)); logMsg(" %", true);
    logMsg("========================================\n", true);
  }
 
  delay(1); // Gibt dem Betriebssystem Zeit für Hintergrundprozesse auf Core 0
}


// =========================================================================
// CORE 1 KÜMMERT SICH NUR UM DEN BUS (Absolut immun gegen WLAN-Verzögerungen)
// =========================================================================
void rs485SnifferTask(void * parameter) {
  const int LOCAL_BUFFER_SIZE = 256;
  byte rxBuffer[LOCAL_BUFFER_SIZE];
  int bufferIndex = 0;
  unsigned long lastCharTime = 0;
  
  // Variablen für die zeitgesteuerte 60ms-Injektion
  unsigned long cycleStartTime = 0;
  bool timerArmed = false;
  bool bereitsGesendetOderVetoInDiesemZyklus = false; // Verhindert Dauer-Trigger im selben Takt


  // Variablen für das lesen der Antwort auf mein SET
  const int SUB_BUFFER_SIZE = 64;
  byte rxSubBuffer[SUB_BUFFER_SIZE];
  int subBufferIndex = 0;
  unsigned long subLastCharTime = 0;
  bool warteAufAntwort = false; // Neuer Status-Flag für die Antwort-Uhr
 
  memset(rxBuffer, 0, sizeof(rxBuffer));

  for (;;) {
    // =======================================================================
    // SCHRITT 1: REINER, STARRER BUS-EMPFANG
    // =======================================================================
    while (Serial2.available()  && !warteAufAntwort) {
      byte incomingByte = Serial2.read();
      
      // Wenn der Bus vorher still war (>10ms), beginnt ein neuer 100ms-Zyklus!
      if (bufferIndex == 0 && (millis() - lastCharTime > 10)) {
        cycleStartTime = millis(); // Startzeitpunkt des 100ms-Takts merken
        timerArmed = true;         // Sende-Timer scharf schalten
        bereitsGesendetOderVetoInDiesemZyklus = false; // Neuen Zyklus freigeben
      }

      if (bufferIndex < LOCAL_BUFFER_SIZE) {
        rxBuffer[bufferIndex] = incomingByte;
        bufferIndex++;
      }
      lastCharTime = millis();
    }

    // =======================================================================
    // SCHRITT 2: SUB-BUS-EMPFANG (Wenn wir auf die Antwort unseres SETs warten)
    // =======================================================================
    while (Serial2.available() && warteAufAntwort) {
      byte incomingByte = Serial2.read();
      
      if (subBufferIndex < SUB_BUFFER_SIZE) {
        rxSubBuffer[subBufferIndex] = incomingByte;
        subBufferIndex++;
      }
      subLastCharTime = millis();
    }
    
    // =======================================================================
    // SCHRITT 3: NORMALES SAMMEL-ENDE (Gesteuert durch das PACKET_TIMEOUT)
    // =======================================================================
    // Ein Timeout von 12ms trifft exakt die 20ms-Lücke am Ende des großen Pakets 
    // und trennt es perfekt ab, bevor nach 100ms der nächste Zyklus startet!
    if (!warteAufAntwort && bufferIndex > 0 && ( (millis() - lastCharTime > PACKET_TIMEOUT) || (millis() - cycleStartTime > 95 )) ) {

      // Kopieren in die Queue für Core 0 (MQTT)
      int nextIn = (qIn + 1) % QUEUE_SIZE;
      if (nextIn != qOut && bufferIndex <= LOCAL_BUFFER_SIZE) {
        
        memcpy(hespBinQueue[qIn].data, rxBuffer, bufferIndex);
        hespBinQueue[qIn].frameSize = bufferIndex;
        
        qIn = nextIn;
        msgReadCounter++;
      }
      
      // Zustand für den normalen Hauptpuffer nullen
      bufferIndex = 0;    
    }

    // =======================================================================
    // SCHRITT 4: SENDEN im Zeitfenster von sendTimeBegin bis sendTimeEnd ( 35-50 msec ) NACH ZYKLUS-START
    // =======================================================================
    if (timerArmed && !bereitsGesendetOderVetoInDiesemZyklus && (millis() - cycleStartTime >= sendTimeBegin)  && (millis() - cycleStartTime < sendTimeEnd)) {
      
      if (readyToSend) {
        // Typ bestimmen (0x1 = SET, 0x0 = QUERY)
        byte txType = txBuffer[0] & 0x0F;

        // ZEITSTEUERUNG-CHECK: Wenn das SET bereits seit mehr als 1500 ms feuert, jetzt stoppen!
        if (txType == 1 && injektionLaeuft && (millis() - injektionStartZeit >= SET_REPEAT_TIME)) {
          readyToSend = false;
          injektionLaeuft = false;
          logMsg("\n[Timer] ");
          logMsg(String(SET_REPEAT_TIME / 1000));
          logMsg(" Sekunden erreicht! SET-Injektion automatisch beendet.", true);
        } 
        // Falls das Smart Home mitten im Feuern ein NEUES SET schickt (bevor die Zeit um war)
        else if (txType == 1 && !injektionLaeuft && bereitsGesendetOderVetoInDiesemZyklus == false) {
          // Neuer Befehl reingekommen -> Stoppuhr für diesen neuen Befehl frisch starten
          injektionStartZeit = millis();
          injektionLaeuft = true;
        }
        
        if (readyToSend) {
          // DER ECHTE BELEGUNGS-CHECK: 
          // Wir legen NUR ein Veto ein, wenn AKTUELL wieder Bytes reinkommen (die lange Antwort läuft bereits)
          // Ein RS485-Startbit zieht den RX-Pin auf LOW (0). 
          // Wenn der Pin LOW ist ODER sich doch schon ein Byte in die UART verirrt hat:
          if (digitalRead(RX2_PIN) == LOW || Serial2.available() > 0) {
            
            logMsg(">> VETO: Senden blockiert – Dieser Zyklus gehört der LANGEN Antwort!", true);
            bereitsGesendetOderVetoInDiesemZyklus = true; // Sperren für diesen 100ms-Takt
   
          } else {  // BUs ist frei -> Senden!
           
            if (txType == 1 ) {
              logMsg("[Injektion SET] ");
            } else {
              logMsg("[Sende QUERY] ");
            }
            int iSendeZeit = millis() - cycleStartTime;
            logMsg(String(iSendeZeit)); logMsg(" msec ");
  
            // Befehl über Hardware-UART2 ausgeben
            for (unsigned int i = 0; i < txLength; i++) {
              Serial2.write(txBuffer[i]);
            }
            Serial2.flush(); // Warten bis physikalisch komplett gesendet
            
            delayMicroseconds(300); // Richtungsumschaltung des Transceivers abwarten
            while (Serial2.available() > 0) { Serial2.read(); } // Lokale Echos (Tx-Self-Hear) sauber löschen
            
            // Sub-Lesezyklus initialisieren
            subBufferIndex = 0;
            subLastCharTime = millis();
            warteAufAntwort = true; // Dem Sniffer sagen: "Ab jetzt alles in den rxSubBuffer umleiten!"
            
            // =======================================================================
            // UNTERSCHIEDLICHE ABSCHALT-LOGIK NACH DEM SENDEN:
            // =======================================================================
            if (txType == 1) {
              // Es ist ein SET: Falls die Stoppuhr noch nicht läuft, jetzt beim ersten geglückten Senden starten!
              if (!injektionLaeuft) {
                injektionStartZeit = millis();
                injektionLaeuft = true;
                logMsg("(Timer gestartet) ");
              }
              // readyToSend bleibt TRUE -> Es wird im nächsten Zyklus wiederholt
            } else {
              // Es ist ein QUERY: Sofort nach dem ersten Schuss abschalten
              readyToSend = false; 
            }
            msgWriteCounter++;
            bereitsGesendetOderVetoInDiesemZyklus = true; // Sperren für diesen Takt
          }
        } else {
          // Wenn kein Sende-Wunsch vorliegt, ignorieren wir den Rest des Zyklus
          bereitsGesendetOderVetoInDiesemZyklus = true;
        }
      }
    }

    // =======================================================================
    // SCHRITT 5 & 6: AUSWERTUNG DES SUB-LESEZYKLUS (Antwort-Timeout)
    // =======================================================================
    if (warteAufAntwort) {
      // Kriterien für das Ende der Antwort:
      // A: Es kamen Daten und nun ist seit 4ms Ruhe (Antwort vollständig erhalten)
      // B: Es kamen GAR KEINE Daten und der Bus steht seit 15ms still (Gegenstelle antwortet nicht)
      bool antwortErfolgreich = (subBufferIndex > 0 && (millis() - subLastCharTime > 4));
      bool antwortTimeout     = (subBufferIndex == 0 && (millis() - subLastCharTime > 20));
      
      if (antwortErfolgreich || antwortTimeout || (millis() - cycleStartTime > 95)) {
        
        if (subBufferIndex > 0) {
          // In die Queue für Core 0 (MQTT / Parser) schieben
          int nextIn = (qRespIn + 1) % QUEUE_SIZE;
          if (nextIn != qRespOut && subBufferIndex <= SUB_BUFFER_SIZE) {
            memcpy(hespRespQueue[qRespIn].data, rxSubBuffer, subBufferIndex);
            hespRespQueue[qRespIn].frameSize = subBufferIndex;
            qRespIn = nextIn;
            msgAnswerCounter++;
          }
         
          // Typ-Bestimmung über das Low-Nibble des Headers (rxSubBuffer[0])
          byte typeNibble = rxSubBuffer[0] & 0x0F;
          uint16_t DP = (rxSubBuffer[4] << 8) | rxSubBuffer[3];
          char hexDP[5];
          sprintf(hexDP, "%04X", DP);
          logMsg(" Antwort: ("); logMsg(String(subBufferIndex)); logMsg(" Bytes): "); logMsg(hexDP); logMsg(" ");
  
          switch(typeNibble) {
              case 0: logMsg("QUERY (0x0)", true); break;
              case 1: logMsg("SET (0x1)", true); break;
              case 2: logMsg("RESPONSE (0x2)", true); break;
              case 3: logMsg("CONFIRM (0x3)", true); break;
              default: 
                logMsg("UNBEKANNT (0x"); 
                logMsg(String(typeNibble), HEX); 
                logMsg(")", true); 
                break;
            }
          } else {
            logMsg(" -> Antwort: TIMEOUT (Keine Gegenstelle reagiert)", true);
          }

          // Sub-Lesezyklus sauber beenden und Hauptbus wieder freigeben
          subBufferIndex = 0;
          warteAufAntwort = false;
          lastCharTime = millis(); // Haupt-Zeitstempel aktualisieren, um Fehltrigger im Haupt-Schnitt zu verhindern
        }
      }

      // Sicherheitsnetz (Falls der Puffer aus irgendeinem Grund vollzulaufen droht)
      if (bufferIndex >= LOCAL_BUFFER_SIZE - 5) {
        bufferIndex = 0;
        timerArmed = false;
      }

      vTaskDelay(pdMS_TO_TICKS(1)); // FreeRTOS-konformer Yield
    }
}

// Hilfsfunktion: Wandelt ein einzelnes Hex-Zeichen in eine Zahl um
byte hexCharToByte(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

void setup_wifi() {
  delay(1);
  logMsg("Schritt 3: Verbinde mit WLAN (Kiwi)...");
  WiFi.begin(ssid, password);

  // Ein Timeout einbauen, falls das WLAN blockiert, damit der Code nicht ewig hängt
  unsigned long wifiTimeout = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    logMsg(".");
    if (millis() - wifiTimeout > 10000) { // Nach 10 Sekunden abbrechen
      logMsg("\n[!] FEHLER: WLAN-Verbindung fehlgeschlagen (Timeout)!", true);
      break;
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    logMsg("\nSchritt 4: WLAN erfolgreich verbunden!", true);
  }
}

void reconnect() {
    while (!mqttClient.connected()) {
      logMsg("Versuche MQTT Verbindung... ");
      String clientId = "ESP32HespGateway-";
      clientId += String(random(0xffff), HEX);
      if (mqttClient.connect(clientId.c_str())) {
        mqttClient.subscribe(topic_send);
        logMsg("Erfolgreich verbunden & abonniert.", true);
      } else { 
        logMsg("Fehlgeschlagen. Erneuter Versuch in 2 Sek.", true);
        delay(2000); 
      }
    }
}

void logMsg(String msg, bool newLine) {
  if (!logToWeb) return; // Sofort abbrechen, wenn Logging AUS ist

  // 1. Ausgabe am USB-Terminal
  if (newLine) Serial.println(msg);
  else Serial.print(msg);

  // 2. Sicherheitsnetz: Wenn das Web-Logging noch nicht bereit oder abgeschaltet ist, hier abbrechen
  if (!logToWeb) return; 

  // 3. Text sicher in die Text-Queue legen (Dauert nur Mikrosekunden, blockiert Core 1 nicht)
  int nextLogIn = (logIn + 1) % LOG_QUEUE_SIZE;
  if (nextLogIn != logOut) {
    webLogQueue[logIn] = msg + (newLine ? "\n" : "");
    logIn = nextLogIn;
  }
}


// Variante 2: Für C-Strings / Text in Anführungszeichen "" (Wichtig für setup_wifi!)
void logMsg(const char* msg, bool newLine) {
  // Wandelt den C-String einfach in das String-Objekt um und ruft Variante 1 auf
  logMsg(String(msg), newLine);
}

void handleWebLogQueue() {
  // Wenn Core 1 Textnachrichten hinterlassen hat, funken wir sie JETZT über Core 0 ins WLAN
  while (logOut != logIn) {
    webSocket.broadcastTXT(webLogQueue[logOut]);
    logOut = (logOut + 1) % LOG_QUEUE_SIZE;
  }
}

void printLocalTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    logMsg("Fehler: Uhrzeit konnte nicht abgerufen werden", true);
    return;
  }

  char timeBuffer[25];
  // 2. Die Zeit formatiert in den Puffer schreiben
  strftime(timeBuffer, sizeof(timeBuffer), "%d.%m.%Y %H:%M:%S", &timeinfo);

  // 3. Den Puffer in einen echten Arduino-String umwandeln
  String zeitAlsString = String(timeBuffer);
  // Formatiert die Ausgabe: DD.MM.YYYY HH:MM:SS
  logMsg(zeitAlsString, true);
}
