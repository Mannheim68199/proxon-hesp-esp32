/*

    Modul:  ESP32 DEV Module
            RS485 2 TTL Modul:  Amazon: RS485 to TTL module für 3,3 - 5 V!  automatische Umschaltung Lesen/Schreiben  rote Platine

    Anschlüsse:   ESP32       RS485 /            Proxon  P1  X6
                  Pin17       TXD  /  D-/A       braunes Kabel / A     X6-4
                  Pin16       RXD  /  D+/B       blaues Kabel  / B     X6-3
                  3.3V        VCC                              / +12V  X6-2
                  GND         GND   /  GND       weisses Kabel / GND   X6-1

*/
//#include <WiFi.h>
//#include <WebServer.h>
//#include <WebSocketsServer.h>
//#include <PubSubClient.h>
//#include <ArduinoOTA.h>
//#include <ArduinoJson.h>
#include "tools.h"

// --- KONFIGURATION ---
const char* ssid = "<ssid>";
const char* password = "<own_password>";
const char* mqtt_server = "<mqtt_ip>";
const int   mqtt_port = 1883;
const char* topic_raw = "rs485/sniffer/raw";   // Hierhin sendet der D1 mini
const char* topic_send = "rs485/sniffer/send"; // Hierauf hört der D1 mini (HEX-String senden)
const char* topic_response = "rs485/sniffer/response";   // Hierhin sendet der D1 mini  Antworten auf SET oder Query

// ESP32 Standard-Pins für UART2
#define RX2_PIN 22
#define TX2_PIN 23
/*
// Ringpuffer für die MQTT-Entkopplung (erhöht auf 8 Frames bei 20Hz)
// === ULTRA-SCHNELLE BINÄRE QUEUE ===
#define QUEUE_SIZE 31
#define BUFFER_SIZE 128
#define MAX_FRAME_LEN 256
#define MAX_HESP_FRAME_SIZE 128
*/
// Ein separater, kleiner Ringpuffer NUR für Texte zur Webseite
volatile int logIn = 0;
volatile int logOut = 0;
volatile bool logToWeb = false;

// lokale Buffer für Queries und Update-Queries
ControllerQueriesRow ramBuffer[NUM_CONTROLLER_QUERIES];
int currentControllerQueryIndex = 0;
                            
// Kombinierte Struktur für die Übertragen an Core 0 / MQTT
struct HespMessageFrame {
  byte requestData[MAX_HESP_FRAME_SIZE];
  uint8_t requestLength;
  byte responseData[MAX_HESP_FRAME_SIZE];
  uint8_t responseLength;
  bool isControllerMode;   // true = ESP32 war Controller (Frage+Antwort sind gekoppelt), false = Passiver Sniffer-Frame
  unsigned long timestamp; // Optional für Performance-Messungen
};

// Die gemeinsamen Queues (Ringpuffer)
volatile HespMessageFrame hespCombinedQueue[QUEUE_SIZE];
volatile int qCombinedIn = 0;
volatile int qCombinedOut = 0;


// Definiere den Zustand ganz oben im Task oder global:
enum BusMode {
  MODE_SNIFFER,
  MODE_CONTROLLER
};
BusMode currentBusMode = MODE_SNIFFER;
  
// Vorwärtsdeklaration
void rs485SnifferTask(void * parameter);
void handleJSONCallbackPayload(char* topic, byte* payload, unsigned int length);
void handleHEXCallbackPayload(char* topic, byte* payload, unsigned int length);
void showStatistik();

unsigned int reconnectCounter = 0;              // jeder reconnect Versuch wird gezählt, bei > 20 wird ein reboot durchgeführt
unsigned long injektionStartZeit = 0;           // Merkt sich, wann das Dauerfeuer für das SET begann
bool injektionLaeuft = false;                   // Status, ob die Stoppuhr gerade aktiv ist

// Sende-Variablen
byte txBuffer[BUFFER_SIZE];
unsigned int txLength = 0;
volatile bool sendRequest = false;              // Flag zeigt an, daß eine neue Sendenachricht eingegangen ist ( warten auf eine periodische Nachricht mit gleicher DP )
volatile bool readyToSend = false;              // Flag zeigt an, daß die Sendenachricht gesendet werden kann

// lässt sich über MQTT send ändern
unsigned int  sendTimeBegin = 30;               // 30 msec - 33 msec time window to send own message
unsigned int  sendTimeEnd = 33;
unsigned int  SET_REPEAT_TIME = 100;            // Definiert die Zeit, die ein SET Befehl wiederholt werden soll
unsigned long PACKET_TIMEOUT = 12;              // [ms] Stille signalisiert das Zyklus-Ende
unsigned int  LONG_ANSWER_MIN = 64;             // #Zeichen Mindestlänge für eine "lange Antwort"
unsigned int  SEND_AFTER_MATCHING_DP = 1;       // Versuch:  Flag zum Starten des Sendens beim gleichen Datenpunkt ( Werte:  0: nein  1: ja  )
unsigned int  ShowDPListCounter = 0;            // showCounter for list of DP read starting with 032e
unsigned int  ShowSETChanges = 0;               // show changes on SET requests, that have changed the value in ramBuffer
unsigned int  ShowStatistikMinutes = 1;         // show Statistiks every xx Minutes
unsigned int  iShowStatistik = 60000;           // = ShowStatistikMinutes*60000
// =================================

