/*******************************************************
 * This is the final version from the tests conducted in April 2026 and loaded onto the controller in the box.
 * ESP8266 NodeMCU V3 + PIR SR501
 * MQTT (Home Assistant Discovery)
 * WiFiManager + LittleFS (MQTT settings storage)
 * Reset button for settings + MQTT reset command
 * Control two relays for lighting and fan
 * Boost function for fan
 * 1 sec continuous motion to turn on lighting
* Fan decision point: exactly at 70 seconds after light ON
* Recent motion window: last 20 seconds before decision
 *******************************************************/

#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ================= SETTINGS =================
const char* DEVICE_NAME = "toilet_relay";                           // Device name for MQTT 
const char* DEVICE_FRIENDLY_NAME = "Motion Sensor & Relay Control";  // Friendly Name for Home Assistant
const char* DEVICE_MODEL = "Toilet Relay Mov Sensor";   
const char* DEVICE_SW_VERSION = "2026.00";              

// Intervals and timeouts
const unsigned long MQTT_RETRY_MS = 5000;   // repeat attempt for MQTT
const unsigned long PIR_DEBOUNCE_MS = 300;  // debounce for PIR
const long wifiReportInterval = 60000;      // WiFi reporting

// Relay control times
const unsigned long LIGHT_DELAY_MS = 600;             // 0.6 sec continuous motion for lighting
const unsigned long FAN_DELAY_MS = 70000;             // 70 sec delay for fan
const unsigned long MOTION_TIMEOUT_MS = 70000;        // 1 min 10 sec without movement
const unsigned long FAN_OFF_DELAY_MS = 90000;         // 1 min 30 after lights off
const unsigned long BOOST_DURATION_MS = 180000;       // 3 minutes for Boost function
const unsigned long RECENT_MOTION_WINDOW_MS = 20000;  // 20 sec window for recent motion check

// Pins
const uint8_t PIR_PIN = D5;     // PIR (GPIO14)
const uint8_t RESET_PIN = D3;   // reset button (GPIO0)
const uint8_t RELAY1_PIN = D6;  // Relay 1 - lightening (GPIO12)
const uint8_t RELAY2_PIN = D7;  // Relay 2 - fan (GPIO13)

// MQTT settings (by default)
char mqtt_server[40] = "192.168.1.xxx";
char mqtt_port[6] = "1883";
char mqtt_user[32] = "relay_user";
char mqtt_pass[32] = "xxxxxxxx";

// ================= ADDITIONAL SETTINGS =================
const unsigned long MQTT_FAILSAFE_TIMEOUT_MS = 300000;  // 5 minutes without MQTT connection before Failsafe activation
// =====================================================
bool messageShown = false;          // To track if the message is shown
bool failsafeMessageShown = false;  // To track if the message is shown in Failsafe
// ================= SAFETY VARIABLES =================
static unsigned long resetPressStart = 0;
static unsigned long lastSafetyCheck = 0;
static unsigned long lastResetCheck = 0;  // Added for debounce of the reset button

unsigned long lastMotionTimeForFan = 0;  // NEW: Last motion time for fan logic
unsigned long boostEndTime = 0;

// Rate limiting for reset commands
static unsigned long lastResetAttempt = 0;
static int resetAttemptCount = 0;
static unsigned long resetBlockedUntil = 0;
const int MAX_RESET_ATTEMPTS = 3;
const unsigned long RESET_BLOCK_DURATION = 300000; // 5 minutes in milliseconds
const unsigned long RESET_ATTEMPT_WINDOW = 60000;  // 1 minute window
// Променлива за ръчното управление
bool manualLightControl = false;  // Flag for manual light control
unsigned long manualControlStart = 0;
const unsigned long MANUAL_CONTROL_DURATION = 300000; // 5 minutes of manual control
// ===================================================

WiFiClient espClient;
PubSubClient mqtt(espClient);

// Home Assistant basic topics - char[] buffers, filled in setup() via snprintf
char baseTopic[48];
char stateMotionTopic[72];
char stateLightRelayTopic[72];
char stateFanRelayTopic[72];
char boostFanTopic[80];
char availabilityTopic[64];
char stateRssiTopic[72];
char resetTopic[72];
char resetStatusTopic[80];
char modeTopic[64];
char modeSetTopic[72];

// Home Assistant Auto Discovery topics
char haMotionConfigTopic[96];
char haLightConfigTopic[96];
char haFanConfigTopic[96];
char haBoostConfigTopic[96];
char haResetConfigTopic[96];

// Subscription topics that include "/set" suffix (pre-built for strcmp in callback)
char lightSetTopic[80];
char fanSetTopic[80];

bool lastPirState = false;                  //Saves the previous state of the PIR sensor
volatile bool pirReady = false;             //Flag indicating whether the PIR sensor is stabilized and ready for operation
volatile unsigned long lastPirChangeMs = 0; //Saves time of last PIR sensor change (in milliseconds)

// Variables for relay control
bool lightOn = false;
bool fanOn = false;
unsigned long continuousMotionStart = 0;
unsigned long lastMotionTime = 0;
unsigned long lightOnTime = 0;
unsigned long lightOffTime = 0;
bool lightScheduled = false;
bool fanScheduled = false;
bool motionActive = false;

// Smart OFF filter variables
unsigned long noMotionStartTime = 0;
bool noMotionCandidate = false;
const unsigned long MOTION_CONFIRM_OFF_MS = 5000;  // 5 sec confirmation before turning off light
unsigned long lastPirOffTime = 0;                  // Time when PIR last went OFF

// Global variables for failed attempts counter
int lightRelayFailureCount = 0;
int fanRelayFailureCount = 0;
const int MAX_FAILURE_COUNT = 3;

