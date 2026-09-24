#include <Arduino.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include <driver/gpio.h>

#include "config.h"

namespace
{
// -----------------------------------------------------------------------------
// Farben und Display
// -----------------------------------------------------------------------------
constexpr uint16_t COLOR_BLACK = RGB565_BLACK;
constexpr uint16_t COLOR_WHITE = RGB565_WHITE;
constexpr uint16_t COLOR_TEXT_DIM = 0x8410;
constexpr uint16_t COLOR_PANEL = 0x1082;
constexpr uint16_t COLOR_PANEL_SELECTED = 0x2945;
constexpr uint16_t COLOR_GREEN = 0x05E8;
constexpr uint16_t COLOR_GREEN_DARK = 0x02A3;
constexpr uint16_t COLOR_ORANGE = 0xFD20;
constexpr uint16_t COLOR_RED = 0xF925;
constexpr uint16_t COLOR_RED_DARK = 0x6000;
constexpr uint16_t COLOR_BLUE_DARK = 0x09B1;

Arduino_DataBus *displayBus = new Arduino_ESP32QSPI(
    Config::PIN_TFT_CS,
    Config::PIN_TFT_SCK,
    Config::PIN_TFT_D0,
    Config::PIN_TFT_D1,
    Config::PIN_TFT_D2,
    Config::PIN_TFT_D3);

Arduino_GFX *displayPanel = new Arduino_ST77916(
    displayBus,
    Config::PIN_TFT_RST,
    0,
    true,
    360,
    360,
    0,
    0,
    0,
    0,
    st77916_150_init_operations,
    sizeof(st77916_150_init_operations));

Arduino_Canvas *displayCanvas = new Arduino_Canvas(360, 360, displayPanel);
Arduino_GFX *ui = displayCanvas;
bool displayAvailable = false;
bool canvasAvailable = false;
bool uiDirty = true;
uint32_t lastUiDrawAt = 0;
uint16_t *ringRadiusLookupQ8 = nullptr;

// -----------------------------------------------------------------------------
// Einstellungen
// -----------------------------------------------------------------------------
struct Settings
{
  float setTemperature = -4.0f;
  float hysteresis = 2.0f;
  uint16_t startDelaySeconds = 10;
  uint16_t minimumOffSeconds = 300;
  uint16_t minimumOnSeconds = 60;
  uint8_t sensorFaultDelaySeconds = 10;
  float yellowDelta = 5.0f;
  float redDelta = 10.0f;
  uint8_t brightness = 220;
  bool autostart = false;
};

constexpr uint16_t SETTINGS_VERSION = 5;
Settings settings;
Settings editBackup;
Preferences preferences;
bool deferredSettingsSave = false;
uint32_t lastSettingsChangeAt = 0;

float clampFloat(float value, float minimum, float maximum)
{
  if (value < minimum)
  {
    return minimum;
  }
  if (value > maximum)
  {
    return maximum;
  }
  return value;
}

void validateSettings()
{
  settings.setTemperature = clampFloat(settings.setTemperature, -20.0f, 15.0f);
  settings.hysteresis = clampFloat(settings.hysteresis, 0.5f, 10.0f);
  settings.startDelaySeconds = constrain(settings.startDelaySeconds, 0, 600);
  settings.minimumOffSeconds = constrain(settings.minimumOffSeconds, 0, 900);
  settings.minimumOnSeconds = constrain(settings.minimumOnSeconds, 0, 600);
  settings.sensorFaultDelaySeconds = constrain(settings.sensorFaultDelaySeconds, 0, 60);
  settings.yellowDelta = clampFloat(settings.yellowDelta, 0.5f, 30.0f);
  settings.redDelta = clampFloat(settings.redDelta, settings.yellowDelta + 0.5f, 40.0f);
  settings.brightness = constrain(settings.brightness, 10, 255);
}

void saveSettings()
{
  validateSettings();
  preferences.putUShort("version", SETTINGS_VERSION);
  preferences.putFloat("setTemp", settings.setTemperature);
  preferences.putFloat("hyst", settings.hysteresis);
  preferences.putUShort("startDelay", settings.startDelaySeconds);
  preferences.putUShort("minOff", settings.minimumOffSeconds);
  preferences.putUShort("minOn", settings.minimumOnSeconds);
  preferences.putUChar("sensorDelay", settings.sensorFaultDelaySeconds);
  preferences.putFloat("yellowD", settings.yellowDelta);
  preferences.putFloat("redD", settings.redDelta);
  preferences.putUChar("bright", settings.brightness);
  preferences.putBool("autostart", settings.autostart);
  deferredSettingsSave = false;
  Serial.println(F("[SETTINGS] gespeichert"));
}

void loadSettings()
{
  preferences.begin("biercool", false);
  const uint16_t storedVersion = preferences.getUShort("version", 0);
  // Bekannte Einzelwerte werden auch aus einer aelteren Fassung uebernommen.
  // Dadurch loescht ein reines Firmware-Update keine bereits gesetzten Werte.
  settings.setTemperature = preferences.getFloat("setTemp", settings.setTemperature);
  settings.hysteresis = preferences.getFloat("hyst", settings.hysteresis);
  settings.startDelaySeconds = preferences.getUShort("startDelay", settings.startDelaySeconds);
  settings.minimumOffSeconds = preferences.getUShort("minOff", settings.minimumOffSeconds);
  settings.minimumOnSeconds = preferences.getUShort("minOn", settings.minimumOnSeconds);
  settings.sensorFaultDelaySeconds = preferences.getUChar("sensorDelay", settings.sensorFaultDelaySeconds);
  settings.yellowDelta = preferences.getFloat("yellowD", settings.yellowDelta);
  settings.redDelta = preferences.getFloat("redD", settings.redDelta);
  settings.brightness = preferences.getUChar("bright", settings.brightness);
  settings.autostart = preferences.getBool("autostart", settings.autostart);
  validateSettings();
  if (storedVersion != SETTINGS_VERSION)
  {
    saveSettings();
  }
}

void scheduleSettingsSave(uint32_t now)
{
  deferredSettingsSave = true;
  lastSettingsChangeAt = now;
}

void handleDeferredSettingsSave(uint32_t now)
{
  if (deferredSettingsSave && (now - lastSettingsChangeAt >= Config::SETTINGS_SAVE_DELAY_MS))
  {
    saveSettings();
  }
}

// -----------------------------------------------------------------------------
// Relais - logischer Zustand und elektrischer Pegel sind bewusst getrennt.
// -----------------------------------------------------------------------------
constexpr uint8_t relayLevel(bool logicalOn, bool activeLow)
{
  return (logicalOn != activeLow) ? HIGH : LOW;
}

static_assert(relayLevel(true, false) == HIGH, "ACTIVE_HIGH: EIN muss HIGH sein");
static_assert(relayLevel(false, false) == LOW, "ACTIVE_HIGH: AUS muss LOW sein");
static_assert(relayLevel(true, true) == LOW, "ACTIVE_LOW: EIN muss LOW sein");
static_assert(relayLevel(false, true) == HIGH, "ACTIVE_LOW: AUS muss HIGH sein");

void prepareRelayPin(int pin, bool activeLow)
{
  const uint8_t safeOffLevel = relayLevel(false, activeLow);
  digitalWrite(pin, safeOffLevel);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, safeOffLevel);
}

