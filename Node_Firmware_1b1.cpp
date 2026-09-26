/*
 * ============================================================
 * BOSON PLANT MONITOR - PICO 2 W BARCODE NODE
 * ============================================================
 *
 * Purpose
 * -------
 * Receives GS1-128 barcode IDs over the Pico's USB serial
 * interface, packages them as JSON, and transmits them over
 * Wi-Fi to the Boson Plant Monitor engine.
 *
 * Source protocol confirmed from:
 * Barcode_generator_GS1_T-0.py
 *
 * The barcode generator's NORMAL mode sends:
 *
 *     barcode_id + "\n"
 *
 * at the configured baud rate (default 9600).
 *
 * Example received barcode:
 *
 *     (91)001(10)260915AX4(21)0001
 *
 * The Pico sends the engine:
 *
 *     {"device_id":"PICO_001","sequence":1,
 *      "barcode_id":"(91)001(10)260915AX4(21)0001"}
 *
 * The current Boson engine replies:
 *
 *     ACK:1
 *
 * ============================================================
 *
 * Hardware / software target
 * --------------------------
 * Raspberry Pi Pico 2 W
 * Arduino-Pico core by Earle Philhower
 *
 * Serial:
 *   USB CDC Serial
 *
 * Network:
 *   2.4 GHz Wi-Fi
 *
 * Engine:
 *   TCP server
 *
 * ============================================================
 */

#include <WiFi.h>

// ============================================================
// 1. USER CONFIGURATION
// ============================================================

// -------------------- Device identity -----------------------

const char* DEVICE_ID = "PICO_001";

// -------------------- Wi-Fi ---------------------------------

const char* WIFI_SSID     = "ENTER_PLANT_WIFI_SSID";
const char* WIFI_PASSWORD = "ENTER_YOUR_PLANT_WIFI_PASSWORD";

// -------------------- Boson Engine --------------------------

// Use the laptop / plant-server LAN IP.
// Do NOT use 127.0.0.1 here.
// 127.0.0.1 would mean "this Pico".
const char* ENGINE_HOST = "192.168.1.9";
const uint16_t ENGINE_PORT = 5000;

// -------------------- Barcode serial ------------------------

// Barcode_generator_GS1_T-0.py defaults to 9600 baud.
const uint32_t BARCODE_BAUD = 9600;

// USB Serial is used by default on Pico 2 W.
// The barcode generator must open the Pico's USB COM port.

// -------------------- Queue ---------------------------------

const uint8_t BARCODE_QUEUE_SIZE = 32;

// -------------------- Network timing ------------------------

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const unsigned long WIFI_RETRY_INTERVAL_MS  = 5000;

const unsigned long SERVER_CONNECT_TIMEOUT_MS = 5000;

const unsigned long ACK_TIMEOUT_MS = 3000;
const uint8_t MAX_SEND_RETRIES = 3;

// -------------------- Diagnostics ---------------------------

const unsigned long STATUS_PRINT_INTERVAL_MS = 10000;


// ============================================================
// 2. DATA STRUCTURES
// ============================================================

struct BarcodeMessage
{
  String barcodeId;
  uint32_t sequence;
};

BarcodeMessage barcodeQueue[BARCODE_QUEUE_SIZE];

uint8_t queueHead = 0;
uint8_t queueTail = 0;
uint8_t queueCount = 0;


// ============================================================
// 3. GLOBAL STATE
// ============================================================

WiFiClient engineClient;

bool wifiConnected = false;
bool engineConnected = false;

unsigned long lastWiFiAttempt = 0;
unsigned long lastStatusPrint = 0;

uint32_t packetsSent = 0;
uint32_t packetsAcked = 0;
uint32_t packetsFailed = 0;
uint32_t packetsQueued = 0;
uint32_t serialErrors = 0;


// ============================================================
// 4. SERIAL INPUT BUFFER
// ============================================================

String serialBuffer;


// ============================================================
// 5. FORWARD DECLARATIONS
// ============================================================

void printBanner();
void runStartupDiagnostics();