unsigned int msgReadCounter = 0;                // Anzahl der Msg pro Minute;
unsigned int msgWriteCounter = 0;               // Anzahl der Msg pro Minute;
unsigned int msgSendCounter = 0;                // Anzahl der Msg pro Minute;
unsigned int msgAnswerCounter = 0;              // Anzahl der eigenen Antworten pro Minute;
unsigned long lastStatTime = 0;                 // Zeit in msec zum Zählen der Msg pro Minute;
const char* ntpServer = "pool.ntp.org";
const char* timeZone = "CET-1CEST,M3.5.0,M10.5.0/3";


// Task-Handle für den RS485-Task auf Core 1
TaskHandle_t RS485TaskHandle = NULL;

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
  logMsg("Schritt 3: Hardware-Serial für PC steht.", true);
  
  // UART2 für die Heizung starten (Echte Hardware-Pins 16 und 17)
  Serial2.begin(19200, SERIAL_8N1, RX2_PIN, TX2_PIN);
  logMsg("Schritt 4: Serial2 (Pins 22/23) erfolgreich initialisiert.", true);
  
  // GANZ AM ENDE VON setup(): Jetzt ist das System stabil und bereit für das Web-Log!
  logToWeb = true; 
  delay(1000);

  // NTP-Server und Zeitzone für Deutschland konfigurieren
  configTzTime(timeZone, ntpServer);
  
  // MQTT konfigurieren
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback); // Funktion für eingehende MQTT-Mails verknüpfen
  // ERZWINGEN: Setzt den internen MQTT-Sende-Puffer auf 1024 Bytes hoch!
  mqttClient.setBufferSize(2048);
  logMsg("",true); 
  logMsg("Schritt 5: MQTT-Server konfiguriert.", true);

  // Jetzt versuchen wir den Task auf Core 1 zu starten
  logMsg("Schritt 6: Versuche Core 1 Task zu erstellen...", true);

  // controllerQueriesData lesen
  initStorage();

  
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
    logMsg("Schritt 8: ERFOLG: RS485-Task erfolgreich auf Core 1 gestartet! <<<", true);
  } else {
    logMsg("[!] KRITISCHER FEHLER: Task-Erstellung fehlgeschlagen! Fehlercode: "); logMsg(String(taskResult), true);
  }

  logMsg("--- DIAGNOSE-ENDE (setup abgeschlossen) ---\n", true);
  getFreeSpace();
}

// =========================================================================
// CORE 0 ARBEITET DIE STANDARD-LOOP AB (WLAN, MQTT-Verbindung & MQTT-Senden)
// =========================================================================
void loop() {
  if (!mqttClient.connected()) {
    reconnect();
  }
  mqttClient.loop();
  
  server.handleClient();
  webSocket.loop();
  
  handleWebLogQueue();   // FUNKT DIE LOGS NUN SICHER AUF CORE 0 INS WLAN!

  processQueueOnCore0();

  showStatistik();
 
  delay(1); // Gibt dem Betriebssystem Zeit für Hintergrundprozesse auf Core 0
}


// Diese Funktion wird aufgerufen, wenn eine MQTT-Nachricht für den ESP32 ankommt
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // leere Nachrichten gleich abfangen
  if (length == 0) return;
  
  // Falls die loop() noch sendet, neuen Befehl verwerfen
  if (sendRequest) return; 

  // Ein JSON beginnt typischerweise mit '{' (Dezimalwert 123 im ASCII-Code)
  if (payload[0] == '{') {
    handleJSONCallbackPayload( topic, payload, length );
  } else {
    handleHEXCallbackPayload( topic, payload, length );
  }
}

// =========================================================================
// CORE 1 KÜMMERT SICH NUR UM DEN BUS (Absolut immun gegen WLAN-Verzögerungen)
// =========================================================================
void rs485SnifferTask(void * parameter) {
  const int LOCAL_BUFFER_SIZE = 256;
  byte rxBuffer[LOCAL_BUFFER_SIZE];

  // Variablen für das lesen der Antwort auf mein SET
  const int SUB_BUFFER_SIZE = 256;   // vorher 64, 128  ==> MAX_HESP_FRAME_SIZE
  byte rxSubBuffer[SUB_BUFFER_SIZE];

  currentBusMode = MODE_SNIFFER;
  unsigned long lastBusActivityTime = millis();

  memset(rxBuffer, 0, sizeof(rxBuffer));

  // Für präzisen 100ms Takt im Controller-Modus
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    // =======================================================================
    // ZUSTANDS-SWITCH: SNIFFER vs. MASTER
    // =======================================================================
    if (currentBusMode == MODE_SNIFFER) {
      // =======================================================================
      // AKTIVER SNIFFER-MODUS (Wir hören nur auf die Fragen/Antwort Paare)
      // =======================================================================
      handleSnifferMode(rxBuffer, LOCAL_BUFFER_SIZE, rxSubBuffer, SUB_BUFFER_SIZE, lastBusActivityTime, xLastWakeTime);
    } else if (currentBusMode == MODE_CONTROLLER) {
      // =======================================================================
      // AKTIVER MASTER-MODUS (Wir stellen die Fragen selbst im 100ms-Takt)
      // =======================================================================
      handleControllerMode(rxSubBuffer, SUB_BUFFER_SIZE, lastBusActivityTime, xLastWakeTime);
    } 

    vTaskDelay(1); // FreeRTOS-konformer Yield
  }
}