void writeRelay(int pin, bool logicalOn, bool activeLow)
{
  digitalWrite(pin, relayLevel(logicalOn, activeLow));
}

bool stirrerPwmAvailable = false;
uint8_t stirrerRequestedPercent = 0;
uint8_t stirrerAppliedPercent = 0;
bool stirrerStartBoostActive = false;
uint32_t stirrerStartBoostAt = 0;

void prepareStirrerPins()
{
  // Beide Eingaenge LOW bedeutet beim DRV8871: Ausgaenge hochohmig und Sleep.
  digitalWrite(Config::PIN_STIRRER_DRV_IN1, LOW);
  pinMode(Config::PIN_STIRRER_DRV_IN1, OUTPUT);
  digitalWrite(Config::PIN_STIRRER_DRV_IN1, LOW);

  digitalWrite(Config::PIN_STIRRER_DRV_IN2, LOW);
  pinMode(Config::PIN_STIRRER_DRV_IN2, OUTPUT);
  digitalWrite(Config::PIN_STIRRER_DRV_IN2, LOW);
}

void initializeStirrerDriver()
{
  stirrerPwmAvailable = ledcAttach(
      Config::PIN_STIRRER_DRV_IN2,
      Config::STIRRER_PWM_FREQUENCY_HZ,
      Config::STIRRER_PWM_RESOLUTION_BITS);
  if (!stirrerPwmAvailable)
  {
    Serial.println(F("[RUEHRWERK] DRV8871-PWM konnte nicht gestartet werden; Motor bleibt AUS"));
    return;
  }

  ledcWrite(Config::PIN_STIRRER_DRV_IN2, 0);
  Serial.printf("[RUEHRWERK] DRV8871 aktiv, PWM=%lu Hz, Idle=%u%%, Kuehlen=%u%%\n",
                static_cast<unsigned long>(Config::STIRRER_PWM_FREQUENCY_HZ),
                Config::STIRRER_IDLE_PERCENT,
                Config::STIRRER_COOLING_PERCENT);
}

void writeStirrerPercent(uint8_t percent)
{
  percent = constrain(percent, 0, 100);
  if (!stirrerPwmAvailable)
  {
    stirrerAppliedPercent = 0;
    return;
  }

  if (percent == 0)
  {
    // Erst PWM LOW, dann IN1 LOW: kein kurzer Gegenimpuls beim Abschalten.
    ledcWrite(Config::PIN_STIRRER_DRV_IN2, 0);
    digitalWrite(Config::PIN_STIRRER_DRV_IN1, LOW);
  }
  else
  {
    // IN1 bleibt HIGH. IN2=LOW treibt vorwaerts, IN2=HIGH bremst.
    // Deshalb ist der PWM-Wert auf IN2 der Bremsanteil (invers zur Drehzahl).
    digitalWrite(Config::PIN_STIRRER_DRV_IN1, HIGH);
    const uint32_t brakeDuty =
        (static_cast<uint32_t>(100U - percent) * 255U + 50U) / 100U;
    ledcWrite(Config::PIN_STIRRER_DRV_IN2, brakeDuty);
  }
  stirrerAppliedPercent = percent;
}

void updateStirrerMotor(uint8_t requestedPercent, uint32_t now)
{
  requestedPercent = constrain(requestedPercent, 0, 100);
  if (requestedPercent != stirrerRequestedPercent)
  {
    if (stirrerRequestedPercent == 0 && requestedPercent > 0)
    {
      // Ein stehendes Ruehrwerk laeuft unter Last bei 50 % nicht immer sicher an.
      stirrerStartBoostActive = true;
      stirrerStartBoostAt = now;
    }
    else if (requestedPercent == 0)
    {
      stirrerStartBoostActive = false;
    }
    stirrerRequestedPercent = requestedPercent;
  }

  if (stirrerStartBoostActive &&
      now - stirrerStartBoostAt >= Config::STIRRER_START_BOOST_MS)
  {
    stirrerStartBoostActive = false;
  }

  const uint8_t outputPercent =
      stirrerStartBoostActive ? Config::STIRRER_COOLING_PERCENT : stirrerRequestedPercent;
  if (outputPercent != stirrerAppliedPercent)
  {
    writeStirrerPercent(outputPercent);
  }
}

enum class SystemState : uint8_t
{
  Off,
  Waiting,
  Cooling,
  Ready,
  Error
};

bool coolingEnabled = false;
bool compressorOn = false;
SystemState systemState = SystemState::Off;
uint32_t coolingEnabledAt = 0;
uint32_t compressorTurnedOnAt = 0;
uint32_t compressorTurnedOffAt = 0;

const char *stateText(SystemState state)
{
  switch (state)
  {
  case SystemState::Cooling:
    return "KÜHLT";
  case SystemState::Ready:
    return "BEREIT";
  case SystemState::Waiting:
    return "WARTET";
  case SystemState::Error:
    return "FEHLER";
  case SystemState::Off:
  default:
    return "OFF";
  }
}

