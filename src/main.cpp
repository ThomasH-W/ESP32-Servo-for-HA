/*
  File : main.cpp
  Date : 3.4.2026

  ToDo:

* ESP32 Servo MQTT Controller
* - Controls a servo via MQTT
* - Web interface served from LittleFS
* - Configurable device name, WiFi, MQTT via web UI
* - Config stored in LittleFS as /config.json
*
* Board: ESP32 (any variant)
* Libraries needed:
*   - ESP32Servo            (by Kevin Harrington)
*   - PubSubClient          (by Nick O'Leary)
*   - ArduinoJson           (by Benoit Blanchon) v6+
*   - LittleFS (built-in with ESP32 Arduino core >= 2.x)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// Try secret file first, fall back to defaults
#if __has_include("user_config_secret.h")
  #include "user_config_secret.h"
#else
  #include "user_config.h"
#endif

// ─── Runtime config struct ────────────────────────────────────────────────────
struct Config
{
  char wifiSSID[64];
  char wifiPassword[64];
  char mqttServer[64];
  int mqttPort;
  char mqttUser[64];
  char mqttPassword[64];
  char deviceName[64]; // used as MQTT topic prefix
  char mqttPrefix[64]; // e.g. "home" or "house"
};

Config cfg;

// ─── MQTT topic helpers (built from deviceName at runtime) ───────────────────
// Subscribed:  <deviceName>/servo/set    → payload: 0–180 (degrees)
// Published:   <deviceName>/servo/state  → current angle
// Published:   <deviceName>/status       → "online" / "offline" (LWT)
char topicSet[128];
char topicSetPct[128];
char topicState[128];
char topicStatus[128];
char topicDiscovery[512];
char topicDiscoveryPct[512];

// ─── Objects ─────────────────────────────────────────────────────────────────
Servo servo;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer server(80);

int currentAngle = SERVO_INITIAL;
bool needsDiscovery = true;
bool wifiConnected = false;
bool mqttConnected = false;
bool mqttReturnCodeOK = false;
unsigned long lastMqttReconnect = 0;

// ═════════════════════════════════════════════════════════════════════════════
// CONFIG  –  load / save / build topics
// ═════════════════════════════════════════════════════════════════════════════

void buildTopics()
{
  snprintf(topicSet, sizeof(topicSet), "%scmnd/%s/set", cfg.mqttPrefix, cfg.deviceName);
  snprintf(topicSetPct, sizeof(topicSetPct), "%scmnd/%s/set_pct", cfg.mqttPrefix, cfg.deviceName);
  snprintf(topicState, sizeof(topicState), "%sstat/%s/state", cfg.mqttPrefix, cfg.deviceName);
  snprintf(topicStatus, sizeof(topicStatus), "%stele/%s/status", cfg.mqttPrefix, cfg.deviceName);

  snprintf(topicDiscovery, sizeof(topicDiscovery), "homeassistant/number/%s/config", cfg.mqttPrefix, cfg.deviceName);
  snprintf(topicDiscoveryPct, sizeof(topicDiscoveryPct), "homeassistant/number/%s_pct/config", cfg.mqttPrefix, cfg.deviceName);
}

void loadDefaultConfig()
{
  strlcpy(cfg.wifiSSID, DEFAULT_WIFI_SSID, sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPassword, DEFAULT_WIFI_PASSWORD, sizeof(cfg.wifiPassword));
  strlcpy(cfg.mqttServer, DEFAULT_MQTT_SERVER, sizeof(cfg.mqttServer));
  cfg.mqttPort = DEFAULT_MQTT_PORT;
  strlcpy(cfg.mqttUser, DEFAULT_MQTT_USER, sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPassword, DEFAULT_MQTT_PASSWORD, sizeof(cfg.mqttPassword));
  strlcpy(cfg.deviceName, DEFAULT_DEVICE_NAME, sizeof(cfg.deviceName));
  strlcpy(cfg.mqttPrefix, DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
}

bool loadConfig()
{
  File f = LittleFS.open("/config.json", "r");
  if (!f)
  {
    Serial.println("[Config] No config.json found, using defaults.");
    loadDefaultConfig();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err)
  {
    Serial.printf("[Config] JSON parse error: %s\n", err.c_str());
    loadDefaultConfig();
    return false;
  }

  strlcpy(cfg.wifiSSID, doc["wifiSSID"] | DEFAULT_WIFI_SSID, sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPassword, doc["wifiPassword"] | DEFAULT_WIFI_PASSWORD, sizeof(cfg.wifiPassword));
  strlcpy(cfg.mqttServer, doc["mqttServer"] | DEFAULT_MQTT_SERVER, sizeof(cfg.mqttServer));
  cfg.mqttPort = doc["mqttPort"] | DEFAULT_MQTT_PORT;
  strlcpy(cfg.mqttUser, doc["mqttUser"] | DEFAULT_MQTT_USER, sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPassword, doc["mqttPassword"] | DEFAULT_MQTT_PASSWORD, sizeof(cfg.mqttPassword));
  strlcpy(cfg.deviceName, doc["deviceName"] | DEFAULT_DEVICE_NAME, sizeof(cfg.deviceName));
  strlcpy(cfg.mqttPrefix, doc["mqttPrefix"] | DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));

  Serial.printf("[Config] Loaded. Device: %s\n", cfg.deviceName);
  return true;
}

bool saveConfig()
{
  JsonDocument doc;
  doc["wifiSSID"] = cfg.wifiSSID;
  doc["wifiPassword"] = cfg.wifiPassword;
  doc["mqttServer"] = cfg.mqttServer;
  doc["mqttPort"] = cfg.mqttPort;
  doc["mqttUser"] = cfg.mqttUser;
  doc["mqttPassword"] = cfg.mqttPassword;
  doc["deviceName"] = cfg.deviceName;
  doc["mqttPrefix"] = cfg.mqttPrefix;


  File f = LittleFS.open("/config.json", "w");
  if (!f)
  {
    Serial.println("[Config] Failed to open config.json for writing.");
    return false;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("[Config] Saved.");
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// SERVO
// ═════════════════════════════════════════════════════════════════════════════

void moveServo(int angle)
{
  angle = constrain(angle, 0, 180);
  currentAngle = angle;
  servo.write(angle);
  Serial.printf("[Servo] → %d°\n", angle);

  // Publish state back
  if (mqtt.connected())
  {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", angle);
    mqtt.publish(topicState, buf, true);
  }
}

// ─────────────────────────────────────────────
//  HA DISCOVERY  – publishes a retained config
//  message so Home Assistant auto-creates the entity
// ─────────────────────────────────────────────
void publishDiscovery()
{
  JsonDocument doc;

  doc["name"] = "Servo Position";
  doc["unique_id"] = cfg.deviceName;
  doc["command_topic"] = topicSet;
  doc["state_topic"] = topicState;
  doc["availability_topic"] = topicStatus;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  doc["min"] = SERVO_MIN_DEG;
  doc["max"] = SERVO_MAX_DEG;
  doc["step"] = 1;
  doc["unit_of_measurement"] = "°";
  doc["icon"] = "mdi:rotate-right";

  // Nested device block groups entity under one device in HA
  doc["device"]["identifiers"][0] = cfg.deviceName;
  doc["device"]["name"] = cfg.deviceName;
  doc["device"]["model"] = "ESP32 + MG996R";
  doc["device"]["manufacturer"] = "DIY";
  doc["device"]["sw_version"] = "1.0.0";

  char payload[512];
  serializeJson(doc, payload);

  // Publish retained so HA picks it up after restart
  Serial.printf("[MQTT] %s → %s; length:%d\n", topicDiscovery, payload, strlen(payload));
  bool ok = mqtt.publish(topicDiscovery, payload, /*retain=*/true);
  Serial.printf("Publish Discovery %s: %s\n\n", ok ? "OK" : "FAILED", topicDiscovery);

  // ── Optional second entity: percentage control ──────────────────
  JsonDocument doc2;
  // String discoveryPct = "homeassistant/number/" + baseTopic + "_pct/config";

  doc2["name"] = "Servo Position %";
  doc2["unique_id"] = String(cfg.deviceName) + "_servo_pct";
  doc2["command_topic"] = topicSetPct;
  doc2["state_topic"] = topicState; // reuses the degree state; or add separate
  doc2["availability_topic"] = topicStatus;
  doc2["payload_available"] = "online";
  doc2["payload_not_available"] = "offline";
  doc2["min"] = 0;
  doc2["max"] = 100;
  doc2["step"] = 1;
  doc2["unit_of_measurement"] = "%";
  doc2["icon"] = "mdi:percent";
  
  doc2["device"]["identifiers"][0] = cfg.deviceName;
  doc2["device"]["name"] = cfg.deviceName;


  // char payload2[512];
  serializeJson(doc2, payload);
  Serial.printf("[MQTT] %s → %s; length:%d\n", topicDiscoveryPct, payload, strlen(payload));
  ok = mqtt.publish(topicDiscoveryPct, payload, /*retain=*/true);
  Serial.printf("Publish Discovery %s: %s\n\n", ok ? "OK" : "FAILED", topicDiscoveryPct);

}

