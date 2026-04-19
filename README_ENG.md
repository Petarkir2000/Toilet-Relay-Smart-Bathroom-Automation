# Toilet Relay — Smart Bathroom Automation

> **ESP8266 NodeMCU V3** · PIR motion sensor · Dual relay (light + fan) · MQTT · Home Assistant Auto Discovery  
> Version: `2026.00` — Final release, April 2026

---

## Overview

Firmware for automatic control of lighting and a ventilation fan in a bathroom/toilet, based on a PIR motion sensor. The device integrates with **Home Assistant** via MQTT with Auto Discovery and can operate standalone (Failsafe mode) when the network connection is lost.

---

## Hardware

| Component | Model / Detail |
|---|---|
| Microcontroller | ESP8266 NodeMCU V3 |
| PIR sensor | HC-SR501 (or compatible) |
| Relay 1 | Lighting — D6 (GPIO12) |
| Relay 2 | Fan — D7 (GPIO13) |
| Reset button | D3 (GPIO0) |

### Wiring

**PIR sensor (HC-SR501)**
```
VCC → 3.3V
GND → GND
OUT → D5 (GPIO14)
```
> ⚠️ The jumper **must** be set to **H (Repeat)** mode.  
> Tx (delay) → minimum (3–5 sec), Sx (sensitivity) → medium.

**Relay module**
```
VCC → 3.3V
GND → GND
IN1 → D6 (GPIO12)   ← lighting
IN2 → D7 (GPIO13)   ← fan
```

**Reset button**
```
one terminal   → D3 (GPIO0)
other terminal → GND
```

---

## Dependencies (Arduino Libraries)