uint32_t effectiveMilliseconds(uint16_t seconds)
{
  uint32_t value = static_cast<uint32_t>(seconds) * 1000UL;
  if (Config::DEMO_MODE)
  {
    value /= Config::DEMO_TIME_DIVISOR;
  }
  return value;
}

void setCompressor(bool on, uint32_t now, const __FlashStringHelper *reason)
{
  if (compressorOn == on)
  {
    return;
  }

  compressorOn = on;
  if (on)
  {
    compressorTurnedOnAt = now;
  }
  else
  {
    compressorTurnedOffAt = now;
  }
  uiDirty = true;
  Serial.printf("[CONTROL] Kompressor %s - ", on ? "EIN" : "AUS");
  Serial.println(reason);
}

void applyOutputs(uint32_t now)
{
  const uint8_t logicalStirrerPercent = coolingEnabled
                                             ? (compressorOn
                                                    ? Config::STIRRER_COOLING_PERCENT
                                                    : Config::STIRRER_IDLE_PERCENT)
                                             : 0;
  const bool logicalFan = coolingEnabled;
  const bool logicalCompressor = compressorOn;

  // Demo zeigt die Regelung in der UI, aktiviert aber nie reale Lasten.
  const uint8_t physicalStirrerPercent =
      Config::DEMO_MODE ? 0 : logicalStirrerPercent;
  const bool physicalFan = Config::DEMO_MODE ? false : logicalFan;
  const bool physicalCompressor = Config::DEMO_MODE ? false : logicalCompressor;

  updateStirrerMotor(physicalStirrerPercent, now);
  writeRelay(Config::PIN_RELAY_FAN, physicalFan, Config::FAN_RELAY_ACTIVE_LOW);
  writeRelay(Config::PIN_RELAY_COMPRESSOR, physicalCompressor, Config::COMPRESSOR_RELAY_ACTIVE_LOW);

  static bool first = true;
  static uint8_t previousStirrerPercent = 0;
  static uint8_t previousStirrerOutputPercent = 0;
  static bool previousFan = false;
  static bool previousCompressor = false;
  if (first || logicalStirrerPercent != previousStirrerPercent ||
      stirrerAppliedPercent != previousStirrerOutputPercent || logicalFan != previousFan ||
      logicalCompressor != previousCompressor)
  {
    first = false;
    previousStirrerPercent = logicalStirrerPercent;
    previousStirrerOutputPercent = stirrerAppliedPercent;
    previousFan = logicalFan;
    previousCompressor = logicalCompressor;
    Serial.printf(
        "[OUTPUTS] UI=%s | Ruehrwerk Soll=%u%% Ausgang=%u%% | Luefter=%s Kompressor=%s | GPIO L=%s K=%s%s\n",
        stateText(systemState),
        logicalStirrerPercent,
        stirrerAppliedPercent,
        logicalFan ? "EIN" : "AUS",
        logicalCompressor ? "EIN" : "AUS",
        relayLevel(physicalFan, Config::FAN_RELAY_ACTIVE_LOW) == HIGH ? "HIGH" : "LOW",
        relayLevel(physicalCompressor, Config::COMPRESSOR_RELAY_ACTIVE_LOW) == HIGH ? "HIGH" : "LOW",
        Config::DEMO_MODE ? " | DEMO: physisch AUS" : "");
  }
}

// -----------------------------------------------------------------------------
// DS18B20 und Sensorfehlerverzoegerung
// -----------------------------------------------------------------------------
OneWire oneWire(Config::PIN_DS18B20);
DallasTemperature temperatureSensors(&oneWire);
float currentTemperature = 0.0f;
bool haveValidTemperature = false;
bool latestSensorSampleValid = false;
bool sensorHardError = false;
bool sensorInvalidPeriodActive = false;
uint32_t sensorInvalidSince = 0;
bool conversionPending = false;
uint32_t conversionStartedAt = 0;
uint32_t lastSensorCycleAt = 0;

bool isValidSensorTemperature(float value)
{
  return isfinite(value) && value > -55.0f && value <= 125.0f &&
         value != DEVICE_DISCONNECTED_C && fabsf(value - 85.0f) > 0.01f;
}

void registerValidTemperature(float value)
{
  currentTemperature = value;
  haveValidTemperature = true;
  latestSensorSampleValid = true;
  sensorInvalidPeriodActive = false;
  if (sensorHardError)
  {
    Serial.println(F("[SENSOR] Messung wieder gueltig - Fehler aufgehoben"));
  }
  sensorHardError = false;
  uiDirty = true;
}

void registerInvalidTemperature(uint32_t now)
{
  latestSensorSampleValid = false;
  if (!sensorInvalidPeriodActive)
  {
    sensorInvalidPeriodActive = true;
    sensorInvalidSince = now;
    Serial.println(F("[SENSOR] ungueltiger Messwert - Entprellzeit gestartet"));
  }

  const uint32_t faultDelay = static_cast<uint32_t>(settings.sensorFaultDelaySeconds) * 1000UL;
  if (!sensorHardError && (now - sensorInvalidSince >= faultDelay))
  {
    sensorHardError = true;
    Serial.println(F("[SENSOR] Fehlerverzoegerung abgelaufen - Kompressor gesperrt"));
    uiDirty = true;
  }
}

void updateRealTemperature(uint32_t now)
{
  if (conversionPending)
  {
    if (now - conversionStartedAt >= Config::SENSOR_CONVERSION_MS)
    {
      const float value = temperatureSensors.getTempCByIndex(0);
      conversionPending = false;
      lastSensorCycleAt = now;
      if (isValidSensorTemperature(value))
      {
        registerValidTemperature(value);
      }
      else
      {
        registerInvalidTemperature(now);
      }
    }
    return;
  }

  if (now - lastSensorCycleAt >= Config::SENSOR_SAMPLE_INTERVAL_MS)
  {
    temperatureSensors.requestTemperatures();
    conversionStartedAt = now;
    conversionPending = true;
  }
}