bool connectWiFi();
bool ensureWiFi();

bool connectToEngine();
bool ensureEngine();

void readBarcodeSerial();
void processSerialLine(String line);

bool parseBarcodeSequence(
  const String& barcodeId,
  uint32_t& sequence
);

bool enqueueBarcode(
  const String& barcodeId,
  uint32_t sequence
);

bool dequeueBarcode(BarcodeMessage& message);

bool sendBarcodeMessage(
  const BarcodeMessage& message
);

bool waitForAck(
  uint32_t expectedSequence
);

void closeEngineConnection();

void printStatus();

String buildJsonPacket(
  const BarcodeMessage& message
);


// ============================================================
// 6. SETUP
// ============================================================

void setup()
{
  Serial.begin(BARCODE_BAUD);

  // Give USB CDC time to enumerate.
  delay(1500);

  printBanner();

  runStartupDiagnostics();

  Serial.println();
  Serial.println("========================================");
  Serial.println(" BOSON NODE READY");
  Serial.println(" Waiting for barcode...");
  Serial.println("========================================");
  Serial.println();
}


// ============================================================
// 7. MAIN LOOP
// ============================================================

void loop()
{
  // ----------------------------------------------------------
  // Always service incoming barcode serial data.
  // ----------------------------------------------------------

  readBarcodeSerial();

  // ----------------------------------------------------------
  // Maintain Wi-Fi.
  // ----------------------------------------------------------

  ensureWiFi();

  // ----------------------------------------------------------
  // If there is queued barcode data, try to deliver it.
  // ----------------------------------------------------------

  if (wifiConnected && queueCount > 0)
  {
    if (ensureEngine())
    {
      BarcodeMessage message;

      if (dequeueBarcode(message))
      {
        bool success = sendBarcodeMessage(message);

        if (!success)
        {
          // Put the message back at the front by moving the
          // queue head backwards.
          queueHead =
            (queueHead == 0)
              ? BARCODE_QUEUE_SIZE - 1
              : queueHead - 1;

          barcodeQueue[queueHead] = message;
          queueCount++;

          packetsFailed++;

          closeEngineConnection();
        }
      }
    }
  }

  // ----------------------------------------------------------
  // Periodic diagnostics.
  // ----------------------------------------------------------

  if (millis() - lastStatusPrint >= STATUS_PRINT_INTERVAL_MS)
  {
    lastStatusPrint = millis();
    printStatus();
  }

  delay(2);
}


// ============================================================
// 8. STARTUP DIAGNOSTICS
// ============================================================

void printBanner()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("     BOSON PLANT MONITOR NODE");
  Serial.println("========================================");
  Serial.println();

  Serial.print("DEVICE ID       : ");
  Serial.println(DEVICE_ID);

  Serial.print("FIRMWARE        : 1.0.0");
  Serial.println();

  Serial.print("BARCODE BAUD    : ");
  Serial.println(BARCODE_BAUD);

  Serial.print("ENGINE          : ");
  Serial.print(ENGINE_HOST);
  Serial.print(":");
  Serial.println(ENGINE_PORT);

  Serial.println();
}


