#ifdef ARDUINO_ARCH_ESP32
  #include <WiFi.h>
  #define ESP_RESET ESP.restart()
#else
  #include <ESP8266WiFi.h>
  #define ESP_RESET ESP.reset()
#endif

#include <Arduino.h>
#include <arduino_homekit_server.h>
#include <WS2812FX.h>
#include <time.h>

#define LOG_D(fmt, ...)   printf_P(PSTR(fmt "\n") , ##__VA_ARGS__);

bool received_sat = false;
bool received_hue = false;

bool is_on = false;
float current_brightness =  100.0;
float current_sat = 0.0;
float current_hue = 0.0;
int rgb_colors[3];

extern const char index_html[];
extern const char main_js[];

//#define WIFI_SSID ""
//#define WIFI_PASSWORD ""
#include "secrets.h"

//#define STATIC_IP                       // uncomment for static IP, set IP below
#ifdef STATIC_IP
  IPAddress ip(192,168,105,195);
  IPAddress gateway(192,168,0,1);
  IPAddress subnet(255,255,255,0);
#endif

// QUICKFIX...See https://github.com/esp8266/Arduino/issues/263
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

#define LED_PIN 14                       // 0 = GPIO0, 2=GPIO2
#define INT_LED_PIN 2
#define LED_COUNT 59

int IntLedState = HIGH;
int led_off_hour = 22;
int led_on_hour = 9;

#define MYTZ           "EET-2EEST,M3.5.0/3,M10.5.0/4"
time_t now;
tm tm;

#define WIFI_TIMEOUT 300000  // checks WiFi every ...ms. Reset after this time, if WiFi cannot reconnect.

unsigned long auto_last_change = 0;
unsigned long last_wifi_check_time = 0;
String modes = "";
uint8_t myModes[] = {FX_MODE_RAINBOW_CYCLE, FX_MODE_RAINBOW}; // *** optionally create a custom list of effect/mode numbers
bool auto_cycle = false;

