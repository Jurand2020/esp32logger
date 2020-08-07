/*
  Temperature / Humidity logger
  Persistent logs on SD Card
  Logs use RTC time
*/
#include <math.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include "Button2.h";
#define BUTTON_PIN  2
Button2 button = Button2(BUTTON_PIN);

// OLED
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <SimpleTimer.h>

// SD Reader
//#include <SPI.h>
#include "SdFat.h"
#include "sdios.h"

//#define FS_NO_GLOBALS
//#include "SPIFFS.h"

// DS1307 RTC 
//#include <Wire.h>
#include <RTC.h>
static DS1307 RTC;

const char* ssid = "XXX";
const char* password = "xxxxxx";
const char* LogFileName = "/tmp_hmd.csv";

// SD Reader
const int SD_CS = 5;
#define SPI_SPEED SD_SCK_MHZ(4)
SdFat sd;

WebServer server(80);

// Temerature / humidity sensor
#include <dhtnew.h>
DHTNEW am2302(4);

// OLED
Adafruit_SSD1306 display(128, 64, &Wire, -1);


SimpleTimer tempTimer;
SimpleTimer tempLogTimer;
SimpleTimer dispTimer;
SimpleTimer dispTempTimer;

//Web security
const char* www_username = "admin";
const char* www_password = "esp32";
// allows you to set the realm of authentication Default:"Login Required"
const char* www_realm = "Temperature Logger Login";
// the Content of the HTML response in case of Unautherized Access Default:empty
String authFailResponse = "Authentication Failed";

// sensor data buffers
float temperature;
float humidity;
char timeString[25];
int screen = 0;

#define LOG_SUPERSAMPLE 4
struct th_log_item {
  float temperature;
  float humidity;
};

th_log_item th_log_array[LOG_SUPERSAMPLE];
int th_log_idx = 0; //next idx to write

char* module_status_string[] = {"ERR", "OK", "UNK"};
enum module_status {MODULE_ERR, MODULE_OK, MODULE_UNK};
module_status sdState = MODULE_UNK;
module_status rtcState = MODULE_UNK;
module_status dhtState = MODULE_UNK;
module_status wifiState = MODULE_UNK;


String page = "<html><head><meta http-equiv=\"refresh\" content=\"4\"/></head><body><h3>Current temperature and humidity readings</h3><div><span>Temperature: %TEMP%</span></div><div><span>Humidity: %HUMID%</span><div></body></html>";

void InitLogArray() {
  for(int i=0;i<LOG_SUPERSAMPLE;i++) {
    th_log_array[i].temperature = NAN;
    th_log_array[i].humidity = NAN;
  }
  th_log_idx = 0;
}

void setup() {
  Serial.begin(115200);

//  if(!SPIFFS.begin()){
//     Serial.println("An Error has occurred while mounting SPIFFS");
//     return;
//  }
  
  RTC.begin();

  for(int i=0;i<LOG_SUPERSAMPLE;i++) {
    th_log_array[i].temperature = NAN;
    th_log_array[i].humidity = NAN;
  }
  

  if (!RTC.isRunning()) {
    rtcState = MODULE_ERR;
    InitRTC();
  }

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED SSD1306 allocation failed"));
  }
  
  display.clearDisplay();
  display.display();
  Serial.println(F("OLED SSD1306 initialized"));

  Serial.println("Initializing Wifi...");
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(WiFiGotIP, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(WiFiLostIP, WiFiEvent_t::SYSTEM_EVENT_STA_LOST_IP);
  WiFi.begin(ssid, password);

  Serial.print("Initializing SD card...");
  // see if the card is present and can be initialized:
  if (!sd.begin(SD_CS, SPI_SPEED)) {
    Serial.println("Card failed, or not present");
    sdState = MODULE_ERR;
  }
  else  {  
     sdState = MODULE_OK;
     Serial.println("SD card initialized.");
  }

  PrintSysInfo();

  
  tempLogTimer.setInterval(60000);
  tempTimer.setInterval(60000/(LOG_SUPERSAMPLE-1));
  dispTempTimer.setInterval(3000);
  dispTimer.setInterval(200);
  button.setTapHandler(ButtonTap);
}

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.print("WiFi connected, IP address: ");
    Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));

    server.on("/", []() {
      if (!server.authenticate(www_username, www_password))
      {
        return server.requestAuthentication(DIGEST_AUTH, www_realm, authFailResponse);
      }
      server.send(200, "text/plain", "Login OK");
    });

//    server.on("/index", HTTP_GET, [](){
//            fs::File file = SPIFFS.open("index.htm", "r");
//            server.streamFile(file, "text/html");
//            file.close();
//          });
  
  
    server.on("/temp", []() {
      if (!server.authenticate(www_username, www_password))
      {
        return server.requestAuthentication(DIGEST_AUTH, www_realm, authFailResponse);
      }
    
    String resp = page;
    resp.replace("%TEMP%", GetTemperature());
    resp.replace("%HUMID%", GetHumidity());
    server.send(200, "text/html", resp);
  });
    server.begin();
    Serial.print("Open http://");
    Serial.print(WiFi.localIP());
    Serial.println("/ in your browser to see it working");

    
    if (!MDNS.begin("templogger")) {
      Serial.println("Error setting up MDNS responder!");
    } else {
      Serial.println("mDNS responder started");
      // Add service to MDNS-SD
      MDNS.addService("http", "tcp", 80);
    }

    wifiState = MODULE_OK;
}