void runStartupDiagnostics()
{
  Serial.println("[DIAGNOSTICS]");
  Serial.println();

  // ----------------------------------------------------------
  // MCU sanity
  // ----------------------------------------------------------

  uint64_t checksum = 0;

  for (uint32_t i = 0; i < 100000; i++)
  {
    checksum += i;
  }

  if (checksum == 4999950000ULL)
  {
    Serial.println("[OK] CPU calculation");
  }
  else
  {
    Serial.println("[FAIL] CPU calculation");
  }

  // ----------------------------------------------------------
  // GPIO configuration/readback sanity.
  // This is not a physical PCB trace test.
  // ----------------------------------------------------------

  pinMode(LED_BUILTIN, OUTPUT);

  digitalWrite(LED_BUILTIN, LOW);

  if (digitalRead(LED_BUILTIN) == LOW)
  {
    Serial.println("[OK] GPIO readback");
  }
  else
  {
    Serial.println("[WARN] GPIO readback");
  }

  digitalWrite(LED_BUILTIN, HIGH);

  if (digitalRead(LED_BUILTIN) == HIGH)
  {
    Serial.println("[OK] GPIO readback HIGH");
  }
  else
  {
    Serial.println("[WARN] GPIO readback HIGH");
  }

  digitalWrite(LED_BUILTIN, LOW);

  // ----------------------------------------------------------
  // Wi-Fi
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("[NETWORK]");

  if (connectWiFi())
  {
    Serial.println("[OK] Wi-Fi");
  }
  else
  {
    Serial.println("[WARN] Wi-Fi not connected yet");
    Serial.println("       Node will keep retrying.");
  }

  // ----------------------------------------------------------
  // Engine
  // ----------------------------------------------------------

  if (wifiConnected)
  {
    if (connectToEngine())
    {
      Serial.println("[OK] Boson engine");
      closeEngineConnection();
    }
    else
    {
      Serial.println("[WARN] Boson engine not reachable");
      Serial.println("       Node will keep retrying.");
    }
  }

  Serial.println();
}


// ============================================================
// 9. WI-FI MANAGEMENT
// ============================================================

bool connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    return true;
  }

  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < WIFI_CONNECT_TIMEOUT_MS
  )
  {
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;

    Serial.println("[OK] Wi-Fi connected");

    Serial.print("IP ADDRESS      : ");
    Serial.println(WiFi.localIP());

    Serial.print("GATEWAY         : ");
    Serial.println(WiFi.gatewayIP());

    Serial.print("RSSI            : ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    return true;
  }

  wifiConnected = false;

  Serial.println("[WARN] Wi-Fi connection failed");

  return false;
}


bool ensureWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (!wifiConnected)
    {
      wifiConnected = true;

      Serial.println();
      Serial.println("[OK] Wi-Fi reconnected");

      Serial.print("IP ADDRESS      : ");
      Serial.println(WiFi.localIP());

      Serial.print("RSSI            : ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
    }

    return true;
  }

  wifiConnected = false;

  // Engine TCP connection is no longer valid.
  if (engineConnected)
  {
    closeEngineConnection();
  }

  if (millis() - lastWiFiAttempt >= WIFI_RETRY_INTERVAL_MS)
  {
    lastWiFiAttempt = millis();

    Serial.println();
    Serial.println("[NETWORK] Attempting Wi-Fi reconnect...");

    connectWiFi();
  }

  return false;
}


// ============================================================
// 10. ENGINE TCP MANAGEMENT
// ============================================================

bool connectToEngine()
{
  if (!wifiConnected)
  {
    return false;
  }

  if (engineClient.connected())
  {
    engineConnected = true;
    return true;
  }

  Serial.print("Connecting to Boson engine ");
  Serial.print(ENGINE_HOST);
  Serial.print(":");
  Serial.println(ENGINE_PORT);

  engineClient.stop();

  unsigned long start = millis();

  if (engineClient.connect(ENGINE_HOST, ENGINE_PORT))
  {
    engineConnected = true;

    Serial.println("[OK] Engine connection");

    return true;
  }

  // connect() normally blocks internally, but retain a simple
  // elapsed-time guard for future platform/core behavior.
  while (
    !engineClient.connected() &&
    millis() - start < SERVER_CONNECT_TIMEOUT_MS
  )
  {
    delay(10);
  }

  if (engineClient.connected())
  {
    engineConnected = true;

    Serial.println("[OK] Engine connection");

    return true;
  }

  engineConnected = false;

  Serial.println("[WARN] Engine connection failed");

  return false;
}


bool ensureEngine()
{
  if (!wifiConnected)
  {
    return false;
  }

  if (engineClient.connected())
  {
    engineConnected = true;
    return true;
  }

  engineConnected = false;

  return connectToEngine();
}


void closeEngineConnection()
{
  if (engineClient.connected())
  {
    engineClient.stop();
  }

  engineConnected = false;
}