// Variables for the Boost function
bool boostActive = false;
unsigned long boostStartTime = 0;
unsigned long boostDuration = BOOST_DURATION_MS;

unsigned long lastWifiReport = 0;
unsigned long lastMqttAttempt = 0;

// FAILSAFE VARIABLE
bool failsafeMode = false;
unsigned long lastMqttConnectionTime = 0;

// --------- STORAGE ---------
bool saveConfig() {
  StaticJsonDocument<768> doc;
  doc["mqtt_server"] = mqtt_server;
  doc["mqtt_port"] = mqtt_port;
  doc["mqtt_user"] = mqtt_user;
  doc["mqtt_pass"] = mqtt_pass;
  File f = LittleFS.open("/config.json", "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}
// Configuration Loading 
void loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS.begin() failed");
    return;
  }

  if (!LittleFS.exists("/config.json")) {
    Serial.println("/config.json is missing, the default settings will be used");
    return;
  }

  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    Serial.println("Failed to open config file");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(err.c_str());
    return;
  }

  if (doc.containsKey("mqtt_server")) {
    strlcpy(mqtt_server, doc["mqtt_server"] | "", sizeof(mqtt_server));
  }

  if (doc.containsKey("mqtt_port")) {
    const char* portStr = doc["mqtt_port"];
    int port = atoi(portStr);
    if (port <= 0 || port > 65535) {
      strlcpy(mqtt_port, "1883", sizeof(mqtt_port));
    } else {
      strlcpy(mqtt_port, portStr, sizeof(mqtt_port));
    }
  } else {
    strlcpy(mqtt_port, "1883", sizeof(mqtt_port));
  }

  if (doc.containsKey("mqtt_user")) {
    strlcpy(mqtt_user, doc["mqtt_user"] | "", sizeof(mqtt_user));
  }

  if (doc.containsKey("mqtt_pass")) {
    strlcpy(mqtt_pass, doc["mqtt_pass"] | "", sizeof(mqtt_pass));
  }

  Serial.println("Configuration loaded successfully:");
  Serial.print("MQTT Server: ");
  Serial.println(mqtt_server);
  Serial.print("MQTT Port: ");
  Serial.println(mqtt_port);
  Serial.print("MQTT User: ");
  Serial.println(mqtt_user);
}

// ========= FUNCTIONAL DECLARATIONS =========
void validateAndCorrectState();
void enterFailsafeMode();
void handleFailsafeLogic();
void activateBoost(unsigned long duration);  // Declaration for Boost function
void performReset();                         // Reset function declaration
bool isAuthorizedReset(const char* payload); // Authorisation verification declaration
// =========================================

ICACHE_RAM_ATTR void pirISR() {
  static unsigned long last_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  if (interrupt_time - last_interrupt_time > PIR_DEBOUNCE_MS) {
    lastPirChangeMs = interrupt_time;
  }
  last_interrupt_time = interrupt_time;
}

// Light relay control function 
void setLightRelay(bool state, bool isManual)
{
  if (lightOn == state) return;

  digitalWrite(RELAY1_PIN, state ? LOW : HIGH);
  lightOn = state;

  if (state) {
    lightOnTime = millis();

    if (isManual) {
      manualLightControl = true;
      manualControlStart = millis();
      Serial.println("The light is On (manual control)");

      if (mqtt.connected()) {
        mqtt.publish(modeTopic, "MANUAL", true);
      }
    } else {
      // 👉 we make a clear distinction 
      Serial.println("The light is On (auto/PIR)");

      if (mqtt.connected()) {
        mqtt.publish(modeTopic, "AUTO", true);
      }
    }

  } else {
    lightOffTime = millis();
    noMotionCandidate = false;  // Always reset on light off

    if (manualLightControl) {
      manualLightControl = false;
      Serial.println("Manual control disabled");

      if (mqtt.connected()) {
        mqtt.publish(modeTopic, "AUTO", true);
      }
    }

    Serial.println("The light is Off");
  }

  if (mqtt.connected()) {
    mqtt.publish(stateLightRelayTopic, state ? "ON" : "OFF", true);
  }
}

// Fan relay control function
void setFanRelay(bool state) {
  if (fanOn == state) return;

  digitalWrite(RELAY2_PIN, state ? LOW : HIGH);
  fanOn = state;

  if (state) {
    Serial.println("The fan is On");
  } else {
    Serial.println("The fan is Off");
  }

  if (mqtt.connected()) {
    mqtt.publish(stateFanRelayTopic, state ? "ON" : "OFF", true);
  }
}

// Boost mode activation function
void activateBoost(unsigned long duration) {
  boostActive = true;
  boostStartTime = millis();
  boostDuration = duration;
  setFanRelay(true);     // Switching on the fan
  fanScheduled = false;  // Override standard scheduling
  Serial.printf("Boost mode activated for %lu millisecond\n", duration);
}

// Function to perform a full reset
void performReset() {
  Serial.println("Perform a full reset via MQTT command...");
  
  // Publishing a message before restart
  if (mqtt.connected()) {
    mqtt.publish(resetStatusTopic, "Resetting device...", true);
    delay(100);
  }
  
  // Delete settings
  WiFiManager wm;
  wm.resetSettings();
  LittleFS.remove("/config.json");
  
  delay(3000);
  ESP.restart();
}

bool isResetRateLimited() {
  unsigned long currentTime = millis();
  
  // Check if we are in a blocked period
  if (currentTime < resetBlockedUntil) {
    Serial.printf("Reset blocked for %lu more seconds\n", (resetBlockedUntil - currentTime) / 1000);
    return true;
  }
  
  // Reset counter if window has expired
  if (currentTime - lastResetAttempt > RESET_ATTEMPT_WINDOW) {
    resetAttemptCount = 0;
  }
  
  return false;
}

