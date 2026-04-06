# ESP32 Servo MQTT Controller

Control a servo via MQTT (Home Assistant) with a built-in web interface for configuration.

---

## Features

- **MQTT control** – send an angle (0–180) to move the servo
- **Web interface** served from LittleFS – dial, slider, preset buttons
- **Configurable device name** – changes the MQTT topic prefix live
- **WiFi + MQTT credentials** stored in `/config.json` on LittleFS
- **AP fallback** – if WiFi fails, opens `ESP32_Servo_Config` hotspot
- **LWT** (Last Will & Testament) – publishes `offline` on disconnect
- **DISCOVERY** - device will be discovered by home assitant MQTT integration

---

## Hardware

| Item | Detail |
|------|--------|
| Board | Any ESP32 (DevKit, WROOM, S3, …) |
| Servo | Standard 3-wire PWM positional 180 degree servo, e.g. SG90 or MG996R |
| Servo pin | GPIO 2 (change `SERVO_PIN` in user_config.h ) |

Wiring: servo signal → GPIO 2, servo VCC → 5 V (external supply recommended), GND → common ground.

---

## Software Requirements

| Library | Author |
|---------|--------|
| ESP32Servo | Kevin Harrington |
| PubSubClient | Nick O'Leary |
| ArduinoJson | Benoit Blanchon (v7) |

LittleFS is built into the ESP32 Arduino core ≥ 2.x — no separate install needed.

---

## Project Structure

```
esp32_servo_mqtt/
├── main.cpp   ← main sketch
├── data/
│   └── index.html         ← web UI (uploaded to LittleFS)
├── user_config.h 
└── README.md
```

---

## Upload Steps

### 1 — Install LittleFS upload tool

- **Arduino IDE 1.x**: install [arduino-esp32fs-plugin](https://github.com/lorol/arduino-esp32fs-plugin)
- **Arduino IDE 2.x**: install [LittleFS_esp32 uploader](https://github.com/earlephilhower/arduino-littlefs-upload) (`Ctrl+Shift+P` → *Upload LittleFS to Pico/ESP8266/ESP32*)

### 2 — Set your board

Board: `ESP32 Dev Module` (or your specific variant)  
Flash size: at least **4 MB** with partition scheme **"Default 4MB with spiffs"** or **"Minimal SPIFFS"** — both expose LittleFS.

### 3 — Edit defaults (optional)

Open `esp32_servo_mqtt.ino` and change the `DEFAULT_*` constants at the top.  
You can also configure everything via the web UI after first boot.

### 4 — Upload sketch

Compile and upload normally (`Ctrl+U`).

### 5 — Upload LittleFS data

Place your cursor in the sketch, then use the upload tool (step 1) to push the `data/` folder.  
This puts `index.html` at `/index.html` on the device filesystem.

---

## First Boot

1. Open Serial Monitor (115200 baud).
2. If WiFi credentials are correct the device connects and prints its IP.
3. If WiFi fails it starts an AP: **SSID** `ESP32_Servo_Config`, **password** `config1234`.
4. Browse to the printed IP (or `192.168.4.1` in AP mode).

---

## Web Interface

| Tab | Description |
|-----|-------------|
| **Servo Control** | Dial + slider + preset buttons, sends angle immediately |
| **Topics** | Shows live MQTT topic names derived from device name |
| **Config** | Edit device name, WiFi, MQTT broker; click *Save & Apply* |

**Changing the Device Name** updates MQTT topics immediately — no restart needed.  
A restart is required only when changing WiFi credentials.

---

## MQTT Topics

All topics use the **Device Name** as a prefix (default `esp32_servo`):

| Topic | Direction | Payload |
|-------|-----------|---------|
| `<name>/servo/set` | Subscribe | `0`–`180` (integer string) |
| `<name>/servo/state` | Publish | current angle, retained |
| `<name>/status` | Publish | `online` / `offline` (LWT), retained |

### Home Assistant example

See `homeassistant_mqtt.yaml`. After adding the YAML and reloading HA, a  
`number.servo_angle` entity appears with a 0–180° slider.

---

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Serve `index.html` from LittleFS |
| GET | `/api/status` | JSON: angle, wifi/mqtt state, topics |
| GET | `/api/config` | JSON: current config (no passwords) |
| POST | `/api/config` | JSON body: update config fields |
| POST | `/api/servo` | JSON `{"angle":90}`: move servo directly |
| POST | `/api/restart` | Reboot ESP32 |

---

## Customisation in user_config.h

- **Servo pin**: change `SERVO_PIN`
- **Pulse range**: change `SERVO_MIN_US` / `SERVO_MAX_US` for non-standard servos
- **Multiple servos**: duplicate the servo object, pin, and MQTT topic pattern
