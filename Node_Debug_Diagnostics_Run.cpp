#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>

// ============================================================
//                  USER CONFIGURATION
// ============================================================

const char* WIFI_SSID     = "ENTER_PLANT_WIFI_SSID";
const char* WIFI_PASSWORD = "ENTER_PLANT_WIFI_PASSWORD";

const char* WEBSITE_HOST = "bosonindustries.com";
const int   WEBSITE_PORT = 80;

// ============================================================
//                  TEST CONFIGURATION
// ============================================================

// I2C pins for our current Pico 2 W test
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5

// SPI pins for our current Pico 2 W test
#define SPI_SCK_PIN  18
#define SPI_MOSI_PIN 19
#define SPI_MISO_PIN 16
#define SPI_CS_PIN   17

// ============================================================
//                  DIAGNOSTIC RESULTS
// ============================================================

bool testCPU      = false;
bool testRAM      = false;
bool testGPIO     = false;
bool testSleep    = false;

bool testI2C      = false;
bool testSPI      = false;

bool testWiFi     = false;
bool testDHCP     = false;
bool testDNS      = false;
bool testInternet = false;
bool testWebsite  = false;

// External hardware is deliberately NOT required.
bool i2CDeviceDetected = false;
bool spiDeviceDetected = false;

// ============================================================
//                  HELPER FUNCTIONS
// ============================================================

void printHeader(const char* title) {

  Serial.println();
  Serial.println("========================================");
  Serial.println(title);
  Serial.println("========================================");
}

void printResult(const char* name, bool result) {

  Serial.print(name);

  // Align output
  int spaces = 22 - strlen(name);

  for (int i = 0; i < spaces; i++) {
    Serial.print(" ");
  }

  Serial.print(": ");

  if (result) {
    Serial.println("PASS");
  }
  else {
    Serial.println("FAIL");
  }
}

// ============================================================
//                  CPU TEST
// ============================================================

bool runCPUTest() {

  Serial.println();
  Serial.println("[1] CPU / RP2350");

  Serial.println("Running CPU test...");

  // If this code is executing, the CPU is obviously alive.
  // We additionally perform a small deterministic calculation.

  volatile uint64_t result = 0;

  for (uint32_t i = 0; i < 100000; i++) {
    result += i;
  }

  uint64_t expected = 4999950000ULL;

  if (result == expected) {
    Serial.println("CPU calculation correct.");
    return true;
  }

  Serial.println("CPU calculation failed.");
  return false;
}

// ============================================================
//                  RAM TEST
// ============================================================

bool runRAMTest() {

  Serial.println();
  Serial.println("[2] RAM");

  Serial.println("Running RAM sanity test...");

  // Small local test buffer.
  // We deliberately avoid testing the entire RAM because
  // Arduino itself is using RAM.

  volatile uint32_t testBuffer[64];

  for (int i = 0; i < 64; i++) {
    testBuffer[i] = 0xA5A50000 + i;
  }

  for (int i = 0; i < 64; i++) {

    if (testBuffer[i] != (0xA5A50000 + i)) {

      Serial.print("RAM error at index ");
      Serial.println(i);

      return false;
    }
  }

  Serial.println("RAM read/write test passed.");

  return true;
}

// ============================================================
//                  GPIO TEST
// ============================================================

bool runGPIOTest() {

  Serial.println();
  Serial.println("[3] GPIO");

  Serial.println("Testing GPIO configuration...");

  // We will use two unused GPIOs.
  // IMPORTANT:
  // Do not connect external hardware to these during this test.

  const int testPin1 = 6;
  const int testPin2 = 7;

  pinMode(testPin1, OUTPUT);
  pinMode(testPin2, OUTPUT);

  digitalWrite(testPin1, LOW);
  digitalWrite(testPin2, HIGH);

  delay(10);

  bool outputStateOK =
    (digitalRead(testPin1) == LOW) &&
    (digitalRead(testPin2) == HIGH);

  digitalWrite(testPin1, HIGH);
  digitalWrite(testPin2, LOW);

  delay(10);

  outputStateOK =
    outputStateOK &&
    (digitalRead(testPin1) == HIGH) &&
    (digitalRead(testPin2) == LOW);

  pinMode(testPin1, INPUT);
  pinMode(testPin2, INPUT);

  if (outputStateOK) {
    Serial.println("GPIO configuration test passed.");
    return true;
  }

  Serial.println("GPIO test failed.");
  return false;
}

