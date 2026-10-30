#include "tools.h" // Bindet die Definitionen ein

String webLogQueue[LOG_QUEUE_SIZE];


// Hilfsfunktion: Wandelt ein einzelnes Hex-Zeichen in eine Zahl um
byte hexCharToByte(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
};

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
};


// Variante 2: Für C-Strings / Text in Anführungszeichen "" (Wichtig für setup_wifi!)
void logMsg(const char* msg, bool newLine) {
  // Wandelt den C-String einfach in das String-Objekt um und ruft Variante 1 auf
  logMsg(String(msg), newLine);
};



void handleWebLogQueue() {
  // Wenn Core 1 Textnachrichten hinterlassen hat, funken wir sie JETZT über Core 0 ins WLAN
  while (logOut != logIn) {
    webSocket.broadcastTXT(webLogQueue[logOut]);
    logOut = (logOut + 1) % LOG_QUEUE_SIZE;
  }
};

// Funktion zur Berechnung der ID aus Byte 4 und 5 (0-indiziert: Byte [3] und [4])
uint16_t getID(const uint8_t* data) {
  return ((uint16_t)data[4] << 8) | data[3];
}

// Lädt Daten aus dem Dateisystem oder initialisiert sie neu
void initStorage() {
  if (!LittleFS.begin(true)) { // true = Formatiert das Dateisystem, falls es fehlschlägt
    logMsg("LittleFS Mount fehlgeschlagen");
    return;
  }

  // Prüfen, ob die Datei bereits existiert
  if (!LittleFS.exists(FILENAME)) {
    logMsg("Keine bestehenden Daten gefunden. Schreibe Initialdaten...");
    
    // RAM-Buffer aus dem 2D-Array füllen
    for (int i = 0; i < NUM_CONTROLLER_QUERIES; i++) {
      ramBuffer[i].length = controllerQueriesData[i][0];
      for (int j = 0; j < ramBuffer[i].length; j++) {
        ramBuffer[i].data[j] = controllerQueriesData[i][j + 1];
      }
    }
    saveToFlash();
  } else {
    // Daten aus dem Flash in den RAM-Spiegel laden
    File file = LittleFS.open(FILENAME, "r");
    if (file) {
      file.read((uint8_t*)ramBuffer, sizeof(ramBuffer));
      file.close();
      logMsg("Daten erfolgreich aus Flash geladen.");
    }
  }
}

// Sichert den aktuellen RAM-Spiegel dauerhaft in Datei
void saveToFlash() {
  File file = LittleFS.open(FILENAME, "w");
  if (file) {
    file.write((uint8_t*)ramBuffer, sizeof(ramBuffer));
    file.close();
    logMsg("Daten dauerhaft in Datei gespeichert.");
  } else {
    logMsg("Fehler beim Schreiben der Datei.");
  }
}

// METHODE 1: Ultraschneller Zugriff / Suche über die ID (Gibt Zeilen-Index zurück oder -1)
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

// METHODE 2: Einzelne Zeile abändern und dauerhaft speichern
bool updateRowByID(uint16_t targetID, const uint8_t* newData, uint8_t newLength) {
  int index = findRowIndexByID(targetID);
  
  if (index == -1) {
    logMsg("Fehler: ID nicht gefunden.");
    return false;
  }
  
  if (newLength > MAX_HESP_QUERY_LEN) {
    logMsg("Fehler: Zeile überschreitet MAX_HESP_QUERY_LEN.");
    return false;
  }

  // RAM-Spiegel aktualisieren
  ramBuffer[index].length = newLength;
  memcpy(ramBuffer[index].data, newData, newLength);

  // Sofort dauerhaft im Flash sichern
  saveToFlash();
  return true;
}
