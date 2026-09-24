#pragma once

#include <Arduino.h>

namespace Config
{
// -----------------------------------------------------------------------------
// Betriebsart
// -----------------------------------------------------------------------------
// false = echter DS18B20 und echte Ausgaenge
// true  = simulierte Temperatur; alle echten Ausgaenge bleiben sicher AUS
static constexpr bool DEMO_MODE = false;

// -----------------------------------------------------------------------------
// Display: 1,53 Zoll ST77916, 360 x 360, QSPI
// -----------------------------------------------------------------------------
static constexpr int PIN_TFT_CS = 10;
static constexpr int PIN_TFT_SCK = 9;
static constexpr int PIN_TFT_D0 = 11; // Display SDA / IO0
static constexpr int PIN_TFT_D1 = 12;
static constexpr int PIN_TFT_D2 = 13;
static constexpr int PIN_TFT_D3 = 14;
static constexpr int PIN_TFT_RST = 47;
static constexpr int PIN_TFT_BL = 15;
static constexpr bool TFT_BACKLIGHT_ACTIVE_HIGH = true;
static constexpr int32_t TFT_BUS_FREQUENCY = 20000000;

// -----------------------------------------------------------------------------
// Bedienung und Sensor
// -----------------------------------------------------------------------------
static constexpr int PIN_ENCODER_CLK = 4;
static constexpr int PIN_ENCODER_DT = 5;
static constexpr int PIN_ENCODER_SW = 6;
static constexpr int PIN_DS18B20 = 7;

// -----------------------------------------------------------------------------
// DRV8871 fuer das Ruehrwerk
// -----------------------------------------------------------------------------
// Feste Vorwaertsrichtung: IN1 ist das Richtungssignal, IN2 erhaelt PWM.
// Die PWM wechselt nach TI-Empfehlung zwischen Vorwaertslauf und Bremsen.
static constexpr int PIN_STIRRER_DRV_IN1 = 16;
static constexpr int PIN_STIRRER_DRV_IN2 = 8;
static constexpr uint32_t STIRRER_PWM_FREQUENCY_HZ = 20000;
static constexpr uint8_t STIRRER_PWM_RESOLUTION_BITS = 8;
static constexpr uint8_t STIRRER_IDLE_PERCENT = 50;
static constexpr uint8_t STIRRER_COOLING_PERCENT = 100;
static constexpr uint32_t STIRRER_START_BOOST_MS = 800;

// -----------------------------------------------------------------------------
// Relaisausgaenge fuer Kompressor und Verfluessigerluefter
// -----------------------------------------------------------------------------
static constexpr int PIN_RELAY_COMPRESSOR = 17;
static constexpr int PIN_RELAY_FAN = 18;

// Die angeschlossenen Module des aktuellen Aufbaus schalten bei HIGH ein.
// Darum stehen beide Werte auf false (ACTIVE_HIGH).
//
// Modul schaltet bei HIGH ein: ACTIVE_LOW = false
// Modul schaltet bei LOW  ein: ACTIVE_LOW = true
//
// Jeder Relaisausgang ist einzeln konfigurierbar, falls Module gemischt werden.
static constexpr bool COMPRESSOR_RELAY_ACTIVE_LOW = false;
static constexpr bool FAN_RELAY_ACTIVE_LOW = false;

// -----------------------------------------------------------------------------
// Zeitverhalten
// -----------------------------------------------------------------------------
static constexpr uint32_t SENSOR_SAMPLE_INTERVAL_MS = 1000;
static constexpr uint32_t SENSOR_CONVERSION_MS = 750;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 750;
static constexpr uint32_t SETTINGS_SAVE_DELAY_MS = 1200;

// Im Demo-Modus laufen die Schutzzeiten zehnmal schneller.
static constexpr uint32_t DEMO_TIME_DIVISOR = 10;
} // namespace Config