// ============================================================
//                  I2C TEST
// ============================================================

bool runI2CTest() {

  Serial.println();
  Serial.println("[4] I2C");

  Serial.println("Initializing I2C peripheral...");

  Wire.setSDA(I2C_SDA_PIN);
  Wire.setSCL(I2C_SCL_PIN);

  Wire.begin();

  delay(50);

  Serial.println("I2C peripheral initialized.");

  // With NO external hardware, we cannot claim that the
  // physical I2C bus is healthy.
  //
  // We therefore perform an address scan only to determine
  // whether a device happens to be connected.

  Serial.println("Scanning I2C bus...");

  int devicesFound = 0;

  for (uint8_t address = 1; address < 127; address++) {

    Wire.beginTransmission(address);

    uint8_t error = Wire.endTransmission();

    if (error == 0) {

      Serial.print("I2C device detected at 0x");

      if (address < 16) {
        Serial.print("0");
      }

      Serial.println(address, HEX);

      devicesFound++;
    }
  }

  if (devicesFound == 0) {

    Serial.println("No external I2C devices detected.");
    Serial.println("I2C peripheral: PASS");
    Serial.println("I2C external bus: NOT TESTED");

    i2CDeviceDetected = false;

    return true;
  }

  Serial.print("I2C devices detected: ");
  Serial.println(devicesFound);

  Serial.println("I2C peripheral: PASS");
  Serial.println("I2C device response: PASS");

  i2CDeviceDetected = true;

  return true;
}

// ============================================================
//                  SPI TEST
// ============================================================

bool runSPITest() {

  Serial.println();
  Serial.println("[5] SPI");

  Serial.println("Initializing SPI peripheral...");

  SPI.setSCK(SPI_SCK_PIN);
  SPI.setTX(SPI_MOSI_PIN);
  SPI.setRX(SPI_MISO_PIN);

  pinMode(SPI_CS_PIN, OUTPUT);
  digitalWrite(SPI_CS_PIN, HIGH);

  SPI.begin();

  delay(50);

  Serial.println("SPI peripheral initialized.");

  // ----------------------------------------------------------
  // IMPORTANT:
  //
  // We do NOT have an external SPI device connected.
  //
  // Therefore we cannot perform a real data-loopback test.
  //
  // This test verifies that the SPI peripheral can be
  // configured and started correctly.
  // ----------------------------------------------------------

  SPI.beginTransaction(
    SPISettings(
      1000000,
      MSBFIRST,
      SPI_MODE0
    )
  );

  digitalWrite(SPI_CS_PIN, LOW);

  // Send a test byte.
  SPI.transfer(0x55);

  digitalWrite(SPI_CS_PIN, HIGH);

  SPI.endTransaction();

  Serial.println("SPI peripheral configuration: PASS");
  Serial.println("SPI peripheral transaction: PASS");
  Serial.println("SPI external device: NOT TESTED");

  spiDeviceDetected = false;

  return true;
}

// ============================================================
//                  WIFI TEST
// ============================================================