// ═════════════════════════════════════════════════════════════════════════════
// MQTT
// ═════════════════════════════════════════════════════════════════════════════
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  char msg[16] = {0};
  if (length >= sizeof(msg))
    length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  Serial.printf("[MQTT] %s → %s\n", topic, msg);

  if (strcmp(topic, topicSet) == 0)
  {
    int angle = atoi(msg);
    moveServo(angle);
  }
}

bool mqttReconnect()
{
  if (mqtt.connected())
    return true;
  if (millis() - lastMqttReconnect < 5000)
    return false;
  lastMqttReconnect = millis();

  Serial.printf("[MQTT] Connecting to %s:%d as %s ...\n",
                cfg.mqttServer, cfg.mqttPort, cfg.deviceName);

  bool ok;
  if (strlen(cfg.mqttUser) > 0)
  {
    ok = mqtt.connect(cfg.deviceName, cfg.mqttUser, cfg.mqttPassword,
                      topicStatus, 0, true, "offline");
  }
  else
  {
    ok = mqtt.connect(cfg.deviceName, nullptr, nullptr,
                      topicStatus, 0, true, "offline");
  }

  if (ok)
  {
    Serial.println("[MQTT] Connected.");
    mqtt.publish(topicStatus, "online", true);
    mqtt.subscribe(topicSet);
    mqtt.publish(topicDiscovery, "init", true);

    // sendDiscovery();
    // Publish discovery once per connection
    if (needsDiscovery)
    {
      publishDiscovery();
      needsDiscovery = false;
    }

    mqttConnected = true;

    // Publish current state
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", currentAngle);
    mqtt.publish(topicState, buf, true);
  }
  else
  {
    Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
    mqttConnected = false;
  }
  return ok;
}

