// Zigbee Multi-LED Controller - Arduino sketch for ESP32-C6 (End Device)
// Hardware wiring (LOW-SIDE configuration):
// 
// LED1 (12V):
// - 12V PSU + -> LED +
// - LED - -> MOSFET1 Drain (D)
// - MOSFET1 Source (S) -> PSU1 GND
// - ESP32 GPIO18 -> 220 Ω -> MOSFET1 Gate (G)
//
// LED2 (5V):
// - 5V PSU + -> LED +
// - LED - -> MOSFET2 Drain (D)
// - MOSFET2 Source (S) -> PSU2 GND
// - ESP32 GPIO20 (D9/SDIO_CMD) -> 220 Ω -> MOSFET2 Gate (G)
//
// LED3 (3V via Stepdown):
// - 3V PSU + -> LED +
// - LED - -> MOSFET3 Drain (D)
// - MOSFET3 Source (S) -> PSU3 GND
// - ESP32 GPIO19 (D8/SDIO_CLK) -> 220 Ω -> MOSFET3 Gate (G)
//
// All PSU GNDs -> ESP32 GND (common ground)
// 100 kΩ pull-down from each Gate to GND (hardware)
// Decoupling caps near each supply (100 nF + 10 µF)
//
// Note: Gates forced LOW early in setup to keep MOSFETs off during startup
// Tools: Zigbee Mode = Zigbee ED (End Device), Partition scheme matches flash size

#ifndef ZIGBEE_MODE_ED
#error "Set Tools->Zigbee Mode to Zigbee ED (end device)"
#endif

#include <Arduino.h>
#include "Zigbee.h"
// Ensure LEDC prototypes are available for Arduino cores
#include "esp32-hal-ledc.h"
#include <driver/ledc.h>

// ============================================================================
// PWM Configuration - Three independent LED channels
// ============================================================================
struct LEDConfig {
  uint8_t pin;
  uint8_t channel;
  uint32_t freq;
  uint8_t resolution;
  const char* name;
};

const LEDConfig LED_CONFIGS[3] = {
  {18, 0, 1000, 8, "LED1_12V"},   // GPIO18 (D10), channel 0
  {20, 1, 1000, 8, "LED2_5V"},    // GPIO20 (D9),  channel 1
  {19, 2, 1000, 8, "LED3_3V"}     // GPIO19 (D8),  channel 2
};

// Zigbee endpoints (one per LED)
#define ZIGBEE_LED1_ENDPOINT 10
#define ZIGBEE_LED2_ENDPOINT 11
#define ZIGBEE_LED3_ENDPOINT 12

ZigbeeDimmableLight zbLed1 = ZigbeeDimmableLight(ZIGBEE_LED1_ENDPOINT);
ZigbeeDimmableLight zbLed2 = ZigbeeDimmableLight(ZIGBEE_LED2_ENDPOINT);
ZigbeeDimmableLight zbLed3 = ZigbeeDimmableLight(ZIGBEE_LED3_ENDPOINT);

// Button for factory reset / level change
const uint8_t BUTTON_PIN = BOOT_PIN;

// Track current levels (0..255) for each LED
static uint8_t currentLevels[3] = {0, 0, 0};
static bool pwm_invert[3] = {false, false, false};

// Helper: set the PWM brightness for a specific LED (0..255)
static void setBrightness(uint8_t led_index, uint8_t level) {
  if (led_index >= 3) return;
  
  const LEDConfig& cfg = LED_CONFIGS[led_index];
  uint32_t duty = pwm_invert[led_index] ? (255 - level) : level;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)cfg.channel, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)cfg.channel);
  currentLevels[led_index] = level;
  Serial.printf("%s brightness: %u/255\n", cfg.name, level);
}

// ============================================================================
// Zigbee Callbacks for each LED
// ============================================================================

void onLightChange_LED1(bool state, uint8_t level) {
  Serial.printf("LED1 (12V) change: state=%d level=%u\n", state, level);
  setBrightness(0, state ? level : 0);
}

void onIdentify_LED1(uint16_t time) {
  Serial.printf("LED1 (12V) Identify for %u seconds\n", time);
  if (time == 0) {
    setBrightness(0, currentLevels[0]);
    return;
  }
  static bool blink = false;
  blink = !blink;
  setBrightness(0, blink ? 255 : 0);
}

void onLightChange_LED2(bool state, uint8_t level) {
  Serial.printf("LED2 (5V) change: state=%d level=%u\n", state, level);
  setBrightness(1, state ? level : 0);
}