// ============================================================
// 11. BARCODE SERIAL INPUT
// ============================================================

void readBarcodeSerial()
{
  while (Serial.available() > 0)
  {
    char c = (char)Serial.read();

    // Barcode generator sends newline after each barcode.
    if (c == '\n')
    {
      String line = serialBuffer;

      serialBuffer = "";

      line.trim();

      if (line.length() > 0)
      {
        processSerialLine(line);
      }

      continue;
    }

    // Ignore carriage return. This also supports CRLF.
    if (c == '\r')
    {
      continue;
    }

    // Protect against a corrupted/oversized serial frame.
    if (serialBuffer.length() >= 200)
    {
      serialBuffer = "";

      serialErrors++;

      Serial.println(
        "[ERROR] Barcode serial frame too long"
      );

      continue;
    }

    serialBuffer += c;
  }
}


// ============================================================
// 12. BARCODE VALIDATION / QUEUING
// ============================================================

void processSerialLine(String line)
{
  Serial.println();
  Serial.print("[BARCODE RX] ");
  Serial.println(line);

  // ----------------------------------------------------------
  // Basic GS1 structure check.
  // ----------------------------------------------------------

  if (!line.startsWith("(91)"))
  {
    Serial.println(
      "[ERROR] Barcode does not start with expected (91) AI"
    );

    serialErrors++;

    return;
  }

  // ----------------------------------------------------------
  // Extract sequence from AI (21).
  // ----------------------------------------------------------

  uint32_t sequence = 0;

  if (!parseBarcodeSequence(line, sequence))
  {
    Serial.println(
      "[ERROR] Could not extract sequence from AI (21)"
    );

    serialErrors++;

    return;
  }

  if (sequence < 1 || sequence > 5000)
  {
    Serial.println(
      "[ERROR] Barcode sequence outside expected range 1-5000"
    );

    serialErrors++;

    return;
  }

  // ----------------------------------------------------------
  // Queue.
  // ----------------------------------------------------------

  if (!enqueueBarcode(line, sequence))
  {
    Serial.println(
      "[ERROR] Local barcode queue FULL - barcode rejected"
    );

    serialErrors++;

    return;
  }

  packetsQueued++;

  Serial.print("[QUEUED] Sequence ");
  Serial.print(sequence);
  Serial.print(" | Queue: ");
  Serial.print(queueCount);
  Serial.print("/");
  Serial.println(BARCODE_QUEUE_SIZE);
}


bool parseBarcodeSequence(
  const String& barcodeId,
  uint32_t& sequence
)
{
  const String marker = "(21)";

  int position = barcodeId.indexOf(marker);

  if (position < 0)
  {
    return false;
  }

  position += marker.length();

  String sequenceText = "";

  // The generator creates a 4-digit sequence.
  for (uint8_t i = 0; i < 4; i++)
  {
    int index = position + i;

    if (index >= barcodeId.length())
    {
      return false;
    }

    char c = barcodeId.charAt(index);

    if (c < '0' || c > '9')
    {
      return false;
    }

    sequenceText += c;
  }

  sequence = sequenceText.toInt();

  return true;
}


bool enqueueBarcode(
  const String& barcodeId,
  uint32_t sequence
)
{
  if (queueCount >= BARCODE_QUEUE_SIZE)
  {
    return false;
  }

  barcodeQueue[queueTail].barcodeId = barcodeId;
  barcodeQueue[queueTail].sequence = sequence;

  queueTail =
    (queueTail + 1) % BARCODE_QUEUE_SIZE;

  queueCount++;

  return true;
}


bool dequeueBarcode(BarcodeMessage& message)
{
  if (queueCount == 0)
  {
    return false;
  }

  message = barcodeQueue[queueHead];

  queueHead =
    (queueHead + 1) % BARCODE_QUEUE_SIZE;

  queueCount--;

  return true;
}


// ============================================================
// 13. JSON PACKET CREATION
// ============================================================