void recordResetAttempt(bool successful) {
  unsigned long currentTime = millis();
  lastResetAttempt = currentTime;
  
  if (!successful) {
    resetAttemptCount++;
    Serial.printf("Failed reset attempt %d/%d\n", resetAttemptCount, MAX_RESET_ATTEMPTS);
    
    if (resetAttemptCount >= MAX_RESET_ATTEMPTS) {
      resetBlockedUntil = currentTime + RESET_BLOCK_DURATION;
      Serial.printf("Reset blocked for %lu minutes due to too many failed attempts\n", RESET_BLOCK_DURATION / 60000);
      
      // MQTT alarm
      if (mqtt.connected()) {
        { char alarmTopic[80]; snprintf(alarmTopic, sizeof(alarmTopic), "%s/alarm/reset_blocked", baseTopic); mqtt.publish(alarmTopic, "SECURITY: Reset blocked due to multiple failed attempts", true); }
      }
    }
  } else {
    // If the reset is successful, we reset the counter
    resetAttemptCount = 0;
    resetBlockedUntil = 0;
  }
}

// Authorization check function for reset
bool isAuthorizedReset(const char* payload) {
  // ✅ Secret protection code
  static const char secretCode[] = "xxxxxxxx";

  if (strcmp(payload, secretCode) == 0) {
    Serial.println("Authorized reset command");
    return true;
  }

  Serial.println("Unauthorized reset attempt!");
  return false;
}

void publishHAConfig() {
  StaticJsonDocument<512> doc;
  char buffer[768];
  char temp[80];   // for temporary names

  // Name generation helper function
  #define BUILD_NAME(out, suffix) snprintf(out, sizeof(out), "%s %s", DEVICE_FRIENDLY_NAME, suffix)

  // ===== Motion =====
  doc.clear();
  BUILD_NAME(temp, "Motion");
  doc["name"] = temp;
  snprintf(temp, sizeof(temp), "%s_motion", DEVICE_NAME);
  doc["uniq_id"] = temp;
  doc["stat_t"] = stateMotionTopic;
  doc["dev_cla"] = "motion";
  doc["pl_on"] = "ON";
  doc["pl_off"] = "OFF";
  doc["avty_t"] = availabilityTopic;

  JsonObject dev = doc.createNestedObject("dev");
  JsonArray ids = dev.createNestedArray("identifiers");
  ids.add(DEVICE_NAME);
  dev["manufacturer"] = "DIY";
  dev["model"] = DEVICE_MODEL;
  dev["name"] = "Toilet Relay";
  dev["sw_version"] = DEVICE_SW_VERSION;

  serializeJson(doc, buffer);
  mqtt.publish(haMotionConfigTopic, buffer, true);

  // ===== Light =====
  doc.clear();
  BUILD_NAME(temp, "Light");
  doc["name"] = temp;
  snprintf(temp, sizeof(temp), "%s_light", DEVICE_NAME);
  doc["uniq_id"] = temp;
  snprintf(temp, sizeof(temp), "%s/set", stateLightRelayTopic);
  doc["cmd_t"] = temp;
  doc["stat_t"] = stateLightRelayTopic;
  doc["avty_t"] = availabilityTopic;

  dev = doc.createNestedObject("dev");
  ids = dev.createNestedArray("identifiers");
  ids.add(DEVICE_NAME);
  dev["manufacturer"] = "DIY";
  dev["model"] = DEVICE_MODEL;
  dev["name"] = "Toilet Relay";
  dev["sw_version"] = DEVICE_SW_VERSION;

  serializeJson(doc, buffer);
  mqtt.publish(haLightConfigTopic, buffer, true);

  // ===== Fan =====
  doc.clear();
  BUILD_NAME(temp, "Fan");
  doc["name"] = temp;
  snprintf(temp, sizeof(temp), "%s_fan", DEVICE_NAME);
  doc["uniq_id"] = temp;
  snprintf(temp, sizeof(temp), "%s/set", stateFanRelayTopic);
  doc["cmd_t"] = temp;
  doc["stat_t"] = stateFanRelayTopic;
  doc["avty_t"] = availabilityTopic;

  dev = doc.createNestedObject("dev");
  ids = dev.createNestedArray("identifiers");
  ids.add(DEVICE_NAME);
  dev["manufacturer"] = "DIY";
  dev["model"] = DEVICE_MODEL;
  dev["name"] = "Toilet Relay";
  dev["sw_version"] = DEVICE_SW_VERSION;

  serializeJson(doc, buffer);
  mqtt.publish(haFanConfigTopic, buffer, true);

  // ===== Boost =====
  doc.clear();
  BUILD_NAME(temp, "Fan Boost");
  doc["name"] = temp;
  snprintf(temp, sizeof(temp), "%s_fan_boost", DEVICE_NAME);
  doc["uniq_id"] = temp;
  doc["cmd_t"] = boostFanTopic;
  doc["min"] = 1;
  doc["max"] = 10;
  doc["step"] = 1;
  doc["mode"] = "box";
  doc["avty_t"] = availabilityTopic;

  dev = doc.createNestedObject("dev");
  ids = dev.createNestedArray("identifiers");
  ids.add(DEVICE_NAME);
  dev["manufacturer"] = "DIY";
  dev["model"] = DEVICE_MODEL;
  dev["name"] = "Toilet Relay";
  dev["sw_version"] = DEVICE_SW_VERSION;

  serializeJson(doc, buffer);
  mqtt.publish(haBoostConfigTopic, buffer, true);

  // ===== Reset =====
  doc.clear();
  BUILD_NAME(temp, "Reset");
  doc["name"] = temp;
  snprintf(temp, sizeof(temp), "%s_reset", DEVICE_NAME);
  doc["uniq_id"] = temp;
  doc["cmd_t"] = resetTopic;
  doc["avty_t"] = availabilityTopic;

  dev = doc.createNestedObject("dev");
  ids = dev.createNestedArray("identifiers");
  ids.add(DEVICE_NAME);
  dev["manufacturer"] = "DIY";
  dev["model"] = DEVICE_MODEL;
  dev["name"] = "Toilet Relay";
  dev["sw_version"] = DEVICE_SW_VERSION;

  serializeJson(doc, buffer);
  mqtt.publish(haResetConfigTopic, buffer, true);

  // ===== Mode =====
  char haModeConfigTopic[96];
  snprintf(haModeConfigTopic, sizeof(haModeConfigTopic),
           "homeassistant/select/%s_mode/config", DEVICE_NAME);

  doc.clear();
  BUILD_NAME(temp, "Mode");
  doc["name"] = temp;
  snprintf(temp, sizeof(temp), "%s_mode", DEVICE_NAME);
  doc["uniq_id"] = temp;
  doc["cmd_t"] = modeSetTopic;
  doc["stat_t"] = modeTopic;
  doc["avty_t"] = availabilityTopic;

  JsonArray options = doc.createNestedArray("options");
  options.add("AUTO");
  options.add("MANUAL");

  dev = doc.createNestedObject("dev");
  ids = dev.createNestedArray("identifiers");
  ids.add(DEVICE_NAME);
  dev["manufacturer"] = "DIY";
  dev["model"] = DEVICE_MODEL;
  dev["name"] = "Toilet Relay";
  dev["sw_version"] = DEVICE_SW_VERSION;

  serializeJson(doc, buffer);
  mqtt.publish(haModeConfigTopic, buffer, true);

  Serial.println("Home Assistant Auto Discovery configurations published");
}
void publishMotion(bool motion) {
  mqtt.publish(stateMotionTopic, motion ? "ON" : "OFF", true);
}