// =========================================================================
// HILFSMETHODE: SNIFFER-MODUS
// =========================================================================
void handleSnifferMode(byte* rxBuffer, const int LOCAL_BUFFER_SIZE, byte* rxSubBuffer, const int SUB_BUFFER_SIZE, unsigned long &lastBusActivityTime, TickType_t &xLastWakeTime) {

  // Statische Variablen behalten ihren Wert zwischen den Funktionsaufrufen
  static int bufferIndex = 0;
  static unsigned long lastCharTime = 0;
  static unsigned long cycleStartTime = 0;
  static bool timerArmed = false;
  static bool bereitsGesendetOderVetoInDiesemZyklus = false;

  static int subBufferIndex = 0;
  static unsigned long subLastCharTime = 0;
  static bool warteAufAntwort = false;
  const unsigned long INACTIVITY_THRESHOLD = 250;
  
  // Prüfen, ob seit > 250ms nichts mehr auf dem Bus kam
  if (millis() - lastBusActivityTime > INACTIVITY_THRESHOLD) {
    logMsg(">> Bus still (>250ms) -> Wechsel in den AKTIVEN MASTER-Modus", true);
    currentBusMode = MODE_CONTROLLER;
    currentControllerQueryIndex = 0; // Bei Frage 0 der Sequenz anfangen
    bufferIndex = 0;             // Eventuelle Fragmente verwerfen
    warteAufAntwort = false;
    xLastWakeTime = xTaskGetTickCount();  // Timer-Basis für Controller-Modus frisch initialisieren
    return;                               // zurück zum Haupt Programm, um direkt in den Controller Mode zu wechseln
  }
  
  // =======================================================================
  // SCHRITT 1: REINER, STARRER BUS-EMPFANG
  // =======================================================================
  while (Serial2.available()  && !warteAufAntwort) {
    byte incomingByte = Serial2.read();
    
    // Wenn der Bus vorher still war (>12ms), beginnt ein neuer 100ms-Zyklus!
    if (bufferIndex == 0 && (millis() - lastCharTime > PACKET_TIMEOUT)) {
      cycleStartTime = millis(); // Startzeitpunkt des 100ms-Takts merken
      timerArmed = true;         // Sende-Timer scharf schalten
      bereitsGesendetOderVetoInDiesemZyklus = false; // Neuen Zyklus freigeben
    }

    if (bufferIndex < LOCAL_BUFFER_SIZE) {
      rxBuffer[bufferIndex] = incomingByte;
      bufferIndex++;
    }
    lastCharTime = millis();
    lastBusActivityTime = millis(); // Aktivität auf dem Bus registriert
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
    lastBusActivityTime = millis(); // Auch Antworten sind Bus-Aktivität
  }
  
  // =======================================================================
  // SCHRITT 3: NORMALES SAMMEL-ENDE (Gesteuert durch das PACKET_TIMEOUT)
  // =======================================================================
  // Ein Timeout von 12ms (PACKET_TIMEOUT) oder das Zyklus-Ende (1 msec vorher) beenden den Lese-Prozess
  // bevor nach 100ms der nächste Zyklus startet!
  if (!warteAufAntwort && bufferIndex > 0 && (millis() - lastCharTime > PACKET_TIMEOUT)) {

    // Kopieren in die Queue für Core 0 (MQTT)
    int nextIn = (qCombinedIn + 1) % QUEUE_SIZE;
    if (nextIn != qCombinedOut && bufferIndex <= MAX_HESP_FRAME_SIZE) {
      byte* destReq = (byte*) hespCombinedQueue[qCombinedIn].requestData;
      memcpy(destReq, rxBuffer, bufferIndex);
      hespCombinedQueue[qCombinedIn].requestLength = bufferIndex;
      hespCombinedQueue[qCombinedIn].responseLength = 0;  // Sniffer liest sequentiell, keine feste Kopplung hier
      hespCombinedQueue[qCombinedIn].isControllerMode = false;
      hespCombinedQueue[qCombinedIn].timestamp = millis();
      
      qCombinedIn = nextIn;
      msgReadCounter++;
    }

    // Zustand für den normalen Hauptpuffer nullen
    bufferIndex = 0;    
  }

  // ==============================================================================================================================================
  // SCHRITT 4: SENDEN im Zeitfenster von sendTimeBegin bis sendTimeEnd ( 30-33 msec ) NACH ZYKLUS-START, wenn frei ist
  // ==============================================================================================================================================
  if (timerArmed && !bereitsGesendetOderVetoInDiesemZyklus && (millis() - cycleStartTime >= sendTimeBegin)  && (millis() - cycleStartTime < sendTimeEnd)) {

    if (sendRequest && bufferIndex > 4) {
        if (SEND_AFTER_MATCHING_DP == 0 || (txBuffer[4] == rxBuffer[4] && txBuffer[3] == rxBuffer[3])) {
          readyToSend = true;
          sendRequest = false;
        }
    }
    
    if (readyToSend) {  // 1
      // Typ bestimmen (0x1 = SET, 0x0 = QUERY)
      byte txType = txBuffer[0] & 0x0F;

      // ZEITSTEUERUNG-CHECK: Wenn das SET bereits seit mehr als 1500 ms feuert, jetzt stoppen!
      if (txType == 1 && injektionLaeuft && (millis() - injektionStartZeit >= SET_REPEAT_TIME)) {
        readyToSend = false;
        injektionLaeuft = false;
        logMsg("\n[Timer] Sende-Injektion automatisch beendet.", true);
      } 
      // Falls das Smart Home mitten im Feuern ein NEUES SET schickt (bevor die Zeit um war)
      else if (txType == 1 && !injektionLaeuft && bereitsGesendetOderVetoInDiesemZyklus == false) {
        // Neuer Befehl reingekommen -> Stoppuhr für diesen neuen Befehl frisch starten
        injektionStartZeit = millis();
        injektionLaeuft = true;
      }
      
      if (readyToSend) {  // 2
        // DER ECHTE BELEGUNGS-CHECK: 
        // Wir legen NUR ein Veto ein, wenn AKTUELL wieder Bytes reinkommen (die lange Antwort läuft bereits)
        // Ein RS485-Startbit zieht den RX-Pin auf LOW (0). 
        // Wenn der Pin LOW ist ODER sich doch schon ein Byte in die UART verirrt hat:
        if (digitalRead(RX2_PIN) == LOW || Serial2.available() > 0) {
          logMsg(">> VETO: Senden blockiert – Dieser Zyklus gehört der LANGEN Antwort!", true);
          bereitsGesendetOderVetoInDiesemZyklus = true; // Sperren für diesen 100ms-Takt
        } else {  // Bus ist frei -> Senden!
          if (txType == 1 ) {
            logMsg("[Injektion SET] ");
          } else {
            logMsg("[Sende QUERY] ");
          }
          logMsg(String(millis() - cycleStartTime) + " msec ");

          // Befehl über Hardware-UART2 ausgeben  / Senden
          for (unsigned int i = 0; i < txLength; i++) {
            Serial2.write(txBuffer[i]);
          }
          Serial2.flush(); // Warten bis physikalisch komplett gesendet
          
          delayMicroseconds(200); // Richtungsumschaltung des Transceivers abwarten
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
        } // if (digitalRead(RX2_PIN) == LOW || Serial2.available() > 0) {
      } else {
        // Wenn kein Sende-Wunsch vorliegt, ignorieren wir den Rest des Zyklus
        bereitsGesendetOderVetoInDiesemZyklus = true;
      }   // if (readyToSend) {   2
    }     // if (readyToSend) {   1
  }       // if (timerArmed && !bereitsGesendetOderVetoInDiesemZyklus && (millis() - cycleStartTime >= sendTimeBegin)  && (millis() - cycleStartTime < sendTimeEnd))

  // =======================================================================
  // SCHRITT 5 & 6: AUSWERTUNG DES SUB-LESEZYKLUS (Antwort-Timeout)
  // =======================================================================
  if (warteAufAntwort) {
    // Kriterien für das Ende der Antwort bei 19200 Baud:
    // A: Daten kamen und nun ist seit > 4ms Ruhe (Antwort vollständig erhalten / Paketende)
    // B: KEINE Daten und der Bus steht seit 20ms still (Gegenstelle antwortet nicht)
    bool antwortErfolgreich = (subBufferIndex > 0 && (millis() - subLastCharTime > 4));
    bool antwortTimeout     = (subBufferIndex == 0 && (millis() - subLastCharTime > 20));
    
    if (antwortErfolgreich || antwortTimeout ) {
      if (subBufferIndex > 0) {
        // In die Queue für Core 0 (MQTT / Parser) schieben
        int nextIn = (qCombinedIn + 1) % QUEUE_SIZE;
        if (nextIn != qCombinedOut && subBufferIndex <= MAX_HESP_FRAME_SIZE) {
          byte* destResp = (byte*) hespCombinedQueue[qCombinedIn].responseData;
          memcpy(destResp, rxSubBuffer, subBufferIndex);
          hespCombinedQueue[qCombinedIn].responseLength = subBufferIndex;
          hespCombinedQueue[qCombinedIn].requestLength = 0;
          hespCombinedQueue[qCombinedIn].isControllerMode = false;
          hespCombinedQueue[qCombinedIn].timestamp = millis();
          qCombinedIn = nextIn;
          msgAnswerCounter++;
        }
       
        // Typ-Bestimmung über das Low-Nibble des Headers (rxSubBuffer[0])
        byte typeNibble = rxSubBuffer[0] & 0x0F;
        uint16_t DP = (rxSubBuffer[4] << 8) | rxSubBuffer[3];
        char hexDP[5];
        sprintf(hexDP, "%04X", DP);
        logMsg(" Antwort: (" + String(subBufferIndex) + " Bytes): " + String(hexDP) + " => ");

        switch(typeNibble) {
            case 0: logMsg("QUERY (0x0)", true); break;
            case 1: logMsg("SET (0x1)", true); break;
            case 2: logMsg("RESPONSE (0x2)", true); break;
            case 3: logMsg("CONFIRM (0x3)", true); break;
            default: logMsg("UNBEKANNT (0x"); logMsg(String(typeNibble), HEX); logMsg(")", true); break;
        }
      } else {
        logMsg(" -> Antwort: TIMEOUT (Keine Gegenstelle reagiert)", true);
      } // if (subBufferIndex > 0)

      // Sub-Lesezyklus sauber beenden und Hauptbus wieder freigeben
      subBufferIndex = 0;
      warteAufAntwort = false;
      lastCharTime = millis(); // Haupt-Zeitstempel aktualisieren, um Fehltrigger im Haupt-Schnitt zu verhindern
    }   // if (antwortErfolgreich || antwortTimeout )
  }     // if (warteAufAntwort)
  
  // Sicherheitsnetz für Pufferüberlauf innerhalb der Methode
  if (bufferIndex >= LOCAL_BUFFER_SIZE - 5) {
    bufferIndex = 0;
    timerArmed = false;
  }
}

