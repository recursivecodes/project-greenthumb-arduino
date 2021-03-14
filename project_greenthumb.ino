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
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <TelnetStream.h>
#include "creds.h"

#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
#define OLED_RESET LED_BUILTIN 
#define SCREEN_ADDRESS 0x3C 
#define DHT11_PIN 10
#define RELAY_PIN D6
#define PROBE_THERMOMETER D4
#define WATER_SENSOR A0
#define OLED_SCL D7
#define OLED_SDA D8

#define S0 D0
#define S1 D7
#define S2 D8
#define S3 D3
#define SIG A0 

const long utcOffsetInSeconds = -14400;
int dayTemp = 80;
int morningTemp = 75;
int nightTemp = 70;
int incomingByte = 0;
int light;
int moisture;
int relayState;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
dht DHT;
OneWire oneWire(PROBE_THERMOMETER);
DallasTemperature probeThermometer(&oneWire);
WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, RABBIT_SERVER, RABBIT_PORT, RABBIT_USER, RABBIT_PASSWORD);
Adafruit_MQTT_Publish readingsTopic = Adafruit_MQTT_Publish(&mqtt, "greenthumb/readings");
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
TwoWire Wire1 = TwoWire();
BH1750 lightMeter;

void initOta(){
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
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
  pinMode(RELAY_PIN, OUTPUT);

  pinMode(S0,OUTPUT);        
  pinMode(S1,OUTPUT);
  pinMode(S2,OUTPUT);
  pinMode(S3,OUTPUT);
  pinMode(SIG, INPUT); 

  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    TelnetStream.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }

  display.clearDisplay();
  display.setTextSize(1);            
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  String conMsg = "Connecting to " + String(ssid);
  display.print(conMsg);
  display.display();
    
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

  timeClient.begin();
  Wire1.begin();
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire1);
}

void loop(){
  ArduinoOTA.handle();
  
  timeClient.update();
  
  int chk = DHT.read11(DHT11_PIN);
  String airTempMsg = "Air: " + String((int) cToF(DHT.temperature)) + "F (" + String((int) DHT.temperature) + "C)";
  String humidityMsg = "Humidity: " + String((int) DHT.humidity) + "%";

  probeThermometer.requestTemperatures(); 
  float probeTempC = probeThermometer.getTempCByIndex(0);
  float probeTempF = probeThermometer.toFahrenheit(probeTempC);
  String probeTempMsg = "Soil: " + String((int) probeTempF) + "F (" + String((int) probeTempC) + "C)";
  
  moisture = analogRead(WATER_SENSOR);
  float moisturePct = map(moisture,30,600,0,100);
  if( moisturePct < 0 ){
    moisturePct = 0; 
  }
  String moistureMsg = "Moisture: " + String((int) moisturePct) + "%";
  
  uint16_t lux = lightMeter.readLightLevel();
  String lightMsg = "Light: " + String(lux) + " lux";
  
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
  display.setCursor(0,40);
  display.println(lightMsg);

  int currentHour = timeClient.getHours();
  TelnetStream.println(timeClient.getFormattedTime());

  // after 5am, starting warming soil back up
  // between 7a and 9p, keep soil at dayTemp 
  /*
  if( currentHour >= 5 && currentHour <= 7 ) {
    if( relayState == HIGH && probeTempF > (dayTemp + 2) ) {
      relayState = LOW;
      digitalWrite(RELAY_PIN, LOW);
    }
    if(relayState == LOW && probeTempF < (morningTemp - 2) ) {
      relayState = HIGH;
      digitalWrite(RELAY_PIN, HIGH);
    }
  }
  */
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
  display.setCursor(0,50);
  String state = relayState == LOW ? "Off" : "On";
  String outletMsg = "Outlet: " + state;
  display.println(outletMsg);
  display.display();
  
  StaticJsonDocument<150> doc;
  char readingsJson[256];

  doc["outletState"] = relayState;
  doc["airTemp"] = cToF(DHT.temperature);
  doc["soilTemp"] = probeTempF;
  doc["humidity"] = DHT.humidity;
  doc["moisture"] = moisturePct;
  doc["light"] = lux;
  
  serializeJson(doc, readingsJson);
  
  TelnetStream.println(readingsJson);
  MQTT_connect();
  readingsTopic.publish(readingsJson);
  delay(10000);
}

// courtesy of https://www.electronicwings.com/nodemcu/nodemcu-mqtt-client-with-arduino-ide
void MQTT_connect() {
  int8_t ret;
  if (mqtt.connected()) {
    return;
  }
  TelnetStream.print("Connecting to MQTT... ");
  uint8_t retries = 3;
  while ((ret = mqtt.connect()) != 0) {
       TelnetStream.println(mqtt.connectErrorString(ret));
       TelnetStream.println("Retrying MQTT connection in 5 seconds...");
       mqtt.disconnect();
       delay(5000);
       retries--;
       if (retries == 0) {
         while (1);
       }
  }
  TelnetStream.println("MQTT Connected!");
}

float cToF(float c) {
  return (c*1.8)+32;
}
