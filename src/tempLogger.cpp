#include <Arduino.h>

/*
  Temperature / Humidity logger
  Persistent logs on SD Card
  Logs use RTC time
*/
#include <WiFi.h>
#include "RTClib.h"

#include <sys/time.h>
#include <WiFiUdp.h>

#include <ESPmDNS.h>


#include "Button2.h"
#define BUTTON_PIN  2
Button2 button = Button2(BUTTON_PIN);

#include <Preferences.h>
Preferences preferences;

// OLED
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_SSD1306.h"

#include <SimpleTimer.h>

// SD Reader
#include "SdFat.h"
//#include "sdios.h"

#define FS_NO_GLOBALS
#include "SPIFFS.h"

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "AsyncSDFileResponse.h"


RTC_DS3231 RTC;

const char* LogFileName = "/logs/%04ld-%02d_hmd.csv";

// SD Reader
const int SD_CS = 5;
#define SPI_SPEED SD_SCK_MHZ(16)
SdFat sd;

AsyncWebServer server(80);

// Temerature / humidity sensor
#include <dhtnew.h>
#define DHTPIN 4     // what pin dht is connected to
#define DHTTYPE 22   // DHT 22  (AM2302)
DHTNEW dht(DHTPIN);

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
String noReading = "--.-";

// sensor data buffers
float temperature = NAN;
float humidity = NAN;
char timeString[25];
int screen = 0;
time_t last_action_time = 0xffffffff;
time_t boot_time;
const int screen_off_time = 1200;
const int screen_dim_time = 60;
bool screen_dimmed = false;
bool screen_saver = false;

#define LOG_SUPERSAMPLE 4
struct th_log_item {
  float temperature;
  float humidity;
};

th_log_item th_log_array[LOG_SUPERSAMPLE];
int th_log_idx = 0; //next idx to write

char const * module_status_string[] = {"ERR", "OK", "UNK"};
enum module_status {MODULE_ERR, MODULE_OK, MODULE_UNK};
module_status sdState = MODULE_UNK;
module_status rtcState = MODULE_UNK;
module_status dhtState = MODULE_UNK;
module_status wifiState = MODULE_UNK;

void InitRTC();
int setUnixtime(time_t unixtime);
void dateTime(uint16_t* date, uint16_t* time);
void PrintSysInfo();
void ButtonTap(Button2& btn);
void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
void WiFiLostIP(WiFiEvent_t event, WiFiEventInfo_t info);
char* GetSysTimeString();
char* GetTimeString();
void onGetLogs(AsyncWebServerRequest * request);
void onApiLogsGet(AsyncWebServerRequest * request);
void onApiWifi(AsyncWebServerRequest * request);
void onApiState(AsyncWebServerRequest * request);
void notFound(AsyncWebServerRequest * request);
String indexProc(const String& var);
String GetTemperature();
String GetHumidity();
String GetButtonStyle(module_status s);
float GetAvgTemperature ();
float GetAvgHumidity ();
void ScreenSaver(bool on);
void DisplayReadings();
bool IsValidReading(float reading);
String MakeLogsTable();

void InitLogArray() {
  for (int i = 0; i < LOG_SUPERSAMPLE; i++) {
    th_log_array[i].temperature = NAN;
    th_log_array[i].humidity = NAN;
  }
  th_log_idx = 0;
}