// =========================================================================
// HILFSMETHODE: CONTROLLER-MODUS
// =========================================================================
void handleControllerMode(byte* rxSubBuffer, const int SUB_BUFFER_SIZE, unsigned long &lastBusActivityTime, TickType_t &xLastWakeTime) {

  static int subBufferIndex = 0;
  static unsigned long subLastCharTime = 0;

  // Sicherheits-Veto: Falls plötzlich ein anderes Gerät sendet, bevor wir starten
  if (Serial2.available() > 0) {
    logMsg(">> VETO MASTER: Fremde Daten vor Sendestart! Zurück zu Sniffer.", true);
    currentBusMode = MODE_SNIFFER;
    lastBusActivityTime = millis();
    return;
  }

  // ---------------------------------------------------------------------
  // SCHRITT 1: Frage direkt aus dem ramBuffer lesen
  // ---------------------------------------------------------------------
  byte activeQuery[MAX_HESP_QUERY_LEN];
  memcpy(activeQuery, ramBuffer[currentControllerQueryIndex].data, ramBuffer[currentControllerQueryIndex].length);
  
  // ---------------------------------------------------------------------
  // SCHRITT 2: Frage aktiv auf den Bus schreiben
  // ---------------------------------------------------------------------
  for (unsigned int i = 0; i < ramBuffer[currentControllerQueryIndex].length; i++) {
    Serial2.write(activeQuery[i]);
  }
  Serial2.flush();
  
  delayMicroseconds(200); // Tranceiver Umschaltzeit abwarten
  //while (Serial2.available() > 0) { Serial2.read(); } // Tx-Self-Hear löschen
  // Präzises Löschen des eigenen Echos im Mastermodus
  unsigned int echoBytesToClear = ramBuffer[currentControllerQueryIndex].length;
  unsigned long echoTimeout = millis();
  while (echoBytesToClear > 0 && (millis() - echoTimeout < 15)) {
    if (Serial2.available() > 0) {
      Serial2.read();
      echoBytesToClear--;
    }
  }

  // ---------------------------------------------------------------------
  // SCHRITT 3: Antwort einlesen (Direktes, blockierendes Fenster für maximale Präzision)
  // ---------------------------------------------------------------------
  subBufferIndex = 0;
  unsigned long waitStart = millis();
  subLastCharTime = millis();

  // Wir geben der Wärmepumpe ein Zeitfenster von z.B. 35ms zum Antworten
  while (millis() - waitStart < 35) {
    while (Serial2.available() > 0) {
      if (subBufferIndex < SUB_BUFFER_SIZE) {
        rxSubBuffer[subBufferIndex++] = Serial2.read();
      }
      subLastCharTime = millis();
    }
    // Wenn Daten da sind und seit 3ms kein neues Byte kam -> Antwort komplett
    if (subBufferIndex > 0 && (millis() - subLastCharTime > 3)) {
      // Antwort komplett eingetroffen
      break;
    }
    delayMicroseconds(200);
  }

  // ---------------------------------------------------------------------
  // SCHRITT 4: SYNCHRONES PACKETIEREN in die kombinierte Queue
  // ---------------------------------------------------------------------

  int nextIn = (qCombinedIn + 1) % QUEUE_SIZE;
  if (nextIn != qCombinedOut) {
    // Kopiere die gestellte Frage
    byte* destReq = (byte*) hespCombinedQueue[qCombinedIn].requestData;
    memcpy(destReq, activeQuery, MAX_HESP_QUERY_LEN);
    hespCombinedQueue[qCombinedIn].requestLength = MAX_HESP_QUERY_LEN;
    
    // Gekoppelte Antwort kopieren (wenn empfangen, sonst Length = 0 -> Timeout)
    if (subBufferIndex > 0) {
      byte* destResp = (byte*) hespCombinedQueue[qCombinedIn].responseData;
      memcpy(destResp, rxSubBuffer, subBufferIndex);
      hespCombinedQueue[qCombinedIn].responseLength = subBufferIndex;
      msgAnswerCounter++;
    } else {
      hespCombinedQueue[qCombinedIn].responseLength = 0; // Signalisiert Core 0 ein Timeout
    }
    
    hespCombinedQueue[qCombinedIn].isControllerMode = true;
    hespCombinedQueue[qCombinedIn].timestamp = millis();
    
    qCombinedIn = nextIn;
    msgReadCounter++;
  }
  
  // ---------------------------------------------------------------------
  // SCHRITT 5: Nächste Frage für den kommenden 100ms Takt vorbereiten
  // ---------------------------------------------------------------------
  
  currentControllerQueryIndex = (currentControllerQueryIndex + 1) % NUM_CONTROLLER_QUERIES;

  // ---------------------------------------------------------------------
  // SCHRITT 6: Kollisionsprüfung: Hat parallel jemand dazwischengesendet?
  // ---------------------------------------------------------------------
  
  if (Serial2.available() > 0) {
    logMsg(">> Kollision im Controller-Modus erkannt. Zurück zu Sniffer.", true);
    currentBusMode = MODE_SNIFFER;
    lastBusActivityTime = millis();
    return;
  }

  // ---------------------------------------------------------------------
  // SCHRITT 7: Exakten 100ms-Takt halten
  // ---------------------------------------------------------------------
  // HÄLT DEN TAKT EXAKT BEI 100MS (gleicht die Rechen- und Lesezeit automatisch aus!)
  vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100)); // Überspringt das vTaskDelay(1) am Schleifenende, da vTaskDelayUntil bereits gewartet hat
  
}

