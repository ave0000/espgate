#include "Arduino.h"
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <EEPROM.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager/tree/development

#define FASTLED_INTERNAL // no other way to suppress build warnings
#include <FastLED.h> // Just for the handy EVERY_N macros
FASTLED_USING_NAMESPACE
#include <LittleFS.h>
#define MYFS LittleFS
#include "./simplehacks/static_eval.h"
#include "./simplehacks/constexpr_strlen.h"
#include "./simplehacks/array_size2.h"

#define NAME_PREFIX             "ESP8266-"
#define DATA_PIN                D1
#define MON_PIN                D5
#define ANALOG_PIN              A0
#define ANALOG_SAMPLES        10
#define NTP_UPDATE_THROTTLE_MILLLISECONDS (5UL * 60UL * 60UL * 1000UL) // Ping NTP server no more than every 5 minutes

String WiFi_SSID(bool persistent);
extern int utcOffsetInSeconds = -6 * 60 * 60;

WiFiManager wifiManager;
ESP8266WebServer webServer(80);
//WebSocketsServer webSocketsServer = WebSocketsServer(81);
ESP8266HTTPUpdateServer httpUpdateServer;

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds, NTP_UPDATE_THROTTLE_MILLLISECONDS);

String nameString;
uint8_t treeState = 0;

int sensorValue = 0;  // value read from the pot
float ratioFactor=5.714;  //Resistors Ratio Factor
int analog_samples = 10;

void setup() {
  pinMode(DATA_PIN, OUTPUT);
  pinMode(MON_PIN, OUTPUT);
  digitalWrite(DATA_PIN, HIGH);
  digitalWrite(MON_PIN, HIGH);
  
  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP    
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println(F("System Info:"));
  Serial.print( F("Heap: ") ); Serial.println(system_get_free_heap_size());
  Serial.print( F("Boot Vers: ") ); Serial.println(system_get_boot_version());
  Serial.print( F("CPU: ") ); Serial.println(system_get_cpu_freq());
  Serial.print( F("SDK: ") ); Serial.println(system_get_sdk_version());
  Serial.print( F("Chip ID: ") ); Serial.println(system_get_chip_id());
  Serial.print( F("Flash ID: ") ); Serial.println(spi_flash_get_id());
  Serial.print( F("Flash Size: ") ); Serial.println(ESP.getFlashChipRealSize());
  Serial.print( F("Vcc: ") ); Serial.println(ESP.getVcc());
  Serial.print( F("MAC Address: ") ); Serial.println(WiFi.macAddress());
  Serial.println();


  if (!MYFS.begin()) {
    Serial.println(F("An error occurred when attempting to mount the flash file system"));
  } else {
    Serial.println("FS contents:");

    Dir dir = MYFS.openDir("/");
    while (dir.next()) {
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), String(fileSize).c_str());
    }
    Serial.printf("\n");
  }

  // Do a little work to get a unique-ish name. Get the
  // last two bytes of the MAC (HEX'd)":

  // copy the mac address to a byte array
  uint8_t mac[WL_MAC_ADDR_LENGTH];
  WiFi.softAPmacAddress(mac);

  // format the last two digits to hex character array, like 0A0B
  char macID[5];
  sprintf(macID, "%02X%02X", mac[WL_MAC_ADDR_LENGTH - 2], mac[WL_MAC_ADDR_LENGTH - 1]);

  // convert the character array to a string
  String macIdString = macID;
  macIdString.toUpperCase();
  nameString = NAME_PREFIX + macIdString;

  // Allocation of variable-sized arrays on the stack is a GCC extension.
  // Converting this to be compile-time evaluated is possible:
  //     nameString.length() === strlen(NAME_PREFIX) + strlen(maxIdString)
  //     strlen(NAME_PREFIX) is compile-time constexpr (but changes per NAME_PREFIX)
  //     strlen(macIdString) is always 4
  // Therefore, can use the following to ensure statically evaluated at compile-time,
  // and avoid use of GCC extensions, with no performance loss.
  const size_t nameCharCount = static_eval<size_t, constexpr_strlen(NAME_PREFIX) + 4>::value;
  const size_t nameBufferSize = static_eval<size_t, nameCharCount+1>::value;
  char nameChar[nameBufferSize];
  memset(nameChar, 0, nameBufferSize);
  // Technically, this should *NEVER* need to check the nameString length.
  // However, I prefer to code defensively, since no static_assert() can detect this.
  size_t loopUntil = (nameCharCount <= nameString.length() ? nameCharCount : nameString.length());
  for (size_t i = 0; i < loopUntil; i++) {
    nameChar[i] = nameString.charAt(i);
  }

  Serial.printf("Name: %s\n", nameChar );

  wifiManager.setConfigPortalBlocking(false);

  //automatically connect using saved credentials if they exist
  //If connection fails it starts an access point with the specified name
  if(wifiManager.autoConnect(nameChar)){
    Serial.println("Wi-Fi connected");
  }
  else {
    Serial.println("Wi-Fi manager portal running");
  }

  httpUpdateServer.setup(&webServer);

  webServer.on("/gate", HTTP_GET, []() {
    sendInt(treeState);
  });

  webServer.on("/gate", HTTP_POST, []() {
    //String value = webServer.arg("value");
    toggle();
    sendInt(state_incr());
  });

  webServer.on("/gate", HTTP_PUT, []() {
    sendInt(state_incr());
  });

  webServer.on("/voltage", HTTP_GET, []() {
    sendFloat(sensorValue);
  });

  webServer.enableCORS(false);
  webServer.serveStatic("/", LittleFS, "/", "max-age=86400");

  MDNS.begin(nameChar);
  MDNS.setHostname(nameChar);

  webServer.begin();
  Serial.println("HTTP web server started");

  timeClient.begin();
}

