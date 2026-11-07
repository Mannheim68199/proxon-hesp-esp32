#include "tools.h" // Bindet die Definitionen ein

String webLogQueue[LOG_QUEUE_SIZE];

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

void setupWebServer() {
  
  // --- HAUPTSEITE (LIVE-LOG) ---
  server.on("/", []() {
    server.send(200, "text/html", index_html);
  });

  server.on("/getLog", []() {
    String logContent = getLogFileContent();
    server.send(200, "text/plain", logContent);
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

  logMsg("Schritt 2:  WebServer aktiviert", true);
}

// Hilfsfunktion: Wandelt ein einzelnes Hex-Zeichen in eine Zahl um
byte hexCharToByte(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
};

// Hilfsfunktion: Wandelt eine Zahl (0-15) in ein Hex-Zeichen um
char byteToHexChar(byte b) {
    if (b >= 0 && b <= 9)  return b + '0';
    if (b >= 10 && b <= 15) return b - 10 + 'A';
    return '0'; // Fallback für ungültige Werte
};

// Hauptfunktion: Wandelt ein Byte in einen 2-stelligen Hex-String um
String byteToHexString(byte b) {
    const char hexChars[] = "0123456789abcdef";
    String result = "";
    result += hexChars[(b >> 4) & 0x0F]; // Oberes Halbbyte (High Nibble)
    result += hexChars[b & 0x0F];        // Unteres Halbbyte (Low Nibble)
    return result;
};

void removeBufferFile() {
  LittleFS.remove(QUERIES_FILENAME);
  initStorage();
  logMsg("BufferFile removed and initialized: " + String(QUERIES_FILENAME), true);
};

// Hauptfunktion zum Loggen des gesamten Buffers
void logRamBuffer(ControllerQueriesRow* buffer, size_t numEntries) {
    for (size_t i = 0; i < numEntries; i++) {
        // Nur Einträge verarbeiten, die Daten und eine Länge > 0 haben
        if (buffer[i].length > 0) {
          
            // Sicherheitscheck: Falls length fälschlicherweise MAX_HESP_QUERY_LEN überschreitet
            size_t bytesToLog = buffer[i].length;
            if (bytesToLog > MAX_HESP_QUERY_LEN) {
                bytesToLog = MAX_HESP_QUERY_LEN;
            }
                        
            String logLine = "";

            // Prüfen, ob genügend Bytes für die ID vorhanden sind (mindestens 5 Bytes)
            if (bytesToLog >= 5) {
                // 4. Byte (Index 3) und 5. Byte (Index 4) als Hex-ID zusammensetzen
                logLine = "\n" + String(i) + " (" + String(bytesToLog) + ")" + byteToHexString(buffer[i].data[4]) + byteToHexString(buffer[i].data[3]) + ": ";
            } else {
                // Fallback, falls die Nachricht zu kurz ist, um eine ID zu enthalten
                logLine = "\nQuery " + String(i) + " (Short): ";
            }
            
            // Schleife durch alle Bytes des aktuellen Eintrags
            for (size_t j = 0; j < bytesToLog; j++) {
                logLine += byteToHexString(buffer[i].data[j]);
                
                // Optional: Leerzeichen zwischen den Hex-Bytes für bessere Lesbarkeit
                if (j < buffer[i].length - 1) {
                    logLine += " ";
                }
            }
            // Senden der fertigen Zeile an deine Ausgabe
            logMsg(logLine);
        }
    }
};

String getLogFileContent() {
  String logContent = "";
  
  // zuerst die "alten Nachrichten oberhalb von logIn"
  for (int i = logIn; i < LOG_QUEUE_SIZE; i++ ) {
    if ( webLogQueue[i] != "" ) {
      logContent += webLogQueue[i];
    }
  }

  // dann alle von Anfang der Queue bis zur aktuellen Nachricht
  for (int i = 0; i < logIn; i++) {
    if ( webLogQueue[i] != "" ) {
      logContent += webLogQueue[i];
    }
  }

  return logContent;
}

void logMsg(String msg, bool newLine) {
  //if (!logToWeb) return; // Sofort abbrechen, wenn Logging AUS ist

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
};

// Variante 2: Für C-Strings / Text in Anführungszeichen "" (Wichtig für setup_wifi!)
void logMsg(const char* msg, bool newLine) {
  // Wandelt den C-String einfach in das String-Objekt um und ruft Variante 1 auf
  logMsg(String(msg), newLine);
};

void handleWebLogQueue() {
  // Wenn Core 1 Textnachrichten hinterlassen hat, funken wir sie JETZT über Core 0 ins WLAN
  while (logOut != logIn) {
    mqttClient.publish(topic_log, webLogQueue[logOut].c_str());
    webSocket.broadcastTXT(webLogQueue[logOut]);
    logOut = (logOut + 1) % LOG_QUEUE_SIZE;
  }
};

// Funktion zur Berechnung der ID aus Byte 4 und 5 (0-indiziert: Byte [4] und [3])
uint16_t getID(const uint8_t* data) {
  return ((uint16_t)data[4] << 8) | data[3];
}

// Lädt Daten aus dem Dateisystem oder initialisiert sie neu
void initStorage() {
  logMsg("Schritt 7: InitStorage ==> ");
  if (!LittleFS.begin(true)) { // true = Formatiert das Dateisystem, falls es fehlschlägt
    logMsg("LittleFS Mount fehlgeschlagen", true);
    return;
  }

  // Prüfen, ob die Queries-Datei bereits existiert
  if (!LittleFS.exists(QUERIES_FILENAME)) {
    logMsg("Keine bestehenden Query-Daten gefunden. Schreibe Initialdaten...", true);
    
    // RAM-Querie-Buffer aus dem 2D-Array füllen
    for (int i = 0; i < NUM_CONTROLLER_QUERIES; i++) {
      ramBuffer[i].length = controllerQueriesData[i][0];
      for (int j = 0; j < ramBuffer[i].length; j++) {
        ramBuffer[i].data[j] = controllerQueriesData[i][j + 1];
      }
    }
    saveToFlash();
  } else {
    // Daten aus dem File in den RAM-Spiegel laden
    File file = LittleFS.open(QUERIES_FILENAME, "r");
    if (file) {
      file.read((uint8_t*)ramBuffer, sizeof(ramBuffer));
      file.close();
      logMsg("Query-Daten erfolgreich aus File geladen. Größe: " + String(sizeof(ramBuffer)), true);
    } else {
      logMsg("Query-Daten File nicht gefunden. Keine Daten geladen!", true);
    }
  }

  // Prüfen, ob die Log-Datei bereits existiert
  if (!LittleFS.exists(LOG_FILENAME)) {
    File logFile = LittleFS.open(LOG_FILENAME, FILE_WRITE);
    logFile.close();
  }
}

// Sichert den aktuellen RAM-Spiegel dauerhaft in Datei
void saveToFlash() {
  File file = LittleFS.open(QUERIES_FILENAME, "w");
  if (file) {
    file.write((uint8_t*)ramBuffer, sizeof(ramBuffer));
    file.close();
    //logMsg("updateRowByID: Daten dauerhaft in Datei gespeichert.", true);
  } else {
    logMsg("updateRowByID: Fehler beim Schreiben der Datei.", true);
  }
}

// schneller Zugriff / Suche über die ID (Gibt Zeilen-Index zurück oder -1)
int findRowIndexByID(uint16_t targetID) {
  for (int i = 0; i < NUM_CONTROLLER_QUERIES; i++) {
    if (ramBuffer[i].length >= 5) { // Sicherstellen, dass die Zeile überhaupt lang genug ist
      if (getID(ramBuffer[i].data) == targetID) {
        return i; // Index im RAM-Buffer gefunden
      }
    }
  }
  return -1; // ID nicht gefunden
}

// Einzelne Zeile abändern und dauerhaft speichern, sowohl im Query- als auch im Update-Buffer
bool updateRowByID(uint16_t targetID, const uint8_t* newData, uint8_t newLength) {
  int index = findRowIndexByID(targetID);
  
  if (index == -1) {
    logMsg("\nFehler: ID in QueryBuffer nicht gefunden.", true);
    return false;
  }
  
  if (newLength > MAX_HESP_QUERY_LEN) {
    logMsg("\nFehler: Zeile überschreitet MAX_HESP_QUERY_LEN. " + String(newLength) + " > " + String(MAX_HESP_QUERY_LEN), true);
    return false;
  }

  // RAM-Spiegel aktualisieren
  if ( ramBuffer[index].length == 0 ) ramBuffer[index].length = newLength;
  memcpy(ramBuffer[index].data, newData, ramBuffer[index].length);

  // Sofort dauerhaft im Flash sichern
  saveToFlash();

  return true;
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
};

void setup_wifi() {
  delay(1);
  logMsg("\nSchritt 1: Verbinde mit WLAN (Kiwi)...");
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
    logMsg(" WLAN erfolgreich verbunden!", true);
  }
};

void reconnect() {
    while (!mqttClient.connected()) {
      logMsg("Versuche MQTT Verbindung... ");
      reconnectCounter++;
      String clientId = "ESP32HespGateway-";
      clientId += String(random(0xffff), HEX);
      if (mqttClient.connect(clientId.c_str())) {
        mqttClient.subscribe(topic_send);
        reconnectCounter = 0;
        logMsg("Erfolgreich verbunden & abonniert.", true);
      } else { 
        if ( reconnectCounter > 20 ) {
          // Software-Reset auslösen
          logMsg("zu viele Fehlversuche: reboot", true);
          ESP.restart();
        } else {
          logMsg("Fehlgeschlagen. Erneuter Versuch in 2 Sek.", true);
          delay(2000); 
        }
      }
    }
};
