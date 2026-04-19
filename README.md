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

## Структура на проекта

```
Relay_Only_Mov_Release_2026.ino   ← основен файл
```

Конфигурационен файл на устройството (runtime, не в repo):
```
/config.json   (LittleFS)
```

---

## Версии

| Версия | Дата | Бележки |
|---|---|---|
| 2026.00 | Април 2026 | Финална версия след тестове; качена на контролера |

---

## Лиценз

DIY проект за лично ползване. Не е предназначен за комерсиална употреба.