void setup() {
  Serial.begin(115200);
  preferences.begin("dht-app", false);

  if (!SPIFFS.begin()) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  if(!RTC.begin()) {
    Serial.println("RTC initialization failed");
    rtcState = MODULE_ERR;
  } else {
    rtcState = MODULE_OK;
  }



  for (int i = 0; i < LOG_SUPERSAMPLE; i++) {
    th_log_array[i].temperature = NAN;
    th_log_array[i].humidity = NAN;
  }

  if(rtcState == MODULE_OK) {
    if (RTC.lostPower()) {
      Serial.println("RTC lost power, initializing with build time");
      rtcState = MODULE_ERR;
      InitRTC();
      setUnixtime(RTC.now().unixtime());
    } else {
      setUnixtime(RTC.now().unixtime());
    }
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED SSD1306 allocation failed"));
  }

  display.clearDisplay();
  display.display();
  Serial.println(F("OLED SSD1306 initialized"));

  Serial.println("Initializing Wifi...");
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(WiFiGotIP, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(WiFiLostIP, WiFiEvent_t::SYSTEM_EVENT_STA_LOST_IP);


  String ssid = preferences.getString("clientSSID");
  String password = preferences.getString("clientSSIDpass");

  if(ssid.length() != 0 && password.length() !=0){
    WiFi.begin(ssid.c_str(), password.c_str());
  }
  else {
    Serial.println("Wifi client not configured.");
  }

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

  SdFile::dateTimeCallback(dateTime);

  PrintSysInfo();


  tempLogTimer.setInterval(60000);
  tempTimer.setInterval(60000 / (LOG_SUPERSAMPLE - 1));
  dispTempTimer.setInterval(3000);
  dispTimer.setInterval(200);
  button.setTapHandler(ButtonTap);

  time(&last_action_time);
  boot_time = last_action_time;
  th_log_idx = 0;
  screen = 0;
}

//=============================================================================
int setUnixtime(time_t unixtime) {
  timeval epoch = {unixtime, 0};
  Serial.println("SYS time adjusted");
  return settimeofday((const timeval*)&epoch, 0);
}
//=============================================================================

//=============================================================================
time_t getUnixtime() {
  time_t t;
  time(&t);
  return t;
}

//=============================================================================

//=============================================================================
// this function is called by SD if it wants to know the time
void dateTime(uint16_t* date, uint16_t* time)
{
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  // return date using FAT_DATE macro to format fields
  *date = FAT_DATE(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  // return time using FAT_TIME macro to format fields
  *time = FAT_TIME(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}
//=============================================================================

void SetupNTP(){
  char ntpPool[25];

  size_t len = preferences.getString("NTP_POOL", ntpPool, 25);
  if (len == 0) {
    String pool = "pl.pool.ntp.org";
    sprintf(ntpPool, pool.c_str());
    preferences.putString("NTP_POOL", pool);
  }

  configTime(0, 0, ntpPool);
}


//=============================================================================
// set hardware clock
void SetRTC(tm *new_time) {
  RTC.adjust(mktime(new_time));  
  Serial.println("RTC adjusted");
}
//=============================================================================

void InitRTC() {
  DateTime init = DateTime(__DATE__, __TIME__);
  RTC.adjust(init);
}

//=============================================================================
// on wifi connected
void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.print("WiFi connected, IP address: ");
  Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));

  SetupNTP();

  Serial.printf("Local Time: %s\n", GetSysTimeString());
  Serial.printf("RTC Time: %s\n", GetTimeString());

  //  server.on("/", []() {
  //    if (!server.authenticate(www_username, www_password))
  //    {
  //      return server.requestAuthentication(DIGEST_AUTH, www_realm, authFailResponse);
  //    }
  //    server.send(200, "text/plain", "Login OK");
  //  });

  server.serveStatic("/src", SPIFFS, "/src/").setCacheControl("public, max-age=31536000");
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html").setTemplateProcessor(indexProc);

  // Send a GET request to <IP>/sensor/<number>
  server.on("^\\/logs\\/(.+)$", HTTP_GET, [] (AsyncWebServerRequest * request) {
    onGetLogs(request);
  });

  server.on("/api/logs", HTTP_GET,  [] (AsyncWebServerRequest * request) {
    onApiLogsGet(request);
  });

  //First request will return 0 results unless you start scan from somewhere else (loop/setup)
  //Do not request more often than 3-5 seconds
  server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest * request) {
    onApiWifi(request);
  });

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest * request) {
    onApiState(request);
  });

  server.onNotFound(notFound);

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

//=============================================================================
//=============================================================================
bool startSD() {
  if (!sd.begin(SD_CS, SPI_SPEED)) {
    Serial.println("SD Card failed, or not present");
    sdState = MODULE_ERR;
    return false;
  }
  return true;
}

