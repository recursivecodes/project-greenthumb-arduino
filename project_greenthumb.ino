// include libraries
#include <ESP8266WiFi.h> 
#include <dht.h>
#include <SPI.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include "EspMQTTClient.h"
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <TelnetStream.h>
#include "creds.h"

// define variables
#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
#define OLED_RESET LED_BUILTIN 
#define SCREEN_ADDRESS 0x3C 
#define DHT11_PIN 10
#define RELAY_PIN D6
#define PUMP_PIN D7
#define PROBE_THERMOMETER D4
#define WATER_SENSOR A0
#define OLED_SCL D7
#define OLED_SDA D8
#define S0 D0
#define S1 D7
#define S2 D8
#define S3 D3
#define SIG A0 
#define ENCODER_1 D3
#define ENCODER_2 D5

// global variables 
volatile int lastEncoded = 0;
volatile long waterLow = 50;
const long utcOffsetInSeconds = -18000;
long pumpStart = 0;
long pumpDuration = 5000;
long pumpDelay = 10000;
int pumpState = LOW;
int dayTemp = 80;
int nightTemp = 70;
int light;
int moisture;
int relayState;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
dht DHT;
OneWire oneWire(PROBE_THERMOMETER);
DallasTemperature probeThermometer(&oneWire);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
TwoWire Wire1 = TwoWire();
BH1750 lightMeter;

// MQTT topic/clientId/client
const char* greenthumbTopic = "greenthumb/readings";
const char* greenthumbClientId = "greenthumb_arduino";

EspMQTTClient mqttClient(
  ssid,
  password,
  MQTT_SERVER,
  MQTT_USER,
  MQTT_PASSWORD,
  greenthumbClientId
);

// MQTT connection callback
void onConnectionEstablished() {
  TelnetStream.println("MQTT Connected!");
}

// initialize Over The Air for updating via HTTP connection
void initOta(){
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }
    TelnetStream.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    TelnetStream.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    TelnetStream.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    TelnetStream.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      TelnetStream.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      TelnetStream.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      TelnetStream.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      TelnetStream.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      TelnetStream.println("End Failed");
    }
  });
  ArduinoOTA.begin();
}

void setup(){
  Serial.begin(115200);
  TelnetStream.begin();

  // setup pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(S0,OUTPUT);        
  pinMode(S1,OUTPUT);
  pinMode(S2,OUTPUT);
  pinMode(SIG, INPUT); 
  pinMode(ENCODER_1, INPUT_PULLUP);
  pinMode(ENCODER_2, INPUT_PULLUP);
  attachInterrupt(ENCODER_1, updateEncoder, RISING);
  attachInterrupt(ENCODER_2, updateEncoder, RISING);

  // handle display errors
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    TelnetStream.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }

  // setup display
  display.clearDisplay();
  display.setTextSize(1);            
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  String conMsg = "Connecting to " + String(ssid);
  display.print(conMsg);
  display.display();
    
  // connect to WiFi network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500);
    display.print(F("."));
    display.display();
  }
  if( WiFi.status() == WL_CONNECTED ) {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
  }

  initOta();
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println(F("Connected!"));
  display.display();
  delay(1000);

  // init time client and light meter
  timeClient.begin();
  Wire1.begin();
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire1);
}