void updateDemoTemperature(uint32_t now)
{
  static uint32_t previousAt = now;
  const float elapsedSeconds = static_cast<float>(now - previousAt) / 1000.0f;
  previousAt = now;

  if (!haveValidTemperature)
  {
    currentTemperature = 12.0f;
    haveValidTemperature = true;
    latestSensorSampleValid = true;
  }

  if (compressorOn)
  {
    currentTemperature -= (6.0f / 60.0f) * elapsedSeconds;
    currentTemperature = max(currentTemperature, -12.0f);
  }
  else
  {
    currentTemperature += (2.0f / 60.0f) * elapsedSeconds;
    currentTemperature = min(currentTemperature, 18.0f);
  }
}

void updateTemperature(uint32_t now)
{
  if (Config::DEMO_MODE)
  {
    updateDemoTemperature(now);
  }
  else
  {
    updateRealTemperature(now);
  }
}

// -----------------------------------------------------------------------------
// Regelung
// -----------------------------------------------------------------------------
void updateControl(uint32_t now)
{
  const SystemState oldState = systemState;

  if (!coolingEnabled)
  {
    setCompressor(false, now, F("Kuehlmodus aus"));
    systemState = SystemState::Off;
  }
  else if (sensorHardError)
  {
    setCompressor(false, now, F("Sensorfehler"));
    systemState = SystemState::Error;
  }
  else if (!haveValidTemperature)
  {
    setCompressor(false, now, F("noch kein gueltiger Messwert"));
    systemState = SystemState::Waiting;
  }
  else if (compressorOn)
  {
    const bool minimumOnElapsed =
        now - compressorTurnedOnAt >= effectiveMilliseconds(settings.minimumOnSeconds);
    if (currentTemperature <= settings.setTemperature && minimumOnElapsed)
    {
      setCompressor(false, now, F("Solltemperatur erreicht"));
      systemState = SystemState::Ready;
    }
    else
    {
      systemState = SystemState::Cooling;
    }
  }
  else
  {
    const bool needsCooling = currentTemperature >= settings.setTemperature + settings.hysteresis;
    if (!needsCooling)
    {
      systemState = SystemState::Ready;
    }
    else if (!latestSensorSampleValid)
    {
      // Ein laufender Kompressor darf die kurze Entprellzeit ueberbruecken.
      // Neu gestartet wird mit einem veralteten Messwert jedoch nicht.
      systemState = SystemState::Waiting;
    }
    else
    {
      const bool startDelayElapsed =
          now - coolingEnabledAt >= effectiveMilliseconds(settings.startDelaySeconds);
      const bool minimumOffElapsed =
          now - compressorTurnedOffAt >= effectiveMilliseconds(settings.minimumOffSeconds);
      if (startDelayElapsed && minimumOffElapsed)
      {
        setCompressor(true, now, F("Temperaturanforderung und Schutzzeiten frei"));
        systemState = SystemState::Cooling;
      }
      else
      {
        systemState = SystemState::Waiting;
      }
    }
  }

  if (oldState != systemState)
  {
    uiDirty = true;
    Serial.printf("[STATE] %s\n", stateText(systemState));
  }
}

void setCoolingEnabled(bool enabled, uint32_t now)
{
  if (coolingEnabled == enabled)
  {
    return;
  }

  coolingEnabled = enabled;
  if (enabled)
  {
    coolingEnabledAt = now;
    Serial.println(F("[CONTROL] Kuehlmodus EIN"));
  }
  else
  {
    setCompressor(false, now, F("manuell ausgeschaltet"));
    Serial.println(F("[CONTROL] Kuehlmodus AUS"));
  }
  uiDirty = true;
}

// -----------------------------------------------------------------------------
// Encoder und Taste
// -----------------------------------------------------------------------------
enum class ButtonEvent : uint8_t
{
  None,
  ShortPress,
  LongPress
};

portMUX_TYPE inputMux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR static const int8_t encoderTransitionTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0};
volatile uint8_t encoderPreviousState = 0;
volatile int16_t encoderQuarterSteps = 0;

volatile bool buttonInterruptPressed = false;
volatile bool buttonReleasePending = false;
volatile uint32_t buttonPressedAtUs = 0;
volatile uint32_t buttonLastAcceptedEdgeAtUs = 0;
volatile uint32_t buttonReleasedAfterUs = 0;

void ARDUINO_ISR_ATTR onEncoderEdge()
{
  const uint8_t currentState =
      (static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(Config::PIN_ENCODER_CLK))) << 1) |
      static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(Config::PIN_ENCODER_DT)));

  portENTER_CRITICAL_ISR(&inputMux);
  if (currentState != encoderPreviousState)
  {
    const int8_t movement =
        encoderTransitionTable[(encoderPreviousState << 2) | currentState];
    const int16_t next = encoderQuarterSteps + movement;
    encoderQuarterSteps = constrain(next, -120, 120);
    encoderPreviousState = currentState;
  }
  portEXIT_CRITICAL_ISR(&inputMux);
}

void ARDUINO_ISR_ATTR onButtonEdge()
{
  const uint32_t nowUs = micros();
  const bool pressed =
      gpio_get_level(static_cast<gpio_num_t>(Config::PIN_ENCODER_SW)) == 0;
  constexpr uint32_t debounceUs = Config::BUTTON_DEBOUNCE_MS * 1000UL;

  portENTER_CRITICAL_ISR(&inputMux);
  if (pressed != buttonInterruptPressed &&
      nowUs - buttonLastAcceptedEdgeAtUs >= debounceUs)
  {
    buttonLastAcceptedEdgeAtUs = nowUs;
    buttonInterruptPressed = pressed;
    if (pressed)
    {
      buttonPressedAtUs = nowUs;
    }
    else
    {
      buttonReleasedAfterUs = nowUs - buttonPressedAtUs;
      buttonReleasePending = true;
    }
  }
  portEXIT_CRITICAL_ISR(&inputMux);
}

int8_t readEncoderStep()
{
  int8_t steps = 0;
  portENTER_CRITICAL(&inputMux);
  if (encoderQuarterSteps >= 4 || encoderQuarterSteps <= -4)
  {
    int16_t completeSteps = encoderQuarterSteps / 4;
    completeSteps = constrain(completeSteps, -8, 8);
    encoderQuarterSteps -= completeSteps * 4;
    steps = static_cast<int8_t>(completeSteps);
  }
  portEXIT_CRITICAL(&inputMux);
  return steps;
}