void handleHEXCallbackPayload(char* topic, byte* payload, unsigned int length) {
  // HEX-String in echte Bytes wandeln, in Puffer sichern und in ControllerQueriesRow dauerhaft übertragen
  txLength = 0;
  uint8_t buffer[MAX_HESP_QUERY_LEN];
  for (unsigned int i = 0; i < length; i += 2) {
    if (i + 1 < length && txLength < MAX_HESP_QUERY_LEN ) {  // && txLength < sizeof(txBuffer)
      byte high = hexCharToByte((char)payload[i]);
      byte low = hexCharToByte((char)payload[i+1]);
      buffer[txLength] = (high << 4) | low;
      txBuffer[txLength] = (high << 4) | low;
      txLength++;
    }
  }
  uint16_t dpID = getID(buffer);
  String sID = byteToHexString(buffer[4]) + byteToHexString(buffer[3]);
  logMsg("",true);
  logMsg("MQTT_Callback: ID " + sID + " -> " + String(dpID), true);
  updateRowByID( dpID, buffer, txLength );
  if ( currentBusMode == MODE_SNIFFER ) {
    logMsg("SNIFFER: send Query for " + sID + " ");
    // Signal an die loop() senden, dass gesendet werden soll
    sendRequest = true;   // Signal an Core 1: Bitte in der nächsten Pause senden!
  } else if ( currentBusMode == MODE_CONTROLLER ) {
    logMsg("CONTROLLER: change Query for " + sID + " ");
  } else {
    logMsg("Callback: unbekannter BusModus");
  }
}

