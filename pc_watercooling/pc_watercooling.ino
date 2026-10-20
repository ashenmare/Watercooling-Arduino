//PC watercooling monitor, Version 2.0 (2026 rework of the 2016 v1.1).
/* What changed from 1.1:
   - Flow is counted by an interrupt over a fixed 1 second window, so the screen updates every second no matter
     what the pump does, and a stopped pump shows 0.00 GPM in red instead of a small fake number.
   - A thermistor that is unplugged or shorted shows "SENS?" instead of a garbage temperature.
   - One function does the thermistor math for all five sensors.
   - Numbers are padded to a fixed width, so a shorter number never leaves old digits behind.
   - Heat numbers are floats (an int on the Uno tops out at 32767 and could wrap negative).
   - New line: CPU heat, measured across the CPU block (radiator out -> GPU in).
   Wiring is unchanged.
*/
#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_ILI9340.h"

// ---------- Display ----------
#define _cs 10
#define _dc 9
#define _rst 8
Adafruit_ILI9340 tft = Adafruit_ILI9340(_cs, _dc, _rst);   // hardware SPI

// ---------- Thermistors ----------
// Order around the loop: PUMP -> RAD IN (A0) -> RAD -> RAD OUT (A1) -> CPU -> GPU IN (A2) -> GPU -> GPU OUT (A3)
#define RADIN   A0
#define RADOUT  A1   // also CPU in
#define GPUTIN  A2   // also CPU out
#define GPUTOUT A3
#define AMBIENT A4

#define THERMISTORNOMINAL  10000   // resistance at 25 C
#define TEMPERATURENOMINAL 25
#define NUMSAMPLES         20      // samples averaged per reading
#define BETA_WATER   4950          // XSPC plug sensors
#define BETA_AMBIENT 3950          // NTC on the ambient probe

// The series resistors, measured with a good meter (this is why the readings are accurate).
const float SERIES_R[5] = { 9925, 9863, 9881, 9966, 9983 };
const uint8_t PINS[5]   = { RADIN, RADOUT, GPUTIN, GPUTOUT, AMBIENT };
const float BETAS[5]    = { BETA_WATER, BETA_WATER, BETA_WATER, BETA_WATER, BETA_AMBIENT };

// ---------- Flow meter (Koolance INS-FM19, 0.307 L/min per Hz with 10 mm tubing) ----------
// Pin 5 isn't one of the Uno's two external-interrupt pins (2 and 3), but EVERY pin can raise a pin-change
// interrupt. Pin 5 is bit 5 of port D, which is pin-change group 2 (PCINT21). The interrupt fires on every change
// (rising AND falling), so we count only rising edges by checking that the pin is now HIGH.
const uint8_t FLOW_PIN = 5;
const float LPM_PER_HZ = 0.307;
volatile uint16_t flowPulses = 0;   // 'volatile': changed inside the interrupt, read in loop()

ISR(PCINT2_vect) {                   // runs on any change of a port-D pin we enabled (only pin 5)
  if (PIND & _BV(PIND5)) flowPulses++;    // count it only if the pin went HIGH (a rising edge)
}

// ---------- Thresholds ----------
const float RAD_MAX_W   = 300;   // red above this much heat rejected by the radiator
const float GPU_MAX_W   = 200;   // red above this much GPU heat into the loop
const float CPU_MAX_W   = 150;   // red above this much CPU heat into the loop
const float MIN_FLOW_GPM = 0.20; // red below this (pump struggling or stopped)

const uint32_t UPDATE_MS = 1000; // one reading + redraw per second
uint32_t lastUpdate = 0;

// ---------- Helpers ----------
// Average NUMSAMPLES readings of one pin. The first read after switching the ADC to a new pin is thrown away,
// because the ADC's sampling capacitor needs a moment to settle on the new voltage.
float readAverage(uint8_t pin) {
  analogRead(pin);
  long sum = 0;
  for (uint8_t i = 0; i < NUMSAMPLES; i++) { sum += analogRead(pin); delay(1); }
  return (float)sum / NUMSAMPLES;
}

// ADC reading -> temperature in C with the Steinhart (beta) equation. Returns NAN if the probe looks unplugged
// or shorted (reading pinned near 0 or near 1023), so the screen can say so instead of printing nonsense.
float thermistorC(float raw, float seriesR, float beta) {
  if (raw < 5 || raw > 1018) return NAN;
  float r = seriesR / (1023.0 / raw - 1.0);         // the thermistor's resistance
  float t = log(r / THERMISTORNOMINAL) / beta;      // 1/B * ln(R/Ro)
  t += 1.0 / (TEMPERATURENOMINAL + 273.15);         // + 1/To
  return 1.0 / t - 273.15;                          // invert, Kelvin -> C
}

float cToF(float c) { return c * 9.0 / 5.0 + 32.0; }

// Heat carried by the water: BTU/h = 500 x GPM x deltaT(F), then BTU/h -> watts.
float heatWatts(float gpm, float tInC, float tOutC) {
  return 500.0 * gpm * fabs(cToF(tInC) - cToF(tOutC)) * 0.29307107;
}