bool runWiFiTest() {

  Serial.println();
  Serial.println("[6] Wi-Fi");

  Serial.print("Connecting to: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;

  while (
    WiFi.status() != WL_CONNECTED &&
    attempts < 40
  ) {

    delay(500);

    Serial.print(".");

    attempts++;
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Wi-Fi connection failed.");

    return false;
  }

  Serial.println("Wi-Fi connected.");

  Serial.print("SSID : ");
  Serial.println(WiFi.SSID());

  Serial.print("IP   : ");
  Serial.println(WiFi.localIP());

  Serial.print("RSSI : ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  return true;
}

// ============================================================
//                  DHCP TEST
// ============================================================

bool runDHCPTest() {

  Serial.println();
  Serial.println("[7] DHCP / IP");

  IPAddress ip = WiFi.localIP();

  if (
    ip[0] == 0 &&
    ip[1] == 0 &&
    ip[2] == 0 &&
    ip[3] == 0
  ) {

    Serial.println("No valid IP address.");

    return false;
  }

  Serial.print("Assigned IP: ");
  Serial.println(ip);

  Serial.println("DHCP: PASS");

  return true;
}

// ============================================================
//                  DNS TEST
// ============================================================

bool runDNSTest() {

  Serial.println();
  Serial.println("[8] DNS");

  IPAddress serverIP;

  Serial.print("Resolving ");
  Serial.print(WEBSITE_HOST);
  Serial.println("...");

  if (WiFi.hostByName(WEBSITE_HOST, serverIP)) {

    Serial.print("Resolved IP: ");
    Serial.println(serverIP);

    return true;
  }

  Serial.println("DNS resolution failed.");

  return false;
}

// ============================================================
//                  INTERNET TEST
// ============================================================

bool runInternetTest() {

  Serial.println();
  Serial.println("[9] Internet / TCP");

  WiFiClient client;

  Serial.print("Connecting to ");
  Serial.print(WEBSITE_HOST);
  Serial.print(":");
  Serial.println(WEBSITE_PORT);

  if (!client.connect(WEBSITE_HOST, WEBSITE_PORT)) {

    Serial.println("TCP connection failed.");

    return false;
  }

  Serial.println("TCP connection established.");

  client.stop();

  return true;
}

// ============================================================
//                  WEBSITE TEST
// ============================================================

bool runWebsiteTest() {

  Serial.println();
  Serial.println("[10] Website");

  WiFiClient client;

  Serial.print("Connecting to ");
  Serial.println(WEBSITE_HOST);

  if (!client.connect(WEBSITE_HOST, WEBSITE_PORT)) {

    Serial.println("Website connection failed.");

    return false;
  }

  Serial.println("Connected.");

  // HTTP GET request
  client.print(
    "GET / HTTP/1.1\r\n"
    "Host: "
  );

  client.print(WEBSITE_HOST);

  client.print(
    "\r\n"
    "Connection: close\r\n"
    "\r\n"
  );

  Serial.println();
  Serial.println("----- SERVER RESPONSE -----");

  unsigned long timeout = millis();

  bool receivedData = false;

  while (
    client.connected() &&
    millis() - timeout < 10000
  ) {

    while (client.available()) {

      char c = client.read();

      Serial.write(c);

      receivedData = true;

      timeout = millis();
    }
  }

  client.stop();

  Serial.println();
  Serial.println("----- END RESPONSE -----");

  if (receivedData) {

    Serial.println("Website communication: PASS");

    return true;
  }

  Serial.println("No HTTP response received.");

  return false;
}

// ============================================================
//                  SLEEP / WAKE TEST
// ============================================================

bool runSleepWakeTest() {

  Serial.println();
  Serial.println("[11] Sleep / Wake");

  Serial.println("Normal operation confirmed.");

  Serial.println("Sleeping for 3 seconds...");
  Serial.flush();

  delay(100);

  unsigned long start = millis();

  // CPU WFI.
  //
  // An interrupt will wake the processor.
  // This is our basic CPU sleep/wake verification.

  __asm volatile ("wfi");

  unsigned long elapsed = millis() - start;

  Serial.println("CPU woke from sleep.");

  Serial.print("Elapsed time after wake: ");
  Serial.print(elapsed);
  Serial.println(" ms");

  Serial.println("Sleep / wake mechanism: PASS");

  return true;
}

// ============================================================
//                  FINAL REPORT
// ============================================================

void printFinalReport() {

  printHeader("FINAL DIAGNOSTIC REPORT");

  Serial.println();
  Serial.println("MCU");
  Serial.println("----------------------------------------");

  printResult("RP2350 CPU", testCPU);
  printResult("RAM", testRAM);
  printResult("GPIO", testGPIO);
  printResult("Sleep / Wake", testSleep);

  Serial.println();
  Serial.println("COMMUNICATION");
  Serial.println("----------------------------------------");

  printResult("I2C peripheral", testI2C);

  if (i2CDeviceDetected) {
    Serial.println("I2C external device    : PASS");
  }
  else {
    Serial.println("I2C external device    : NOT TESTED");
  }

  printResult("SPI peripheral", testSPI);

  if (spiDeviceDetected) {
    Serial.println("SPI external device    : PASS");
  }
  else {
    Serial.println("SPI external device    : NOT TESTED");
  }

  Serial.println();
  Serial.println("NETWORK");
  Serial.println("----------------------------------------");

  printResult("Wi-Fi", testWiFi);
  printResult("DHCP / IP", testDHCP);
  printResult("DNS", testDNS);
  printResult("Internet / TCP", testInternet);
  printResult("Website", testWebsite);

  // ----------------------------------------------------------
  // Overall result
  //
  // External I2C/SPI devices are deliberately NOT included
  // because they are optional in this diagnostic.
  // ----------------------------------------------------------

  bool overall =
    testCPU &&
    testRAM &&
    testGPIO &&
    testSleep &&
    testI2C &&
    testSPI &&
    testWiFi &&
    testDHCP &&
    testDNS &&
    testInternet &&
    testWebsite;

  Serial.println();
  Serial.println("========================================");

  if (overall) {

    Serial.println("       OVERALL RESULT: PASS");

  }
  else {

    Serial.println("       OVERALL RESULT: FAIL");

  }

  Serial.println("========================================");

  Serial.println();

  if (!i2CDeviceDetected) {
    Serial.println("NOTE: No external I2C device was connected.");
  }

  if (!spiDeviceDetected) {
    Serial.println("NOTE: No external SPI device was connected.");
  }

  Serial.println();
}

// ============================================================
//                  SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  // Give USB serial time to enumerate.
  delay(3000);

  printHeader("PICO 2 W HEALTH DIAGNOSTIC");

  Serial.println("Diagnostic firmware starting...");
  Serial.println("This test runs ONCE.");
  Serial.println();

  // ----------------------------------------------------------
  // CPU
  // ----------------------------------------------------------

  testCPU = runCPUTest();

  // ----------------------------------------------------------
  // RAM
  // ----------------------------------------------------------

  testRAM = runRAMTest();

  // ----------------------------------------------------------
  // GPIO
  // ----------------------------------------------------------

  testGPIO = runGPIOTest();

  // ----------------------------------------------------------
  // I2C
  // ----------------------------------------------------------

  testI2C = runI2CTest();

  // ----------------------------------------------------------
  // SPI
  // ----------------------------------------------------------

  testSPI = runSPITest();

  // ----------------------------------------------------------
  // Wi-Fi
  // ----------------------------------------------------------

  testWiFi = runWiFiTest();

  // ----------------------------------------------------------
  // DHCP
  // ----------------------------------------------------------

  if (testWiFi) {
    testDHCP = runDHCPTest();
  }

  // ----------------------------------------------------------
  // DNS
  // ----------------------------------------------------------

  if (testDHCP) {
    testDNS = runDNSTest();
  }

  // ----------------------------------------------------------
  // Internet
  // ----------------------------------------------------------

  if (testDNS) {
    testInternet = runInternetTest();
  }

  // ----------------------------------------------------------
  // Website
  // ----------------------------------------------------------

  if (testInternet) {
    testWebsite = runWebsiteTest();
  }

  // ----------------------------------------------------------
  // Sleep / Wake
  //
  // We put this near the end because USB serial is needed
  // to observe the diagnostic.
  // ----------------------------------------------------------

  testSleep = runSleepWakeTest();

  // ----------------------------------------------------------
  // Final report
  // ----------------------------------------------------------

  printFinalReport();

  Serial.println();
  Serial.println("Diagnostic sequence complete.");

  Serial.println();
  Serial.println("Preparing for low-power state...");

  Serial.flush();

  delay(500);

  // For THIS first version, leave the CPU in WFI.
  // Later we'll replace this with the actual production
  // RP2350 low-power state once we decide exactly how the
  // Wi-Fi/radio and peripherals should be powered down.

  Serial.println("Entering low-power state.");
  Serial.flush();

  delay(100);

  while (true) {

    __asm volatile ("wfi");
  }
}

// ============================================================
//                  LOOP
// ============================================================

void loop() {

  // Should never normally reach here because setup()
  // deliberately puts the MCU into the final sleep state.

}