void handleJSONCallbackPayload(char* topic, byte* payload, unsigned int length) {
      // 1. JSON-VERARBEITUNG
    // Dynamisches Dokument erstellen (Größe an deinen JSON anpassen)
    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, payload, length);
 
    if (error) {
      logMsg("JSON-Fehler: "); logMsg(error.f_str(), true);
      return; // Fehler beim Parsen -> Abbrechen
    }

    JsonObject obj = doc.as<JsonObject>();
    logMsg(" ",true);
    if ( !obj.isNull() && obj.size() > 0 ) {
      auto firstPair = obj.begin();
      String firstKey = firstPair->key().c_str();
      logMsg(" Parameteraenderungen: " + firstKey,true);
    } else {
      logMsg(" Parameteraenderungen: ",true);
    }

    // Parameter kompakt abfragen:
    updateParamIfPresent(doc, "sendTimeBegin", sendTimeBegin, "sendTimeBegin");
    updateParamIfPresent(doc, "sendTimeEnd", sendTimeEnd, "sendTimeEnd");
    updateParamIfPresent(doc, "SET_REPEAT_TIME", SET_REPEAT_TIME, "SET_REPEAT_TIME");
    updateParamIfPresent(doc, "PACKET_TIMEOUT", PACKET_TIMEOUT, "PACKET_TIMEOUT");
    updateParamIfPresent(doc, "LONG_ANSWER_MIN", LONG_ANSWER_MIN, "LONG_ANSWER_MIN");
    updateParamIfPresent(doc, "SEND_AFTER_MATCHING_DP", SEND_AFTER_MATCHING_DP, "SEND_AFTER_MATCHING_DP");
    updateParamIfPresent(doc, "SHOW_DP_LIST_COUNTER", ShowDPListCounter, "SHOW_DP_LIST_COUNTER");
    updateParamIfPresent(doc, "ShowSETChanges", ShowSETChanges, "ShowSETChanges");
    
    // Besonderheit bei ShowStatistikMinutes mit Multiplikator (Minuten -> ms):
    updateParamIfPresent(doc, "ShowStatistikMinutes", iShowStatistik, "ShowStatistikMinutes", true, 60000);

    if (doc.containsKey("setDefaultValues")) {
      sendTimeBegin = 30;       // msec
      sendTimeEnd = 33;         // msec
      SET_REPEAT_TIME = 100;    // msec
      PACKET_TIMEOUT = 12;      // msec
      LONG_ANSWER_MIN = 64;     // bytes
      SEND_AFTER_MATCHING_DP = 1;  // 0 oder 1
      ShowDPListCounter = 0;       // 
      ShowSETChanges = 0;          //
      iShowStatistik = 60000;   // every Minute
      logMsg(String("defaultWert per JSON geändert: sendTimeBegin=30, sendTimeEnd=33, SET_REPEAT_TIME=100, PACKET_TIMEOUT=12, LONG_ANSWER_MIN=64, SEND_AFTER_MATCHING_DP=1, ShowDPListCount=0, ShowSETChanges=0"), true);
    }
    
    if (doc.containsKey("getQueries")) logRamBuffer(ramBuffer, NUM_CONTROLLER_QUERIES);

    if (doc.containsKey("removeBufferFile")) removeBufferFile();
}