// Print a number at (x, y) in a fixed-width field, so a shorter value overwrites every old digit.
// 6 characters at text size 2 are 72 px wide: starting at x=124 they end at 196, clear of the units at x=200.
// NAN prints as "SENS?" (a probe problem upstream).
void printField(int16_t x, int16_t y, float value, uint8_t decimals, uint16_t color) {
  char buf[10];
  if (isnan(value)) { strcpy(buf, " SENS?"); color = ILI9340_YELLOW; }
  else dtostrf(value, 6, decimals, buf);          // width 6, right-aligned, padded with spaces
  tft.setTextColor(color, ILI9340_BLACK);         // background colour erases what was there
  tft.setCursor(x, y);
  tft.print(buf);
}

uint16_t colorAbove(float value, float limit) { return value > limit ? ILI9340_RED : ILI9340_WHITE; }

// ---------- Setup ----------
void setup(void) {
  pinMode(FLOW_PIN, INPUT);
  PCMSK2 |= _BV(PCINT21);   // enable the pin-change interrupt on pin 5 only
  PCICR  |= _BV(PCIE2);     // enable pin-change group 2 (port D)

  tft.begin();
  tft.fillScreen(ILI9340_BLACK);
  tft.setRotation(0);

  // Static text, drawn once (so each update only redraws the numbers: much faster).
  tft.setTextColor(ILI9340_BLACK, ILI9340_BLUE);  tft.setTextSize(3);
  tft.setCursor(0, 17);   tft.print("Radiator     ");
  tft.setCursor(0, 75);   tft.print("i5-6600K     ");
  tft.setCursor(0, 133);  tft.print("GTX 980Ti    ");
  tft.setCursor(0, 190);  tft.setTextSize(1);  tft.print("                                       ");

  tft.setTextColor(ILI9340_WHITE, ILI9340_BLACK);  tft.setTextSize(2);
  const char* labels[] = { "Ambient T:", "Temp in", "Temp out", "Temp in", "Temp out", "Temp in", "Temp out",
                           "Flow", "Watts out", "GPU Heat", "Loop Delta", "CPU Heat" };
  const int16_t ys[]   = { 0, 42, 58, 100, 116, 158, 174, 200, 216, 232, 250, 266 };
  const char* units[]  = { "*C", "*C", "*C", "*C", "*C", "*C", "*C", "GPM", "W", "W", "*C", "W" };
  for (uint8_t i = 0; i < 12; i++) {
    tft.setCursor(i == 0 ? 5 : 0, ys[i]);  tft.print(labels[i]);
    tft.setCursor(200, ys[i]);             tft.print(units[i]);
  }
  lastUpdate = millis();
}

// ---------- Loop ----------
// No blocking waits: loop() spins freely, and once a second we take the pulse count for that second and redraw.
void loop(void) {
  uint32_t now = millis();
  if (now - lastUpdate < UPDATE_MS) return;     // unsigned subtraction: safe when millis() wraps after ~49 days
  uint32_t elapsed = now - lastUpdate;
  lastUpdate = now;

  // Take the count and reset it in one go. Interrupts are paused for those two lines so a pulse can't land
  // between reading and zeroing (a 16-bit value is read in two halves on an 8-bit chip).
  noInterrupts();
  uint16_t pulses = flowPulses;
  flowPulses = 0;
  interrupts();

  float hz  = pulses * 1000.0 / elapsed;
  float gpm = hz * LPM_PER_HZ * 0.26417;        // L/min -> US GPM; 0 pulses really is 0.00

  float t[5];
  for (uint8_t i = 0; i < 5; i++) t[i] = thermistorC(readAverage(PINS[i]), SERIES_R[i], BETAS[i]);
  float radIn = t[0], radOut = t[1], gpuIn = t[2], gpuOut = t[3], ambient = t[4];

  float radW = heatWatts(gpm, radIn, radOut);   // heat the radiator rejected
  float cpuW = heatWatts(gpm, radOut, gpuIn);   // heat the CPU block added (rad out -> GPU in spans the CPU)
  float gpuW = heatWatts(gpm, gpuIn, gpuOut);   // heat the GPU block added
  float loopDelta = fabs(radIn - ambient);      // ~10 C is the target

  // Temperatures
  printField(124, 0,   ambient, 2, ILI9340_WHITE);
  printField(124, 42,  radIn,   2, ILI9340_WHITE);
  printField(124, 58,  radOut,  2, ILI9340_WHITE);
  printField(124, 100, radOut,  2, ILI9340_WHITE);   // CPU in = radiator out
  printField(124, 116, gpuIn,   2, ILI9340_WHITE);   // CPU out = GPU in
  printField(124, 158, gpuIn,   2, ILI9340_WHITE);
  printField(124, 174, gpuOut,  2, ILI9340_WHITE);

  // Flow: red when low or stopped
  printField(124, 200, gpm, 2, gpm < MIN_FLOW_GPM ? ILI9340_RED : ILI9340_WHITE);

  // Heat
  printField(124, 216, radW, 0, colorAbove(radW, RAD_MAX_W));
  printField(124, 232, gpuW, 0, colorAbove(gpuW, GPU_MAX_W));
  printField(124, 266, cpuW, 0, colorAbove(cpuW, CPU_MAX_W));

  // Loop delta: red above 11, green 9-11, white below 9
  uint16_t dc = loopDelta > 11 ? ILI9340_RED : (loopDelta < 9 ? ILI9340_WHITE : ILI9340_GREEN);
  printField(124, 250, loopDelta, 2, dc);
}