void sendInt(uint8_t value)
{
  sendString(String(value));
}

void sendFloat(float value)
{
  sendString(String(value));
}

void sendString(String value)
{
  webServer.send(200, "text/plain", value);
}

uint8_t state_incr() {
  return treeState = ( treeState + 1 ) % 9;
}

void toggle() {
  digitalWrite(DATA_PIN, LOW);
  digitalWrite(MON_PIN, LOW);
  delay(300);
  digitalWrite(DATA_PIN, HIGH);   // turn the LED on (HIGH is the voltage level)
  delay(800);                       // wait for a second
  digitalWrite(MON_PIN, HIGH);    // turn the LED off by making the voltage LOW
}

void loop() {
  // put your main code here, to run repeatedly:

  // read the analog in value
  if(treeState > 0) {
    sensorValue = analogRead(ANALOG_PIN);
    for(unsigned int i=0;i<analog_samples;i++){
      sensorValue=sensorValue+analogRead(ANALOG_PIN);         //Read analog Voltage
      delay(5);                              //ADC stable
    }
    sensorValue = (float) sensorValue/10.0;            //Find average of N values
    sensorValue = (float) (sensorValue/1024.0)*5;      //Convert Voltage in 5v factor
    sensorValue = sensorValue*ratioFactor;          //Find original voltage by multiplying with factor
   
    // print the readings in the Serial Monitor
    Serial.print("sensor = ");
    Serial.println(sensorValue);
  }

  wifiManager.process();
  webServer.handleClient();
  MDNS.update();

  static bool hasConnected = false;

  EVERY_N_SECONDS(1) {
    if (WiFi.status() != WL_CONNECTED) {
      //      Serial.printf("Connecting to %s\n", ssid);
      hasConnected = false;
    }
    else if (!hasConnected) {
      hasConnected = true;
      MDNS.begin(nameString);
      MDNS.setHostname(nameString);
      webServer.begin();
      Serial.println("HTTP web server started");
      Serial.print("Connected! Open http://");
      Serial.print(WiFi.localIP());
      Serial.print(" or http://");
      Serial.print(nameString);
      Serial.println(".local in your browser");
    } else {
      timeClient.update(); // NTPClient has throttling built-in
    }
  }
}