ButtonEvent readButtonEvent(uint32_t now)
{
  (void)now;
  bool released = false;
  uint32_t heldForUs = 0;

  portENTER_CRITICAL(&inputMux);
  if (buttonReleasePending)
  {
    released = true;
    heldForUs = buttonReleasedAfterUs;
    buttonReleasePending = false;
  }
  portEXIT_CRITICAL(&inputMux);

  if (!released)
  {
    return ButtonEvent::None;
  }
  return heldForUs >= Config::BUTTON_LONG_PRESS_MS * 1000UL
             ? ButtonEvent::LongPress
             : ButtonEvent::ShortPress;
}

// -----------------------------------------------------------------------------
// Menuebedienung
// -----------------------------------------------------------------------------
enum class Screen : uint8_t
{
  Main,
  Menu
};

enum MenuItem : uint8_t
{
  MenuSetTemperature,
  MenuHysteresis,
  MenuStartDelay,
  MenuMinimumOff,
  MenuMinimumOn,
  MenuSensorFault,
  MenuYellowDelta,
  MenuRedDelta,
  MenuBrightness,
  MenuAutostart,
  MenuFactoryReset,
  MenuBack,
  MenuItemCount
};

Screen currentScreen = Screen::Main;
uint8_t selectedMenuItem = 0;
bool editingMenuItem = false;

bool isEditableMenuItem(uint8_t item)
{
  return item <= MenuAutostart;
}

void applyBacklight()
{
  const uint8_t duty = Config::TFT_BACKLIGHT_ACTIVE_HIGH
                           ? settings.brightness
                           : static_cast<uint8_t>(255 - settings.brightness);
  ledcWrite(Config::PIN_TFT_BL, duty);
}

void adjustMenuValue(uint8_t item, int8_t direction)
{
  switch (item)
  {
  case MenuSetTemperature:
    settings.setTemperature = clampFloat(settings.setTemperature + direction * 0.5f, -20.0f, 15.0f);
    break;
  case MenuHysteresis:
    settings.hysteresis = clampFloat(settings.hysteresis + direction * 0.5f, 0.5f, 10.0f);
    break;
  case MenuStartDelay:
    settings.startDelaySeconds = constrain(static_cast<int>(settings.startDelaySeconds) + direction, 0, 600);
    break;
  case MenuMinimumOff:
    settings.minimumOffSeconds = constrain(static_cast<int>(settings.minimumOffSeconds) + direction * 30, 0, 900);
    break;
  case MenuMinimumOn:
    settings.minimumOnSeconds = constrain(static_cast<int>(settings.minimumOnSeconds) + direction * 10, 0, 600);
    break;
  case MenuSensorFault:
    settings.sensorFaultDelaySeconds = constrain(static_cast<int>(settings.sensorFaultDelaySeconds) + direction, 0, 60);
    break;
  case MenuYellowDelta:
    settings.yellowDelta = clampFloat(settings.yellowDelta + direction * 0.5f, 0.5f, settings.redDelta - 0.5f);
    break;
  case MenuRedDelta:
    settings.redDelta = clampFloat(settings.redDelta + direction * 0.5f, settings.yellowDelta + 0.5f, 40.0f);
    break;
  case MenuBrightness:
    settings.brightness = constrain(static_cast<int>(settings.brightness) + direction * 10, 10, 255);
    applyBacklight();
    break;
  case MenuAutostart:
    settings.autostart = !settings.autostart;
    break;
  default:
    break;
  }
  uiDirty = true;
}

void handleMainInput(int8_t encoderStep, ButtonEvent button, uint32_t now)
{
  if (encoderStep != 0)
  {
    settings.setTemperature = clampFloat(settings.setTemperature + encoderStep * 0.5f, -20.0f, 15.0f);
    scheduleSettingsSave(now);
    uiDirty = true;
  }

  if (button == ButtonEvent::ShortPress)
  {
    setCoolingEnabled(!coolingEnabled, now);
  }
  else if (button == ButtonEvent::LongPress)
  {
    currentScreen = Screen::Menu;
    editingMenuItem = false;
    uiDirty = true;
  }
}

void handleMenuInput(int8_t encoderStep, ButtonEvent button)
{
  if (editingMenuItem)
  {
    if (encoderStep != 0)
    {
      adjustMenuValue(selectedMenuItem, encoderStep);
    }
    if (button == ButtonEvent::ShortPress)
    {
      editingMenuItem = false;
      saveSettings();
      uiDirty = true;
    }
    else if (button == ButtonEvent::LongPress)
    {
      settings = editBackup;
      editingMenuItem = false;
      applyBacklight();
      uiDirty = true;
    }
    return;
  }

  if (encoderStep != 0)
  {
    int next = static_cast<int>(selectedMenuItem) + encoderStep;
    next %= MenuItemCount;
    if (next < 0)
    {
      next += MenuItemCount;
    }
    selectedMenuItem = static_cast<uint8_t>(next);
    uiDirty = true;
  }

  if (button == ButtonEvent::LongPress)
  {
    currentScreen = Screen::Main;
    uiDirty = true;
  }
  else if (button == ButtonEvent::ShortPress)
  {
    if (isEditableMenuItem(selectedMenuItem))
    {
      editBackup = settings;
      editingMenuItem = true;
    }
    else if (selectedMenuItem == MenuFactoryReset)
    {
      settings = Settings{};
      saveSettings();
      applyBacklight();
      uiDirty = true;
    }
    else if (selectedMenuItem == MenuBack)
    {
      currentScreen = Screen::Main;
      uiDirty = true;
    }
  }
}