String MakeLogsTable(){
  if (!startSD()){    
    return String("<div class=\"alert alert-danger\" role=\"alert\">SD card not present!</div>");
  }

    File logs = sd.open("/logs", O_READ);
  FatFile file;
  dir_t entry;

  String table = "<table class=\"table table-bordered table-condensed table-striped table-hover\"><thead><tr><th scope=\"col\">#</th><th scope=\"col\">Name</th><th scope=\"col\">Size [kB]</th><th scope=\"col\">Time</th></tr></thead><tbody>";
  char filename[50];
  char filetime[50];
  int idx = 1;
  while (file.openNext(&logs, O_READ))
  {
    file.getName(filename, 50);
    if (!file.dirEntry(&entry)) {
      Serial.println("file.dirEntry failed");
    }

    sprintf(filetime, "%04d-%02d-%02d %02d:%02d:%02d", FAT_YEAR(entry.lastWriteDate), FAT_MONTH(entry.lastWriteDate), FAT_DAY(entry.lastWriteDate), FAT_HOUR(entry.lastWriteDate), FAT_MINUTE(entry.lastWriteDate), FAT_SECOND(entry.lastWriteDate));
    table += "<tr class=\"table-row\" data-href=\"logs/"+String(filename)+"\"><th scope=\"row\">"+String(idx++)+"</th><td>"+String(filename)+"</td><td>"+String(file.fileSize()/1024)+"</td><td>"+String(filetime)+"</td></tr>";
    file.close();
  }
  logs.close();
  table += "</tbody></table>";
  return table;
}

void onGetLogs(AsyncWebServerRequest * request) {
  if (!startSD()){
    request->send(500);
    return;
  }
  

  char filename[50];
  char path[50];
  request->pathArg(0).toCharArray(path, 50);
  snprintf(filename, 50, "/logs/%s", path);
  Serial.printf("get log %s\n", filename);
  if (!sd.exists(filename)) {
    Serial.printf("%s not found\n", filename);
    request->send(404);
    return;
  }

  AsyncSDFileResponse* resp = new AsyncSDFileResponse(sd, String(filename), String(), true);
  request->send(resp);
}

void onApiWifi(AsyncWebServerRequest * request) {
  String json = "[";
  int n = WiFi.scanComplete();
  if (n == -2) {
    WiFi.scanNetworks(true);
  } else if (n) {
    for (int i = 0; i < n; ++i) {
      if (i) json += ",";
      json += "{";
      json += "\"rssi\":" + String(WiFi.RSSI(i));
      json += ",\"ssid\":\"" + WiFi.SSID(i) + "\"";
      json += ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"";
      json += ",\"channel\":" + String(WiFi.channel(i));
      json += ",\"secure\":" + String(WiFi.encryptionType(i));
      json += "}";
    }
    WiFi.scanDelete();
    if (WiFi.scanComplete() == -2) {
      WiFi.scanNetworks(true);
    }
  }
  json += "]";
  request->send(200, "application/json", json);
  json = String();
}

void onApiState(AsyncWebServerRequest * request) {
  String json = "{";
  json += "\"temperature\":\"" + GetTemperature() + "\"";
  json += ",\"humidity\":\"" + GetHumidity() + "\"";
  json += ",\"sdState\":" + String(sdState);
  json += ",\"rtcState\":" + String(rtcState);
  json += ",\"dhtState\":" + String(dhtState);
  json += ",\"wifiState\":" + String(wifiState);
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += "}";
  request->send(200, "application/json", json);
  json = String();
}

void onApiLogsGet (AsyncWebServerRequest * request) {
  if (!startSD())
    return;

  File logs = sd.open("/logs", O_READ);
  FatFile file;
  dir_t entry;

  String json = "[";
  char filename[50];
  char filetime[50];
  bool first = true;
  while (file.openNext(&logs, O_READ))
  {
    if (first) {
      first = false;
    } else {
      json += ",";
    }

    file.getName(filename, 50);
    if (!file.dirEntry(&entry)) {
      Serial.println("file.dirEntry failed");
    }

    sprintf(filetime, "%04d-%02d-%02d %02d:%02d:%02d", FAT_YEAR(entry.lastWriteDate), FAT_MONTH(entry.lastWriteDate), FAT_DAY(entry.lastWriteDate), FAT_HOUR(entry.lastWriteDate), FAT_MINUTE(entry.lastWriteDate), FAT_SECOND(entry.lastWriteDate));
    json += "{";
    json += "\"name\":\"" + String(filename) + "\"";
    json += ",\"date\":\"" + String(filetime) + "\"";
    json += ",\"size\":" + String(file.fileSize());
    json += "}";

    file.close();
  }
  logs.close();
  json += "]";
  request->send(200, "application/json", json);
  json = String();
}