void WiFiLostIP(WiFiEvent_t event, WiFiEventInfo_t info){
    Serial.println("Lost Wifi IP, stopping WebServer and mDNS..");
    server.stop();
    MDNS.end();
    wifiState = MODULE_ERR;    
}

void RefreshTemp() {
  int chk = am2302.read();
  if(chk == DHTLIB_OK) {
    dhtState = MODULE_OK;
    temperature = am2302.getTemperature();
    Serial.print("Temperature: ");
    Serial.print(temperature);
    humidity = am2302.getHumidity();
    Serial.print("  Humidity: ");
    Serial.println(humidity);
  }
  else {
    PrintAM2302Status(chk);
    dhtState = MODULE_ERR;
  }
}

void PrintAM2302Status(int chk) {
  Serial.print("AM2302 sensor status: ");
  switch (chk)
  {
    case DHTLIB_OK:
      Serial.print("OK,\t");
      break;
    case DHTLIB_ERROR_CHECKSUM:
      Serial.print("Checksum error,\t");
      break;
    case DHTLIB_ERROR_TIMEOUT_A:
      Serial.print("Time out A error,\t");
      break;
    case DHTLIB_ERROR_TIMEOUT_B:
      Serial.print("Time out B error,\t");
      break;
    case DHTLIB_ERROR_TIMEOUT_C:
      Serial.print("Time out C error,\t");
      break;
    case DHTLIB_ERROR_TIMEOUT_D:
      Serial.print("Time out D error,\t");
      break;
    case DHTLIB_ERROR_SENSOR_NOT_READY:
      Serial.print("Sensor not ready,\t");
      break;
    case DHTLIB_ERROR_BIT_SHIFT:
      Serial.print("Bit shift error,\t");
      break;
    default:
      Serial.print("Unknown: ");
      Serial.print(chk);
      Serial.print(",\t");
      break;
  }
  Serial.print("\n");
}

void InitRTC() {
    Serial.println("RTC NOT runnig, trying to initialize..");
    RTC.setHourMode(CLOCK_H24);
    RTC.setDateTime(__DATE__, __TIME__);
    Serial.print("New Time Set To: ");
    Serial.print(__DATE__);
    Serial.print(" ");
    Serial.println(__TIME__);
    RTC.startClock();
}

String GetTemperature() {
  return String(temperature);  
}

String GetHumidity() {
  return String(humidity);    
}

void DisplayReadings() {
  display.clearDisplay();

  display.setTextSize(2); // Draw 2X-scale text
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Tmp: ");
  display.println(temperature);
  display.print("Hmd: ");
  display.println(humidity);
  display.setTextSize(1); 
  display.println("");
  display.println("");
  display.print(" SD: ");
  display.print(module_status_string[sdState]);  
  display.print("   RTC: ");
  display.print(module_status_string[rtcState]);  
  display.print("\n");
  display.print("DTH: ");
  display.print(module_status_string[dhtState]);  
  display.print("  WIFI: ");
  display.println(module_status_string[wifiState]);  
  display.display();      // Show initial text
}

char* GetTimeString() {
    if (!RTC.isRunning()) {
      rtcState = MODULE_ERR;
      Serial.println("RTC not running");
    }
    else {
      rtcState = MODULE_OK;
    }

  sprintf(timeString, "%04ld-%02d-%02d %02d:%02d:%02d", RTC.getYear(), RTC.getMonth(), RTC.getDay(), RTC.getHours(), RTC.getMinutes(), RTC.getSeconds());
  return timeString;
}

void WriteReadingsToSD() {
  if (!sd.begin(SD_CS, SPI_SPEED)) {
    Serial.println("SD Card failed, or not present");
    sdState = MODULE_ERR;
    return;
  }
  
  File logFile = File(LogFileName, O_WRONLY | O_APPEND | O_CREAT);
  if(logFile.fileSize() == 0){
    //write header
//    if(logFile.write("\"sep=;\"")<0){
//      Serial.printf("Write to %s failed\n", LogFileName);
//      return;
//    }
    if(logFile.write("Time;Temperature;Humidity\n")<0){
      Serial.printf("Write to %s failed\n", LogFileName);
      return;
    }

    InitLogArray();
  }
    
  char logLine[100];
  sprintf(logLine, "%s;%f;%f\n", GetTimeString(), GetAvgTemperature(), GetAvgHumidity());
  if(logFile.write(logLine)<0){
      Serial.printf("Write to %s failed\n", LogFileName);    
  }
  else
  {
      sdState = MODULE_OK;
      Serial.printf("Log: %s\n", logLine);    
  }
  
  logFile.close();
}