void onIdentify_LED2(uint16_t time) {
  Serial.printf("LED2 (5V) Identify for %u seconds\n", time);
  if (time == 0) {
    setBrightness(1, currentLevels[1]);
    return;
  }
  static bool blink = false;
  blink = !blink;
  setBrightness(1, blink ? 255 : 0);
}

void onLightChange_LED3(bool state, uint8_t level) {
  Serial.printf("LED3 (3V) change: state=%d level=%u\n", state, level);
  setBrightness(2, state ? level : 0);
}

void onIdentify_LED3(uint16_t time) {
  Serial.printf("LED3 (3V) Identify for %u seconds\n", time);
  if (time == 0) {
    setBrightness(2, currentLevels[2]);
    return;
  }
  static bool blink = false;
  blink = !blink;
  setBrightness(2, blink ? 255 : 0);
}

// Setup
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) { delay(10); }

  Serial.println("\n\n=== Multi-LED Zigbee Controller Starting ===\n");

  // ========================================================================
  // Initialize all GPIO pins and ensure MOSFETs are OFF during startup
  // ========================================================================
  for (int i = 0; i < 3; i++) {
    const LEDConfig& cfg = LED_CONFIGS[i];
    pinMode(cfg.pin, INPUT_PULLDOWN);
    delay(5);
    pinMode(cfg.pin, OUTPUT);
    digitalWrite(cfg.pin, LOW);
    Serial.printf("%s (GPIO%u) initialized LOW\n", cfg.name, cfg.pin);
  }

  // ========================================================================
  // Setup PWM for all three channels using ESP-IDF ledc API
  // ========================================================================
  
  // Timer configuration (shared across all channels)
  ledc_timer_config_t timer_conf;
  timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_conf.duty_resolution = LEDC_TIMER_8_BIT;
  timer_conf.timer_num = LEDC_TIMER_0;
  timer_conf.freq_hz = 1000;
  timer_conf.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&timer_conf);
  Serial.println("LEDC Timer 0 configured (8-bit, 1 kHz)");

  // Configure each LED channel
  for (int i = 0; i < 3; i++) {
    const LEDConfig& cfg = LED_CONFIGS[i];
    ledc_channel_config_t ch_conf;
    ch_conf.channel = (ledc_channel_t)cfg.channel;
    ch_conf.duty = 0;
    ch_conf.gpio_num = cfg.pin;
    ch_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_conf.hpoint = 0;
    ch_conf.timer_sel = LEDC_TIMER_0;
    ledc_channel_config(&ch_conf);
    Serial.printf("%s (Channel %u, GPIO%u) configured\n", cfg.name, cfg.channel, cfg.pin);
  }

  // Initialize brightness to 0
  for (int i = 0; i < 3; i++) {
    setBrightness(i, 0);
  }

  // ========================================================================
  // Button setup
  // ========================================================================
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // ========================================================================
  // Zigbee endpoint configuration
  // ========================================================================
  Serial.println("\nConfiguring Zigbee Endpoints:");

  zbLed1.onLightChange(onLightChange_LED1);
  zbLed1.onIdentify(onIdentify_LED1);
  zbLed1.setManufacturerAndModel("Espressif", "ZBMultiLED_12V");
  Serial.println("  LED1 (12V) - Endpoint 10");

  zbLed2.onLightChange(onLightChange_LED2);
  zbLed2.onIdentify(onIdentify_LED2);
  zbLed2.setManufacturerAndModel("Espressif", "ZBMultiLED_5V");
  Serial.println("  LED2 (5V) - Endpoint 11");

  zbLed3.onLightChange(onLightChange_LED3);
  zbLed3.onIdentify(onIdentify_LED3);
  zbLed3.setManufacturerAndModel("Espressif", "ZBMultiLED_3V");
  Serial.println("  LED3 (3V) - Endpoint 12");

  Serial.println("\nAdding Zigbee endpoints to Zigbee Core");
  Zigbee.addEndpoint(&zbLed1);
  Zigbee.addEndpoint(&zbLed2);
  Zigbee.addEndpoint(&zbLed3);

  // Start Zigbee (End Device)
  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    delay(1000);
    ESP.restart();
  }

  Serial.println("\nConnecting to Zigbee network...");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println("\n✓ Zigbee connected!");
  Serial.println("\n=== Ready to receive commands ===\n");
}