void loop(){
  ArduinoOTA.handle();
  timeClient.update();
  
  // read air temp & humidity
  int chk = DHT.read11(DHT11_PIN);
  String airTempMsg = "Air: " + String((int) cToF(DHT.temperature)) + "F (" + String((int) DHT.temperature) + "C)";
  String humidityMsg = "Humidity: " + String((int) DHT.humidity) + "%";

  // read soil temp
  probeThermometer.requestTemperatures(); 
  float probeTempC = probeThermometer.getTempCByIndex(0);
  float probeTempF = probeThermometer.toFahrenheit(probeTempC);
  String probeTempMsg = "Soil: " + String((int) probeTempF) + "F (" + String((int) probeTempC) + "C)";
  
  // read soil moisture
  moisture = analogRead(WATER_SENSOR);
  float moisturePct = map(moisture,30,600,0,100);
  if( moisturePct < 0 ){
    moisturePct = 0; 
  }
  String moistureMsg = "Moist: " + String((int) moisturePct) + "%";
  String waterMsg = "Target: " + String((int) waterLow) + "%";

  // toggle pump, if necessary
  if(moisturePct < waterLow) {
    int _now = millis();
    int pumpRuntime = _now - pumpStart;

    if( pumpState == LOW ) {
      if(pumpRuntime >= (pumpDelay+pumpDuration)) {
        pumpState = HIGH;
        pumpStart = millis();
        digitalWrite(PUMP_PIN, HIGH);        
      }
    }
    else {
      if( pumpRuntime >= pumpDuration ) {
        digitalWrite(PUMP_PIN, LOW);
        pumpState = LOW;
      }
    }
  }
  else {
    pumpState = LOW;
    digitalWrite(PUMP_PIN, LOW); // FAIL SAFE - pump should be off here!
  }

  // read light level
  uint16_t lux = lightMeter.readLightLevel();
  String lightMsg = "Light: " + String(lux) + " lux";
  
  // update display with latest readings
  display.clearDisplay();
  display.setTextSize(1);            
  display.setTextColor(SSD1306_WHITE);        
  display.setCursor(0,0);             
  display.println(airTempMsg);
  display.setCursor(0,10);             
  display.println(probeTempMsg);
  display.setCursor(0,20);             
  display.println(humidityMsg);
  display.setCursor(0,30);
  display.println(moistureMsg);
  display.setCursor(65,30);
  display.println(waterMsg);
  display.setCursor(0,40);
  display.println(lightMsg);

  int currentHour = timeClient.getHours();

  // between 7a and 9p, keep soil at dayTemp 
  if( currentHour >= 7 && currentHour <= 21 ) {
    if( relayState == HIGH && probeTempF > (dayTemp + 2) ) {
      relayState = LOW;
      digitalWrite(RELAY_PIN, LOW);
    }
    if(relayState == LOW && probeTempF < (dayTemp - 2) ) {
      relayState = HIGH;
      digitalWrite(RELAY_PIN, HIGH);
    }
  }
  // at night, keep soil at nightTemp
  else {
    if( relayState == HIGH && probeTempF > (nightTemp + 2) ) {
      relayState = LOW;
      digitalWrite(RELAY_PIN, LOW);
    }
    if(relayState == LOW && probeTempF < (nightTemp - 2) ) {
      relayState = HIGH;
      digitalWrite(RELAY_PIN, HIGH);
    }
  }

  // update display with outlet and pump state
  display.setCursor(0,50);
  String state = relayState == LOW ? "Off" : "On";
  String outletMsg = "Outlet: " + state;
  display.println(outletMsg);
  
  display.setCursor(65,50);
  String pState = pumpState == LOW ? "Off" : "On";
  String pumpMsg = "Pump: " + pState;
  display.println(pumpMsg);
  
  display.display();
  
  // serialize JSON document to publish to MQTT topic
  StaticJsonDocument<150> doc;
  char readingsJson[256];

  doc["outletState"] = relayState;
  doc["pumpState"] = pumpState;
  doc["airTemp"] = cToF(DHT.temperature);
  doc["soilTemp"] = probeTempF;
  doc["humidity"] = DHT.humidity;
  doc["moisture"] = moisturePct;
  doc["waterTarget"] = waterLow;
  doc["light"] = lux;
  
  serializeJson(doc, readingsJson);
  TelnetStream.println(readingsJson);

  // publish JSON document to MQTT
  mqttClient.loop();
  mqttClient.publish(greenthumbTopic, readingsJson);
  delay(1000); // 10000
}

// convenience function to convert celsius to fahrenheit
float cToF(float c) {
  return (c*1.8)+32;
}

// handle rotary encoder change
ICACHE_RAM_ATTR void updateEncoder() {
  int a = digitalRead(ENCODER_1);
  int b = digitalRead(ENCODER_2);
  if( a==b ) return;
  if( a<b && waterLow > 0 ) waterLow = waterLow - 5;
  if( a>b && waterLow < 100 ) waterLow = waterLow + 5;
}