void publishRSSI() {
  long rssi = WiFi.RSSI();
  char buf[12];
  ltoa(rssi, buf, 10);
  mqtt.publish(stateRssiTopic, buf, true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Fixed-size buffer - no heap fragmentation
  char message[128];
  if (length >= sizeof(message)) {
    Serial.printf("MQTT message too long (%u bytes) on topic %s - ignored\n", length, topic);
    return;
  }
  memcpy(message, payload, length);
  message[length] = '\0';

  Serial.printf("MQTT message received: Topic: %s, Message: %s\n", topic, message);

  if (strcmp(topic, lightSetTopic) == 0) {
    if (strcmp(message, "ON") == 0) {
      // We reset the motion variables when manually enabling
      motionActive = false;
      lightScheduled = false;
      continuousMotionStart = 0;
      setLightRelay(true, true);
      Serial.println("Light turned ON via MQTT command");
    }
    else if (strcmp(message, "OFF") == 0) {
      setLightRelay(false, true);
      Serial.println("Light turned OFF via MQTT command");
    }
  } else if (strcmp(topic, fanSetTopic) == 0) {
    if (strcmp(message, "ON") == 0) {
      setFanRelay(true);
      Serial.println("Fan turned ON via MQTT command");
    }
    else if (strcmp(message, "OFF") == 0) {
      setFanRelay(false);
      Serial.println("Fan turned OFF via MQTT command");
    }
  } else if (strcmp(topic, boostFanTopic) == 0) {
    // Processing a Boost command
    unsigned long boostTime = (unsigned long)atol(message) * 60000UL;  // Конвертиране от минути в милисекунди
    if (boostTime > 0) {
      activateBoost(boostTime);
      Serial.printf("Boost activated for %lu minutes via MQTT\n", boostTime / 60000);
    } else {
      Serial.println("Invalid boost duration received");
    }
  } else if (strcmp(topic, resetTopic) == 0) {
    // Checking for rate limiting
    if (isResetRateLimited()) {
      if (mqtt.connected()) {
        mqtt.publish(resetStatusTopic, "Reset temporarily blocked - too many attempts", true);
      }
      Serial.println("Reset attempt blocked due to rate limiting");
      return;
    }
    
    // Reset command processing
    if (isAuthorizedReset(message)) {
      recordResetAttempt(true);  // Successful attempt
      Serial.println("Authorized reset command received via MQTT");
      performReset();
    } else {
      recordResetAttempt(false); // Unsuccessful attempt
      Serial.println("Unauthorized reset attempt via MQTT");
      
      // Posting error on unauthenticated attempt
      if (mqtt.connected()) {
        char errorMsg[128];
        if (resetAttemptCount >= MAX_RESET_ATTEMPTS - 1) {
          snprintf(errorMsg, sizeof(errorMsg), "Unauthorized reset attempt! On next attempt the function will be blocked.");
        } else {
          snprintf(errorMsg, sizeof(errorMsg), "Unauthorized reset attempt!");
        }
        mqtt.publish(resetStatusTopic, errorMsg, true);
      }
    }
} else if (strcmp(topic, modeSetTopic) == 0) {
  if (strcmp(message, "AUTO") == 0) {
    manualLightControl = false;

    // We reset the motion variables
    motionActive = false;
    lightScheduled = false;
    continuousMotionStart = 0;

    if (mqtt.connected()) {
      mqtt.publish(modeTopic, "AUTO", true);
    }

    Serial.println("Switched to AUTO mode via MQTT");

  } else if (strcmp(message, "MANUAL") == 0) {
    manualLightControl = true;
    manualControlStart = millis();

    // We reset the motion variables
    motionActive = false;
    lightScheduled = false;
    continuousMotionStart = 0;

    if (mqtt.connected()) {
      mqtt.publish(modeTopic, "MANUAL", true);
    }

    Serial.println("Switched to MANUAL mode for 5 minutes via MQTT");
  }

} else {
  Serial.printf("Unknown MQTT topic: %s\n", topic);
}
}

void connectMQTT() {
  if (mqtt.connected()) return;

  int port = atoi(mqtt_port);
  if (strlen(mqtt_server) == 0 || port <= 0 || port > 65535) {
    Serial.println("The MQTT configuration is invalid");
    return;
  }

  mqtt.setServer(mqtt_server, port);
  mqtt.setCallback(mqttCallback);

  char clientId[48];
  snprintf(clientId, sizeof(clientId), "%s_%08X", DEVICE_NAME, ESP.getChipId());
  if (mqtt.connect(clientId, (strlen(mqtt_user) ? mqtt_user : nullptr), (strlen(mqtt_pass) ? mqtt_pass : nullptr), availabilityTopic, 0, true, "offline")) {
    Serial.println("MQTT connected");
    mqtt.publish(availabilityTopic, "online", true);
    mqtt.subscribe(lightSetTopic);
    mqtt.subscribe(fanSetTopic);
    mqtt.subscribe(boostFanTopic);  // Subscribe to Boost topic
    mqtt.subscribe(resetTopic);     // Reset topic subscription
    mqtt.subscribe(modeSetTopic);   // Subscribe to mode set topic - КОРИГИРАНО
    
    publishHAConfig();
    publishMotion(digitalRead(PIR_PIN) == HIGH);
    publishRSSI();
    mqtt.publish(stateLightRelayTopic, lightOn ? "ON" : "OFF", true);
    mqtt.publish(stateFanRelayTopic, fanOn ? "ON" : "OFF", true);
    mqtt.publish(modeTopic, manualLightControl ? "MANUAL" : "AUTO", true); // КОРИГИРАНО
    mqtt.publish(resetStatusTopic, "Ready", true);
  } else {
    Serial.print("MQTT connect failed, rc=");
    Serial.println(mqtt.state());
  }
}

bool timeoutExpired(unsigned long start, unsigned long timeout) {
  unsigned long current = millis();
  if (current >= start) {
    return (current - start) >= timeout;
  } else {
    // Overflow handling
    return (ULONG_MAX - start + current) >= timeout;
  }
}

void validateAndCorrectState() {
  bool actualLightState = (digitalRead(RELAY1_PIN) == LOW);
  bool actualFanState = (digitalRead(RELAY2_PIN) == LOW);

  // ============ LIGHT PROCESSING ============
  if (actualLightState != lightOn) {
    Serial.printf("WARNING: Mismatch in light! SW: %s, HW: %s\n",
                  lightOn ? "ON" : "OFF", actualLightState ? "ON" : "OFF");

    // Trying to enforce the software state
    digitalWrite(RELAY1_PIN, lightOn ? LOW : HIGH);
    delay(50);

    // Check if the hardware has obeyed
    bool newLightState = (digitalRead(RELAY1_PIN) == LOW);

    if (newLightState == lightOn) {
      // Success - hardware follows software
      lightRelayFailureCount = 0;
      Serial.println("Light relay adjusted successfully");
    } else {
      // Failure to succeed is probably a hardwire of problems
      lightRelayFailureCount++;
      Serial.printf("ERROR: Light relay not responding (%d/%d)\n",
                    lightRelayFailureCount, MAX_FAILURE_COUNT);

      if (lightRelayFailureCount >= MAX_FAILURE_COUNT) {
        // We accept the hardware condition as true due to failure
        lightOn = actualLightState;
        Serial.println("Software adapted to hardware state (probable light failure)");
      }
    }
  }

  // ============ Fan processing ============
  if (actualFanState != fanOn) {
    Serial.printf("WARNING: Mismatch in the fan! SW: %s, HW: %s\n",
                  fanOn ? "ON" : "OFF", actualFanState ? "ON" : "OFF");

    // Trying to enforce the software state
    digitalWrite(RELAY2_PIN, fanOn ? LOW : HIGH);
    delay(50);

    // Check if the hardware has obeyed
    bool newFanState = (digitalRead(RELAY2_PIN) == LOW);

    if (newFanState == fanOn) {
      // Success - hardware follows software
      fanRelayFailureCount = 0;
      Serial.println("Fan relay adjusted successfully");
    } else {
      // Failure to succeed is probably a hardwire of problems
      fanRelayFailureCount++;
      Serial.printf("ERROR: Fan relay not responding (%d/%d)\n",
                    fanRelayFailureCount, MAX_FAILURE_COUNT);

      if (fanRelayFailureCount >= MAX_FAILURE_COUNT) {
        // We accept the hardware condition as true due to failure
        fanOn = actualFanState;
        Serial.println("Software adapted to hardware state (probable fan failure)");
      }
    }
  }

  // ============ MQTT ALARMS FOR FAILURES ============
  if (lightRelayFailureCount >= MAX_FAILURE_COUNT && mqtt.connected()) {
    { char alarmTopic[80]; snprintf(alarmTopic, sizeof(alarmTopic), "%s/alarm/light_failure", baseTopic); mqtt.publish(alarmTopic, "CRITICAL", true); }
  }
  if (fanRelayFailureCount >= MAX_FAILURE_COUNT && mqtt.connected()) {
    { char alarmTopic[80]; snprintf(alarmTopic, sizeof(alarmTopic), "%s/alarm/fan_failure", baseTopic); mqtt.publish(alarmTopic, "CRITICAL", true); }
  }

// Safety: Emergency shutdown after 30 minutes
  if (lightOn && timeoutExpired(lastMotionTime, 1800000)) {
    Serial.println("SAFETY: Shutdown after 30 minutes without movement");
    setLightRelay(false, false);
    setFanRelay(false);
    motionActive = false;
    lightScheduled = false;
    fanScheduled = false;
  }

  // Safety: Memory check
  if (ESP.getFreeHeap() < 1000) {
    Serial.printf("SAFETY: Low memory! Free: %u byte\n", ESP.getFreeHeap());
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIR_PIN, INPUT_PULLUP);
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);

  digitalWrite(RELAY1_PIN, HIGH);
  digitalWrite(RELAY2_PIN, HIGH);

  attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, CHANGE);
  if (digitalRead(PIR_PIN) == LOW) pirReady = true;

  pinMode(RESET_PIN, INPUT_PULLUP);

  ESP.wdtEnable(3000);
  loadConfig();

  WiFiManager wm;
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_user("user", "MQTT User", mqtt_user, 32);
  WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Password", mqtt_pass, 32);

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);

  wm.setTimeout(180);
  if (!wm.autoConnect("ESP_MOTION_AP")) {
    Serial.println("Failed connection, reboot...");
    delay(3000);
    ESP.restart();
  }

  strlcpy(mqtt_server, custom_mqtt_server.getValue(), sizeof(mqtt_server));
  strlcpy(mqtt_port, custom_mqtt_port.getValue(), sizeof(mqtt_port));
  strlcpy(mqtt_user, custom_mqtt_user.getValue(), sizeof(mqtt_user));
  strlcpy(mqtt_pass, custom_mqtt_pass.getValue(), sizeof(mqtt_pass));
  saveConfig();

  // Build all MQTT topics once, using fixed char[] buffers
  snprintf(baseTopic,             sizeof(baseTopic),             "home/%s",                            DEVICE_NAME);
  snprintf(stateMotionTopic,      sizeof(stateMotionTopic),      "%s/binary_sensor/motion",            baseTopic);
  snprintf(stateLightRelayTopic,  sizeof(stateLightRelayTopic),  "%s/switch/light",                    baseTopic);
  snprintf(stateFanRelayTopic,    sizeof(stateFanRelayTopic),    "%s/switch/fan",                      baseTopic);
  snprintf(boostFanTopic,         sizeof(boostFanTopic),         "%s/switch/fan/boost/set",            baseTopic);
  snprintf(availabilityTopic,     sizeof(availabilityTopic),     "%s/status",                          baseTopic);
  snprintf(stateRssiTopic,        sizeof(stateRssiTopic),        "%s/sensor/rssi",                     baseTopic);
  snprintf(resetTopic,            sizeof(resetTopic),            "%s/reset/set",                       baseTopic);
  snprintf(resetStatusTopic,      sizeof(resetStatusTopic),      "%s/reset/status",                    baseTopic);
  snprintf(modeTopic,             sizeof(modeTopic),             "%s/mode",                            baseTopic);
  snprintf(modeSetTopic,          sizeof(modeSetTopic),          "%s/mode/set",                        baseTopic);
  snprintf(haMotionConfigTopic,   sizeof(haMotionConfigTopic),   "homeassistant/binary_sensor/%s_motion/config", DEVICE_NAME);
  snprintf(haLightConfigTopic,    sizeof(haLightConfigTopic),    "homeassistant/switch/%s_light/config",         DEVICE_NAME);
  snprintf(haFanConfigTopic,      sizeof(haFanConfigTopic),      "homeassistant/switch/%s_fan/config",           DEVICE_NAME);
  snprintf(haBoostConfigTopic,    sizeof(haBoostConfigTopic),    "homeassistant/number/%s_fan_boost/config",     DEVICE_NAME);
  snprintf(haResetConfigTopic,    sizeof(haResetConfigTopic),    "homeassistant/button/%s_reset/config",         DEVICE_NAME);
  // Pre-built "/set" topics used for strcmp in mqttCallback
  snprintf(lightSetTopic,         sizeof(lightSetTopic),         "%s/set",                             stateLightRelayTopic);
  snprintf(fanSetTopic,           sizeof(fanSetTopic),           "%s/set",                             stateFanRelayTopic);

  mqtt.setServer(mqtt_server, atoi(mqtt_port));
  mqtt.setBufferSize(768);  // Увеличен буфер за HA Discovery JSON съобщения
  connectMQTT();
  lastMqttConnectionTime = millis();
  lastWifiReport = millis();
  validateAndCorrectState();
  Serial.println("Safety system initialized");
}

