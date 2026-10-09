/*
 * ============================================================================
 * OperatorOS — ESP32 ILI9341 USB-Serial Live Streaming & Telemetry Dashboard
 * ============================================================================
 * Hardware Pinout (ESP32 -> SPI ILI9341 Display):
 *   VCC        -> VIN  (5V Power)
 *   GND        -> GND  (Ground)
 *   CS         -> D14  (GPIO 14 - Display CS)
 *   RESET      -> D4   (GPIO 4  - Display Reset)
 *   D/C        -> D27  (GPIO 27 - Data / Command)
 *   SDI (MOSI) -> D23  (GPIO 23 - Shared SPI MOSI)
 *   SCK        -> D18  (GPIO 18 - Shared SPI Clock)
 *   LED        -> 3V3  (Backlight Power)
 *   SDO (MISO) -> D19  (GPIO 19 - Shared SPI MISO)
 *
 * Hardware Sensors & Actuators:
 *   PIN_TEMP_SENSOR -> D36 (GPIO 36 / ADC1_CH0 - Analog Temp Sensor)
 *   PIN_GAS_SENSOR  -> D39 (GPIO 39 / ADC1_CH3 - Analog MQ Gas Sensor)
 *   PIN_SOIL_SENSOR -> D34 (GPIO 34 / ADC1_CH6 - Analog Soil Moisture)
 *   PIN_SERVO       -> D13 (GPIO 13 - PWM Servo Motor Control)
 *   PIN_PUMP_RELAY  -> D12 (GPIO 12 - Digital Relay Output for Pump)
 *   PIN_BUZZER      -> D32 (GPIO 32 - Digital Active Buzzer for Alarms)
 *   BTN_BOOT        -> D0  (GPIO 0  - Built-in ESP32 BOOT Button)
 *
 * Serial Protocol (115200 Baud):
 *   Incoming line-delimited JSON:
 *   {"temp": 28.5, "gas": 120, "soil": 45, "servo": 90, "pump": 0, "alarm": 0, "msg": "LAPTOP STREAM ACTIVE"}
 * ============================================================================
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// -------------------- HARDWARE PIN DEFINITIONS --------------------
// SPI Display Pins (ILI9341)
#define TFT_CS          14  // GPIO 14 - Display CS
#define TFT_RST          4  // GPIO 4  - Display Reset
#define TFT_DC          27  // GPIO 27 - Data / Command
#define TFT_MOSI        23  // GPIO 23 - Shared SPI MOSI (SDI)
#define TFT_SCK         18  // GPIO 18 - Shared SPI Clock (SCK)
#define TFT_MISO        19  // GPIO 19 - Shared SPI MISO (SDO)

// Sensors & Actuators
#define PIN_TEMP_SENSOR 36  // GPIO 36 - Temp Sensor (ADC)
#define PIN_GAS_SENSOR  39  // GPIO 39 - Gas Sensor (ADC)
#define PIN_SOIL_SENSOR 34  // GPIO 34 - Soil Moisture Sensor (ADC)
#define PIN_SERVO       13  // GPIO 13 - Servo PWM
#define PIN_PUMP_RELAY  12  // GPIO 12 - Relay Pump Output
#define PIN_BUZZER      32  // GPIO 32 - Alarm Buzzer Output
#define BTN_BOOT         0  // GPIO 0  - ESP32 BOOT Button

// -------------------- PWM SERVO CONFIGURATION ---------------------
#define SERVO_LEDC_CH    0  // LEDC Channel 0
#define SERVO_FREQ      50  // 50Hz standard RC Servo
#define SERVO_RES       14  // 14-bit timer (0 .. 16383)

// -------------------- RGB565 DASHBOARD COLOR PALETTE --------------
#define COL_BG          0x0861  // Deep Slate Background (#080D18)
#define COL_HEADER      0x0904  // Dark Navy Header (#0A2042)
#define COL_CARD        0x1124  // Card Surface (#112442)
#define COL_BORDER      0x2A69  // Subtle Card Border
#define COL_WHITE       0xFFFF
#define COL_MUTED       0x8C71  // Muted Label Gray
#define COL_GREEN       0x07E8  // Emerald Green (#00FF00) - LIVE
#define COL_CYAN        0x07FF  // Electric Cyan (#00FFFF)
#define COL_AMBER       0xFD20  // Warning Amber / Yellow - OFFLINE
#define COL_CORAL       0xF986  // Coral Red / Alert
#define COL_OFFLINE_BG  0x2124  // Dark Slate Gray for Offline Pill

// -------------------- DISPLAY DRIVER ------------------------------
Adafruit_ILI9341 tft = Adafruit_ILI9341(&SPI, TFT_DC, TFT_CS, TFT_RST);

// -------------------- TELEMETRY STATE -----------------------------
struct TelemetryState {
  float  temp     = 25.0;
  int    gas      = 80;
  int    soil     = 55;
  int    servo    = 90;
  int    pump     = 0;
  int    alarm    = 0;
  String msg      = "STANDBY - WAITING SERIAL STREAM";
} telemetry;

// Flicker-Free Targeted Redraw State Tracker
struct LastDrawnState {
  float  temp          = -999.0;
  int    gas           = -999;
  int    soil          = -999;
  int    servo         = -999;
  int    pump          = -999;
  int    alarm         = -999;
  String msg           = "";
  bool   isLive        = false;
  bool   isInitialized = false;
} lastDrawn;

// Stream Timing & Timeout Management
unsigned long lastStreamRxTime    = 0;
const unsigned long STREAM_TIMEOUT_MS = 3000; // 3 seconds timeout
unsigned long packetCount         = 0;
unsigned long lastPanelAgeUpdate  = 0;

// Non-blocking Serial Buffer
String rxBuffer = "";

// Physical Button State
unsigned long btnDownMs  = 0;
bool          btnWasDown = false;

// ============================================================================
// MINIMAL FAST JSON PARSER (Zero external library dependencies)
// ============================================================================
String extractJsonString(const String& json, const char* key, const String& fallback) {
  String pattern = String("\"") + key + "\":";
  int idx = json.indexOf(pattern);
  if (idx < 0) return fallback;
  int start = json.indexOf('"', idx + pattern.length());
  if (start < 0) return fallback;
  start++;
  String out = "";
  for (int i = start; i < (int)json.length(); i++) {
    if (json[i] == '\\' && i + 1 < (int)json.length()) {
      out += json[i + 1];
      i++;
    } else if (json[i] == '"') {
      break;
    } else {
      out += json[i];
    }
  }
  return out;
}

int extractJsonInt(const String& json, const char* key, int fallback) {
  String pattern = String("\"") + key + "\":";
  int idx = json.indexOf(pattern);
  if (idx < 0) return fallback;
  int start = idx + pattern.length();
  while (start < (int)json.length() && (json[start] == ' ' || json[start] == '"')) start++;
  int end = start;
  while (end < (int)json.length() && (isDigit(json[end]) || json[end] == '-')) end++;
  if (end == start) return fallback;
  return json.substring(start, end).toInt();
}

float extractJsonFloat(const String& json, const char* key, float fallback) {
  String pattern = String("\"") + key + "\":";
  int idx = json.indexOf(pattern);
  if (idx < 0) return fallback;
  int start = idx + pattern.length();
  while (start < (int)json.length() && (json[start] == ' ' || json[start] == '"')) start++;
  int end = start;
  while (end < (int)json.length() && (isDigit(json[end]) || json[end] == '-' || json[end] == '.')) end++;
  if (end == start) return fallback;
  return json.substring(start, end).toFloat();
}

// ============================================================================
// HARDWARE ACTUATOR DRIVER
// ============================================================================
void initServoPWM() {
  #if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcAttach(PIN_SERVO, SERVO_FREQ, SERVO_RES);
  #else
    ledcSetup(SERVO_LEDC_CH, SERVO_FREQ, SERVO_RES);
    ledcAttachPin(PIN_SERVO, SERVO_LEDC_CH);
  #endif
}

void writeServoAngle(int angle) {
  angle = constrain(angle, 0, 180);
  // 50Hz = 20ms period. 0.5ms (0 deg) to 2.5ms (180 deg) pulse width
  // In 14-bit (16384 ticks per 20ms):
  // 0.5ms / 20ms * 16384 ~= 410 ticks
  // 2.5ms / 20ms * 16384 ~= 2048 ticks
  uint32_t duty = map(angle, 0, 180, 410, 2048);
  #if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcWrite(PIN_SERVO, duty);
  #else
    ledcWrite(SERVO_LEDC_CH, duty);
  #endif
}

void applyActuatorStates() {
  // Digital Relay for Pump
  digitalWrite(PIN_PUMP_RELAY, telemetry.pump ? HIGH : LOW);

  // Digital Buzzer for Audible Alarm
  digitalWrite(PIN_BUZZER, (telemetry.alarm > 0) ? HIGH : LOW);

  // PWM Servo Angle
  writeServoAngle(telemetry.servo);
}

// ============================================================================
// UI RENDERING ENGINE (320x240 Landscape - Target Redraws / Zero Full Refresh)
// ============================================================================

// 1. TOP HEADER STATUS INDICATOR
void drawStreamStatusPill(bool isLive) {
  // Cleanly erase the pill zone with header background to prevent text overprinting
  tft.fillRect(174, 4, 142, 20, COL_HEADER);

  if (isLive) {
    // Green Pill: "STREAMING [LIVE]"
    // Pill bounds: x=176, y=5, w=138, h=18, r=4
    tft.fillRoundRect(176, 5, 138, 18, 4, COL_GREEN);
    
    // Status Dot + Text
    tft.setTextSize(1);
    tft.setTextColor(0x0000); // Black contrast on emerald green
    // "STREAMING [LIVE]" is 16 chars * 6px = 96px width
    int textX = 176 + (138 - 96) / 2;
    tft.setCursor(textX, 10);
    tft.print("STREAMING [LIVE]");
  } else {
    // Gray/Yellow Pill: "STREAMING [OFFLINE]"
    // Pill bounds: x=176, y=5, w=138, h=18, r=4
    tft.fillRoundRect(176, 5, 138, 18, 4, COL_OFFLINE_BG);
    tft.drawRoundRect(176, 5, 138, 18, 4, COL_AMBER);

    tft.setTextSize(1);
    tft.setTextColor(COL_AMBER); // Amber/Yellow warning text
    // "STREAMING [OFFLINE]" is 19 chars * 6px = 114px width
    int textX = 176 + (138 - 114) / 2;
    tft.setCursor(textX, 10);
    tft.print("STREAMING [OFFLINE]");
  }
}

void drawHeader() {
  tft.fillRect(0, 0, 320, 28, COL_HEADER);
  tft.drawFastHLine(0, 28, 320, COL_BORDER);

  // Left Title & Subtitle
  tft.setTextSize(1);
  tft.setTextColor(COL_CYAN);
  tft.setCursor(8, 6);
  tft.print("OPERATOR-OS TELEMETRY");

  tft.setTextColor(COL_WHITE);
  tft.setCursor(8, 16);
  tft.print("USB-UART STREAM @ 115200");

  // Top-Right Live Status Pill
  bool isLive = (millis() - lastStreamRxTime <= STREAM_TIMEOUT_MS && lastStreamRxTime > 0);
  drawStreamStatusPill(isLive);
}

// 2. METRIC CARD GENERATOR
void drawMetricCard(int x, int y, int w, int h, const char* label, const String& val, uint16_t accent) {
  // Clear and redraw card box
  tft.fillRoundRect(x, y, w, h, 4, COL_CARD);
  tft.drawRoundRect(x, y, w, h, 4, COL_BORDER);
  tft.fillRoundRect(x, y, 4, h, 2, accent);

  tft.setTextSize(1);
  tft.setTextColor(COL_MUTED);
  tft.setCursor(x + 10, y + 5);
  tft.print(label);

  tft.setTextSize(2);
  tft.setTextColor(COL_WHITE);
  tft.setCursor(x + 10, y + 16);
  tft.print(val);
}

void drawCardTemp() {
  drawMetricCard(6, 33, 151, 35, "TEMPERATURE", String(telemetry.temp, 1) + " C", COL_GREEN);
}

void drawCardGas() {
  uint16_t accent = (telemetry.gas > 300) ? COL_CORAL : COL_AMBER;
  drawMetricCard(163, 33, 151, 35, "GAS SENSOR (MQ)", String(telemetry.gas) + " PPM", accent);
}

void drawCardSoil() {
  uint16_t accent = (telemetry.soil < 20) ? COL_AMBER : COL_CYAN;
  drawMetricCard(6, 72, 151, 35, "SOIL MOISTURE", String(telemetry.soil) + " %", accent);
}

void drawCardActuators() {
  String val = (telemetry.pump ? "P:ON" : "P:OFF");
  val += " | S:" + String(telemetry.servo) + (char)247;
  uint16_t accent = telemetry.pump ? COL_CORAL : COL_GREEN;
  drawMetricCard(163, 72, 151, 35, "PUMP & SERVO", val, accent);
}

void drawKpiGrid() {
  drawCardTemp();
  drawCardGas();
  drawCardSoil();
  drawCardActuators();
}

// 3. COMMS & LOG PANEL
void drawWrappedText(int x, int y, int maxCharsPerLine, int maxLines, const String& text, uint16_t color) {
  tft.setTextSize(1);
  tft.setTextColor(color);

  int len = text.length();
  int pos = 0;
  for (int line = 0; line < maxLines && pos < len; line++) {
    int end = min(pos + maxCharsPerLine, len);
    if (end < len) {
      int lastSpace = text.lastIndexOf(' ', end);
      if (lastSpace > pos) end = lastSpace;
    }
    String slice = text.substring(pos, end);
    slice.trim();
    tft.setCursor(x, y + line * 11);
    tft.print(slice);
    pos = end;
    while (pos < len && text[pos] == ' ') pos++;
  }
}

void drawCommsPanel() {
  uint16_t boxBg     = (telemetry.alarm > 0) ? 0x2940 : COL_CARD;
  uint16_t boxBorder = (telemetry.alarm > 0) ? COL_CORAL : COL_CYAN;

  tft.fillRoundRect(6, 111, 308, 95, 5, boxBg);
  tft.drawRoundRect(6, 111, 308, 95, 5, boxBorder);

  // Sub-header inside Comms Panel
  tft.setTextSize(1);
  if (telemetry.alarm > 0) {
    tft.setTextColor(COL_CORAL);
    tft.setCursor(12, 116);
    tft.print("! CRITICAL ALARM ACTIVE [PIN 32 HIGH] !");
  } else {
    tft.setTextColor(COL_CYAN);
    tft.setCursor(12, 116);
    tft.print("LIVE USB-UART STREAM LOG");
  }

  // Divider
  tft.drawFastHLine(12, 126, 296, COL_BORDER);

  // Stream Metrics
  tft.setTextSize(1);
  tft.setTextColor(COL_MUTED);
  tft.setCursor(12, 131);
  tft.print("Rx Pkts: ");
  tft.setTextColor(COL_WHITE);
  tft.print(packetCount);

  tft.setTextColor(COL_MUTED);
  tft.setCursor(160, 131);
  tft.print("Last Rx: ");
  tft.setTextColor(COL_WHITE);
  if (lastStreamRxTime == 0) {
    tft.print("Never");
  } else {
    unsigned long elapsed = (millis() - lastStreamRxTime) / 1000;
    tft.print(String(elapsed) + "s ago");
  }

  // Stream Message
  tft.setTextColor(COL_MUTED);
  tft.setCursor(12, 143);
  tft.print("> Msg: ");
  tft.setTextColor(COL_WHITE);
  drawWrappedText(54, 143, 40, 2, telemetry.msg, COL_WHITE);

  // Actuator Status Badges
  // Pump Relay Badge (PIN 12)
  uint16_t pumpBg = telemetry.pump ? COL_CORAL : 0x0A20;
  uint16_t pumpFg = telemetry.pump ? 0x0000 : COL_MUTED;
  tft.fillRoundRect(12, 168, 90, 20, 3, pumpBg);
  tft.drawRoundRect(12, 168, 90, 20, 3, telemetry.pump ? COL_CORAL : COL_BORDER);
  tft.setTextColor(pumpFg);
  tft.setCursor(18, 174);
  tft.print(telemetry.pump ? "PUMP: [ON]" : "PUMP: [OFF]");

  // Alarm Buzzer Badge (PIN 32)
  uint16_t alarmBg = (telemetry.alarm > 0) ? COL_CORAL : 0x0A20;
  uint16_t alarmFg = (telemetry.alarm > 0) ? 0x0000 : COL_MUTED;
  tft.fillRoundRect(110, 168, 90, 20, 3, alarmBg);
  tft.drawRoundRect(110, 168, 90, 20, 3, (telemetry.alarm > 0) ? COL_CORAL : COL_BORDER);
  tft.setTextColor(alarmFg);
  tft.setCursor(116, 174);
  tft.print((telemetry.alarm > 0) ? "BUZZ: [ON]" : "BUZZ: [OFF]");

  // Servo Badge (PIN 13)
  tft.fillRoundRect(208, 168, 96, 20, 3, 0x0A20);
  tft.drawRoundRect(208, 168, 96, 20, 3, COL_BORDER);
  tft.setTextColor(COL_CYAN);
  tft.setCursor(214, 174);
  tft.print("SRV: " + String(telemetry.servo) + (char)247);
}

// 4. BOTTOM HARDWARE DOCK
void drawBottomDock() {
  tft.fillRect(0, 210, 320, 30, COL_HEADER);
  tft.drawFastHLine(0, 210, 320, COL_BORDER);

  const char* dockBadges[4] = { "D12:PUMP", "D32:BUZZ", "D13:SRV", "ADC:36,39" };
  for (int i = 0; i < 4; i++) {
    int bx = 6 + i * 78;
    tft.fillRoundRect(bx, 214, 72, 22, 4, COL_CARD);
    tft.drawRoundRect(bx, 214, 72, 22, 4, COL_BORDER);
    tft.setTextSize(1);
    tft.setTextColor(COL_MUTED);
    int tx = bx + (72 - strlen(dockBadges[i]) * 6) / 2;
    tft.setCursor(tx, 221);
    tft.print(dockBadges[i]);
  }
}

// Full Initial Render (Called ONCE at setup)
void renderDashboard() {
  drawHeader();
  drawKpiGrid();
  drawCommsPanel();
  drawBottomDock();
  
  // Initialize state tracker
  lastDrawn.temp          = telemetry.temp;
  lastDrawn.gas           = telemetry.gas;
  lastDrawn.soil          = telemetry.soil;
  lastDrawn.servo         = telemetry.servo;
  lastDrawn.pump          = telemetry.pump;
  lastDrawn.alarm         = telemetry.alarm;
  lastDrawn.msg           = telemetry.msg;
  lastDrawn.isLive        = (millis() - lastStreamRxTime <= STREAM_TIMEOUT_MS && lastStreamRxTime > 0);
  lastDrawn.isInitialized = true;
}

// Targeted Flicker-Free Redraw Dispatcher
void updateDashboardUI() {
  bool isLive = (millis() - lastStreamRxTime <= STREAM_TIMEOUT_MS && lastStreamRxTime > 0);
  if (isLive != lastDrawn.isLive || !lastDrawn.isInitialized) {
    drawStreamStatusPill(isLive);
    lastDrawn.isLive = isLive;
  }

  if (fabs(telemetry.temp - lastDrawn.temp) >= 0.1 || !lastDrawn.isInitialized) {
    drawCardTemp();
    lastDrawn.temp = telemetry.temp;
  }

  if (telemetry.gas != lastDrawn.gas || !lastDrawn.isInitialized) {
    drawCardGas();
    lastDrawn.gas = telemetry.gas;
  }

  if (telemetry.soil != lastDrawn.soil || !lastDrawn.isInitialized) {
    drawCardSoil();
    lastDrawn.soil = telemetry.soil;
  }

  if (telemetry.servo != lastDrawn.servo || telemetry.pump != lastDrawn.pump || !lastDrawn.isInitialized) {
    drawCardActuators();
    lastDrawn.servo = telemetry.servo;
    lastDrawn.pump  = telemetry.pump;
  }

  if (telemetry.msg != lastDrawn.msg || telemetry.alarm != lastDrawn.alarm || !lastDrawn.isInitialized) {
    drawCommsPanel();
    lastDrawn.msg   = telemetry.msg;
    lastDrawn.alarm = telemetry.alarm;
  }

  lastDrawn.isInitialized = true;
}

// ============================================================================
// SERIAL PACKET PARSER & COMMAND ENGINE
// ============================================================================
void processSerialPacket(const String& line) {
  String raw = line;
  raw.trim();
  if (raw.length() == 0) return;

  // Check if incoming packet is JSON telemetry stream
  if (raw.startsWith("{") && raw.endsWith("}")) {
    telemetry.temp  = extractJsonFloat(raw, "temp",  telemetry.temp);
    telemetry.gas   = extractJsonInt(raw,   "gas",   telemetry.gas);
    telemetry.soil  = extractJsonInt(raw,   "soil",  telemetry.soil);
    telemetry.servo = extractJsonInt(raw,   "servo", telemetry.servo);
    telemetry.pump  = extractJsonInt(raw,   "pump",  telemetry.pump);
    telemetry.alarm = extractJsonInt(raw,   "alarm", telemetry.alarm);
    telemetry.msg   = extractJsonString(raw, "msg",  telemetry.msg);

    lastStreamRxTime = millis();
    packetCount++;

    // Apply hardware outputs immediately
    applyActuatorStates();

    // Trigger targeted UI refresh
    updateDashboardUI();

    // Send JSON ACK back to laptop
    Serial.print("{\"ack\":1,\"rx_pkts\":");
    Serial.print(packetCount);
    Serial.print(",\"live\":1,\"time_ms\":");
    Serial.print(millis());
    Serial.println("}");
  } else {
    // Fallback: Echo standard text commands if typed manually
    telemetry.msg = raw;
    lastStreamRxTime = millis();
    updateDashboardUI();
    Serial.println("[CMD ACK] " + raw);
  }
}

// ============================================================================
// ARDUINO SETUP & MAIN LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);

  // Configure Actuator Pins
  pinMode(PIN_PUMP_RELAY, OUTPUT);
  digitalWrite(PIN_PUMP_RELAY, LOW);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  pinMode(BTN_BOOT, INPUT_PULLUP);

  // Configure Analog Sensor Inputs
  pinMode(PIN_TEMP_SENSOR, INPUT);
  pinMode(PIN_GAS_SENSOR,  INPUT);
  pinMode(PIN_SOIL_SENSOR, INPUT);

  // Initialize Hardware PWM for Servo
  initServoPWM();
  writeServoAngle(telemetry.servo);

  // Ensure TFT CS is HIGH before bus initialization
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  // Initialize Shared SPI Bus (SCK=18, MISO=19, MOSI=23, CS=14)
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);

  // Initialize ILI9341 Screen
  tft.begin();
  tft.setRotation(1); // Landscape 320x240
  tft.fillScreen(COL_BG);

  // Perform full initial render
  renderDashboard();

  Serial.println("================================================================");
  Serial.println("  ESP32 ILI9341 USB-SERIAL LIVE STREAMING FIRMWARE INITIALIZED ");
  Serial.println("================================================================");
  Serial.println("Baud Rate: 115200");
  Serial.println("Expected Packet Format:");
  Serial.println("{\"temp\": 28.5, \"gas\": 120, \"soil\": 45, \"servo\": 90, \"pump\": 0, \"alarm\": 0, \"msg\": \"LAPTOP STREAM ACTIVE\"}");
  Serial.println("Status Indicator: STREAMING [LIVE] (<3s) / STREAMING [OFFLINE] (>3s)");
  Serial.println("================================================================");
}

void loop() {
  // 1. Non-Blocking USB-UART Serial Parser
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxBuffer.length() > 0) {
        processSerialPacket(rxBuffer);
        rxBuffer = "";
      }
    } else {
      if (rxBuffer.length() < 256) {
        rxBuffer += c;
      }
    }
  }

  // 2. Stream Timeout Evaluation (> 3 seconds)
  bool isLive = (millis() - lastStreamRxTime <= STREAM_TIMEOUT_MS && lastStreamRxTime > 0);
  if (isLive != lastDrawn.isLive) {
    // Redraw status badge in header
    drawStreamStatusPill(isLive);
    lastDrawn.isLive = isLive;

    // Refresh comms panel to update alarm / status styling
    drawCommsPanel();
    lastDrawn.msg   = telemetry.msg;
    lastDrawn.alarm = telemetry.alarm;
  }

  // 3. Periodic Comms Panel Age Refresh (every 1 second when offline or idle)
  if (millis() - lastPanelAgeUpdate > 1000) {
    lastPanelAgeUpdate = millis();
    if (!isLive && lastStreamRxTime > 0) {
      // Refresh the elapsed timestamp without clearing whole screen
      drawCommsPanel();
    }
  }

  // 4. Physical BOOT Button Handler (GPIO 0)
  bool isDown = (digitalRead(BTN_BOOT) == LOW);
  if (isDown && !btnWasDown) {
    btnDownMs = millis();
    btnWasDown = true;
  } else if (!isDown && btnWasDown) {
    unsigned long heldMs = millis() - btnDownMs;
    btnWasDown = false;
    if (heldMs > 40) { // Debounce
      // Toggle Pump Relay manually on physical click
      telemetry.pump = !telemetry.pump;
      telemetry.msg  = telemetry.pump ? "PUMP TOGGLED ON [BOOT BTN]" : "PUMP TOGGLED OFF [BOOT BTN]";
      applyActuatorStates();
      updateDashboardUI();
      Serial.println("[ESP32 BOOT BTN] " + telemetry.msg);
    }
  }
}