// -----------------------------------------------------------------------------
// UI-Zeichnung
// -----------------------------------------------------------------------------
uint16_t scaleColor(uint16_t color, uint8_t scale)
{
  const uint8_t red = ((color >> 11) & 0x1F) * scale / 255;
  const uint8_t green = ((color >> 5) & 0x3F) * scale / 255;
  const uint8_t blue = (color & 0x1F) * scale / 255;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

void initializeRingRadiusLookup()
{
  if (!canvasAvailable)
  {
    return;
  }

  constexpr size_t lookupEntries = 65536;
  ringRadiusLookupQ8 = static_cast<uint16_t *>(
      ps_malloc(lookupEntries * sizeof(uint16_t)));
  if (ringRadiusLookupQ8 == nullptr)
  {
    Serial.println(F("[DISPLAY] Radius-Lookup nicht verfuegbar; Ring nutzt direkte Berechnung"));
    return;
  }

  // Einmal beim Start berechnen. Beim Pulsieren entfallen danach zehntausende
  // sqrtf()-Aufrufe pro Bild, ohne dass wieder Luecken im Ring entstehen.
  for (uint32_t squaredRadius = 0; squaredRadius < lookupEntries; ++squaredRadius)
  {
    ringRadiusLookupQ8[squaredRadius] = static_cast<uint16_t>(
        lroundf(sqrtf(static_cast<float>(squaredRadius)) * 256.0f));
  }
  Serial.println(F("[DISPLAY] schneller Radius-Lookup fuer Farbring aktiv"));
}

uint16_t ringColor()
{
  if (sensorHardError)
  {
    return COLOR_RED;
  }
  if (!haveValidTemperature)
  {
    return COLOR_ORANGE;
  }
  const float delta = currentTemperature - settings.setTemperature;
  if (delta >= settings.redDelta)
  {
    return COLOR_RED;
  }
  if (delta >= settings.yellowDelta)
  {
    return COLOR_ORANGE;
  }
  return COLOR_GREEN;
}

void drawGradientRing(uint32_t now)
{
  const uint16_t color = ringColor();
  float pulseExtra = 0.0f;
  if (compressorOn)
  {
    const float phase = static_cast<float>(now % 2200UL) / 2200.0f * TWO_PI;
    pulseExtra = 2.0f + (sinf(phase) + 1.0f) * 6.0f;
  }

  const float ringWidth = 12.0f + pulseExtra;
  constexpr float gradientOuterRadius = 175.0f;
  const float innerRadius = gradientOuterRadius - ringWidth;

  if (canvasAvailable)
  {
    constexpr int32_t outerRadiusQ8 = 177 * 256;
    constexpr int32_t gradientOuterRadiusQ8 = 175 * 256;
    const int32_t innerRadiusQ8 = static_cast<int32_t>(lroundf(innerRadius * 256.0f));
    const int32_t innerLimitQ8 = innerRadiusQ8 - 128;
    constexpr int32_t outerLimitQ8 = outerRadiusQ8 + 128;
    const int32_t safeInnerLimitQ8 = innerLimitQ8 > 0 ? innerLimitQ8 : 0;
    const uint32_t innerLimitSquared = static_cast<uint32_t>(
        safeInnerLimitQ8 * safeInnerLimitQ8) >> 16;
    constexpr uint32_t outerLimitSquared =
        static_cast<uint32_t>(outerLimitQ8 * outerLimitQ8) >> 16;
    const int32_t gradientSpanQ8 = gradientOuterRadiusQ8 - innerRadiusQ8;

    uint16_t scaledColor[256];
    for (uint16_t brightness = 0; brightness < 256; ++brightness)
    {
      scaledColor[brightness] = scaleColor(color, static_cast<uint8_t>(brightness));
    }

    // Jeder Pixel der Ringflaeche wird genau einmal gesetzt. Radius-Lookup und
    // Farbtabelle halten diese lueckenlose Darstellung schnell genug fuer 10 fps.
    uint16_t *framebuffer = displayCanvas->getFramebuffer();
    for (int y = 2; y < 358; ++y)
    {
      const int dy = y - 180;
      for (int x = 2; x < 358; ++x)
      {
        const int dx = x - 180;
        const uint32_t radiusSquared = static_cast<uint32_t>(dx * dx + dy * dy);
        if (radiusSquared > outerLimitSquared || radiusSquared < innerLimitSquared)
        {
          continue;
        }

        const int32_t radiusQ8 = ringRadiusLookupQ8 != nullptr
                                     ? ringRadiusLookupQ8[radiusSquared]
                                     : static_cast<int32_t>(
                                           lroundf(sqrtf(static_cast<float>(radiusSquared)) * 256.0f));
        const int32_t gradientPositionQ8 = constrain(
            ((radiusQ8 - innerRadiusQ8) * 256 + gradientSpanQ8 / 2) /
                gradientSpanQ8,
            0,
            256);
        const int32_t innerCoverageQ8 =
            constrain(radiusQ8 - innerRadiusQ8 + 128, 0, 256);
        const int32_t outerCoverageQ8 =
            constrain(outerRadiusQ8 - radiusQ8 + 128, 0, 256);
        const int32_t edgeCoverageQ8 =
            (innerCoverageQ8 * outerCoverageQ8 + 128) / 256;
        const int32_t radialBrightness =
            40 + (gradientPositionQ8 * 215 + 128) / 256;
        const uint8_t brightness = static_cast<uint8_t>(
            constrain((radialBrightness * edgeCoverageQ8 + 128) / 256, 0, 255));
        framebuffer[y * 360 + x] = scaledColor[brightness];
      }
    }
  }
  else
  {
    // Speicher-Fallback ohne Canvas: gefuellte Kreise ueberschreiben sich von
    // aussen nach innen und erzeugen ebenfalls geschlossene Farbringen.
    const int layers = static_cast<int>(ceilf(ringWidth));
    ui->fillCircle(180, 180, 177, color);
    for (int layer = 0; layer < layers; ++layer)
    {
      const uint8_t brightness = static_cast<uint8_t>(
          255 - (layer * 215 / max(1, layers - 1)));
      ui->fillCircle(180, 180, 175 - layer, scaleColor(color, brightness));
    }
    ui->fillCircle(180, 180, 175 - layers, COLOR_BLACK);
  }
}

void drawCenteredText(const char *text, int16_t centerX, int16_t baselineY)
{
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  ui->getTextBounds(text, 0, baselineY, &x1, &y1, &width, &height);
  ui->setCursor(centerX - static_cast<int16_t>(width / 2), baselineY);
  ui->print(text);
}

int16_t textWidth(const char *text)
{
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  ui->getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  return static_cast<int16_t>(width);
}

void drawTemperature()
{
  char value[16];
  if (sensorHardError || !haveValidTemperature)
  {
    snprintf(value, sizeof(value), "--.-");
  }
  else
  {
    snprintf(value, sizeof(value), "%.1f", currentTemperature);
  }

  ui->setFont(u8g2_font_logisoso78_tn);
  ui->setTextColor(COLOR_WHITE);
  const int16_t numberWidth = textWidth(value);
  const int16_t numberX = 168 - numberWidth / 2;
  ui->setCursor(numberX, 183);
  ui->print(value);

  // Kleine Einheit neben der grossen Zahl - moderner als ein gleich grosses Grad-C.
  const int16_t unitX = min(315, numberX + numberWidth + 7);
  ui->drawCircle(unitX + 5, 119, 4, COLOR_TEXT_DIM);
  ui->setFont(u8g2_font_helvB18_tr);
  ui->setTextColor(COLOR_TEXT_DIM);
  ui->setCursor(unitX + 13, 139);
  ui->print("C");
}

uint16_t statePillColor(SystemState state)
{
  switch (state)
  {
  case SystemState::Cooling:
    return COLOR_GREEN_DARK;
  case SystemState::Ready:
    return COLOR_BLUE_DARK;
  case SystemState::Waiting:
    return 0x8B00;
  case SystemState::Error:
    return COLOR_RED;
  case SystemState::Off:
  default:
    return COLOR_RED_DARK;
  }
}

void drawMainScreen(uint32_t now)
{
  ui->fillScreen(COLOR_BLACK);
  drawGradientRing(now);

  ui->setFont(u8g2_font_helvB12_tr);
  ui->setTextColor(COLOR_TEXT_DIM);
  drawCenteredText("AKTUELL", 180, 57);

  drawTemperature();

  char setText[32];
  snprintf(setText, sizeof(setText), "Soll  %.1f C", settings.setTemperature);
  ui->setFont(u8g2_font_helvR18_tr);
  ui->setTextColor(COLOR_TEXT_DIM);
  drawCenteredText(setText, 180, 235);
  const int16_t setWidth = textWidth(setText);
  ui->drawCircle(180 + setWidth / 2 - 11, 216, 2, COLOR_TEXT_DIM);

  constexpr int16_t pillWidth = 142;
  constexpr int16_t pillHeight = 42;
  constexpr int16_t pillX = (360 - pillWidth) / 2;
  constexpr int16_t pillY = 270; // gegenueber V1 um etwa 5 px angehoben
  ui->fillRoundRect(pillX, pillY, pillWidth, pillHeight, pillHeight / 2, statePillColor(systemState));
  ui->setFont(u8g2_font_helvB14_tf);
  ui->setTextColor(COLOR_WHITE);
  drawCenteredText(stateText(systemState), 180, pillY + 28);
}

const char *menuLabel(uint8_t item)
{
  static const char *labels[MenuItemCount] = {
      "Solltemperatur",
      "Hysterese",
      "Startverzög.",
      "Mindest AUS",
      "Mindest EIN",
      "Sensorfehler",
      "Gelb ab Soll+",
      "Rot ab Soll+",
      "Helligkeit",
      "Autostart",
      "Werkseinstellung",
      "Zurück"};
  return labels[item];
}

void menuValue(uint8_t item, char *buffer, size_t size)
{
  switch (item)
  {
  case MenuSetTemperature:
    snprintf(buffer, size, "%.1f C", settings.setTemperature);
    break;
  case MenuHysteresis:
    snprintf(buffer, size, "%.1f C", settings.hysteresis);
    break;
  case MenuStartDelay:
    snprintf(buffer, size, "%u s", settings.startDelaySeconds);
    break;
  case MenuMinimumOff:
    snprintf(buffer, size, "%u s", settings.minimumOffSeconds);
    break;
  case MenuMinimumOn:
    snprintf(buffer, size, "%u s", settings.minimumOnSeconds);
    break;
  case MenuSensorFault:
    snprintf(buffer, size, "%u s", settings.sensorFaultDelaySeconds);
    break;
  case MenuYellowDelta:
    snprintf(buffer, size, "%.1f C", settings.yellowDelta);
    break;
  case MenuRedDelta:
    snprintf(buffer, size, "%.1f C", settings.redDelta);
    break;
  case MenuBrightness:
    snprintf(buffer, size, "%u %%", static_cast<unsigned>(settings.brightness) * 100U / 255U);
    break;
  case MenuAutostart:
    snprintf(buffer, size, "%s", settings.autostart ? "AN" : "AUS");
    break;
  case MenuFactoryReset:
    snprintf(buffer, size, "RESET");
    break;
  case MenuBack:
  default:
    buffer[0] = '\0';
    break;
  }
}

void drawMenu()
{
  ui->fillScreen(COLOR_BLACK);
  ui->drawCircle(180, 180, 177, COLOR_PANEL_SELECTED);

  // 20 px tiefer als die urspruengliche Titelposition, damit die Rundung nichts abschneidet.
  ui->setFont(u8g2_font_helvB14_tr);
  ui->setTextColor(COLOR_WHITE);
  drawCenteredText("EINSTELLUNGEN", 180, 57);

  constexpr int visibleRows = 7;
  constexpr int rowHeight = 35;
  constexpr int rowTop = 73;
  int firstItem = static_cast<int>(selectedMenuItem) - visibleRows / 2;
  firstItem = constrain(firstItem, 0, max(0, static_cast<int>(MenuItemCount) - visibleRows));

  ui->setFont(u8g2_font_helvR12_tf);
  for (int row = 0; row < visibleRows; ++row)
  {
    const int item = firstItem + row;
    if (item >= MenuItemCount)
    {
      break;
    }

    const int y = rowTop + row * rowHeight;
    const bool selected = item == selectedMenuItem;
    if (selected)
    {
      ui->fillRoundRect(28, y, 304, 31, 15, COLOR_PANEL_SELECTED);
      if (editingMenuItem)
      {
        ui->drawRoundRect(28, y, 304, 31, 15, COLOR_ORANGE);
      }
    }

    ui->setTextColor(selected ? COLOR_WHITE : COLOR_TEXT_DIM);
    ui->setCursor(42, y + 21);
    ui->print(menuLabel(item));

    char value[20];
    menuValue(item, value, sizeof(value));
    if (value[0] != '\0')
    {
      const int16_t valueWidth = textWidth(value);
      ui->setCursor(317 - valueWidth, y + 21);
      ui->print(value);
    }
  }

  if (firstItem > 0)
  {
    ui->fillTriangle(176, 68, 184, 68, 180, 63, COLOR_TEXT_DIM);
  }
  if (firstItem + visibleRows < MenuItemCount)
  {
    ui->fillTriangle(176, 324, 184, 324, 180, 329, COLOR_TEXT_DIM);
  }
}

void flushUi()
{
  if (canvasAvailable)
  {
    displayCanvas->flush();
  }
}

void updateUi(uint32_t now)
{
  if (!displayAvailable)
  {
    return;
  }

  const uint32_t redrawInterval =
      (currentScreen == Screen::Main && compressorOn) ? 100UL : 500UL;
  if (!uiDirty && now - lastUiDrawAt < redrawInterval)
  {
    return;
  }

  if (currentScreen == Screen::Main)
  {
    drawMainScreen(now);
  }
  else
  {
    drawMenu();
  }
  flushUi();
  uiDirty = false;
  lastUiDrawAt = now;
}

void initializeDisplay()
{
  pinMode(Config::PIN_TFT_BL, OUTPUT);
  digitalWrite(Config::PIN_TFT_BL, Config::TFT_BACKLIGHT_ACTIVE_HIGH ? LOW : HIGH);

  if (displayCanvas->begin(Config::TFT_BUS_FREQUENCY))
  {
    ui = displayCanvas;
    canvasAvailable = true;
    displayAvailable = true;
    Serial.println(F("[DISPLAY] 360x360 Canvas aktiv"));
  }
  else if (displayPanel->begin(Config::TFT_BUS_FREQUENCY))
  {
    ui = displayPanel;
    canvasAvailable = false;
    displayAvailable = true;
    Serial.println(F("[DISPLAY] Canvas nicht verfuegbar - direkte Ausgabe aktiv"));
  }
  else
  {
    Serial.println(F("[DISPLAY] Initialisierung fehlgeschlagen"));
    return;
  }

  initializeRingRadiusLookup();
  ui->setRotation(0);
  ui->setTextWrap(false);
  ui->setUTF8Print(true);
  ui->fillScreen(COLOR_BLACK);
  flushUi();

  if (!ledcAttach(Config::PIN_TFT_BL, 5000, 8))
  {
    Serial.println(F("[DISPLAY] Backlight-PWM konnte nicht gestartet werden"));
  }
  applyBacklight();
}
} // namespace