void notFound(AsyncWebServerRequest *request) {
  Serial.printf("NOT_FOUND: ");
  if(request->method() == HTTP_GET)
    Serial.printf("GET");
  else if(request->method() == HTTP_POST)
    Serial.printf("POST");
  else if(request->method() == HTTP_DELETE)
    Serial.printf("DELETE");
  else if(request->method() == HTTP_PUT)
    Serial.printf("PUT");
  else if(request->method() == HTTP_PATCH)
    Serial.printf("PATCH");
  else if(request->method() == HTTP_HEAD)
    Serial.printf("HEAD");
  else if(request->method() == HTTP_OPTIONS)
    Serial.printf("OPTIONS");
  else
    Serial.printf("UNKNOWN");
  Serial.printf(" http://%s%s\n", request->host().c_str(), request->url().c_str());

  if(request->contentLength()){
    Serial.printf("_CONTENT_TYPE: %s\n", request->contentType().c_str());
    Serial.printf("_CONTENT_LENGTH: %u\n", request->contentLength());
  }

  int headers = request->headers();
  int i;
  for(i=0;i<headers;i++){
    AsyncWebHeader* h = request->getHeader(i);
    Serial.printf("_HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
  }

  int params = request->params();
  for(i=0;i<params;i++){
    AsyncWebParameter* p = request->getParam(i);
    if(p->isFile()){
      Serial.printf("_FILE[%s]: %s, size: %u\n", p->name().c_str(), p->value().c_str(), p->size());
    } else if(p->isPost()){
      Serial.printf("_POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
    } else {
      Serial.printf("_GET[%s]: %s\n", p->name().c_str(), p->value().c_str());
    }
  }

  request->send(404, "text/plain", "Not found");
}
//=============================================================================
//=============================================================================

String indexProc(const String& var) {
  if (var == "TEMP")
    return GetTemperature();
  if (var == "HUMID")
    return GetHumidity();

  if (var == "DHT_S")
    return GetButtonStyle(dhtState);
  if (var == "SD_S")
    return GetButtonStyle(sdState);
  if (var == "RTC_S")
    return GetButtonStyle(rtcState);
  if (var == "WIFI_S")
    return GetButtonStyle(wifiState);

  if (var == "LOG_TABLE")
    return MakeLogsTable();


  return String();
}
//=============================================================================
//=============================================================================

String GetButtonStyle(module_status s) {
  switch (s) {
    case MODULE_OK:
      return String("success");
      break;
    case MODULE_ERR:
      return String("danger");
      break;
    case MODULE_UNK:
    default:
      return String("warning");
      break;
  }
}
//=============================================================================

//=============================================================================
// on wifi disconnected
void WiFiLostIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println("Lost Wifi IP, stopping WebServer and mDNS..");
  server.end();
  MDNS.end();
  wifiState = MODULE_ERR;
}
//=============================================================================


//=============================================================================
// read temperature and humidity from sensor
void RefreshTemp() {


  if(dht.read() == DHTLIB_OK) {
    humidity = dht.getHumidity();
    temperature = dht.getTemperature();
    dhtState = MODULE_OK;
  }
  else {
    Serial.println("Failed to get temprature and humidity value.");
    humidity = NAN;
    temperature = NAN;
    dhtState = MODULE_ERR;
  }
}
//=============================================================================


//=============================================================================
// get temperature as string
String GetTemperature() {
  if (isnan(temperature)) {
    return noReading;
  } else {
    return String(temperature, 1);
  }
}
//=============================================================================


//=============================================================================
// get humidity as string
String GetHumidity() {
  if (isnan(humidity)) {
    return noReading;
  } else {
    return String(humidity, 1);
  }
}
//=============================================================================

//=============================================================================
// get RTC time as string
char* GetTimeString() {
  if (RTC.lostPower()) {
    rtcState = MODULE_ERR;
    Serial.println("RTC not running");
  }
  else {
    rtcState = MODULE_OK;
  }

  DateTime now = RTC.now();
  sprintf(timeString, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  return timeString;
}
//=============================================================================

//=============================================================================
// get system time as string
char* GetSysTimeString() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  sprintf(timeString, "%04d-%02d-%02d %02d:%02d:%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return timeString;
}
//=============================================================================

void GetLogFileName(char* name_buffer) {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  sprintf(name_buffer, LogFileName, timeinfo.tm_year + 1900, timeinfo.tm_mon + 1);
}

//=============================================================================
// write readings to LOG
void WriteReadingsToSD() {
  if (!startSD())
    return;

  sdState = MODULE_OK;

  float avgT = GetAvgTemperature();
  float avgH = GetAvgHumidity();
  InitLogArray();
  char name_buffer[50];
  GetLogFileName(name_buffer);
  if (!sd.exists("/logs")) {
    sd.mkdir("/logs");
  }

  if (!isnan(avgT) && !isnan(avgH)) {
    File logFile = File(name_buffer, O_WRONLY | O_APPEND | O_CREAT);

    if (logFile.fileSize() == 0) {
      if (logFile.write("Time;Temperature;Humidity\n") < 0) {
        Serial.printf("Write to %s failed\n", name_buffer);
        return;
      }
    }

    char logLine[100];
    sprintf(logLine, "%s;%f;%f\n", GetTimeString(), avgT, avgH);
    if (logFile.write(logLine) < 0) {
      Serial.printf("Write to %s failed\n", name_buffer);
      sdState = MODULE_ERR;
    }
    else {
      sdState = MODULE_OK;
      Serial.printf("Log: %s\n", logLine);
    }

    logFile.close();
  }
  else {
    Serial.println("Log: skipped - no valid data");
  }

}
//=============================================================================


//=============================================================================
// screen display dispatch
void UpdateDisplay() {
  time_t now;
  time(&now);

  if (now - last_action_time > screen_dim_time) {
    if (!screen_dimmed) {
      screen_dimmed = true;
      display.dim(true);
      return;
    }
  } else if (screen_dimmed) {
    display.dim(false);
    screen_dimmed = false;
    return;
  }

  if (now - last_action_time > screen_off_time) {
    if (!screen_saver) {
      screen_saver = true;
      ScreenSaver(screen_saver);
    }
    return;
  }

  if (screen_saver) {
    screen_saver = false;
    ScreenSaver(screen_saver);
  }

  switch (screen) {
    case 0:
      DisplayReadings();
      break;
    case 1:
      PrintSysInfo();
      break;
  }
}
//=============================================================================

void ScreenSaver(bool on) {
  if (on) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  } else {
    display.ssd1306_command(SSD1306_DISPLAYON);
  }
}



//=============================================================================
//display readings screen
void DisplayReadings() {
  display.clearDisplay();

  display.setTextSize(2); // Draw 2X-scale text
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Tmp:");
  display.println(GetTemperature());
  display.print("Hmd:");
  display.println(GetHumidity());
  display.setTextSize(1);
  display.println("");
  display.println("");
  display.print(" SD: ");
  display.print(module_status_string[sdState]);
  display.print("   RTC: ");
  display.print(module_status_string[rtcState]);
  display.print("\n");
  display.print("DHT: ");
  display.print(module_status_string[dhtState]);
  display.print("  WIFI: ");
  display.println(module_status_string[wifiState]);
  display.display();      // Show initial text
}
//=============================================================================

//=============================================================================
//display system info
void PrintSysInfo() {
  display.clearDisplay();
  display.setTextSize(1); // Draw 2X-scale text
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  if ( sdState == MODULE_OK) {
    uint64_t cardSize = (0.512 * sd.card()->cardSize()) / 1024;
    uint64_t freeSpace = (0.512 * sd.vol()->freeClusterCount() * sd.vol()->blocksPerCluster()) / 1024;
    display.printf("SD: %llu/%lluMB\n", cardSize, freeSpace);
  } else {
    display.println("SD NOT available");
  }

  if (rtcState == MODULE_OK) {
    display.print("R:");
    display.println(GetTimeString());
  } else {
    display.printf("RTC NOT Running\n");
  }

  display.print("S:");
  display.println(GetSysTimeString());


  //  switch(sntp_get_sync_status()){
  //    case SNTP_SYNC_STATUS_RESET:
  //      display.println("SNTP not active");
  //      break;
  //    case SNTP_SYNC_STATUS_COMPLETED:
  //      display.println("SNTP synchronized");
  //      break;
  //    case SNTP_SYNC_STATUS_IN_PROGRESS:
  //      display.println("SNTP in progress");
  //      break;
  //    default:
  //      display.printf("SNTP unknown: %d\n",sntp_get_sync_status());
  //      break;
  //  }

  if (WiFi.isConnected()) {
    display.print("SSID:");
    display.println(WiFi.SSID());
    display.print("MAC:");
    display.println(WiFi.macAddress());
    display.print("IP:");
    display.println(WiFi.localIP());
  } else {
    display.println("Wifi NOT available");
    display.print("MAC:");
    display.println(WiFi.macAddress());
  }

  display.display();
}
//=============================================================================

//=============================================================================
// on button tap
void ButtonTap(Button2& btn) {
  Serial.println("ButtonTap");
  if (!screen_saver && !screen_dimmed)
    screen = ++screen % 2;
  time(&last_action_time);
  UpdateDisplay();
}
//=============================================================================


//=============================================================================
//avg humidity accumulation
void AddTempHumidToArray() {

  th_log_array[th_log_idx].temperature = temperature;
  //  Serial.printf("Added idx %d temp %f humid %f\n", th_log_idx, temperature, humidity);
  th_log_array[th_log_idx].humidity = humidity;

  th_log_idx = ++th_log_idx % LOG_SUPERSAMPLE;
}
//=============================================================================

//=============================================================================
//calc avg temperature
float GetAvgTemperature () {
  int count = 0;
  float aggr = 0;
  Serial.print("Avg temp comp: ");
  for (int i = 0; i < LOG_SUPERSAMPLE; i++) {

    if (IsValidReading(th_log_array[i].temperature)) {
      aggr += th_log_array[i].temperature;
      Serial.print(th_log_array[i].temperature);
      Serial.print(" ");
      count++;
    }
  }

  if (count > 0)
  {
    Serial.println(aggr / count);
    return aggr / count;
  }
  else
  {
    Serial.println(NAN);
    return NAN;
  }
}
//=============================================================================

//=============================================================================
// calc avg humidity
float GetAvgHumidity () {
  int count = 0;
  float aggr = 0;
  Serial.print("Avg humid comp: ");
  for (int i = 0; i < LOG_SUPERSAMPLE; i++) {
    if (IsValidReading(th_log_array[i].humidity)) {
      aggr += th_log_array[i].humidity;
      Serial.print(th_log_array[i].humidity);
      Serial.print(" ");

      count++;
    }
  }
  if (count > 0) {
    Serial.println(aggr / count);
    return aggr / count;
  }
  else {
    Serial.println(NAN);
    return NAN;
  }
}
//=============================================================================

//=============================================================================
//true if given value is valid sensor reading
bool IsValidReading(float reading) {
  return !isnan(reading); // library constant for error
}
//=============================================================================



//=============================================================================
//=============================================================================
// MAIN LOOP
//=============================================================================
//=============================================================================

void loop() {
  button.loop();
  if (dispTempTimer.isReady()) {
    RefreshTemp();
    dispTempTimer.reset();
  }

  if (tempTimer.isReady()) {
    AddTempHumidToArray();
    tempTimer.reset();
  }

  if (tempLogTimer.isReady()) {
    tempLogTimer.reset();
    WriteReadingsToSD();
  }
  if (dispTimer.isReady()) {
    UpdateDisplay();
    dispTimer.reset();
  }

}











//=============================================================================
//dump system info
void PrintSysInfo2Serial() {
  if ( sdState == MODULE_OK) {
    uint8_t cardType = sd.card()->type();

    Serial.print("SD Card Type: ");
    if (cardType == SD_CARD_TYPE_SD1) {
      Serial.println("SD1");
    } else if (cardType == SD_CARD_TYPE_SD2) {
      Serial.println("SD2");
    } else if (cardType == SD_CARD_TYPE_SDHC) {
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

  if (rtcState == MODULE_OK) {
    Serial.print("RTC Time: ");
    Serial.println(GetTimeString());
  } else {
    Serial.printf("RTC NOT Running\n");
  }

  if (WiFi.isConnected()) {
    Serial.print("SSID:");
    Serial.println(WiFi.SSID());
    Serial.print("MAC:");
    Serial.println(WiFi.macAddress());
    Serial.print("IP:");
    Serial.println(WiFi.localIP());

  } else {
    Serial.println("Wifi NOT available");
    Serial.print("MAC:");
    Serial.println(WiFi.macAddress());
  }

}
//=============================================================================
