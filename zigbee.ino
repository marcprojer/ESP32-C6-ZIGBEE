// Zigbee Dimmable Light - Arduino sketch for ESP32-C6 (End Device)
// Hardware wiring (LOW-SIDE configuration):
// - 12V PSU + -> LED +
// - LED - -> MOSFET Drain (D)
// - MOSFET Source (S) -> PSU GND
// - ESP32 GND -> PSU GND (common ground)
// - ESP32 GPIO18 -> 220 Ω -> MOSFET Gate (G)
// - 100 kΩ pull-down from Gate to GND (hardware)
// - Optional: small series resistor (10–100 Ω) for LED driver, add decoupling caps near
//   supply (100 nF + 10 µF).
// Note: To avoid gate floating/glitches on boot we force the GPIO LOW early in setup
// before configuring PWM/LEDC to ensure the MOSFET stays off during startup.
// Tools: Zigbee Mode = Zigbee ED (End Device), Partition scheme matches flash size

#ifndef ZIGBEE_MODE_ED
#error "Set Tools->Zigbee Mode to Zigbee ED (end device)"
#endif

#include <Arduino.h>
#include "Zigbee.h"
// Ensure LEDC prototypes are available for Arduino cores
#include "esp32-hal-ledc.h"
#include <driver/ledc.h>

// PWM config
const uint8_t PWM_PIN = 18;
const uint8_t PWM_CHANNEL = 0;
const uint32_t PWM_FREQ = 1000;
const uint8_t PWM_RESOLUTION = 8; // 0..255

// Zigbee endpoint
#define ZIGBEE_LIGHT_ENDPOINT 10
ZigbeeDimmableLight zbDimmableLight = ZigbeeDimmableLight(ZIGBEE_LIGHT_ENDPOINT);

// Button for factory reset / level change
const uint8_t BUTTON_PIN = BOOT_PIN;

// track current level (0..255)
static uint8_t currentLevel = 0;
static int pwm_channel = PWM_CHANNEL; // allow switching channel at runtime
static bool pwm_invert = false;

// Helper: set the PWM brightness (0..255)
static void setBrightness(uint8_t level) {
  // LEDC expects duty in 0..(2^resolution-1)
  uint32_t duty = pwm_invert ? (255 - level) : level; // resolution 8 bit => same range
  ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)pwm_channel, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)pwm_channel);
  currentLevel = level;
  Serial.printf("Set brightness: %u/255\n", level);
}

// Zigbee callback: called when cluster requests on/off + level changes
void onLightChange(bool state, uint8_t level) {
  Serial.printf("onLightChange called: state=%d level=%u\n", state, level);
  if (!state) {
    setBrightness(0);
  } else {
    // level provided by cluster: 0..255
    setBrightness(level);
  }
}

// Optional identify handler (blink)
void onIdentify(uint16_t time) {
  static bool blink = false;
  Serial.printf("Identify called for %u seconds\n", time);
  if (time == 0) {
    // restore state
    setBrightness(currentLevel);
    return;
  }
  blink = !blink;
  setBrightness(blink ? 255 : 0);
}

// Setup
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) { delay(10); }

  // Ensure PWM pin is driven LOW early so the MOSFET gate is not left floating
  // during startup. This keeps the low-side MOSFET off while peripherals initialize.
  // If you rely on the internal pull-down, you can also enable it first:
  pinMode(PWM_PIN, INPUT_PULLDOWN);
  delay(5);
  pinMode(PWM_PIN, OUTPUT);
  digitalWrite(PWM_PIN, LOW);

  // Setup PWM (monochrome) using ESP-IDF ledc API
  ledc_timer_config_t timer_conf;
  timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_conf.duty_resolution = LEDC_TIMER_8_BIT;
  timer_conf.timer_num = LEDC_TIMER_0;
  timer_conf.freq_hz = PWM_FREQ;
  timer_conf.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&timer_conf);

  ledc_channel_config_t ch_conf;
  ch_conf.channel = (ledc_channel_t)pwm_channel;
  ch_conf.duty = 0;
  ch_conf.gpio_num = PWM_PIN;
  ch_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  ch_conf.hpoint = 0;
  ch_conf.timer_sel = LEDC_TIMER_0;
  ledc_channel_config(&ch_conf);
  setBrightness(0);

  // Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Zigbee callbacks
  zbDimmableLight.onLightChange(onLightChange);
  zbDimmableLight.onIdentify(onIdentify);
  zbDimmableLight.setManufacturerAndModel("Espressif", "ZBLightBulb");

  Serial.println("Adding Zigbee endpoint to Zigbee Core");
  Zigbee.addEndpoint(&zbDimmableLight);

  // Start Zigbee (End Device)
  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    delay(1000);
    ESP.restart();
  }

  Serial.println("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();
  Serial.println("Zigbee connected");
}

// Loop: handle button
void loop() {
  // Check button for press actions
  if (digitalRead(BUTTON_PIN) == LOW) {
    // debounce
    delay(100);
    unsigned long start = millis();
    while (digitalRead(BUTTON_PIN) == LOW) {
      delay(50);
      if ((millis() - start) > 3000) {
        // long press -> factory reset
        Serial.println("Factory resetting Zigbee and rebooting...");
        delay(500);
        Zigbee.factoryReset();
      }
    }
    // short press: increase brightness by +50 (wrap)
    uint16_t newLevel = (uint16_t)zbDimmableLight.getLightLevel() + 50;
    if (newLevel > 255) newLevel = 255;
    zbDimmableLight.setLightLevel((uint8_t)newLevel);
  }

  // Serial manual test: send 'bNNN' to set brightness (0-255), 't' to toggle
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s.length() > 0) {
      if (s.charAt(0) == 'b') {
        int val = s.substring(1).toInt();
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        Serial.printf("Serial command: set brightness %d\n", val);
        setBrightness((uint8_t)val);
        } else if (s.charAt(0) == 'd') {
          // digital pin test: d1 -> HIGH, d0 -> LOW
          if (s.length() > 1 && s.charAt(1) == '1') {
            digitalWrite(PWM_PIN, HIGH);
            Serial.println("Digital write: HIGH");
          } else {
            digitalWrite(PWM_PIN, LOW);
            Serial.println("Digital write: LOW");
          }
        } else if (s.startsWith("ci")) {
          pwm_invert = !pwm_invert;
          Serial.printf("PWM invert now: %s\n", pwm_invert ? "ON" : "OFF");
        } else if (s.startsWith("ch")) {
          int newch = s.substring(2).toInt();
          if (newch < 0) newch = 0;
          if (newch > 7) newch = 7;
          pwm_channel = newch;
          Serial.printf("Switching PWM to channel %d\n", pwm_channel);
            // reconfigure channel mapping with a local config struct
            ledc_channel_config_t new_ch_conf;
            new_ch_conf.channel = (ledc_channel_t)pwm_channel;
            new_ch_conf.duty = 0;
            new_ch_conf.gpio_num = PWM_PIN;
            new_ch_conf.speed_mode = LEDC_LOW_SPEED_MODE;
            new_ch_conf.hpoint = 0;
            new_ch_conf.timer_sel = LEDC_TIMER_0;
            ledc_channel_config(&new_ch_conf);
      } else if (s.charAt(0) == 't') {
        uint8_t newv = (currentLevel == 0) ? 200 : 0;
        Serial.printf("Serial command: toggle to %d\n", newv);
        setBrightness(newv);
      }
    }
  }

  delay(100);
}