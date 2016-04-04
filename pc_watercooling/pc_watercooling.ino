//PC watercooling monitor, Version 1.1.
/*Future features: 
   Consolidate common areas
   Add PID control to adjust system fan based on Loop Delta.
*/
//Display Library
#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_ILI9340.h"


//PID
//#include <PID_v1.h>

//Hz Counter
const uint8_t frqpin = 5; // digital pin #5
const uint32_t oneSecond = 1000;
uint32_t timer = 0;
uint32_t sts = 0;
const uint32_t c = 20; // wait for 20 pulses, due to possible low flow, system will wait until the number of pusles before temp updates.  Higher numbers can give better accuracy but at longer update interval.
uint32_t ets = 0;

//Display definitions
#if defined(__SAM3X8E__)
#undef __FlashStringHelper::F(string_literal)
#define F(string_literal) string_literal
#endif
#define _sclk 13
#define _miso 12
#define _mosi 11
#define _cs 10
#define _dc 9
#define _rst 8

// Use hardware SPI
Adafruit_ILI9340 tft = Adafruit_ILI9340(_cs, _dc, _rst);

// Thermister analog pins
#define RADIN A0
#define RADOUT A1
#define GPUTIN A2
#define GPUTOUT A3
#define AMBIENT A4

// Thermister resistance at 25 degrees C
#define THERMISTORNOMINAL 10000

// temp. for nominal resistance (almost always 25 C)
#define TEMPERATURENOMINAL 25

// how many samples to take and average, more takes longer
// but is more 'smooth'
#define NUMSAMPLES 20

// The beta coefficient of the thermistor (usually 3000-4000)
#define BCOEFFICIENT1 4950 //Water sensor
#define BCOEFFICIENT2 3950 //Ambient sensor

// the value of the 'other' resistor A0 - A4.  Measured with a high quality multimeter.  This is not nessisary but gives best accuracy.  Default is 10000.
#define SERIESRESISTOR1 9925 
#define SERIESRESISTOR2 9863
#define SERIESRESISTOR3 9881
#define SERIESRESISTOR4 9966
#define SERIESRESISTOR5 9983

// defining variables, I am sure there is a way to array the samples
int samples1[NUMSAMPLES];
int samples2[NUMSAMPLES];
int samples3[NUMSAMPLES];
int samples4[NUMSAMPLES];
int samples5[NUMSAMPLES];

// variables
int btu1 = 0;
int btu2 = 0;
int btu3 = 0;
int watts1 = 0;
int watts2 = 0;
int watts3 = 0;
float steinhart5;
float flow = 0; 
int radmax = 300; //Turn text red if heat rejected is above this number.
int gpumax = 200; //Same for GPU

void setup(void) {

//Flow meter pins
pinMode(frqpin, INPUT);

  
//analogReference(EXTERNAL); not used on this hardware rev, reference is 5v supply from arduino
//start display
  tft.begin();
  tft.fillScreen(ILI9340_BLACK); //Clearing the display at start
  tft.setRotation(0);
  
//This is the update once at reset diplay print.  This keeps the display from refreshing static data and makes the refresh much more responsive.

// Headings
  tft.setTextColor(ILI9340_BLACK, ILI9340_BLUE);  tft.setTextSize(3);
  tft.setCursor(0, 17);
  tft.print("Radiator     ");
  tft.setCursor(0, 75);
  tft.print("i5-6600K     ");
  tft.setCursor(0,133);
  tft.print ("GTX 980Ti    ");
  tft.setCursor(0, 190);
// Bottom blue bar  
  tft.setTextSize(1);
  tft.print("                                       ");
// Sub Headings 
  tft.setTextColor(ILI9340_WHITE, ILI9340_BLACK);  tft.setTextSize(2);
  tft.setCursor(5, 0);
  tft.print("Ambient T:");
  tft.setCursor(200, 0);
  tft.print("*C");
  tft.setCursor(0, 42);
  tft.print("Temp in  ");
  tft.setCursor(200, 42);
  tft.print("*C");
  tft.setCursor(0, 58);
  tft.print("Temp out ");
  tft.setCursor(200, 58);
  tft.print("*C");
  tft.setCursor(0, 100);
  tft.print("Temp in  ");
  tft.setCursor(200, 100);
  tft.print("*C");
  tft.setCursor(0, 116);
  tft.print("Temp out ");
  tft.setCursor(200, 116);
  tft.print("*C");
  tft.setCursor(0, 158);
  tft.print("Temp in  ");
  tft.setCursor(200, 158);
  tft.print("*C");
  tft.setCursor(0, 174);
  tft.print("Temp out ");
  tft.setCursor(200, 174);
  tft.print("*C");
  tft.setCursor(0, 200);
  tft.print("Flow");
  tft.setCursor(200, 200);
  tft.print("GPM");
  tft.setCursor(0, 216);
  tft.print("Watts out");
  tft.setCursor(200, 216);
  tft.print("W");
  tft.setCursor(0, 232);
  tft.print("GPU Heat");
  tft.setCursor(200, 232);
  tft.print("W");
  tft.setCursor(0, 250);
  tft.print("Loop Delta");
  tft.setCursor(200,250);
  tft.print("*C");
  
}