void setup()
{
  // Alle Lasten so frueh wie moeglich auf einen sicheren AUS-Pegel bringen.
  prepareStirrerPins();
  prepareRelayPin(Config::PIN_RELAY_COMPRESSOR, Config::COMPRESSOR_RELAY_ACTIVE_LOW);
  prepareRelayPin(Config::PIN_RELAY_FAN, Config::FAN_RELAY_ACTIVE_LOW);

  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println(F("Bierkuehler ESP32-S3 V6 - DRV8871 und schnelle Bedienung"));
  Serial.printf("[BOOT] Demo=%s, PSRAM=%s\n", Config::DEMO_MODE ? "AN" : "AUS", psramFound() ? "gefunden" : "nicht gefunden");
  Serial.printf("[CONFIG] Relais: Kompressor=%s, Luefter=%s\n",
                Config::COMPRESSOR_RELAY_ACTIVE_LOW ? "ACTIVE_LOW" : "ACTIVE_HIGH",
                Config::FAN_RELAY_ACTIVE_LOW ? "ACTIVE_LOW" : "ACTIVE_HIGH");
  initializeStirrerDriver();

  loadSettings();

  pinMode(Config::PIN_ENCODER_CLK, INPUT_PULLUP);
  pinMode(Config::PIN_ENCODER_DT, INPUT_PULLUP);
  pinMode(Config::PIN_ENCODER_SW, INPUT_PULLUP);
  encoderPreviousState =
      (static_cast<uint8_t>(digitalRead(Config::PIN_ENCODER_CLK)) << 1) |
      static_cast<uint8_t>(digitalRead(Config::PIN_ENCODER_DT));
  const uint32_t inputNowUs = micros();
  buttonInterruptPressed = digitalRead(Config::PIN_ENCODER_SW) == LOW;
  buttonPressedAtUs = inputNowUs;
  buttonLastAcceptedEdgeAtUs =
      inputNowUs - Config::BUTTON_DEBOUNCE_MS * 1000UL;
  attachInterrupt(digitalPinToInterrupt(Config::PIN_ENCODER_CLK), onEncoderEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(Config::PIN_ENCODER_DT), onEncoderEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(Config::PIN_ENCODER_SW), onButtonEdge, CHANGE);

  const uint32_t now = millis();
  compressorTurnedOffAt = now; // Mindest-AUS-Zeit gilt auch nach dem Bestromen.
  coolingEnabledAt = now;
  coolingEnabled = settings.autostart;

  if (!Config::DEMO_MODE)
  {
    temperatureSensors.begin();
    temperatureSensors.setResolution(12);
    temperatureSensors.setWaitForConversion(false);
    temperatureSensors.requestTemperatures();
    conversionPending = true;
    conversionStartedAt = now;
    sensorInvalidPeriodActive = true;
    sensorInvalidSince = now;
  }

  initializeDisplay();
  updateControl(now);
  applyOutputs(now);
  uiDirty = true;
}

void loop()
{
  const uint32_t now = millis();

  updateTemperature(now);
  updateControl(now);
  applyOutputs(now);

  const int8_t encoderStep = readEncoderStep();
  const ButtonEvent button = readButtonEvent(now);
  if (currentScreen == Screen::Main)
  {
    handleMainInput(encoderStep, button, now);
  }
  else
  {
    handleMenuInput(encoderStep, button);
  }

  handleDeferredSettingsSave(now);
  updateUi(now);
  delay(2);
}