// ═════════════════════════════════════════════════════════════════════════════
// WEB SERVER – routes
// ═════════════════════════════════════════════════════════════════════════════

// Helper: serve a file from LittleFS with correct MIME type
bool serveFile(const char *path, const char *mime)
{
  File f = LittleFS.open(path, "r");
  if (!f)
    return false;
  server.streamFile(f, mime);
  f.close();
  return true;
}

String getMimeType(const String &path)
{
  if (path.endsWith(".html"))
    return "text/html";
  if (path.endsWith(".css"))
    return "text/css";
  if (path.endsWith(".js"))
    return "application/javascript";
  if (path.endsWith(".json"))
    return "application/json";
  if (path.endsWith(".ico"))
    return "image/x-icon";
  if (path.endsWith(".svg"))
    return "image/svg+xml";
  return "text/plain";
}

// GET /  →  serve index.html from LittleFS
void handleRoot()
{
  if (!serveFile("/index.html", "text/html"))
  {
    server.send(500, "text/plain", "index.html not found in LittleFS. "
                                   "Please upload data/ folder via LittleFS upload tool.");
  }
}

// GET /api/status  →  JSON status
void handleApiStatus()
{
  JsonDocument doc;
  doc["deviceName"] = cfg.deviceName;
  doc["angle"] = currentAngle;
  doc["wifiConnected"] = wifiConnected;
  doc["mqttConnected"] = mqttConnected;
  doc["mqttServer"] = cfg.mqttServer;
  doc["topicSet"] = topicSet;
  doc["topicState"] = topicState;
  doc["topicStatus"] = topicStatus;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// GET /api/config  →  JSON config (no passwords)
void handleApiConfigGet()
{
  JsonDocument doc;
  doc["wifiSSID"] = cfg.wifiSSID;
  doc["mqttServer"] = cfg.mqttServer;
  doc["mqttPort"] = cfg.mqttPort;
  doc["mqttUser"] = cfg.mqttUser;
  doc["deviceName"] = cfg.deviceName;
  // Passwords intentionally omitted from GET response

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/config  →  update config from JSON body
void handleApiConfigPost()
{
  if (!server.hasArg("plain"))
  {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err)
  {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  // Update only provided fields
  // V6: if (doc.containsKey("value")) {
  // V7: if (doc["value"].is<JsonVariant>()) {
  //if (doc.containsKey("wifiSSID"))
  if (doc["wifiSSID"].is<JsonVariant>()) 
    strlcpy(cfg.wifiSSID, doc["wifiSSID"], sizeof(cfg.wifiSSID));
  if (doc["wifiPassword"].is<JsonVariant>()) 
    strlcpy(cfg.wifiPassword, doc["wifiPassword"], sizeof(cfg.wifiPassword));
  if (doc["mqttServer"].is<JsonVariant>())
    strlcpy(cfg.mqttServer, doc["mqttServer"], sizeof(cfg.mqttServer));
  if (doc["mqttPort"].is<JsonVariant>())
    cfg.mqttPort = doc["mqttPort"];
  if (doc["mqttUser"].is<JsonVariant>())
    strlcpy(cfg.mqttUser, doc["mqttUser"], sizeof(cfg.mqttUser));
  if (doc["mqttPassword"].is<JsonVariant>())
    strlcpy(cfg.mqttPassword, doc["mqttPassword"], sizeof(cfg.mqttPassword));
  if (doc["deviceName"].is<JsonVariant>())
    strlcpy(cfg.deviceName, doc["deviceName"], sizeof(cfg.deviceName));

  saveConfig();
  buildTopics();

  // Re-apply MQTT broker address/port
  mqtt.setServer(cfg.mqttServer, cfg.mqttPort);
  if (mqtt.connected())
  {
    mqtt.disconnect();
    mqttConnected = false;
  }

  server.send(200, "application/json", "{\"status\":\"saved\",\"restarting\":false}");
  Serial.println("[Web] Config updated via web UI.");
}

// POST /api/servo  →  { "angle": 90 }
void handleApiServo()
{
  if (!server.hasArg("plain"))
  {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")))
  {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  int angle = doc["angle"] | currentAngle;
  moveServo(angle);
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// POST /api/restart
void handleApiRestart()
{
  server.send(200, "application/json", "{\"status\":\"restarting\"}");
  delay(500);
  ESP.restart();
}

void setupWebServer()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/config", HTTP_GET, handleApiConfigGet);
  server.on("/api/config", HTTP_POST, handleApiConfigPost);
  server.on("/api/servo", HTTP_POST, handleApiServo);
  server.on("/api/restart", HTTP_POST, handleApiRestart);

  // Serve any other static file from LittleFS
  server.onNotFound([]()
                    {
    String path = server.uri();
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path, "r");
      server.streamFile(f, getMimeType(path));
      f.close();
    } else {
      server.send(404, "text/plain", "Not found");
    } });

  server.enableCORS(true);
  server.begin();
  Serial.println("[Web] HTTP server started on port 80");
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP & LOOP
// ═════════════════════════════════════════════════════════════════════════════

void setup()
{
  Serial.begin(MONITOR_SPEED);
  delay(200);
  Serial.println("\n[Boot] ESP32 Servo MQTT Controller");

  // LittleFS
  if (!LittleFS.begin(true))
  { // true = format on fail
    Serial.println("[FS] LittleFS mount FAILED");
  }
  else
  {
    Serial.println("[FS] LittleFS mounted");
  }

  // Load config
  loadConfig();
  buildTopics();

  // Servo
  ESP32PWM::allocateTimer(0);
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
  servo.write(currentAngle);
  Serial.printf("[Servo] Attached to GPIO%d at %d°\n", SERVO_PIN, currentAngle);

  // WiFi
  Serial.printf("[WiFi] Connecting to %s ...\n", cfg.wifiSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSSID, cfg.wifiPassword);

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    Serial.println("[WiFi] Failed to connect. Starting AP for config...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32_Servo_Config", "config1234");
    Serial.printf("[WiFi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  }

  // MQTT
  mqtt.setServer(cfg.mqttServer, cfg.mqttPort);
  mqtt.setBufferSize(1024); // This is a classic PubSubClient gotcha. The default internal buffer size is 256 bytes, and that budget covers the entire MQTT packet — not just your payload.
  mqtt.setCallback(mqttCallback);

  // Web server
  setupWebServer();
}

void loop()
{
  server.handleClient();

  // Keep WiFi alive
  wifiConnected = (WiFi.status() == WL_CONNECTED);

  // MQTT reconnect + loop
  if (wifiConnected)
  {
    if (!mqtt.connected())
    {
      mqttReconnect();
    }
    if (mqtt.connected())
    {
      mqtt.loop();
      mqttConnected = true;
    }
    else
    {
      mqttConnected = false;
    }
  }

  delay(1);
}