void handleRelayControl() {
  unsigned long currentTime = millis();
  bool currentPir = lastPirState;

  // Check for Boost Timer Expiration
  if (boostActive && timeoutExpired(boostStartTime, boostDuration)) {
    boostActive = false;
    boostEndTime = millis();
 
  // We only turn it off if there is no logical reason for it to be running
    bool shouldFanBeOn = lightOn && 
                      !timeoutExpired(lastMotionTimeForFan, RECENT_MOTION_WINDOW_MS);

    if (!shouldFanBeOn) {
      setFanRelay(false);
    }


    Serial.println("Boost mode expired");
  }

  // Check for manual control leakage
  if (manualLightControl && timeoutExpired(manualControlStart, MANUAL_CONTROL_DURATION)) {
    manualLightControl = false;
    Serial.println("Manual control timeout expired - returning to auto mode");
  }

  // If there is motion and Boost is active, disable Boost and switch to normal operation
  if (boostActive && currentPir && pirReady) {
    boostActive = false;
    Serial.println("Movement detected during Boost - switching to normal mode");
  }

  if (currentPir && pirReady) {
    lastMotionTime = currentTime;
    lastMotionTimeForFan = currentTime;  // We remember the time for the fan

    if (fanOn && !lightOn) {
      setLightRelay(true, false);
      lightOffTime = currentTime;
      Serial.println("New movement when fan is running - light turns on again");
    }

    if (lightOn && fanScheduled) {
      if (!messageShown) {
        Serial.println("Motion with active light and waiting fan");
        messageShown = true;  // Mark that the message is shown
      }
    } else {
      messageShown = false;  // Reset when the condition is not met
    }

    if (!motionActive && !manualLightControl) {  // We do not automatically start moving when in manual mode
      continuousMotionStart = currentTime;
      motionActive = true;
      lightScheduled = true;
      Serial.println("Continuous motion started - scheduled light");
    }

    if (motionActive && lightScheduled && timeoutExpired(continuousMotionStart, LIGHT_DELAY_MS) && !lightOn && !manualLightControl) {
      setLightRelay(true, false);
      lightScheduled = false;
      fanScheduled = true;
      lightOnTime = currentTime;
      Serial.println("Light turned on after 1 sec of continuous motion");
    }
  } else {
    if (motionActive && !manualLightControl) {  // Не прекъсваме движение при ръчно управление
      motionActive = false;
      continuousMotionStart = 0;
      lightScheduled = false;
      Serial.println("Continuous motion interrupted - cancel light scheduling");
    }
    messageShown = false;  // Reset when no motion
  }

  // CHANGED FAN SECTION:
  // The fan turns on if there has been movement in the last 20 seconds
  if (fanScheduled && lightOn && !boostActive) {
    // Check if there has been any movement in the last 20 seconds
    bool hadRecentMotion = !timeoutExpired(lastMotionTimeForFan, RECENT_MOTION_WINDOW_MS);
    
    if (timeoutExpired(lightOnTime, FAN_DELAY_MS) && hadRecentMotion) {
      setFanRelay(true);
      fanScheduled = false;
      messageShown = false;  // Reset when fan turns on
      Serial.println("Fan turned on after 70 sec (with recent motion detected)");
    }
  }

  // === SMART OFF LOGIC  ===
  if (lightOn && 
    !manualLightControl &&
    !boostActive &&
    timeoutExpired(boostEndTime, 5000)) {
    if (!lastPirState) {
      if (!noMotionCandidate) {
        noMotionCandidate = true;
        noMotionStartTime = currentTime;
      }
      // Confirm that there is really no motion
      if (timeoutExpired(noMotionStartTime, MOTION_CONFIRM_OFF_MS)) {
        if (!lastPirState && timeoutExpired(lastPirOffTime, MOTION_TIMEOUT_MS)) {
          setLightRelay(false, false);
          lightScheduled = false;
          motionActive = false;
          fanScheduled = false;
          lightOffTime = currentTime;
          noMotionCandidate = false;
          messageShown = false;
          Serial.println("Light turned off after timeout (auto mode)");
        }
      }
    } else {
      // Motion detected → reset candidate
      noMotionCandidate = false;
    }
  }

  // Fan off after a period without light (unless Boost is active)
  if (fanOn && !lightOn && timeoutExpired(lightOffTime, FAN_OFF_DELAY_MS) && !boostActive) {
    setFanRelay(false);
    messageShown = false;  // Reset on fan off
  }
}