void showStatistik () {
    // =======================================================================
  // STATISTIK-AUSGABE (ALLE iShowStatistik milliSEKUNDEN IM TERMINAL)  deaktiviert bei iShowStatistik = 0
  // =======================================================================
  if (iShowStatistik > 0 && (millis() - lastStatTime >= iShowStatistik)) {
    // Sicheres Auslesen und Zurücksetzen (Zähler kurz einfrieren)
    unsigned int currentRead = msgReadCounter;
    unsigned int currentSend = msgSendCounter;
    unsigned int currentWrite = msgWriteCounter;
    unsigned int currentAnswer = msgAnswerCounter;
    
    msgReadCounter = 0;
    msgWriteCounter = 0;
    msgSendCounter = 0;
    msgAnswerCounter = 0;
    lastStatTime = millis();

    unsigned int iQual = 0;
    if ( currentRead > 0 ) {
      iQual = (100 * currentSend / currentRead );
    }
    unsigned int iMin = iShowStatistik / 60000;
    float frequenz = (currentRead + currentWrite) / 60.0 / iMin;
    
    // Ausgabe im seriellen Monitor (USB-Terminal)
    logMsg("\n========================================", true);
    printLocalTime();
    logMsg(" HESP-BUS STATISTIK (" + String(iMin) + " Min)\n");
    logMsg("----------------------------------------", true);
    logMsg(" read/send Msg  (MQTT): " + String(currentRead) + "/" + String(currentSend), true);
    logMsg(" query/response (TX/RX):" + String(currentWrite) + "/" + String(currentAnswer), true);
    logMsg(" Bus-Freq./Quality:    ~" + String(frequenz,2) + " Hz / " + String(iQual) + " %", true);
    logMsg("========================================\n", true);

    getFreeSpace();
  }
}

size_t getFreeSpace() {
  size_t freeHeap = ESP.getFreeHeap();

  logMsg("Free storage space: "+String(freeHeap/1024)+" kB\n", true);
  return freeHeap;
}

