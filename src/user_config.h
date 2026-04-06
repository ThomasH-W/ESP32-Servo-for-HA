/*
  user_config.h - configuration w/ user credentials and hardware settings

    This file is included by main.cpp and contains:
     - Default config values (used if no config.json exists)
     - Hardware pin definitions and constants

*/
#pragma once

// ─── Default configuration (used if no config.json exists) ───────────────────
#define DEFAULT_WIFI_SSID "yourSSID"
#define DEFAULT_WIFI_PASSWORD "yourPassword"
#define DEFAULT_MQTT_SERVER "192.168.x.y"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_USER ""
#define DEFAULT_MQTT_PASSWORD ""
#define DEFAULT_MQTT_PREFIX "home/"
#define DEFAULT_DEVICE_NAME "esp32_servo"

// ─── Hardware ─────────────────────────────────────────────────────────────────
#define SERVO_PIN 2  // Pin D2 mapped to pin GPIO2/ADC12/TOUCH2 of ESP32
#define SERVO_MIN_US 500  // Minimum pulse width (µs)
#define SERVO_MAX_US 2400 // Maximum pulse width (µs)
const int SERVO_MIN_DEG = 0;
const int SERVO_MAX_DEG = 180;
const int SERVO_INITIAL = 90; // position on boot