String buildJsonPacket(
  const BarcodeMessage& message
)
{
  /*
   * The barcode generator's barcode_id consists of GS1 data
   * such as:
   *
   * (91)001(10)260915AX4(21)0001
   *
   * It does not contain quotes or backslashes, so it is safe
   * for the current JSON protocol.
   */

  String json = "{";

  json += "\"device_id\":\"";
  json += DEVICE_ID;
  json += "\",";

  json += "\"sequence\":";
  json += String(message.sequence);
  json += ",";

  json += "\"barcode_id\":\"";
  json += message.barcodeId;
  json += "\"";

  json += "}";

  return json;
}


// ============================================================
// 14. SEND + ACK
// ============================================================

bool sendBarcodeMessage(
  const BarcodeMessage& message
)
{
  if (!ensureEngine())
  {
    return false;
  }

  String json = buildJsonPacket(message);

  for (
    uint8_t attempt = 1;
    attempt <= MAX_SEND_RETRIES;
    attempt++
  )
  {
    Serial.print("[TX] Attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(MAX_SEND_RETRIES);
    Serial.print(" | Sequence ");
    Serial.println(message.sequence);

    // --------------------------------------------------------
    // Send newline-delimited JSON.
    // --------------------------------------------------------

    engineClient.print(json);
    engineClient.print("\n");

    packetsSent++;

    // --------------------------------------------------------
    // Wait for ACK.
    // --------------------------------------------------------

    if (waitForAck(message.sequence))
    {
      packetsAcked++;

      Serial.print(
        "[ACK] Sequence "
      );

      Serial.print(message.sequence);

      Serial.println(" accepted");

      return true;
    }

    Serial.print(
      "[WARN] ACK timeout for sequence "
    );

    Serial.println(message.sequence);

    // If the TCP connection disappeared, reconnect before
    // retrying.
    if (!engineClient.connected())
    {
      engineConnected = false;

      Serial.println(
        "[NETWORK] Engine connection lost during ACK wait"
      );

      if (!ensureEngine())
      {
        return false;
      }
    }

    delay(100);
  }

  Serial.print(
    "[FAIL] Sequence "
  );

  Serial.print(message.sequence);

  Serial.println(" could not be acknowledged");

  return false;
}


bool waitForAck(
  uint32_t expectedSequence
)
{
  String response = "";

  unsigned long start = millis();

  String expectedAck =
    "ACK:" + String(expectedSequence);

  while (millis() - start < ACK_TIMEOUT_MS)
  {
    while (engineClient.available() > 0)
    {
      char c = (char)engineClient.read();

      if (c == '\n')
      {
        response.trim();

        if (response == expectedAck)
        {
          return true;
        }

        // Ignore unexpected response and continue waiting.
        response = "";

        continue;
      }

      if (c == '\r')
      {
        continue;
      }

      if (response.length() < 64)
      {
        response += c;
      }
    }

    if (!engineClient.connected())
    {
      return false;
    }

    delay(2);
  }

  return false;
}


// ============================================================
// 15. STATUS
// ============================================================

void printStatus()
{
  Serial.println();
  Serial.println("--------------- STATUS ----------------");

  Serial.print("DEVICE          : ");
  Serial.println(DEVICE_ID);

  Serial.print("WIFI            : ");

  if (wifiConnected)
  {
    Serial.print("CONNECTED");

    Serial.print(" | RSSI ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  }
  else
  {
    Serial.println("DISCONNECTED");
  }

  Serial.print("ENGINE          : ");

  if (engineConnected)
  {
    Serial.println("CONNECTED");
  }
  else
  {
    Serial.println("DISCONNECTED");
  }

  Serial.print("QUEUE           : ");
  Serial.print(queueCount);
  Serial.print("/");
  Serial.println(BARCODE_QUEUE_SIZE);

  Serial.print("SENT            : ");
  Serial.println(packetsSent);

  Serial.print("ACKED           : ");
  Serial.println(packetsAcked);

  Serial.print("FAILED          : ");
  Serial.println(packetsFailed);

  Serial.print("SERIAL ERRORS   : ");
  Serial.println(serialErrors);

  Serial.println("---------------------------------------");
}
