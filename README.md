# Toilet Relay — Smart Bathroom Automation

> **ESP8266 NodeMCU V3** · PIR motion sensor · Dual relay (light + fan) · MQTT · Home Assistant Auto Discovery  
> Version: `2026.00` — Final release, April 2026

---

## Overview

Firmware за автоматично управление на осветление и вентилатор в баня/тоалетна, базирано на PIR сензор за движение. Устройството се интегрира с **Home Assistant** чрез MQTT с Auto Discovery и може да работи самостоятелно (Failsafe режим) при загуба на мрежова връзка.

---

## Hardware

| Компонент | Модел / Детайл |
|---|---|
| Микроконтролер | ESP8266 NodeMCU V3 |
| PIR сензор | HC-SR501 (или съвместим) |
| Реле 1 | Осветление — D6 (GPIO12) |
| Реле 2 | Вентилатор — D7 (GPIO13) |
| Reset бутон | D3 (GPIO0) |

### Окабеляване

**PIR сензор (HC-SR501)**
```
VCC → 3.3V
GND → GND
OUT → D5 (GPIO14)
```
> ⚠️ Задължително поставете джъмпера на **H (Repeat)** режим.  
> Tx (delay) → минимум (3–5 сек), Sx (чувствителност) → средна.

**Relay модул**
```
VCC → 3.3V
GND → GND
IN1 → D6 (GPIO12)   ← осветление
IN2 → D7 (GPIO13)   ← вентилатор
```

**Reset бутон**
```
единият извод → D3 (GPIO0)
другият извод → GND
```

---

## Зависимости (Arduino Libraries)