void PrintSysInfo2Serial() {
    if( sdState == MODULE_OK){
      uint8_t cardType = sd.card()->type();
  
      Serial.print("SD Card Type: ");
      if(cardType == SD_CARD_TYPE_SD1){
          Serial.println("SD1");
      } else if(cardType == SD_CARD_TYPE_SD2){
          Serial.println("SD2");
      } else if(cardType == SD_CARD_TYPE_SDHC){
          Serial.println("SDHC");
      } else {
          Serial.println("UNKNOWN");
      }
  
      uint64_t cardSize = sd.card()->cardSize() / 1024;
      Serial.printf("SD Card Size: %lluMB\n", cardSize);
  //    Serial.printf("Total space: %lluMB\n", SD.totalBytes() / (1024 * 1024));
  //    Serial.printf("Used space: %lluMB\n", SD.usedBytes() / (1024 * 1024));
    } else {
      Serial.println("SD NOT available");
    }
  
    if(RTC.isRunning()){
      Serial.print("RTC Time: ");
      Serial.println(GetTimeString());
    } else {
      Serial.printf("RTC NOT Running\n");
    }

    if(WiFi.isConnected()){
      Serial.print("SSID:");
      Serial.println(WiFi.SSID());
      Serial.print("MAC:");
      Serial.println(WiFi.macAddress());
      Serial.print("IP:");
      Serial.println(WiFi.localIP());
  
    } else {
      Serial.println("Wifi NOT available");      
    }
  
}

void PrintSysInfo() {
    display.clearDisplay();
    display.setTextSize(1); // Draw 2X-scale text
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);

    if( sdState == MODULE_OK){
      uint8_t cardType = sd.card()->type();
  
      display.print("SD Card Type: ");
      if(cardType == SD_CARD_TYPE_SD1){
          display.println("SD1");
      } else if(cardType == SD_CARD_TYPE_SD2){
          display.println("SD2");
      } else if(cardType == SD_CARD_TYPE_SDHC){
          display.println("SDHC");
      } else {
          display.println("UNKNOWN");
      }
  
      uint64_t cardSize = sd.card()->cardSize() / 1024;
      display.printf("SD Card Size: %lluMB\n", cardSize);
  //    display.printf("Total space: %lluMB\n", SD.totalBytes() / (1024 * 1024));
  //    display.printf("Used space: %lluMB\n", SD.usedBytes() / (1024 * 1024));
    } else {
      display.println("SD NOT available");
    }
  
    if(RTC.isRunning()){
      display.print("T:");
      display.println(GetTimeString());
    } else {
      display.printf("RTC NOT Running\n");
    }

    if(WiFi.isConnected()){
      display.print("SSID:");
      display.println(WiFi.SSID());
      display.print("MAC:");
      display.println(WiFi.macAddress());
      display.print("IP:");
      display.println(WiFi.localIP());
    } else {
      display.println("Wifi NOT available");      
    }
    
    display.display();
}

void ButtonTap(Button2& btn) {
    Serial.println("ButtonTap");
    screen = ++screen%2;
    UpdateDisplay();
}

void UpdateDisplay() {
  switch(screen){
    case 0:
      DisplayReadings();
      break;
    case 1:
      PrintSysInfo();
      break;    
  }
}

void AddTempHumidToArray() {
// test
//  if(th_log_idx == 1)
//    temperature = NAN;
  th_log_array[th_log_idx].temperature = temperature;
  Serial.printf("Added idx %d temp %f", th_log_idx, temperature);
  th_log_array[th_log_idx].humidity = humidity;

  th_log_idx = ++th_log_idx % LOG_SUPERSAMPLE;
}

float GetAvgTemperature () {
  int count = 0;
  float aggr = 0;
  Serial.print("Avg temp comp: ");
  for(int i=0; i< LOG_SUPERSAMPLE; i++) {
    
    if(!isnan(th_log_array[i].temperature)) {
      aggr+=th_log_array[i].temperature;
      Serial.print(th_log_array[i].temperature);
      Serial.print(" ");
      count++;
    }
  }
    
  if(count > 0)
  {
      Serial.println(aggr/count);
      return aggr/count;
  }
  else
  {
    Serial.println(NAN);
    return NAN;
  }
}

float GetAvgHumidity () {
  int count = 0;
  float aggr = 0;
  for(int i=0; i< LOG_SUPERSAMPLE; i++) {
    if(!isnan(th_log_array[i].humidity)) {
      aggr+=th_log_array[i].humidity;
      count++;
    }
  }
  if(count > 0)
    return aggr/count;
  else
    return NAN;
}

void loop() {
  button.loop();
  if(dispTempTimer.isReady()){
   RefreshTemp();
   dispTempTimer.reset();
  }

  if(tempTimer.isReady()){
   AddTempHumidToArray();
   tempTimer.reset();
  }
  
  if(tempLogTimer.isReady()){
   WriteReadingsToSD();
   tempLogTimer.reset();
  }
  if(dispTimer.isReady()){
    UpdateDisplay();
    dispTimer.reset();
  }
  server.handleClient();
}