WS2812FX ws2812fx = WS2812FX(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup(){
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\nStarting...");

  configTime(MYTZ, "europe.pool.ntp.org", "pool.ntp.org", "time.nist.gov");

  modes.reserve(5000);
  modes_setup();

  Serial.println("WS2812FX setup");
  ws2812fx.init();
  ws2812fx.setMode(FX_MODE_RAINBOW_CYCLE);
  ws2812fx.setColor(0xFF5900);
  ws2812fx.setSpeed(600);
  ws2812fx.setBrightness(30);
  ws2812fx.start();

  pinMode(INT_LED_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(INT_LED_PIN, IntLedState);
  //digitalWrite(LED_PIN, IntLedState);

  Serial.println("Wifi setup");
  wifi_setup();
  
  my_homekit_setup();

  Serial.println("ready!");
}


//==============================
// HomeKit setup and loop
//==============================

// access your HomeKit characteristics defined in my_accessory.c

extern "C" homekit_server_config_t accessory_config;
extern "C" homekit_characteristic_t cha_on;
extern "C" homekit_characteristic_t cha_bright;
extern "C" homekit_characteristic_t cha_sat;
extern "C" homekit_characteristic_t cha_hue;

static uint32_t next_heap_millis = 0;

void my_homekit_setup() {

  cha_on.setter = set_on;
  cha_bright.setter = set_bright;
  cha_sat.setter = set_sat;
  cha_hue.setter = set_hue;
  
  arduino_homekit_setup(&accessory_config);

}

void set_on(const homekit_value_t v) {
    bool on = v.bool_value;
    cha_on.value.bool_value = on; //sync the value

    if(on) {
        is_on = true;
        Serial.println("On");
    } else  {
        is_on = false;
        Serial.println("Off");
    }

    updateColor();
}

void set_hue(const homekit_value_t v) {
    Serial.println("set_hue");
    float hue = v.float_value;
    cha_hue.value.float_value = hue; //sync the value

    current_hue = hue;
    received_hue = true;
    
    updateColor();
}

void set_sat(const homekit_value_t v) {
    Serial.println("set_sat");
    float sat = v.float_value;
    cha_sat.value.float_value = sat; //sync the value

    current_sat = sat;
    received_sat = true;
    
    updateColor();

}

void set_bright(const homekit_value_t v) {
    Serial.println("set_bright");
    int bright = v.int_value;
    cha_bright.value.int_value = bright; //sync the value

    current_brightness = bright;

    updateColor();
}

void updateColor()
{
  if(is_on)
  {
	  ws2812fx.setSpeed(0);
   
      if(received_hue && received_sat)
      {
        //HSV2RGB(current_hue, current_sat, current_brightness);
		
        uint32_t tmp = (uint32_t) current_hue;
        if(tmp <= 0xFFFFFF) {
          ws2812fx.setColor(tmp);
        }		
        received_hue = false;
        received_sat = false;
      }
      
      int b = map(current_brightness,0, 100,75, 255);
	  
	  uint8_t tmp = (uint8_t) b;
      ws2812fx.setBrightness(tmp);

      /*uint8_t tmp = (uint8_t) strtol(server.arg(i).c_str(), NULL, 10);
      uint8_t new_mode = sizeof(myModes) > 0 ? myModes[tmp % sizeof(myModes)] : tmp % ws2812fx.getModeCount();
      ws2812fx.setMode(new_mode);
      auto_cycle = false;*/

      /*uint16_t tmp = (uint16_t) strtol(server.arg(i).c_str(), NULL, 10);
      ws2812fx.setSpeed(tmp);*/

    }
  else if(!is_on) //lamp - switch to off
  {
      Serial.println("is_on == false");
	  
	  ws2812fx.setBrightness(0);
      ws2812fx.setColor(0x000000);
  }
}

void my_homekit_loop() {
	arduino_homekit_loop();
	const uint32_t t = millis();
	if (t > next_heap_millis) {
		// show heap info every 5 seconds
		next_heap_millis = t + 5 * 1000;
		LOG_D("Free heap: %d, HomeKit clients: %d",
				ESP.getFreeHeap(), arduino_homekit_connected_clients_count());

	}
	
  unsigned long now = millis();

  if(now - last_wifi_check_time > WIFI_TIMEOUT) {
    Serial.print("Checking WiFi... ");
    if(WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost. Reconnecting...");
      wifi_setup();
    } else {
      Serial.println("OK");
      IntLedState = LOW || time_check();
      digitalWrite(INT_LED_PIN, IntLedState);
    }
    last_wifi_check_time = now;

    /*if(IntLedState = LOW){
      Serial.print("time_check = ");
      Serial.print(IntLedState);
      Serial.println("  ---  Led is ON!");
    } else {
      Serial.print("time_check = ");
      Serial.print(IntLedState);
      Serial.println("  ---  Led is OFF!");
    }*/
  }

  if(auto_cycle && (now - auto_last_change > 10000)) { // cycle effect mode every 10 seconds
    uint8_t next_mode = (ws2812fx.getMode() + 1) % ws2812fx.getModeCount();
    if(sizeof(myModes) > 0) { // if custom list of modes exists
      for(uint8_t i=0; i < sizeof(myModes); i++) {
        if(myModes[i] == ws2812fx.getMode()) {
          next_mode = ((i + 1) < sizeof(myModes)) ? myModes[i + 1] : myModes[0];
          break;
        }
      }
    }
    ws2812fx.setMode(next_mode);
    Serial.print("mode is "); Serial.println(ws2812fx.getModeName(ws2812fx.getMode()));
    auto_last_change = now;
  }
}

void loop() {
  ws2812fx.service();
  my_homekit_loop();
  delay(10);
}


/*
 * Check time. If outside daylight set status to turnoff onboard led = wifi status
 */
int time_check() {
   time(&now);
   localtime_r(&now, &tm);
   int ledstatus;

   if((tm.tm_hour >= led_on_hour) && (tm.tm_hour < led_off_hour)) {
     ledstatus = 0; //LOW
   }      
      
   if((tm.tm_hour < led_on_hour) || (tm.tm_hour >= led_off_hour)) {
     ledstatus = 1; //HIGH 
   }
   
   return ledstatus;
}


/*
 * Connect to WiFi. If no connection is made within WIFI_TIMEOUT, ESP gets resettet.
 */
void wifi_setup() {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.mode(WIFI_STA);
  #ifdef STATIC_IP  
    WiFi.config(ip, gateway, subnet);
  #endif

  unsigned long connect_start = millis();
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    IntLedState = HIGH;
    digitalWrite(INT_LED_PIN, IntLedState);

    if(millis() - connect_start > WIFI_TIMEOUT) {
      Serial.println();
      Serial.print("Tried ");
      Serial.print(WIFI_TIMEOUT);
      Serial.print("ms. Resetting ESP now.");
      ESP_RESET;
    }
  }

  Serial.println("");
  Serial.println("WiFi connected");  
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  IntLedState = LOW || time_check();
  digitalWrite(INT_LED_PIN, IntLedState);
}


/*
 * Build <li> string for all modes.
 */
void modes_setup() {
  modes = "";
  uint8_t num_modes = sizeof(myModes) > 0 ? sizeof(myModes) : ws2812fx.getModeCount();
  for(uint8_t i=0; i < num_modes; i++) {
    uint8_t m = sizeof(myModes) > 0 ? myModes[i] : i;
    modes += "<li><a href='#'>";
    modes += ws2812fx.getModeName(m);
    modes += "</a></li>";
  }
}