- `ESP8266WiFi`
- `DNSServer`
- `ESP8266WebServer`
- [`WiFiManager`](https://github.com/tzapu/WiFiManager)
- [`PubSubClient`](https://github.com/knolleary/pubsubclient)
- [`ArduinoJson`](https://arduinojson.org/) (v6)
- `LittleFS` (вградена в ESP8266 core)

---

## Функционалност

### Автоматичен режим (PIR)

| Стъпка | Логика |
|---|---|
| Засичане на движение | PIR трябва да е активен непрекъснато **0.6 сек** преди включване на светлината |
| Включване на вентилатора | **70 сек** след включване на светлината, **само ако е имало движение** в последните 20 сек |
| Изключване на светлината | **70 сек** без движение (Smart OFF: потвърждение 5 сек) |
| Изключване на вентилатора | **90 сек** след изгасване на светлината |

### Smart OFF филтър

За да се избегне фалшиво изключване при кратко прекъсване на PIR сигнала, е имплементиран двуетапен филтър:
1. PIR → OFF → стартира се 5-секунден таймер (`MOTION_CONFIRM_OFF_MS`)
2. Ако след 5 сек PIR е все още OFF **и** са минали 70 сек от последното движение → светлината се изключва

### Ръчен режим

Светлината може да бъде включена/изключена ръчно чрез MQTT. При ръчно включване:
- PIR автоматиката е временно деактивирана
- Ръчният режим изтича автоматично след **5 минути** (`MANUAL_CONTROL_DURATION`)
- Статусът се докладва в Home Assistant чрез `mode` entity (`AUTO` / `MANUAL`)

### Boost функция

Активира вентилатора принудително за зададен брой минути (1–10), независимо от PIR логиката.

```
MQTT topic:   home/toilet_relay/switch/fan/boost/set
Payload:      3       ← брой минути (integer)
```

След изтичане на Boost, вентилаторът се изключва, освен ако не е налице активна PIR сесия.

### Failsafe режим

При липса на MQTT връзка за **5 минути** устройството преминава в Failsafe режим:
- Работи напълно автономно с цялата PIR логика
- При възстановяване на MQTT връзката — автоматично излиза от Failsafe
- Всички relay операции се изпълняват идентично на нормалния режим

### Safety проверки

- **Watchdog**: `ESP.wdtFeed()` в главния loop
- **Периодична валидация**: на 30 сек проверка и корекция на relay статусите
- **Failure counter**: при > 3 неуспешни relay операции → logging
- **Reset rate limiting**: максимум 3 опита за reset за 1 минута; при превишаване → блокиране за 5 минути

---

## Конфигурация

### Начална настройка (WiFiManager)

При първо стартиране (или след reset) устройството стартира WiFi Access Point:

```
SSID:     toilet_relay
Password: (без парола)
```

Свържете се и отворете `http://192.168.4.1` → въведете WiFi и MQTT данни.

MQTT данните се съхраняват в **LittleFS** (`/config.json`) и се зареждат при всяко стартиране.

### Времеви константи (в кода)

```cpp
LIGHT_DELAY_MS          = 600      // 0.6 сек непрекъснато движение → светлина ON
FAN_DELAY_MS            = 70000    // 70 сек след светлина ON → вентилатор ON
MOTION_TIMEOUT_MS       = 70000    // 70 сек без движение → светлина OFF
FAN_OFF_DELAY_MS        = 90000    // 90 сек след светлина OFF → вентилатор OFF
BOOST_DURATION_MS       = 180000   // 3 мин default Boost
RECENT_MOTION_WINDOW_MS = 20000    // 20 сек прозорец за "скорошно движение"
MQTT_FAILSAFE_TIMEOUT   = 300000   // 5 мин без MQTT → Failsafe
MANUAL_CONTROL_DURATION = 300000   // 5 мин ръчен режим
MOTION_CONFIRM_OFF_MS   = 5000     // 5 сек потвърждение преди OFF
```

---

## MQTT Topics

Всички топики са с базов префикс `home/toilet_relay`.

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

При свързване устройството публикува конфигурации в `homeassistant/` топиците за:
- `binary_sensor` → Motion
- `switch` → Light, Fan
- `number` → Fan Boost (1–10 мин)
- `select` → Mode (AUTO / MANUAL)
- `button` → Reset

---

## Reset процедури

### Физически reset (хардуерен бутон)
Задръжте бутона на **D3 за 3 секунди** → изчиства WiFi настройките и `/config.json` → рестартиране в AP режим.

### MQTT reset (отдалечен)
```
Topic:    home/toilet_relay/reset
Payload:  <secret_code>
```
Изисква се тайна парола. При > 3 неуспешни опита за 1 минута, reset командите се блокират за **5 минути** и се публикува аларма в MQTT.

---

## Home Assistant интеграция

### configuration.yaml

Освен Auto Discovery ентитите (публикувани от firmware-а), добавете ръчно следните сензори в `configuration.yaml`:

```yaml
mqtt:
  sensor:
    # Статус сензор
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

    # WiFi RSSI сензор
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

### Dashboard карта (Lovelace)

Изисква инсталирани HACS плъгини: **`custom:button-card`** и **`custom:bignumber-card`**.

Картата включва:
- Toggle бутони за светлина и вентилатор (с blink анимация при ON)
- Boost бутон (tap = 3 мин boost, double-tap = спиране на boost)
- PIR motion сензор (blink при засичане)
- RSSI индикатор с цветова скала (червено / жълто / зелено)
- Reset бутон с потвърждение
- AUTO / MANUAL mode превключвател

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

## Структура на проекта

```
Relay_Only_Mov_Release_2026.ino          ← firmware (основен файл)
Settings_in_Configuration_yaml_for_HA.txt ← MQTT сензори за configuration.yaml
Toilet_Light_Control_with_Relays_Dashboard_card.txt ← Lovelace dashboard карта
```

Конфигурационен файл на устройството (runtime, не в repo):
```
/config.json   (LittleFS — съхранява се на ESP8266)
```

---

## Версии

| Версия | Дата | Бележки |
|---|---|---|
| 2026.00 | Април 2026 | Финална версия след тестове; качена на контролера |

---

## Лиценз

DIY проект за лично ползване. Не е предназначен за комерсиална употреба.