- `ESP8266WiFi`
- `DNSServer`
- `ESP8266WebServer`
- [`WiFiManager`](https://github.com/tzapu/WiFiManager)
- [`PubSubClient`](https://github.com/knolleary/pubsubclient)
- [`ArduinoJson`](https://arduinojson.org/) (v6)
- `LittleFS` (built into ESP8266 core)

---

## Features

### Automatic Mode (PIR)

| Step | Logic |
|---|---|
| Motion detected | PIR must be continuously active for **0.6 sec** before the light turns on |
| Fan activation | **70 sec** after the light turns on, **only if motion was detected** within the last 20 sec |
| Light off | **70 sec** without motion (Smart OFF: 5 sec confirmation delay) |
| Fan off | **90 sec** after the light turns off |

### Smart OFF Filter

To prevent false shutdowns caused by brief PIR signal interruptions, a two-stage filter is implemented:
1. PIR → OFF → a 5-second timer starts (`MOTION_CONFIRM_OFF_MS`)
2. If after 5 sec PIR is still OFF **and** 70 sec have passed since the last detected motion → the light turns off

### Manual Mode

The light can be turned on/off manually via MQTT. When switched on manually:
- PIR automation is temporarily disabled
- Manual mode expires automatically after **5 minutes** (`MANUAL_CONTROL_DURATION`)
- Status is reported to Home Assistant via the `mode` entity (`AUTO` / `MANUAL`)

### Boost Function

Forces the fan on for a set number of minutes (1–10), regardless of PIR logic.

```
MQTT topic:   home/toilet_relay/switch/fan/boost/set
Payload:      3       ← number of minutes (integer)
```

When the Boost timer expires, the fan turns off unless an active PIR session is in progress.

### Failsafe Mode

If the MQTT connection is lost for **5 minutes**, the device enters Failsafe mode:
- Operates fully autonomously with the complete PIR logic
- Automatically exits Failsafe when the MQTT connection is restored
- All relay operations behave identically to normal mode

### Safety Checks

- **Watchdog**: `ESP.wdtFeed()` called in the main loop
- **Periodic validation**: relay states are checked and corrected every 30 sec
- **Failure counter**: logs when more than 3 relay operations fail consecutively
- **Reset rate limiting**: maximum 3 reset attempts per minute; if exceeded → blocked for 5 minutes

---

## Configuration

### Initial Setup (WiFiManager)

On first boot (or after a reset), the device starts a WiFi Access Point:

```
SSID:     toilet_relay
Password: (none)
```

Connect to it and open `http://192.168.4.1` → enter your WiFi and MQTT credentials.

MQTT settings are stored in **LittleFS** (`/config.json`) and loaded on every boot.

### Timing Constants (in code)

```cpp
LIGHT_DELAY_MS          = 600      // 0.6 sec continuous motion → light ON
FAN_DELAY_MS            = 70000    // 70 sec after light ON → fan ON
MOTION_TIMEOUT_MS       = 70000    // 70 sec without motion → light OFF
FAN_OFF_DELAY_MS        = 90000    // 90 sec after light OFF → fan OFF
BOOST_DURATION_MS       = 180000   // 3 min default Boost
RECENT_MOTION_WINDOW_MS = 20000    // 20 sec window for "recent motion" check
MQTT_FAILSAFE_TIMEOUT   = 300000   // 5 min without MQTT → Failsafe
MANUAL_CONTROL_DURATION = 300000   // 5 min manual mode timeout
MOTION_CONFIRM_OFF_MS   = 5000     // 5 sec confirmation before light OFF
```

---

## MQTT Topics

All topics share the base prefix `home/toilet_relay`.

| Entity | State topic | Command topic |
|---|---|---|
| Motion sensor | `.../sensor/motion/state` | — |
| Light relay | `.../switch/light/state` | `.../switch/light/set` |
| Fan relay | `.../switch/fan/state` | `.../switch/fan/set` |
| Fan Boost | — | `.../switch/fan/boost/set` |
| Mode (AUTO/MANUAL) | `.../mode` | `.../mode/set` |
| Availability | `.../availability` | — |
| WiFi RSSI | `.../sensor/rssi/state` | — |
| Reset | — | `.../reset` |
| Reset status | `.../reset/status` | — |

### Home Assistant Auto Discovery

On connection, the device publishes discovery configs to the `homeassistant/` topics for:
- `binary_sensor` → Motion
- `switch` → Light, Fan
- `number` → Fan Boost (1–10 min)
- `select` → Mode (AUTO / MANUAL)
- `button` → Reset

---

## Reset Procedures

### Physical reset (hardware button)
Hold the button on **D3 for 3 seconds** → clears WiFi settings and `/config.json` → restarts in AP mode.

### MQTT reset (remote)
```
Topic:    home/toilet_relay/reset
Payload:  <secret_code>
```
A secret password is required. After more than 3 failed attempts within 1 minute, reset commands are blocked for **5 minutes** and an alarm is published via MQTT.

---

## Home Assistant Integration

### configuration.yaml

In addition to the Auto Discovery entities published by the firmware, add the following sensors manually to `configuration.yaml`:

```yaml
mqtt:
  sensor:
    # Status sensor
    - name: "Toilet Relay Status"
      unique_id: "toilet_relay_status"
      state_topic: "home/toilet_relay/status"
      icon: mdi:application
      availability_topic: "home/toilet_relay/status"
      payload_available: "online"
      payload_not_available: "offline"
      qos: 0
      device:
        name: "Toilet Relay"
        identifiers:
          - "toilet_relay"
        manufacturer: "DIY"
        model: "Toilet Relay Mov Sensor"
        sw_version: "2026.00"

    # WiFi RSSI sensor
    - name: "Toilet Relay RSSI"
      unique_id: "toilet_relay_rssi"
      state_topic: "home/toilet_relay/sensor/rssi"
      availability_topic: "home/toilet_relay/status"
      payload_available: "online"
      payload_not_available: "offline"
      qos: 0
      device_class: "signal_strength"
      unit_of_measurement: "dBm"
      device:
        identifiers:
          - "toilet_relay"
        manufacturer: "DIY"
        model: "Toilet Relay Mov Sensor"
        sw_version: "2026.00"
```

### Dashboard Card (Lovelace)

Requires the following HACS plugins: **`custom:button-card`** and **`custom:bignumber-card`**.

The card includes:
- Toggle buttons for light and fan (with blink animation when ON)
- Boost button (tap = 3 min boost, double-tap = stop boost)
- PIR motion sensor indicator (fast blink when motion detected)
- RSSI indicator with colour scale (red / yellow / green)
- Reset button with confirmation dialog
- AUTO / MANUAL mode toggle

```yaml
views:
  - title: Toilet Light Control with Relays
    type: sections
    max_columns: 4
    theme: ios-dark-mode-dark-blue
    header:
      card:
        type: markdown
        text_only: true
        content: Firmware ver. Relay_Only_Mov_Release_2026.ino ✨
    sections:
      - type: grid
        cards:
          # --- Light ---
          - type: custom:button-card
            entity: switch.toilet_relay_motion_sensor_relay_control_light
            icon: mdi:lightbulb
            name: Toilet Light
            show_name: true
            show_icon: true
            color_type: icon
            color: rgb(101, 240, 55)
            tap_action:
              action: toggle
            hold_action:
              action: more-info
            state:
              - value: 'on'
                color: rgb(101, 240, 55)
                icon: mdi:lightbulb-on
                styles:
                  icon:
                    - animation: blink 3s ease infinite
              - value: 'off'
                color: rgb(255, 0, 0)
                icon: mdi:lightbulb-off
              - value: unavailable
                icon: mdi:lightbulb-alert
            grid_options:
              columns: 6
              rows: 2.4
            styles:
              card:
                - border-radius: 12px
                - box-shadow: 0px 2px 4px rgba(0,0,0,0.1)
              name:
                - font-weight: bold
                - font-size: 14px

          # --- Fan ---
          - type: custom:button-card
            entity: switch.toilet_relay_motion_sensor_relay_control_fan
            icon: mdi:fan
            name: Fan Toilet
            show_name: true
            show_icon: true
            color_type: icon
            color: rgb(101, 240, 55)
            hold_action:
              action: more-info
            state:
              - value: 'on'
                color: rgb(101, 240, 55)
                icon: mdi:fan
                styles:
                  icon:
                    - animation: blink 3s ease infinite
              - value: 'off'
                color: rgb(255, 0, 0)
                icon: mdi:fan-remove
              - value: unavailable
                icon: mdi:fan-alert
            grid_options:
              columns: 6
              rows: 2.4
            styles:
              card:
                - border-radius: 12px
                - box-shadow: 0px 2px 4px rgba(0,0,0,0.1)
              name:
                - font-weight: bold
                - font-size: 14px

          # --- Boost ---
          - type: custom:button-card
            entity: script.boost_fan_for_3_minutes
            icon: mdi:fan-plus
            name: Boost Fan for 3 minutes
            show_name: true
            show_icon: true
            color_type: icon
            color: rgb(4, 0, 255)
            tap_action:
              action: call-service
              service: script.turn_on
              service_data:
                entity_id: script.boost_fan_for_3_minutes
            hold_action:
              action: more-info
            double_tap_action:
              action: call-service
              service: script.turn_on
              service_data:
                entity_id: script.boost_fan_stop
            state:
              - value: 'off'
                color: rgb(4, 0, 255)
                icon: mdi:fan-plus
            grid_options:
              columns: 6
              rows: 2.4

          # --- Motion sensor ---
          - type: custom:button-card
            entity: binary_sensor.toilet_relay_motion_sensor_relay_control_motion
            icon: mdi:motion-sensor
            name: Move Sensor Toilet
            show_name: true
            show_icon: true
            color_type: icon
            color: rgb(101, 240, 55)
            state:
              - value: 'off'
                color: rgb(255, 0, 0)
                icon: mdi:motion-sensor-off
              - value: 'on'
                color: rgb(101, 240, 55)
                icon: mdi:motion-sensor
                styles:
                  icon:
                    - animation: blink 0.2s ease infinite
              - value: unavailable
                icon: mdi:motion-sensor-off
            grid_options:
              columns: 6
              rows: 2

          # --- RSSI ---
          - type: vertical-stack
            title: RSSI Controller
            cards:
              - type: custom:bignumber-card
                entity: sensor.toilet_relay_toilet_relay_rssi
                scale: 30px
                min: -90
                max: -35
                hideunit: false
                color: '#f0f2f2'
                bnStyle: '#c29b02'
                severity:
                  - value: -80
                    bnStyle: '#c20202'
                  - value: -70
                    bnStyle: '#f0e113'
                  - value: -35
                    bnStyle: '#0cc202'

          # --- Reset ---
          - type: custom:button-card
            entity: button.toilet_relay_motion_sensor_relay_control_reset
            icon: mdi:restart
            name: Reset Toilet Device
            show_name: true
            show_icon: true
            color_type: icon
            color: rgb(101, 240, 55)
            confirmation:
              text: Are you sure? The device will reset...
            tap_action:
              action: call-service
              service: mqtt.publish
              data:
                topic: home/toilet_relay/reset/set
                payload: Plovdiv^@0405
            hold_action:
              action: more-info
            grid_options:
              columns: 6
              rows: 2.4

          # --- Mode ---
          - type: custom:button-card
            entity: select.toilet_relay_motion_sensor_relay_control_mode
            icon: mdi:auto-mode
            name: Toilet Mode
            show_name: true
            show_icon: true
            color_type: icon
            color: rgb(101, 240, 55)
            tap_action:
              action: call-service
              service: select.select_option
              service_data:
                entity_id: select.toilet_relay_motion_sensor_relay_control_mode
                option: |
                  [[[ return entity.state === 'AUTO' ? 'MANUAL' : 'AUTO' ]]]
            hold_action:
              action: more-info
            state:
              - value: MANUAL
                color: rgb(255, 0, 0)
                icon: mdi:hand-front-left
                styles:
                  icon:
                    - animation: blink 3s ease infinite
              - value: AUTO
                icon: mdi:auto-mode
                color: rgb(101, 240, 55)
              - value: unavailable
                icon: mdi:alert-minus
            grid_options:
              columns: 6
              rows: 2.4
            styles:
              card:
                - border-radius: 12px
                - box-shadow: 0px 2px 4px rgba(0,0,0,0.1)
              name:
                - font-weight: bold
                - font-size: 14px
    cards: []
```

---

## Repository Structure

```
Relay_Only_Mov_Release_2026.ino                      ← firmware (main file)
Settings_in_Configuration_yaml_for_HA.txt            ← MQTT sensors for configuration.yaml
Toilet_Light_Control_with_Relays_Dashboard_card.txt  ← Lovelace dashboard card
```

Device configuration file (runtime, not in repo):
```
/config.json   (LittleFS — stored on the ESP8266)
```

---

## Changelog

| Version | Date | Notes |
|---|---|---|
| 2026.00 | April 2026 | Final version after testing; flashed to the controller |

---

## License

DIY project for personal use. Not intended for commercial use.