// Loop: handle button and serial commands
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
        Serial.println("\n>>> Factory resetting Zigbee and rebooting...");
        delay(500);
        Zigbee.factoryReset();
      }
    }
    // short press: increase brightness of LED1 by +50 (wrap)
    uint16_t newLevel = (uint16_t)zbLed1.getLightLevel() + 50;
    if (newLevel > 255) newLevel = 255;
    zbLed1.setLightLevel((uint8_t)newLevel);
  }

  // ========================================================================
  // Serial command interface
  // ========================================================================
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s.length() > 0) {
      char cmd = s.charAt(0);
      Serial.printf("Received serial: '%s'\n", s.c_str());

      // Brightness commands: check specific 'b3'/'b2' first, then generic 'b'
      if (s.startsWith("b3")) {
        int val = s.substring(2).toInt();
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        Serial.printf("CMD: Set LED3 (3V) brightness: %d\n", val);
        setBrightness(2, (uint8_t)val);
      }
      else if (s.startsWith("b2")) {
        int val = s.substring(2).toInt();
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        Serial.printf("CMD: Set LED2 (5V) brightness: %d\n", val);
        setBrightness(1, (uint8_t)val);
      }
      else if (cmd == 'b') {
        int val = s.substring(1).toInt();
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        Serial.printf("CMD: Set LED1 (12V) brightness: %d\n", val);
        setBrightness(0, (uint8_t)val);
      }
      
      // t - Toggle LED1
      else if (cmd == 't') {
        uint8_t newv = (currentLevels[0] == 0) ? 200 : 0;
        Serial.printf("CMD: Toggle LED1 (12V) to %d\n", newv);
        setBrightness(0, newv);
      }
      
      // t2 - Toggle LED2
      else if (s.startsWith("t2")) {
        uint8_t newv = (currentLevels[1] == 0) ? 200 : 0;
        Serial.printf("CMD: Toggle LED2 (5V) to %d\n", newv);
        setBrightness(1, newv);
      }
      
      // t3 - Toggle LED3
      else if (s.startsWith("t3")) {
        uint8_t newv = (currentLevels[2] == 0) ? 200 : 0;
        Serial.printf("CMD: Toggle LED3 (3V) to %d\n", newv);
        setBrightness(2, newv);
      }
      
      // d1 / d2 / d3 - Set LED output on/off via PWM (works even when LEDC configured)
      else if (cmd == 'd') {
        if (s.length() < 3) {
          Serial.println("CMD: Invalid d command format. Use dXY where X=1..3, Y=0/1");
        } else {
          int gpio_num = s.charAt(1) - '0';  // Extract LED number
          int val = s.charAt(2) - '0';        // Extract value (0 or 1)
          if (gpio_num >= 1 && gpio_num <= 3 && (val == 0 || val == 1)) {
            // Use PWM set to 0 or 255 so LEDC remains authoritative
            setBrightness(gpio_num - 1, val ? 255 : 0);
            Serial.printf("CMD: LED%d set to %s (via PWM)\n", gpio_num, val ? "ON" : "OFF");
          } else {
            Serial.println("CMD: Invalid d command. Use: d1/d2/d30 (LOW) or d1/d2/d31 (HIGH)");
          }
        }
      }
      
      // ci1/ci2/ci3 - Toggle invert for each LED
      else if (s.startsWith("ci")) {
        int led_num = s.charAt(2) - '0';
        if (led_num >= 1 && led_num <= 3) {
          pwm_invert[led_num - 1] = !pwm_invert[led_num - 1];
          Serial.printf("CMD: LED%d invert now: %s\n", led_num, pwm_invert[led_num - 1] ? "ON" : "OFF");
        }
      }
      
      // info - Print current status
      else if (s == "info") {
        Serial.println("\n=== Current Status ===");
        for (int i = 0; i < 3; i++) {
          Serial.printf("  %s (GPIO%u): %u/255 %s\n", 
            LED_CONFIGS[i].name, LED_CONFIGS[i].pin, currentLevels[i],
            pwm_invert[i] ? "(inverted)" : "");
        }
        Serial.println();
      }
      
      // help - Print command list
      else if (s == "help") {
        Serial.println("\n=== Command List ===");
        Serial.println("  bNNN          - Set LED1 (12V) brightness (0-255)");
        Serial.println("  b2NNN         - Set LED2 (5V) brightness (0-255)");
        Serial.println("  b3NNN         - Set LED3 (3V) brightness (0-255)");
        Serial.println("  t             - Toggle LED1 (12V)");
        Serial.println("  t2            - Toggle LED2 (5V)");
        Serial.println("  t3            - Toggle LED3 (3V)");
        Serial.println("  dXY           - Digital write: X=LED(1-3), Y=0(LOW)/1(HIGH)");
        Serial.println("  ciX           - Toggle PWM invert for LED X (1-3)");
        Serial.println("  info          - Print status");
        Serial.println("  help          - Print this message");
        Serial.println();
      }
      
      else {
        Serial.printf("Unknown command: '%s'. Type 'help' for commands.\n", s.c_str());
      }
    }
  }

  delay(100);
}