void loop() {
  ESP.wdtFeed();

  // Debounce for reset button - check every 50ms
  if (millis() - lastResetCheck > 50) {
    lastResetCheck = millis();

    if (digitalRead(RESET_PIN) == LOW) {
      if (resetPressStart == 0) {
        resetPressStart = millis();
        Serial.println("Reset button pressed... hold for 3 seconds");
      }

      if (timeoutExpired(resetPressStart, 3000)) {
        Serial.println("Reset button held for 3 seconds - reset settings");
        WiFiManager wm;
        wm.resetSettings();
        LittleFS.remove("/config.json");
        delay(500);
        ESP.restart();
      }
    } else {
      if (resetPressStart != 0) {
        Serial.println("Reset button released 3 seconds ago");
        resetPressStart = 0;
      }
    }
  }

  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (timeoutExpired(lastMqttAttempt, MQTT_RETRY_MS)) {
      lastMqttAttempt = now;
      connectMQTT();
    }
  } else {
    mqtt.loop();
  }

  if (mqtt.connected()) {
    lastMqttConnectionTime = millis();
    if (failsafeMode) {
      Serial.println("Exit Failsafe mode - MQTT reconnects");
      failsafeMode = false;
    }
  } else if (!failsafeMode && timeoutExpired(lastMqttConnectionTime, MQTT_FAILSAFE_TIMEOUT_MS)) {
    enterFailsafeMode();
  }

  if (failsafeMode) {
    handleFailsafeLogic();
  }

  if (timeoutExpired(lastSafetyCheck, 30000)) {
    validateAndCorrectState();
    lastSafetyCheck = millis();
  }

  unsigned long now = millis();

  if (!pirReady) {
    if (digitalRead(PIR_PIN) == LOW) pirReady = true;
  } else {
    bool rawPirState = (digitalRead(PIR_PIN) == HIGH);
    if (timeoutExpired(lastPirChangeMs, PIR_DEBOUNCE_MS)) {
      if (rawPirState != lastPirState) {
        lastPirState = rawPirState;
        if (!lastPirState) {
          lastPirOffTime = millis();
        }
        if (mqtt.connected()) publishMotion(lastPirState);
      }
    }
  }

  if (timeoutExpired(lastWifiReport, wifiReportInterval)) {
    lastWifiReport = now;
    if (mqtt.connected()) publishRSSI();
  }

  handleRelayControl();
}