void processQueueOnCore0() {
  while (qCombinedOut != qCombinedIn) {
    // FEHLERBEHEBUNG: Lokales, nicht-volatiles Struct anlegen 
    HespMessageFrame localMsg;

    // Daten sicher per memcpy aus der volatile Queue in den RAM von Core 0 kopieren
    memcpy(&localMsg, (void*)&hespCombinedQueue[qCombinedOut], sizeof(HespMessageFrame));
   
    if (localMsg.isControllerMode) {
      // MASTER MODE: Wir haben das perfekte Paar!
      if (localMsg.responseLength > 0) {
        String hexPayload = "";
        int totalLen = localMsg.requestLength + localMsg.responseLength;
    
        if (totalLen > 0 && totalLen <= MAX_FRAME_LEN) {
          // String-Puffer vorab reservieren, das beschleunigt den ESP32 massiv!
          hexPayload.reserve(totalLen * 2 + 2); 
    
          for (int i = 0; i < localMsg.requestLength; i++) {
            if (localMsg.requestData[i] < 0x10) hexPayload += "0";
            hexPayload += String(localMsg.requestData[i], HEX);
          }

          for (int i = 0; i < localMsg.responseLength; i++) {
            if (localMsg.responseData[i] < 0x10) hexPayload += "0";
            hexPayload += String(localMsg.responseData[i], HEX);
          }
        
          bool publishSuccess = mqttClient.publish(topic_raw, hexPayload.c_str());
          
          if (!publishSuccess) {
            logMsg("\n[!] MQTT-FEHLER Antwort: Konnte langes Paket nicht senden! String-Länge: "+String(hexPayload.length()) + " Zeichen. (Puffer im setup() zu klein?)", true);
          } else {
            msgSendCounter++;
          }
        }
      } else {
         logMsg("\n[!] MQTT-FEHLER keine Antwort zur Frage");
      }
    } else {
      // SNIFFER MODE: Einzelne Fragmente wie gewohnt verarbeiten
      if (localMsg.requestLength > 0) {
         // Wenn der RS485-Task auf Core 1 fertige Frames in der Queue abgelegt hat,
         // werden diese hier von Core 0 stressfrei ins WLAN gefunkt.
         String hexPayload = "";
         String DP = "";
         uint16_t iDP = 0;
         int currentLen = localMsg.requestLength;
      
         if (currentLen > 0 && currentLen <= MAX_FRAME_LEN) {
            // String-Puffer vorab reservieren, das beschleunigt den ESP32 massiv!
            hexPayload.reserve(currentLen * 2 + 2); 
            
            for (int i = 0; i < currentLen; i++) {
              String hexPart = "";
              if (localMsg.requestData[i] < 0x10) hexPart = "0";
              hexPart += String(localMsg.requestData[i], HEX);
              if ( i == 3 ) DP = hexPart;
              if ( i == 4 ) DP = hexPart + DP;
              hexPayload += hexPart;
            }

            bool publishSuccess = mqttClient.publish(topic_raw, hexPayload.c_str());
            
            if (!publishSuccess) {
              logMsg("\n[!] MQTT-FEHLER: Konnte langes Paket nicht senden! String-Länge: " + String(hexPayload.length()) + " Zeichen. (Puffer im setup() zu klein?)", true);
            } else {
              iDP = getID(localMsg.requestData);
              
              if ( String(localMsg.requestData[0], HEX) == "11" ) {   // bei SET Requests => QueryBuffer updaten
                uint8_t bufferIndex = findRowIndexByID(iDP);
                bool bSame = true;
                uint8_t len = localMsg.requestData[7] >> 1;
                String sValue = "", sOldValue = "";
                for ( int i = 0; i < len && i < currentLen-8; i++) {
                  sValue += byteToHexString(localMsg.requestData[8+i]);
                  sOldValue += byteToHexString(ramBuffer[bufferIndex].data[8+i]);
                  if ( localMsg.requestData[8+i] != ramBuffer[bufferIndex].data[8+i] ) bSame = false;
                }
                if ( !bSame ) {
                  if ( ShowSETChanges > 0 ) {
                    logMsg("SET: " + DP + " len: " + len + " Wert: " + sValue + " alter Wert: " + sOldValue, true );
                    ShowSETChanges--;
                  }
                  if ( currentLen > MAX_HESP_QUERY_LEN ) currentLen = MAX_HESP_QUERY_LEN;
                  updateRowByID(iDP, localMsg.requestData, currentLen);
                }
              }
              
              if ( ShowDPListCounter > 0 ) {
                if ( DP == "032e" ) {
                  ShowDPListCounter--;
                  logMsg("",true);
                }
                logMsg(DP + " ");
              }
              msgSendCounter++;
            }
         }
      }
      if (localMsg.responseLength > 0) {
        String hexPayload = "";
        int currentLen = localMsg.responseLength;
    
        if (currentLen > 0 && currentLen <= MAX_FRAME_LEN) {
          // String-Puffer vorab reservieren, das beschleunigt den ESP32 massiv!
          hexPayload.reserve(currentLen * 2 + 2); 
    
          for (int i = 0; i < currentLen; i++) {
            if (localMsg.responseData[i] < 0x10) hexPayload += "0";
            hexPayload += String(localMsg.responseData[i], HEX);
          }
        
          bool publishSuccess = mqttClient.publish(topic_response, hexPayload.c_str());
          
          if (!publishSuccess) {
            logMsg("\n[!] MQTT-FEHLER Antwort: Konnte langes Paket nicht senden! String-Länge: " + String(hexPayload.length()) + " Zeichen. (Puffer im setup() zu klein?)", true);
          } else {
            msgSendCounter++;
          }
        }
      }
    }
    
    qCombinedOut = (qCombinedOut + 1) % QUEUE_SIZE;
  }
}

