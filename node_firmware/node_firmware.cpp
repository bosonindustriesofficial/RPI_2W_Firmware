/*
 * BOSON PLANT MONITOR - PICO 2 W BARCODE NODE (Pico SDK)
 *
 * Faithful port of Node_Firmware_1b1.cpp with USB host HID scanner on micro USB.
 * Log output on UART (USB CDC disabled while port is USB host).
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#if BOSON_USB_HID_HOST
#include "tusb.h"
#endif

#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
#include "lwip/netif.h"

#ifndef BOSON_USB_HID_HOST
#define BOSON_USB_HID_HOST 1
#endif

// ============================================================
// 1. USER CONFIGURATION
// ============================================================

const char* DEVICE_ID = "PICO_001";

const char* WIFI_SSID = "ENTER_PLANT_WIFI_SSID";
const char* WIFI_PASSWORD = "ENTER_YOUR_PLANT_WIFI_PASSWORD";

const char* ENGINE_HOST = "192.168.1.9";
const uint16_t ENGINE_PORT = 5000;

const uint32_t BARCODE_BAUD = 9600;

const uint8_t BARCODE_QUEUE_SIZE = 32;

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
const unsigned long SERVER_CONNECT_TIMEOUT_MS = 5000;
const unsigned long ACK_TIMEOUT_MS = 3000;
const uint8_t MAX_SEND_RETRIES = 3;
const unsigned long STATUS_PRINT_INTERVAL_MS = 10000;
const unsigned long HEARTBEAT_INTERVAL_MS = 5000;

// Ignore brief link / status glitches before clearing Wi-Fi (USB host + SDIO).
const unsigned long WIFI_LINK_LOST_CONFIRM_MS = 4000;

const size_t BARCODE_ID_MAX = 200;
const size_t SERIAL_LINE_MAX = 200;

// ============================================================
// 2. DATA STRUCTURES
// ============================================================

struct BarcodeMessage
{
  char barcodeId[BARCODE_ID_MAX + 1];
  uint32_t sequence;
};

static BarcodeMessage barcodeQueue[BARCODE_QUEUE_SIZE];
static uint8_t queueHead = 0;
static uint8_t queueTail = 0;
static uint8_t queueCount = 0;

// ============================================================
// 3. GLOBAL STATE
// ============================================================

static struct tcp_pcb* engine_pcb = nullptr;
static volatile bool engine_tcp_alive = false;

static bool wifiConnected = false;
static bool engineConnected = false;

static unsigned long lastWiFiAttempt = 0;
static unsigned long lastStatusPrint = 0;
static unsigned long wifi_unstable_since_ms = 0;
static unsigned long lastHeartbeatMs = 0;

static uint32_t packetsSent = 0;
static uint32_t packetsAcked = 0;
static uint32_t packetsFailed = 0;
static uint32_t packetsQueued = 0;
static uint32_t serialErrors = 0;

// ============================================================
// 4. SERIAL INPUT BUFFER (legacy path preserved in source)
// ============================================================

static char serialBuffer[SERIAL_LINE_MAX + 1];
static size_t serialBufferLen = 0;

// ============================================================
// ENGINE TCP RX (ACK lines)
// ============================================================

static char ackLineBuf[64];
static size_t ackLineLen = 0;
static volatile bool ackLineComplete = false;
static char ackLineStorage[64];

// ============================================================
// HID BARCODE INPUT
// ============================================================

static char hidBarcode[SERIAL_LINE_MAX + 1];
static uint16_t hidBarcodeLen = 0;
static bool hidBarcodeOverflow = false;
#if BOSON_USB_HID_HOST
static hid_keyboard_report_t hidPreviousReport = {};
#endif
static volatile bool scannerDetected = false;
static volatile bool fatal_error = false;

// ============================================================
// STATUS LED (barcodescannertest patterns + breathe / events)
// ============================================================

static const uint32_t LED_PULSE_MS = 120;
static const uint32_t LED_PAUSE_MS = 900;
static const uint32_t LED_BREATHE_PERIOD_MS = 2400;
static const uint32_t LED_SOFT_PWM_TICK_MS = 16;

enum LedOverlayType : uint8_t
{
  LED_OVERLAY_NONE = 0,
  LED_OVERLAY_ACK,
  LED_OVERLAY_QUEUED,
  LED_OVERLAY_FAIL
};

static LedOverlayType led_overlay_type = LED_OVERLAY_NONE;
static uint32_t led_overlay_until_ms = 0;
static uint8_t led_overlay_step = 0;
static uint32_t led_overlay_next_ms = 0;

static uint8_t led_active_pattern = 0;
static uint8_t led_pattern_step = 0;
static uint32_t led_pattern_next_ms = 0;

static bool engine_client_connected();

// ============================================================
// UTILITIES
// ============================================================

static unsigned long millis()
{
  return to_ms_since_boot(get_absolute_time());
}

static void delay_ms(unsigned long ms)
{
  sleep_ms(ms);
}

static bool str_starts_with(const char* s, const char* prefix)
{
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void trim_whitespace(char* s)
{
  if (s == nullptr || s[0] == '\0')
  {
    return;
  }

  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1]))
  {
    s[--len] = '\0';
  }

  size_t start = 0;
  while (s[start] != '\0' && isspace((unsigned char)s[start]))
  {
    start++;
  }

  if (start > 0)
  {
    memmove(s, s + start, strlen(s + start) + 1);
  }
}

static int wifi_link_status_raw()
{
  return cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
}

static bool wifi_has_ipv4()
{
  bool has_ip = false;

  cyw43_arch_lwip_begin();
  struct netif* n = &cyw43_state.netif[CYW43_ITF_STA];
  has_ip = !ip4_addr_isany(netif_ip4_addr(n));
  cyw43_arch_lwip_end();

  return has_ip;
}

static bool wifi_link_usable()
{
  const int status = wifi_link_status_raw();

  if (status == CYW43_LINK_FAIL || status == CYW43_LINK_DOWN)
  {
    return false;
  }

  if (status >= CYW43_LINK_JOIN)
  {
    return wifi_has_ipv4();
  }

  return false;
}

static void led_write(bool on)
{
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
}

static bool led_read()
{
  return cyw43_arch_gpio_get(CYW43_WL_GPIO_LED_PIN) != 0;
}

static void led_notify_ack()
{
  led_overlay_type = LED_OVERLAY_ACK;
  led_overlay_step = 0;
  led_overlay_next_ms = millis();
  led_overlay_until_ms = millis() + 350;
}

static void led_notify_queued()
{
  led_overlay_type = LED_OVERLAY_QUEUED;
  led_overlay_step = 0;
  led_overlay_next_ms = millis();
  led_overlay_until_ms = millis() + 500;
}

static void led_notify_send_fail()
{
  led_overlay_type = LED_OVERLAY_FAIL;
  led_overlay_step = 0;
  led_overlay_next_ms = millis();
  led_overlay_until_ms = millis() + 900;
}

static bool led_idle_ready_for_breathe()
{
  return (
    !fatal_error &&
    wifiConnected &&
    scannerDetected &&
    queueCount == 0 &&
    led_overlay_type == LED_OVERLAY_NONE
  );
}

static void led_apply_soft_pwm(uint8_t duty_percent)
{
  if (duty_percent > 100)
  {
    duty_percent = 100;
  }

  uint32_t tick = millis() % LED_SOFT_PWM_TICK_MS;
  uint32_t on_threshold = (LED_SOFT_PWM_TICK_MS * duty_percent) / 100;
  led_write(tick < on_threshold);
}

static void led_update_breathe(uint32_t now)
{
  uint32_t phase = now % LED_BREATHE_PERIOD_MS;
  uint32_t half = LED_BREATHE_PERIOD_MS / 2;
  uint8_t duty;

  if (phase < half)
  {
    duty = (uint8_t)((phase * 100) / half);
  }
  else
  {
    duty = (uint8_t)(((LED_BREATHE_PERIOD_MS - phase) * 100) / half);
  }

  if (duty < 8)
  {
    duty = 8;
  }

  led_apply_soft_pwm(duty);
}

static bool led_update_overlay(uint32_t now)
{
  if (led_overlay_type == LED_OVERLAY_NONE)
  {
    return false;
  }

  if (now > led_overlay_until_ms)
  {
    led_overlay_type = LED_OVERLAY_NONE;
    return false;
  }

  if (led_overlay_type == LED_OVERLAY_ACK)
  {
    led_write(true);
    return true;
  }

  if (now < led_overlay_next_ms)
  {
    return true;
  }

  if (led_overlay_type == LED_OVERLAY_QUEUED)
  {
    if (led_overlay_step < 4)
    {
      led_write((led_overlay_step % 2) == 0);
      led_overlay_step++;
      led_overlay_next_ms = now + 80;
    }
    else
    {
      led_write(false);
    }
    return true;
  }

  if (led_overlay_type == LED_OVERLAY_FAIL)
  {
    if (led_overlay_step < 10)
    {
      led_write((led_overlay_step % 2) == 0);
      led_overlay_step++;
      led_overlay_next_ms = now + 70;
    }
    else
    {
      led_write(false);
    }
    return true;
  }

  return false;
}

static void led_update_pulse_pattern(uint32_t now)
{
  uint8_t pattern;

  if (fatal_error)
  {
    pattern = 5;
  }
  else if (queueCount > 0 && wifiConnected)
  {
    pattern = 3;
  }
  else if (scannerDetected)
  {
    pattern = 4;
  }
  else if (engine_client_connected() || engineConnected)
  {
    pattern = 3;
  }
  else if (wifiConnected)
  {
    pattern = 2;
  }
  else
  {
    pattern = 1;
  }

  if (pattern != led_active_pattern)
  {
    led_active_pattern = pattern;
    led_pattern_step = 0;
    led_pattern_next_ms = now;
    led_write(false);
  }

  if (now < led_pattern_next_ms)
  {
    return;
  }

  uint32_t pulse_gap = (queueCount > 0 && wifiConnected) ? 80 : LED_PULSE_MS;

  if (led_pattern_step < pattern * 2)
  {
    led_write((led_pattern_step % 2) == 0);
    led_pattern_step++;
    led_pattern_next_ms = now + pulse_gap;
  }
  else
  {
    led_write(false);
    led_pattern_step = 0;
    led_pattern_next_ms = now + LED_PAUSE_MS;
  }
}

static void update_status_led()
{
  uint32_t now = millis();

  if (led_update_overlay(now))
  {
    return;
  }

  if (led_idle_ready_for_breathe())
  {
    led_update_breathe(now);
    return;
  }

  led_update_pulse_pattern(now);
}

// ============================================================
// 5. FORWARD DECLARATIONS
// ============================================================

static void printBanner();
static void runStartupDiagnostics();

static bool connectWiFi();
static bool ensureWiFi();

static bool connectToEngine();
static bool ensureEngine();
static void closeEngineConnection();

static void readBarcodeSerial();
static void processSerialLine(char* line);

static bool normalizeCompactBarcode(char* line, size_t cap);
static void finishHidBarcode();

static bool parseBarcodeSequence(const char* barcodeId, uint32_t& sequence);
static bool enqueueBarcode(const char* barcodeId, uint32_t sequence);
static bool dequeueBarcode(BarcodeMessage& message);

static bool buildJsonPacket(const BarcodeMessage& message, char* out, size_t outLen);
static bool sendBarcodeMessage(const BarcodeMessage& message);
static bool waitForAck(uint32_t expectedSequence);
static void sendEngineHeartbeatIfDue();

static void printStatus();

static void initUsbHost();
static void update_status_led();

// ============================================================
// ENGINE TCP CALLBACKS
// ============================================================

extern "C" void engine_tcp_err_cb(void* arg, err_t err)
{
  (void)arg;
  (void)err;
  engine_pcb = nullptr;
  engine_tcp_alive = false;
  engineConnected = false;
}

extern "C" err_t engine_tcp_recv_cb(
  void* arg,
  struct tcp_pcb* pcb,
  struct pbuf* packet,
  err_t err
)
{
  (void)arg;
  (void)err;

  if (packet == nullptr)
  {
    engine_tcp_alive = false;
    engineConnected = false;
    return ERR_OK;
  }

  char* data = (char*)packet->payload;
  uint16_t len = packet->len;

  for (uint16_t i = 0; i < len; i++)
  {
    char c = data[i];

    if (c == '\n')
    {
      ackLineBuf[ackLineLen] = '\0';
      trim_whitespace(ackLineBuf);
      strncpy(ackLineStorage, ackLineBuf, sizeof(ackLineStorage) - 1);
      ackLineStorage[sizeof(ackLineStorage) - 1] = '\0';
      ackLineComplete = true;
      ackLineLen = 0;
      continue;
    }

    if (c == '\r')
    {
      continue;
    }

    if (ackLineLen < sizeof(ackLineBuf) - 1)
    {
      ackLineBuf[ackLineLen++] = c;
    }
  }

  tcp_recved(pcb, packet->tot_len);
  pbuf_free(packet);
  return ERR_OK;
}

extern "C" err_t engine_tcp_connect_cb(
  void* arg,
  struct tcp_pcb* pcb,
  err_t err
)
{
  (void)arg;

  if (err != ERR_OK)
  {
    engine_tcp_alive = false;
    engineConnected = false;
    return err;
  }

  engine_tcp_alive = true;
  engineConnected = true;
  tcp_recv(pcb, engine_tcp_recv_cb);
  return ERR_OK;
}

// ============================================================
// 6. SETUP
// ============================================================

static void setup()
{
  printBanner();
  runStartupDiagnostics();

  printf("\n");
  printf("========================================\n");
  printf(" BOSON NODE READY\n");
  printf(" Waiting for barcode (USB HID host)...\n");
  printf("========================================\n");
  printf("\n");
}

// ============================================================
// 7. MAIN LOOP
// ============================================================

static void loop()
{
  // Legacy USB CDC barcode emulator — inactive while micro USB is host.
  // readBarcodeSerial();

  ensureWiFi();

  if (wifiConnected)
  {
    sendEngineHeartbeatIfDue();
  }

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
          queueHead =
            (queueHead == 0)
              ? BARCODE_QUEUE_SIZE - 1
              : queueHead - 1;

          barcodeQueue[queueHead] = message;
          queueCount++;

          packetsFailed++;

          led_notify_send_fail();
          closeEngineConnection();
        }
      }
    }
  }

  if (millis() - lastStatusPrint >= STATUS_PRINT_INTERVAL_MS)
  {
    lastStatusPrint = millis();
    printStatus();
  }

  update_status_led();
  delay_ms(2);
}

// ============================================================
// 8. STARTUP DIAGNOSTICS
// ============================================================

static void printBanner()
{
  printf("\n");
  printf("========================================\n");
  printf("     BOSON PLANT MONITOR NODE\n");
  printf("========================================\n");
  printf("\n");

  printf("DEVICE ID       : %s\n", DEVICE_ID);
  printf("FIRMWARE        : 1.0.0-sdk\n");
  printf("BARCODE BAUD    : %lu (legacy serial ref)\n", (unsigned long)BARCODE_BAUD);
  printf("BARCODE INPUT   : USB HID host (micro USB OTG)\n");
  printf("ENGINE          : %s:%u\n", ENGINE_HOST, ENGINE_PORT);
  printf("\n");
}

static void runStartupDiagnostics()
{
  printf("[DIAGNOSTICS]\n\n");

  uint64_t checksum = 0;
  for (uint32_t i = 0; i < 100000; i++)
  {
    checksum += i;
  }

  if (checksum == 4999950000ULL)
  {
    printf("[OK] CPU calculation\n");
  }
  else
  {
    printf("[FAIL] CPU calculation\n");
  }

  led_write(false);
  if (!led_read())
  {
    printf("[OK] GPIO readback\n");
  }
  else
  {
    printf("[WARN] GPIO readback\n");
  }

  led_write(true);
  if (led_read())
  {
    printf("[OK] GPIO readback HIGH\n");
  }
  else
  {
    printf("[WARN] GPIO readback HIGH\n");
  }

  led_write(false);

  printf("\n[NETWORK]\n");

  if (connectWiFi())
  {
    printf("[OK] Wi-Fi\n");
  }
  else
  {
    printf("[WARN] Wi-Fi not connected yet\n");
    printf("       Node will keep retrying.\n");
  }

  if (wifiConnected)
  {
    if (connectToEngine())
    {
      printf("[OK] Boson engine\n");
      closeEngineConnection();
    }
    else
    {
      printf("[WARN] Boson engine not reachable\n");
      printf("       Node will keep retrying.\n");
    }
  }

  printf("\n");
}

// ============================================================
// 9. WI-FI MANAGEMENT
// ============================================================

static void wifi_print_status_line()
{
  cyw43_arch_lwip_begin();
  struct netif* n = &cyw43_state.netif[CYW43_ITF_STA];
  printf("IP ADDRESS      : %s\n", ip4addr_ntoa(netif_ip4_addr(n)));
  printf("GATEWAY         : %s\n", ip4addr_ntoa(netif_ip4_gw(n)));
  cyw43_arch_lwip_end();

  int32_t rssi = 0;
  cyw43_wifi_get_rssi(&cyw43_state, &rssi);
  printf("RSSI            : %ld dBm\n", (long)rssi);
}

static bool connectWiFi()
{
  if (wifi_link_usable())
  {
    wifiConnected = true;
    wifi_unstable_since_ms = 0;
    return true;
  }

  const int status = wifi_link_status_raw();

  // Associated but DHCP still running — wait without tearing down the link.
  if (status >= CYW43_LINK_JOIN && !wifi_has_ipv4())
  {
    printf("[NETWORK] Waiting for DHCP...\n");

    unsigned long start = millis();
    while (!wifi_has_ipv4() && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
    {
#if BOSON_USB_HID_HOST
      tuh_task();
#endif
      update_status_led();
      delay_ms(250);
    }

    if (wifi_has_ipv4())
    {
      wifiConnected = true;
      wifi_unstable_since_ms = 0;
      printf("[OK] Wi-Fi connected\n");
      wifi_print_status_line();
      return true;
    }
  }

  if (status != CYW43_LINK_DOWN && status != CYW43_LINK_FAIL)
  {
    printf("[WARN] Wi-Fi not ready (status=%d)\n", status);
    return false;
  }

  printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);

  unsigned long start = millis();

  int result = cyw43_arch_wifi_connect_timeout_ms(
    WIFI_SSID,
    WIFI_PASSWORD,
    CYW43_AUTH_WPA2_MIXED_PSK,
    WIFI_CONNECT_TIMEOUT_MS
  );

  while (!wifi_link_usable() && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
  {
#if BOSON_USB_HID_HOST
    tuh_task();
#endif
    update_status_led();
    delay_ms(250);
    printf(".");
  }

  printf("\n");

  if (result == 0 && wifi_link_usable())
  {
    wifiConnected = true;
    wifi_unstable_since_ms = 0;

    printf("[OK] Wi-Fi connected\n");
    wifi_print_status_line();

    return true;
  }

  wifiConnected = false;
  printf("[WARN] Wi-Fi connection failed\n");
  return false;
}

static bool ensureWiFi()
{
  if (wifi_link_usable())
  {
    wifi_unstable_since_ms = 0;

    if (!wifiConnected)
    {
      wifiConnected = true;
      printf("\n[OK] Wi-Fi reconnected\n");
      wifi_print_status_line();
    }

    return true;
  }

  if (wifiConnected)
  {
    if (wifi_unstable_since_ms == 0)
    {
      wifi_unstable_since_ms = millis();
    }

    if (millis() - wifi_unstable_since_ms < WIFI_LINK_LOST_CONFIRM_MS)
    {
      return true;
    }
  }

  wifi_unstable_since_ms = 0;
  wifiConnected = false;

  if (engineConnected)
  {
    closeEngineConnection();
  }

  if (millis() - lastWiFiAttempt >= WIFI_RETRY_INTERVAL_MS)
  {
    lastWiFiAttempt = millis();
    printf("\n[NETWORK] Attempting Wi-Fi reconnect...\n");
    connectWiFi();
  }

  return false;
}

// ============================================================
// 10. ENGINE TCP MANAGEMENT
// ============================================================

static bool engine_client_connected()
{
  return engine_pcb != nullptr && engine_tcp_alive;
}

static bool connectToEngine()
{
  if (!wifiConnected)
  {
    return false;
  }

  if (engine_client_connected())
  {
    engineConnected = true;
    return true;
  }

  printf("Connecting to Boson engine %s:%u\n", ENGINE_HOST, ENGINE_PORT);

  closeEngineConnection();

  ip4_addr_t server_ip;
  if (!ip4addr_aton(ENGINE_HOST, &server_ip))
  {
    printf("[WARN] Engine connection failed (bad host IP)\n");
    return false;
  }

  cyw43_arch_lwip_begin();

  engine_pcb = tcp_new();
  if (engine_pcb == nullptr)
  {
    cyw43_arch_lwip_end();
    printf("[WARN] Engine connection failed\n");
    return false;
  }

  tcp_arg(engine_pcb, nullptr);
  tcp_err(engine_pcb, engine_tcp_err_cb);
  tcp_recv(engine_pcb, engine_tcp_recv_cb);

  err_t connect_result = tcp_connect(
    engine_pcb,
    &server_ip,
    ENGINE_PORT,
    engine_tcp_connect_cb
  );

  cyw43_arch_lwip_end();

  if (connect_result != ERR_OK)
  {
    closeEngineConnection();
    printf("[WARN] Engine connection failed\n");
    return false;
  }

  unsigned long start = millis();
  while (!engine_client_connected() && millis() - start < SERVER_CONNECT_TIMEOUT_MS)
  {
#if BOSON_USB_HID_HOST
    tuh_task();
#endif
    update_status_led();
    delay_ms(10);
  }

  if (engine_client_connected())
  {
    engineConnected = true;
    printf("[OK] Engine connection\n");
    return true;
  }

  closeEngineConnection();
  printf("[WARN] Engine connection failed\n");
  return false;
}

static bool ensureEngine()
{
  if (!wifiConnected)
  {
    return false;
  }

  if (engine_client_connected())
  {
    engineConnected = true;
    return true;
  }

  engineConnected = false;
  return connectToEngine();
}

static void closeEngineConnection()
{
  cyw43_arch_lwip_begin();

  if (engine_pcb != nullptr)
  {
    tcp_arg(engine_pcb, nullptr);
    tcp_err(engine_pcb, nullptr);
    tcp_recv(engine_pcb, nullptr);
    tcp_close(engine_pcb);
    engine_pcb = nullptr;
  }

  cyw43_arch_lwip_end();

  engine_tcp_alive = false;
  engineConnected = false;
}

// ============================================================
// 11. BARCODE SERIAL INPUT (preserved — loop call disabled)
// ============================================================

static void readBarcodeSerial()
{
#if 0
  // --- Original USB CDC path (Node_Firmware_1b1.cpp) ---
  // Inactive: micro USB runs as host for HID scanner.
  while (Serial.available() > 0)
  {
    char c = (char)Serial.read();
    if (c == '\n')
    {
      char line[SERIAL_LINE_MAX + 1];
      strncpy(line, serialBuffer, sizeof(line) - 1);
      serialBufferLen = 0;
      line[sizeof(line) - 1] = '\0';
      trim_whitespace(line);
      if (line[0] != '\0')
      {
        processSerialLine(line);
      }
      continue;
    }
    if (c == '\r')
    {
      continue;
    }
    if (serialBufferLen >= SERIAL_LINE_MAX)
    {
      serialBufferLen = 0;
      serialErrors++;
      printf("[ERROR] Barcode serial frame too long\n");
      continue;
    }
    serialBuffer[serialBufferLen++] = c;
    serialBuffer[serialBufferLen] = '\0';
  }
#endif
}

// ============================================================
// 12. BARCODE VALIDATION / QUEUING
// ============================================================

static void processSerialLine(char* line)
{
  trim_whitespace(line);
  if (line[0] == '\0')
  {
    return;
  }

  printf("\n[BARCODE RX] %s\n", line);

  if (!str_starts_with(line, "(91)"))
  {
    printf("[ERROR] Barcode does not start with expected (91) AI\n");
    serialErrors++;
    return;
  }

  uint32_t sequence = 0;

  if (!parseBarcodeSequence(line, sequence))
  {
    printf("[ERROR] Could not extract sequence from AI (21)\n");
    serialErrors++;
    return;
  }

  if (sequence < 1 || sequence > 5000)
  {
    printf("[ERROR] Barcode sequence outside expected range 1-5000\n");
    serialErrors++;
    return;
  }

  if (!enqueueBarcode(line, sequence))
  {
    printf("[ERROR] Local barcode queue FULL - barcode rejected\n");
    serialErrors++;
    return;
  }

  packetsQueued++;

  printf(
    "[QUEUED] Sequence %lu | Queue: %u/%u\n",
    (unsigned long)sequence,
    queueCount,
    BARCODE_QUEUE_SIZE
  );

  led_notify_queued();
}

static bool is_numeric_field(const char* value)
{
  if (value == nullptr || value[0] == '\0')
  {
    return false;
  }

  for (size_t i = 0; value[i] != '\0'; i++)
  {
    if (value[i] < '0' || value[i] > '9')
    {
      return false;
    }
  }

  return true;
}

static bool is_alnum_field(const char* value)
{
  if (value == nullptr || value[0] == '\0')
  {
    return false;
  }

  for (size_t i = 0; value[i] != '\0'; i++)
  {
    unsigned char c = (unsigned char)value[i];
    bool valid =
      (c >= '0' && c <= '9') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z');
    if (!valid)
    {
      return false;
    }
  }

  return true;
}

// Input shim: compact HID payload -> parenthesized form expected by processSerialLine.
static bool normalizeCompactBarcode(char* line, size_t cap)
{
  if (str_starts_with(line, "(91)"))
  {
    return true;
  }

  size_t length = strlen(line);
  if (length < 13)
  {
    return false;
  }

  if (line[0] != '9' || line[1] != '1')
  {
    return false;
  }

  if (line[5] != '1' || line[6] != '0')
  {
    return false;
  }

  size_t packet_marker = length - 6;
  if (line[packet_marker] != '2' || line[packet_marker + 1] != '1')
  {
    return false;
  }

  char product[4] = {};
  char batch[21] = {};
  char packet[5] = {};

  memcpy(product, line + 2, 3);
  product[3] = '\0';

  size_t batch_length = packet_marker - 7;
  if (batch_length < 1 || batch_length > 20)
  {
    return false;
  }

  memcpy(batch, line + 7, batch_length);
  batch[batch_length] = '\0';

  memcpy(packet, line + packet_marker + 2, 4);
  packet[4] = '\0';

  if (
    !is_numeric_field(product) ||
    !is_alnum_field(batch) ||
    !is_numeric_field(packet)
  )
  {
    return false;
  }

  int written = snprintf(
    line,
    cap,
    "(91)%s(10)%s(21)%s",
    product,
    batch,
    packet
  );

  return written > 0 && (size_t)written < cap;
}

static bool parseBarcodeSequence(const char* barcodeId, uint32_t& sequence)
{
  const char* marker = "(21)";
  const char* position = strstr(barcodeId, marker);
  if (position == nullptr)
  {
    return false;
  }

  position += strlen(marker);

  char sequenceText[5] = {};

  for (uint8_t i = 0; i < 4; i++)
  {
    char c = position[i];
    if (c == '\0')
    {
      return false;
    }
    if (c < '0' || c > '9')
    {
      return false;
    }
    sequenceText[i] = c;
  }

  sequenceText[4] = '\0';
  sequence = (uint32_t)atoi(sequenceText);
  return true;
}

static bool enqueueBarcode(const char* barcodeId, uint32_t sequence)
{
  if (queueCount >= BARCODE_QUEUE_SIZE)
  {
    return false;
  }

  strncpy(barcodeQueue[queueTail].barcodeId, barcodeId, BARCODE_ID_MAX);
  barcodeQueue[queueTail].barcodeId[BARCODE_ID_MAX] = '\0';
  barcodeQueue[queueTail].sequence = sequence;

  queueTail = (queueTail + 1) % BARCODE_QUEUE_SIZE;
  queueCount++;
  return true;
}

static bool dequeueBarcode(BarcodeMessage& message)
{
  if (queueCount == 0)
  {
    return false;
  }

  message = barcodeQueue[queueHead];
  queueHead = (queueHead + 1) % BARCODE_QUEUE_SIZE;
  queueCount--;
  return true;
}

// ============================================================
// 13. JSON PACKET CREATION
// ============================================================

static bool buildJsonPacket(const BarcodeMessage& message, char* out, size_t outLen)
{
  int n = snprintf(
    out,
    outLen,
    "{\"device_id\":\"%s\",\"sequence\":%lu,\"barcode_id\":\"%s\"}",
    DEVICE_ID,
    (unsigned long)message.sequence,
    message.barcodeId
  );

  return n > 0 && (size_t)n < outLen;
}

// ============================================================
// 14. SEND + ACK
// ============================================================

static void sendEngineHeartbeatIfDue()
{
  if (millis() - lastHeartbeatMs < HEARTBEAT_INTERVAL_MS)
  {
    return;
  }

  lastHeartbeatMs = millis();

  if (!ensureEngine())
  {
    return;
  }

  char json[80];
  snprintf(
    json,
    sizeof(json),
    "{\"device_id\":\"%s\",\"heartbeat\":true}",
    DEVICE_ID
  );

  size_t jsonLen = strlen(json);

  cyw43_arch_lwip_begin();

  err_t write_result = tcp_write(
    engine_pcb,
    json,
    jsonLen,
    TCP_WRITE_FLAG_COPY
  );

  if (write_result == ERR_OK)
  {
    write_result = tcp_write(
      engine_pcb,
      "\n",
      1,
      TCP_WRITE_FLAG_COPY
    );
  }

  if (write_result == ERR_OK)
  {
    write_result = tcp_output(engine_pcb);
  }

  cyw43_arch_lwip_end();

  if (write_result != ERR_OK)
  {
    closeEngineConnection();
    return;
  }

  waitForAck(0);
}

static bool sendBarcodeMessage(const BarcodeMessage& message)
{
  if (!ensureEngine())
  {
    return false;
  }

  char json[320];
  if (!buildJsonPacket(message, json, sizeof(json)))
  {
    return false;
  }

  size_t jsonLen = strlen(json);

  for (uint8_t attempt = 1; attempt <= MAX_SEND_RETRIES; attempt++)
  {
    printf(
      "[TX] Attempt %u/%u | Sequence %lu\n",
      attempt,
      MAX_SEND_RETRIES,
      (unsigned long)message.sequence
    );

    cyw43_arch_lwip_begin();

    err_t write_result = tcp_write(
      engine_pcb,
      json,
      jsonLen,
      TCP_WRITE_FLAG_COPY
    );

    if (write_result == ERR_OK)
    {
      write_result = tcp_write(
        engine_pcb,
        "\n",
        1,
        TCP_WRITE_FLAG_COPY
      );
    }

    if (write_result == ERR_OK)
    {
      write_result = tcp_output(engine_pcb);
    }

    cyw43_arch_lwip_end();

    if (write_result != ERR_OK)
    {
      closeEngineConnection();
      return false;
    }

    packetsSent++;

    if (waitForAck(message.sequence))
    {
      packetsAcked++;
      printf("[ACK] Sequence %lu accepted\n", (unsigned long)message.sequence);
      led_notify_ack();
      return true;
    }

    printf("[WARN] ACK timeout for sequence %lu\n", (unsigned long)message.sequence);

    if (!engine_client_connected())
    {
      engineConnected = false;
      printf("[NETWORK] Engine connection lost during ACK wait\n");
      if (!ensureEngine())
      {
        return false;
      }
    }

    delay_ms(100);
  }

  printf("[FAIL] Sequence %lu could not be acknowledged\n", (unsigned long)message.sequence);
  return false;
}

static bool waitForAck(uint32_t expectedSequence)
{
  char expectedAck[32];
  snprintf(expectedAck, sizeof(expectedAck), "ACK:%lu", (unsigned long)expectedSequence);

  ackLineComplete = false;
  ackLineLen = 0;
  ackLineBuf[0] = '\0';
  ackLineStorage[0] = '\0';

  unsigned long start = millis();

  while (millis() - start < ACK_TIMEOUT_MS)
  {
    if (ackLineComplete)
    {
      ackLineComplete = false;

      if (strcmp(ackLineStorage, expectedAck) == 0)
      {
        return true;
      }

      ackLineBuf[0] = '\0';
      ackLineLen = 0;
    }

    if (!engine_client_connected())
    {
      return false;
    }

#if BOSON_USB_HID_HOST
    tuh_task();
#endif
    update_status_led();
    delay_ms(2);
  }

  return false;
}

// ============================================================
// 15. STATUS
// ============================================================

static void printStatus()
{
  printf("\n--------------- STATUS ----------------\n");
  printf("DEVICE          : %s\n", DEVICE_ID);
  printf("WIFI            : ");

  if (wifiConnected && wifi_link_usable())
  {
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    printf("CONNECTED | RSSI %ld dBm\n", (long)rssi);
  }
  else if (wifiConnected)
  {
    printf("STABILIZING (debounce)\n");
  }
  else
  {
    printf("DISCONNECTED\n");
  }

  printf("ENGINE          : %s\n", engineConnected ? "CONNECTED" : "DISCONNECTED");
  printf("QUEUE           : %u/%u\n", queueCount, BARCODE_QUEUE_SIZE);
  printf("SENT            : %lu\n", (unsigned long)packetsSent);
  printf("ACKED           : %lu\n", (unsigned long)packetsAcked);
  printf("FAILED          : %lu\n", (unsigned long)packetsFailed);
  printf("SERIAL ERRORS   : %lu\n", (unsigned long)serialErrors);
  printf("HID SCANNER     : %s\n", scannerDetected ? "MOUNTED" : "not detected");
  printf("---------------------------------------\n");
}

// ============================================================
// USB HOST + HID (from barcodescannertest)
// ============================================================

#if BOSON_USB_HID_HOST

static char keycode_to_ascii(uint8_t keycode, bool shift)
{
  if (keycode >= 0x04 && keycode <= 0x1D)
  {
    char c = 'a' + (keycode - 0x04);
    if (shift)
    {
      c = (char)(c - 'a' + 'A');
    }
    return c;
  }

  if (keycode >= 0x1E && keycode <= 0x27)
  {
    const char normal[] = "1234567890";
    const char shifted[] = "!@#$%^&*()";
    return shift ? shifted[keycode - 0x1E] : normal[keycode - 0x1E];
  }

  switch (keycode)
  {
    case 0x2C:
      return ' ';
    case 0x2D:
      return shift ? '_' : '-';
    case 0x2E:
      return shift ? '+' : '=';
    case 0x2F:
      return shift ? '{' : '[';
    case 0x30:
      return shift ? '}' : ']';
    case 0x31:
      return shift ? '|' : '\\';
    case 0x33:
      return shift ? ':' : ';';
    case 0x34:
      return shift ? '"' : '\'';
    case 0x35:
      return shift ? '~' : '`';
    case 0x36:
      return shift ? '<' : ',';
    case 0x37:
      return shift ? '>' : '.';
    case 0x38:
      return shift ? '?' : '/';
    default:
      return 0;
  }
}

static void hid_append_char(char c)
{
  if (c == 0)
  {
    return;
  }

  if (hidBarcodeLen < SERIAL_LINE_MAX)
  {
    hidBarcode[hidBarcodeLen++] = c;
    hidBarcode[hidBarcodeLen] = '\0';
  }
  else
  {
    hidBarcodeOverflow = true;
  }
}

static void finishHidBarcode()
{
  if (hidBarcodeLen == 0)
  {
    return;
  }

  if (hidBarcodeOverflow)
  {
    serialErrors++;
    printf("[ERROR] Barcode serial frame too long\n");
  }
  else
  {
    hidBarcode[hidBarcodeLen] = '\0';
    trim_whitespace(hidBarcode);

    if (!normalizeCompactBarcode(hidBarcode, sizeof(hidBarcode)))
    {
      printf("[ERROR] Unrecognized barcode format\n");
      serialErrors++;
    }
    else
    {
      processSerialLine(hidBarcode);
    }
  }

  hidBarcodeLen = 0;
  hidBarcodeOverflow = false;
  hidBarcode[0] = '\0';
}

static void initUsbHost()
{
#if BOSON_USB_HID_HOST
  const tusb_rhport_init_t usb_init = {
    .role = TUSB_ROLE_HOST,
    .speed = TUSB_SPEED_FULL
  };

  if (!tuh_rhport_init(0, &usb_init))
  {
    fatal_error = true;
    printf("[FATAL] USB host init failed\n");
    while (true)
    {
      update_status_led();
      sleep_ms(1);
    }
  }

  printf("[USB] HID host active on micro USB (OTG scanner).\n");
#else
  printf(
    "[USB] HID host disabled (BOSON_USB_HID_HOST=0). "
    "Use USB serial monitor on micro USB.\n"
  );
#endif
}

extern "C" void tuh_hid_mount_cb(
  uint8_t dev_addr,
  uint8_t instance,
  uint8_t const* desc_report,
  uint16_t desc_len
)
{
  (void)desc_report;
  (void)desc_len;

  scannerDetected = true;
  hidPreviousReport = {};
  printf("[HID] Scanner mounted (addr %u)\n", dev_addr);
  tuh_hid_receive_report(dev_addr, instance);
}

extern "C" void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  (void)dev_addr;
  (void)instance;

  scannerDetected = false;
  hidPreviousReport = {};
  hidBarcodeLen = 0;
  hidBarcodeOverflow = false;
  hidBarcode[0] = '\0';
  printf("[HID] Scanner unmounted\n");
}

extern "C" void tuh_hid_report_received_cb(
  uint8_t dev_addr,
  uint8_t instance,
  uint8_t const* report,
  uint16_t len
)
{
  if (
    tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD &&
    len >= sizeof(hid_keyboard_report_t)
  )
  {
    const auto* keyboard = reinterpret_cast<const hid_keyboard_report_t*>(report);
    bool shift = (keyboard->modifier & 0x22) != 0;

    for (uint8_t i = 0; i < 6; i++)
    {
      uint8_t key = keyboard->keycode[i];
      if (key == 0)
      {
        continue;
      }

      bool already_pressed = false;
      for (uint8_t j = 0; j < 6; j++)
      {
        if (hidPreviousReport.keycode[j] == key)
        {
          already_pressed = true;
          break;
        }
      }

      if (already_pressed)
      {
        continue;
      }

      if (key == 0x28 || key == 0x58)
      {
        finishHidBarcode();
      }
      else
      {
        hid_append_char(keycode_to_ascii(key, shift));
      }
    }

    hidPreviousReport = *keyboard;
  }

  tuh_hid_receive_report(dev_addr, instance);
}

#endif // BOSON_USB_HID_HOST

// ============================================================
// MAIN
// ============================================================

int main()
{
  stdio_init_all();
  sleep_ms(1500);

  if (cyw43_arch_init())
  {
    fatal_error = true;
    printf("[FATAL] Wi-Fi init failed\n");
    while (true)
    {
      update_status_led();
      sleep_ms(1);
    }
  }

  cyw43_arch_enable_sta_mode();
  initUsbHost();

  setup();

  while (true)
  {
#if BOSON_USB_HID_HOST
    tuh_task();
#endif
    loop();
  }
}