void enterFailsafeMode() {
  if (failsafeMode) return;
  Serial.println("Enter Failsafe mode - validate hardware status first");
  validateAndCorrectState();
  Serial.println("Introduction of Failsafe mode - local operation without MQTT");
  failsafeMode = true;
}

void handleFailsafeLogic() {
  unsigned long currentTime = millis();
  bool currentPir = lastPirState;

  // Check for manual control leakage
  if (manualLightControl && timeoutExpired(manualControlStart, MANUAL_CONTROL_DURATION)) {
    manualLightControl = false;
    Serial.println("Failsafe: Manual control timeout expired - returning to auto mode");
  }

  if (currentPir && pirReady) {
    lastMotionTime = currentTime;
    lastMotionTimeForFan = currentTime;

    if (fanOn && !lightOn) {
      setLightRelay(true, false);
      lightOffTime = currentTime;
      Serial.println("Failsafe: New motion when fan is running - light turns on again");
      failsafeMessageShown = false;
    }

    if (lightOn && fanScheduled) {
      if (!failsafeMessageShown) {
        Serial.println("Failsafe: Motion with active light and waiting fan");
        failsafeMessageShown = true;
      }
    } else {
      failsafeMessageShown = false;
    }

    if (!motionActive && !manualLightControl) {
      continuousMotionStart = currentTime;
      motionActive = true;
      lightScheduled = true;
      Serial.println("Failsafe: Started continuous motion - scheduled light");
      failsafeMessageShown = false;
    }

    if (motionActive && lightScheduled && timeoutExpired(continuousMotionStart, LIGHT_DELAY_MS) && !lightOn && !manualLightControl) {
      setLightRelay(true, false);
      lightScheduled = false;
      fanScheduled = true;
      lightOnTime = currentTime;
      Serial.println("Failsafe: Light turned on after 1 sec continuous movement");
      failsafeMessageShown = false;
    }
  } else {
    if (motionActive && !manualLightControl) {
      motionActive = false;
      continuousMotionStart = 0;
      lightScheduled = false;
      Serial.println("Failsafe: Interrupt continuous motion - cancel light scheduling");
    }
    failsafeMessageShown = false;
  }

  // FAN SECTION IN FAILSAFE:
  if (fanScheduled && lightOn && !manualLightControl) {
    bool hadRecentMotion = !timeoutExpired(lastMotionTimeForFan, RECENT_MOTION_WINDOW_MS);
    if (timeoutExpired(lightOnTime, FAN_DELAY_MS) && hadRecentMotion) {
      setFanRelay(true);
      fanScheduled = false;
      Serial.println("Failsafe: Fan turned on after 70 sec (with recent motion detected)");
      failsafeMessageShown = false;
    }
  }

  // === BOOST DRAIN (with smart shut-off) ===
  if (fanOn && boostActive && timeoutExpired(boostStartTime, boostDuration)) {
    boostActive = false;
    boostEndTime = millis();   // Запомняме кога е изтекъл Boost

    bool shouldFanBeOn = lightOn &&
                         !timeoutExpired(lastMotionTimeForFan, RECENT_MOTION_WINDOW_MS);
    if (!shouldFanBeOn) {
      setFanRelay(false);
    }
    Serial.println("Failsafe: Boost mode expired");
  }

  // === SMART OFF LOGIC (with a grace period following the Boost) ===
  if (lightOn && !manualLightControl &&
      (boostEndTime == 0 || timeoutExpired(boostEndTime, 5000))) {
    if (!lastPirState) {
      if (!noMotionCandidate) {
        noMotionCandidate = true;
        noMotionStartTime = currentTime;
      }
      if (timeoutExpired(noMotionStartTime, MOTION_CONFIRM_OFF_MS)) {
        if (!lastPirState && timeoutExpired(lastPirOffTime, MOTION_TIMEOUT_MS)) {
          setLightRelay(false, false);
          lightScheduled = false;
          motionActive = false;
          fanScheduled = false;
          lightOffTime = currentTime;
          noMotionCandidate = false;
          failsafeMessageShown = false;
          Serial.println("Failsafe: Light turned off after timeout (auto mode)");
        }
      }
    } else {
      noMotionCandidate = false;
    }
  }

  // Turn off the fan after turning off the light
  if (fanOn && !lightOn && timeoutExpired(lightOffTime, FAN_OFF_DELAY_MS)) {
    setFanRelay(false);
    Serial.println("Failsafe: Fan turned off 90 seconds after light turned off");
    failsafeMessageShown = false;
  }
}

//PIR sensor (HC-SR501 or similar)
//VCC → 3,3V.
//GND → GND on the ESP8266.
//OUT → D5 (GPIO14).
//Recommended default settings:
//Mandatory Jumper → H (Repeat)  In Repeat mode, the sensor will keep OUT = HIGH as long as there is movement. 
//This is necessary for the 0.6-second continuous movement logic.
//Sx (sensitivity) → medium
//Tx (delay) → minimum (3–5 sec)

//Relay module
//VCC → 3.3V
//GND → GND
//IN1 → D6 (GPIO12) - lighting
//IN2 → D7 (GPIO13) - fan

// Reset Button - D3
// GND → GND

//The Boost function is activated by an MQTT command sent to:
// topic: home/toilet_relay/switch/fan/boost/set
// Payload: 3