void loop(void) {
//Pulse counter for flow meter.  Koolance INS-FM19 at 0.307 LPM per pulse with 10mm tubing
pulseIn(frqpin,LOW);
 sts = micros(); // start time stamp
  for (uint32_t i=c; i>0; i--)
   pulseIn(frqpin,HIGH);
 ets = micros(); // end time stamp

  //define average numbers
  uint8_t i;
  float average1;
  float average2;
  float average3;
  float average4;
  float average5;
  
  // take N samples in a row, with a slight delay
  for (i = 0; i < NUMSAMPLES; i++) {
    samples1[i] = analogRead(RADIN);
    samples2[i] = analogRead(RADOUT);
    samples3[i] = analogRead(GPUTIN);
    samples4[i] = analogRead(GPUTOUT);
    samples5[i] = analogRead(AMBIENT);
    delay(5);
  }

  // average all the samples out
  average1 = 0;
  average2 = 0;
  average3 = 0;
  average4 = 0;
  average5 = 0;
  for (i = 0; i < NUMSAMPLES; i++) {
    average1 += samples1[i];
    average2 += samples2[i];
    average3 += samples3[i];
    average4 += samples4[i];
    average5 += samples5[i];
  }
  average1 /= NUMSAMPLES;
  average2 /= NUMSAMPLES;
  average3 /= NUMSAMPLES;
  average4 /= NUMSAMPLES;
  average5 /= NUMSAMPLES;

 
  // convert the value to resistance, probably a way to array this and clean it up
  average1 = 1023 / average1 - 1;
  average1 = SERIESRESISTOR1 / average1;
  average2 = 1023 / average2 - 1;
  average2 = SERIESRESISTOR2 / average2;
  average3 = 1023 / average3 - 1;
  average3 = SERIESRESISTOR3 / average3;
  average4 = 1023 / average4 - 1;
  average4 = SERIESRESISTOR4 / average4;
  average5 = 1023 / average5 - 1;
  average5 = SERIESRESISTOR5 / average5;

//convert resistance to temp.  one for each thermistor.  Array or consolidate?

//Radiator In
  float steinhart1;
  float steinhart1f;  //used for Delta
  steinhart1 = average1 / THERMISTORNOMINAL;     // (R/Ro)
  steinhart1 = log(steinhart1);                  // ln(R/Ro)
  steinhart1 /= BCOEFFICIENT1;                   // 1/B * ln(R/Ro)
  steinhart1 += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
  steinhart1 = 1.0 / steinhart1;                 // Invert
  steinhart1 -= 273.15;                         // convert to C
  steinhart1f = (steinhart1 * 9.0) / 5.0 + 32.0; // Convert Celcius to Fahrenheit if using F
  
//Radiator Out
  float steinhart2;
  float steinhart2f;
  steinhart2 = average2 / THERMISTORNOMINAL;     // (R/Ro)
  steinhart2 = log(steinhart2);                  // ln(R/Ro)
  steinhart2 /= BCOEFFICIENT1;                   // 1/B * ln(R/Ro)
  steinhart2 += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
  steinhart2 = 1.0 / steinhart2;                 // Invert
  steinhart2 -= 273.15;                         // convert to C
  steinhart2f = (steinhart2 * 9.0) / 5.0 + 32.0; // Convert Celcius to Fahrenheit

//GPU In
  float steinhart3;
  float steinhart3f;
  steinhart3 = average3 / THERMISTORNOMINAL;     // (R/Ro)
  steinhart3 = log(steinhart3);                  // ln(R/Ro)
  steinhart3 /= BCOEFFICIENT1;                   // 1/B * ln(R/Ro)
  steinhart3 += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
  steinhart3 = 1.0 / steinhart3;                 // Invert
  steinhart3 -= 273.15;                         // convert to C
  steinhart3f = (steinhart3 * 9.0) / 5.0 + 32.0; // Convert Celcius to Fahrenheit

  
// GPU Out
  float steinhart4;
  float steinhart4f;
  steinhart4 = average4 / THERMISTORNOMINAL;     // (R/Ro)
  steinhart4 = log(steinhart4);                  // ln(R/Ro)
  steinhart4 /= BCOEFFICIENT1;                   // 1/B * ln(R/Ro)
  steinhart4 += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
  steinhart4 = 1.0 / steinhart4;                 // Invert
  steinhart4 -= 273.15;                         // convert to C
  steinhart4f = (steinhart4 * 9.0) / 5.0 + 32.0; // Convert Celcius to Fahrenheit

// Flow Rate Calc
float flow;
flow = ((c*1e6/(ets-sts))); //flow rate in Hz
//Serial.print ("  flow ");
flow *= .307; //Liters pet minute per Hz
flow *= .26417; //Convert to GPM
//Serial.println (flow);

//Rad heat rejected calc
  float delta1;
  delta1 = steinhart1f - steinhart2f;
// Handle negative numbers
  if (delta1 < 0.0)
    delta1 = -delta1;
  btu1 = 500 * flow * delta1; //delta T * flow rate * 500 constant = BTU / Hour
  watts1 = btu1 * 0.29307107; //BTU / Hour * constant = Watts rejected.

 
//GPU Heat in calc  
  float delta2;
  delta2 = steinhart3f - steinhart4f;
  // Handle negative numbers
  if (delta2 < 0.0)
    delta2 = -delta2;

  btu2 = 500 * flow * delta2;
  watts2 = btu2 * 0.29307107;


//Ambient Temp
  float steinhart5f;
  steinhart5 = 0;
  steinhart5 = average5 / THERMISTORNOMINAL;     // (R/Ro)
  steinhart5 = log(steinhart5);                  // ln(R/Ro)
  steinhart5 /= BCOEFFICIENT2;                   // 1/B * ln(R/Ro)
  steinhart5 += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
  steinhart5 = 1.0 / steinhart5;                 // Invert
  steinhart5 -= 273.15;                         // convert to C
  if (steinhart5 < 0 ) {

  }
//loop delta ambient temp vs radiator input.  10 degress ideal.
  float delta4;
  delta4 = steinhart1 - steinhart5;
  if (delta4 < 0.0)
    delta4 = -delta4;

//Screen Formatting and printing
  //Printing Temps
  tft.setTextColor(ILI9340_WHITE, ILI9340_BLACK);  tft.setTextSize(2);
  tft.setCursor(130, 0);
  tft.print(steinhart5);  //Ambient
  tft.setCursor(130, 42);
  tft.print(steinhart1);  //Rad In
  tft.setCursor(130, 58);
  tft.print(steinhart2);  //Rad Out
  tft.setCursor(130, 100);
  tft.print(steinhart2);  //Also CPU In
  tft.setCursor(130, 116);
  tft.print(steinhart3); // GPU In / CPU Out
  tft.setCursor(130, 158);
  tft.print(steinhart3); // GPU In
  tft.setCursor(130, 174);
  tft.print(steinhart4); // GPU Out
  tft.setCursor(130, 200);
  tft.print(flow); // Flow rate in GPM
  tft.setCursor(130,216);
  //Radiator heat rejected
  // changing the text to red if value is above threshold
  if ( watts1 > radmax)
  {
    tft.setTextColor(ILI9340_RED, ILI9340_BLACK);
    tft.print(watts1);
  }
  else {
    tft.setTextColor(ILI9340_WHITE, ILI9340_BLACK);
    tft.print(watts1);
    tft.print("  ");//clears large red numbers
  }
  
  tft.setCursor(130, 232);
  //GPU heat input to loop.
  // changing the text to red if value is above threshold
  if ( watts2 > gpumax)
  {
    tft.setTextColor(ILI9340_RED, ILI9340_BLACK);
    tft.print(watts2);
  }
  else {
    tft.setTextColor(ILI9340_WHITE, ILI9340_BLACK);
    tft.print(watts2);
    tft.print("  ");//clears large red numbers
  }
  tft.setTextColor(ILI9340_WHITE, ILI9340_BLACK);
 
  //Loop delta, white below 9, red above 11, green otherwise.  
  tft.setCursor(130,250);
  if (delta4 > 11)
  {
    tft.setTextColor(ILI9340_RED, ILI9340_BLACK);
    tft.print(delta4);
  }
 else if (delta4 < 9)
 { 
    tft.setTextColor(ILI9340_WHITE, ILI9340_BLACK);
    tft.print(delta4);
  }
  else 
  {
    tft.setTextColor(ILI9340_GREEN, ILI9340_BLACK);
    tft.print (delta4);
  }

//  delay(900);  Delay is not needed as system will wait for the number of pulses of the flowmeter.
}

