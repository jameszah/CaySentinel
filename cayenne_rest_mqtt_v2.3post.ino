
/*
  Cayenne Sentinel (simpler version)
  
  ESP 8266 REST API Demo - Send and Receive data to Cayenne with REST API

  When rebooting, this 8266 program goes to Cayenne mydevices.com to determine the last time
  the device was running, and total reboots for this 8266, then updates cayenne with how long device was down,
  and the total number of reboots.
  It also fetches the current value of the "machine state" a "2-State" and "sine frequency" a "slider".
  In this example the machine is a sine wave generator, which can be on or off, and will generate
  one sine wave with a period of 1 hour, and another with a period of 1 to 6 per hour.

  The main loop undates sine waves every 15 seconds, if the machine is on.
  And once per minute send the time through REST and MQTT to cayenne, and then fetches the previous REST and MQTT time
  and calculates how long it took after the time was sent for it to be timestamped in the cayenne database.
   
  These lag times are sent to cayenne via mqtt.
  
  This means I send the time at 2:10:11, then one minute later and I query cayenne which tells me the 
  timestamp on that data was 2:10:13 seconds, then I calculate the lag time is 2 seconds and I store that in cayenne database.

  My observstions on my internet are that mqtt updates timestamped about 1 seconds later, and rest updates about 4 seconds later.
  
  Since the REST update command is echoed back to the ESP, this roundtrip time is calulated and stored in cayenne with mqtt (about 3 seconds).
  
  A binary command "button" can turn the machine ON/OFF, a slider controls the sine wave 1-6 cycles per minute, and a command
  can reboot the ESP.
  
  The Cayenne MQTT side needs the deviceID from Cayenne, and mqtt user/password.  All in the form "ea767930-5b0f-11e8-9907-5defa4aa8de2"
  
  The Cayenne REST needs a current refresh_token (about 40 chars of hex) and a deviceID from Cayenne.
  The refresh token is generated using your Cayenne username and password (MrPeanut@gmail.com and peanutpassword),
  and then you paste that refresh token into the code and write it to the ESP, which avoids the problem or storing your
  username and password in the code.

  The REST reads and writes also need sensor_ID (of the form "ea767930-5b0f-11e8-9907-5defa4aa8de2"), which is not as as elegant
  as the MQTT write, which replace that long number with a simple channel number (1 .. 99).

  The OAuth access token is generated from the refresh token when the ESP boots and once per day, and the updated refresh
  token is stored in EPROM, so it should run forever without needing the cayenne username/password.  The access_token expires
  in a week or two, and the refresh_token seems to expire as well, but they send a new refresh token which is stored in EPROM,
  so the the original refresh_token in the code is only every used once.
  
  The original Cayenne access_token and refresh token can be generated with the following curl command using your Cayenne
  username and password.  Using the token means username/password don't need to be hardcoded into code.

  curl -X POST https://auth.mydevices.com/oauth/token \
   -H 'cache-control: no-cache' \
   -H 'content-type: multipart/form-data''' \
   -F grant_type=password \
   -F email=mrpeanut@gmail.com \
   -F password=peanutpassword

   Since Windows does not come with curl, and editing a 200 char command line can be a nuisance, you can use the convenient
   spreadsheet to do it:

    https://github.com/jameszah/CayExcel


  James Zahary,  May 17, 2018
  - simpler version Aug 10, 2018

*/

/*
 * setup()
 *   do_eprom_read()    - read refresh token from eprom
 *   do_access_refresh() - fetch access token
 *     do_eprom_write() - write access and refresh token 
 *   reset_cayenne()    - read the starting conditions
 *   
 * loop()
 *   do_mqtt_read_send()       - query last time send, then send time with MQTT
 *     rest_query_cayenne()
 *     cayenne.virtualwrite()
 *   do_rest_read_send()       - query last time send, then send time with REST
 *     rest_query_cayenne()
 *     rest_update_cayenne()
 *   do_sine()                 - update the sine waves
 *   do_access_refresh()
 *   
 * cayenne_in_default()  
 *   
 * cayenne_out_default()
 * 
 * Channel Number (mqtt update) - Sensor Name (rest update/query) - description and frequency
 * 
 * 2 - caytime - unixtime through MQTT (seconds) - once per minute write and read
 * 3 - cayresttime - unixtime through REST (seconds) - once per minute write and read
 * 
 * 11 - downtime before reboot (seconds) - once per boot write
 * 12 - cayreboots - total reboots for ESP (count) - once per boot write and read
 * 
 * 24 - lag in cayenne REST update (seconds) - once per minute write
 * 25 - lag in cayenne MQTT update (seconds) - once per minute write
 * 26 - time for roundtrip for rest update (ms) - once per minute write
 * 
 * 30 - sine wave 1-6 cycles per hour - 4 times per minute write (0 if not enabled)
 * 31 - sine wave 1 cycles per hour - 4 times per minute write (0 if not enabled)
 * 
 * 50 - software version number and date - once per boot write
 * 51 - last refresh token refresh is unixtime - write once per day
 * 
 * 94 - binary switch "button" - turn sine wave machine ON/OFF
 * 95 - caycycles - "slider" - cycles per hours for sine wave 1-6
 * 96 - command "button" to update EPROM
 * 98 - caymachine - binary "2 state" - state of machine ON/OFF
 * 99 - command "button" to reboot ESP
 * 
 */

#include <ESP8266WiFi.h>
#include <CayenneMQTTESP8266.h>
#include <time.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

struct eprom_data {
  char at[700];      // currect access token   - not used
  char rt[100];      // current refresh token  
  char ssid[20];       // wifi name            - not used
  char ssid_pass[20];  // wifi pawword         - not used
  long lastreboot;     // unixtime             
};

long lastreboot = -1;

struct tm * timeinfo;
time_t now;
long sendRESTmillis;

long sendMQTTseconds = -1;
long sendRESTseconds = -1;

long counter = 0;
int cycles = 1;
int machine = -1;

int daily_reset = 2;

double offset;
long rt_offset = 0;

// Wi-Fi Settings

const  char ssid[] = "peanut";           // your wireless network name (SSID)
const  char wifipassword[] = "mrpeanut";  // your Wi-Fi network password

const int postingInterval = 15 * 1000;      // 15 seconds

long fifteen = 0;

// Cayenne Settings

char access_token[600] = "";
char refresh_token[100] = "2068e4f71ed59899peanut3e2c08a9f8e5b140d4";

const char cayusername[] = "971da7b0-fd21-11e6-ac86-a9apeanutcce";
const char caypassword[] = "257f5a90db0155038259970431cpeanutec26194";

const  char cayclientID[] = "ea767930-5b0f-11e8-9907-5defa4aa8de2";   // the device id
const  char caytime[] =     "6c6d96b0-6a1c-11e8-8e48-275f329dc9d5";   // all the sensor ids
const  char cayresttime[] = "19c2db40-6a18-11e8-b57e-cb1f6765c587";
const  char cayreboots[] =  "9ed81900-5b25-11e8-827b-4f89eb065ec7";
const  char caymachine[] =  "0c9b8ed0-5b2c-11e8-b3a0-856fee219c98";
const  char caycycles[] =   "a59b1650-6810-11e8-a25e-d5e347246797";

const  char cayaddr[] =     "mqtt.mydevices.com";
const  char cayserver[] =   "platform.mydevices.com";
const char cayauthserver[] = "auth.mydevices.com";


int cayport = 1883;
int cayREST = 443;

long lastMillis = 0;

int channel = 0;
double v = 0;
String ts = "";
String unit = "";
String type = "";
String device_type = "";

void setup() {
  Serial.begin(115200);
  Serial.println("");
  Serial.println("");
  Serial.println("cayenne_rest_mqtt_v2.3  Aug 11, 2018 -jz");
  Serial.println("Free Heap " + String(ESP.getFreeHeap()));

  //Serial.println("Flash chip size " + String(ESP.getFlashChipSize()));
  //Serial.setDebugOutput(true);
  //WiFi.printDiag(Serial);

  Serial.println(">>>>> Starting Cayenne ...");
  Cayenne.begin( cayusername, caypassword, cayclientID, ssid, wifipassword);

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("\nWaiting for Internet time...");
  while (!time(nullptr)) {
    Serial.print(".");
    delay(1000);
  }

  Serial.println("");
  now = time(nullptr);
  Serial.println(ctime(&now));

  do_eprom_read();
  
  
  Serial.println("<<<<< Update OAuth2 token");
  do_access_refresh();
  
  Cayenne.virtualWrite(50, 2.3 , "analog_sensor", "Aug112018");

  reset_cayenne();
}

void loop() {

  now = time(nullptr);

  //time_t now = time(nullptr);
  //Serial.print(ctime(&now));
  timeinfo = localtime (&now);
  
  //float timefloat = timeinfo->tm_hour*100 + timeinfo->tm_min + timeinfo->tm_sec/100.0;
  //Serial.print(timeinfo->tm_hour); Serial.print(":"); Serial.println(timeinfo->tm_min);

  //
  // at 6 AM GMT or Midnight MST, get a fresh access token and refresh token
  // - this did more in the longer version of this program
  //
  
  if (timeinfo->tm_hour == 6 && timeinfo->tm_min == 0) {
    if (daily_reset == 2) {
      daily_reset = 0;
      do_access_refresh();
    } 
  }
  if (timeinfo->tm_hour == 6 && timeinfo->tm_min == 10) {
    daily_reset = 2;
  }

  Cayenne.loop();

  if (millis() - lastMillis > postingInterval) {  // every 15 seconds, run this
    
    lastMillis = millis();
    
    fifteen = fifteen + 1;
    
    if (fifteen % 4 == 1) {                      // every 1 minute, run this
      do_mqtt_read_send();
      do_rest_read_send();
    }

    if (machine == 1) {                          // if sine wave machine is turned ON
      do_sine();
    }
  }
}

CAYENNE_OUT_DEFAULT() {
  counter = counter + 1;                         // not used
}


CAYENNE_IN_DEFAULT()
{

  int chan = request.channel;
  int id = getValue.asInt();
  String stringid = getValue.asString();
  long nowMillis = millis();

  if (chan == 3) {
    rt_offset = nowMillis - sendRESTmillis;

    Serial.println("<<<<< rest time Send has arrived " + String (rt_offset) + " MilliSeconds");
    Serial.println("");
    
    Cayenne.virtualWrite(26, rt_offset, "analog_sensor", "ms");

  } else {

    Serial.println("");
    Serial.println("<<<<< cayenne command chan/id " + String(chan) + "/" + String(id));
    Serial.println("");

    if (chan == 94) {
      machine = id;
    
      if (machine == 0) {
        Serial.print("Machine turned OFF, = "); Serial.println(machine);
        Cayenne.virtualWrite(98, machine , "digital_sensor", "Machine OFF");
      } else if (machine == 1) {
        Serial.print("Machine turned ON, = "); Serial.println(machine);
        Cayenne.virtualWrite(98, machine , "digital_sensor", "Machine ON");
      } else {
        Serial.print("Machine is confused, = "); Serial.println(machine);
      }
    }

    if (chan == 95) {
      cycles = id;
      Serial.println("Cyles per hour changed to " + String(cycles));
    }

    if (chan == 96) {          // update the access token
      Serial.println("Refresh OAuth2 token - should last for 2 weeks ???");
      do_access_refresh();
    }

    if (chan == 99) {  // reboot
      Serial.println("Rebooting in 3 seconds");
      delay(3000);
      ESP.restart();
    }
  }
}

void reset_cayenne() {

  Serial.println("Fetching parameters from cayenne about last known state ...");

  //
  // get the unixtime of the last time update - last time device was running and connected
  //
  
  if (rest_query_cayenne(cayclientID,  caytime) == 1) {

    Serial.println("");  Serial.println("");
    Serial.println("Query cayenne for last running time ");       // print all details for first example
    Serial.println("   Device: " + String(cayclientID));
    Serial.println("   Sensor: " + String(caytime));
    Serial.println("Results from cayenne ");
    Serial.println("      v = " + String(v));
    Serial.println("     ts = " + String(ts));
    Serial.println(" device = " + String(device_type));
    Serial.println("   unit = " + String(unit));
    Serial.println("");

    now = time(nullptr);
    Serial.println("8266 was down for " + String(now - v) + " seconds or " + String ((now - v) / 3600) + " hours");
    Serial.println("");

    Cayenne.virtualWrite(11, now - v, "analog_sensor", "seconds");
  } else {
    Serial.println("Couldn't get lastboot");
  }
  
  //
  // get the number of reboots of this cpu
  //
  
  if (rest_query_cayenne(cayclientID,  cayreboots) == 1) {

    Serial.print("Total reboots = "); Serial.println(v);

    v = v + 1;
  } else {
    Serial.println("Hmmm ... first boot of this device!");
    v = 0;
  }
  Cayenne.virtualWrite(12, v , "analog_sensor", "count");
  
  //
  // get the state of the machine 0 or 1
  //
  
  if (rest_query_cayenne(  cayclientID,  caymachine) == 1) {

    if (v == 0) {
      Serial.print("Machine is OFF, = "); Serial.println(v);
    } else if (v == 1) {
      Serial.print("Machine is ON, = "); Serial.println(v);
    } else {
      Serial.print("Machine is confused, = "); Serial.println(v);
    }

    machine = v;

  } else {
    Serial.println("Couldn't get machine state");
    v = 1;
  }

  Cayenne.virtualWrite(98, v , "digital_sensor", "Machine ON");
  
  //
  // get the cycles slider 1 .. 6
  //
  
  if (rest_query_cayenne(  cayclientID,  caycycles) == 1) {

    Serial.print("cycles is = "); Serial.println(v);

    cycles = v;
  } else {
    Serial.println("Couldn't get cycles");
    cycles = 6;
  }

  Serial.println("");
}


void do_sine() {
  if (cycles > 0 && cycles < 7) {
    now = time(nullptr);

    float sinnum = 10 * sin ((now % (600 * (7 - cycles)) ) / (600.0 * (7 - cycles)) * 2 * 3.141592654);
    float sinnum1hr = 10 * sin( ( now % 3600 ) / 3600.0  * 2 * 3.141592654);

    //Serial.println("Sine write mqtt "  + String(sinnum) + "  " + String (sinnum1hr));

    Cayenne.virtualWrite(30, sinnum , "analog_sensor", "sintime");
    Cayenne.virtualWrite(31, sinnum1hr , "analog_sensor", "sintime");
  }
}

void do_rest_read_send() {

  if (rest_query_cayenne( cayclientID,   cayresttime) == 1) {

    // v is data returned from cayenne
    // ts is the timestamp in cayenne database
    // sendRESTseconds was the time we sent to cayenne 1 minute ago 
    //        - should be the same as v
    //        - should be 2-3 seconds lower then ts
    
    now = time(nullptr);
    double sec_last = sendRESTseconds % ( 3600 * 24) ;
    double sec_ts = ts.substring(11, 13).toInt() * 3600 + ts.substring(14, 16).toInt() * 60 + ts.substring(17, 19).toInt() + ts.substring(20, 22).toInt() / 100.0;
    
    if (sendRESTseconds != -1) {

      offset = sec_last - sec_ts ;         // should be -2 seconds or so
      if (offset > 50000) {                // we have moved to next day 
        offset = offset - 24 * 3600;
      }

      if (offset > 0) {                    // if offset > 0 then we time in db is before time sent, so it is previous minute or more
        offset = -offset;                  // will display this as 30 or 60 or 200 seconds ago, was last data
      }
      
      if (sendRESTseconds != v) {          // this is same situation as offset > 0 above
        
            Serial.println("Last REST send is not in cayenne database!" );    
            Serial.print("  Last send =" ); Serial.println(sendRESTseconds);
            Serial.print("  Last db   =" ); Serial.println(v);
            Serial.print("  offset    =" ); Serial.println(offset);

      }

      Serial.println("<<<<< REST Time Lag " + String(offset)  );
      Cayenne.virtualWrite(24, offset , "analog_sensor", "seconds");
    }
  } else {
    Serial.println("Couldn't get last REST time");
  }

  // write unixtime to channel 3

  sendRESTmillis = millis();
  now = time(nullptr);
  sendRESTseconds = now;

  int i = rest_update_cayenne(3, double(sendRESTseconds), "analog_sensor", "seconds");

  Serial.println("     >>>>> REST Time Write " + String(sendRESTseconds));
}

void do_mqtt_read_send() {

  if (rest_query_cayenne (cayclientID,  caytime) == 1) {

    // v is data returned from cayenne
    // ts is the timestamp is cayenne database
    // sendRESTseconds was the time we sent to cayenne 1 minute ago - should be the same as v
    
    now = time(nullptr);

    double sec_last = sendMQTTseconds % ( 3600 * 24) ;
    double sec_ts = ts.substring(11, 13).toInt() * 3600 + ts.substring(14, 16).toInt() * 60 + ts.substring(17, 19).toInt() + ts.substring(20, 22).toInt() / 100.0;

///
    if (sendMQTTseconds != -1) {

      offset = sec_last - sec_ts ;         // should be -2 seconds or so
      if (offset > 50000) {                // we have moved to next day 
        offset = offset - 24 * 3600;
      }

      if (offset > 0) {                    // if offset > 0 then we time in db is before time sent, so it is previous minute or more
        offset = -offset;                  // will display this as 30 or 60 or 200 seconds ago, was last data
      }
      
      if (sendMQTTseconds != v) {          // this is same situation as offset > 0 above
        
            Serial.println("Last MQTT send is not in cayenne database!" );    
            Serial.print("  Last send =" ); Serial.println(sendMQTTseconds);
            Serial.print("  Last db   =" ); Serial.println(v);
            Serial.print("  offset    =" ); Serial.println(offset);
 
      }
      
      Serial.println("<<<<< MQTT Time Lag " + String(offset) );
      Cayenne.virtualWrite(25, offset , "analog_sensor", "seconds");
    }
  } else {
    Serial.println("Couldn't get last MQTT time");
  }

///
    
  now = time(nullptr);
  sendMQTTseconds = now;
  Serial.println("     >>>>> MQTT Time Write " + String(sendMQTTseconds));
  Cayenne.virtualWrite(2, sendMQTTseconds, "analog_sensor", "seconds");


}

/*
 * REST_QUERY_CAYENNE - query the cayenne database
 * 
 * rest_query_cayenne ( device, sensor )
 *   rest_query_cayenne2 ( device, sensor )
 *     returns global variables
 *       v, ts, device_type, unit
 *       {"v":"1526700604","ts":"2018-05-19T03:30:05.283Z","unit":"seconds","device_type":"analog"}
 */

int rest_query_cayenne(const char caydev[], const char caysen[]) {

  if (rest_query_cayenne2(caydev, caysen) == 1) {
    return 1;

  } else if (rest_query_cayenne2(caydev, caysen) == 1) {
    Serial.println("");
    Serial.println(" FIRST RETRY SUCCESSFUL");
    return 1;

  } else {
    Serial.println("");
    Serial.println(" SECOND RETRY FAILED -- GIVING UP");
    return -1;
  }
}

int rest_query_cayenne2(const char caydev[], const char caysen[]) {

  // results   v, ts, device, unit

  WiFiClientSecure cayclient;

  int ret = 0;

  if (!cayclient.connect(cayserver, 443)) {
    Serial.println("Cayserver failed to connect");
  } else {
    String get = "GET /v1.1/telemetry/" + String(caydev) + "/sensors/" + String(caysen) + "/summaries?endDateLatest=true&type=latest HTTP/1.1";
    cayclient.println(get);
    cayclient.println("Host: platform.mydevices.com");
    cayclient.println("User-Agent: ESP8266 (JZ)/1.0");
    cayclient.println("authorization: Bearer " + String(access_token));
    cayclient.println("");
    cayclient.print("");

    unsigned long timeout = millis();
    while (cayclient.available() == 0) {
      if (millis() - timeout > 15000) {      // 15 second timeout
        Serial.println(">>> Client Timeout !");
        cayclient.stop();
        return -1;
      }
      delay(50);
    }

    while (cayclient.available()) {
      String h1 = cayclient.readStringUntil('HTTP/1.1 ');
      String stat = cayclient.readStringUntil(' ');
      String h3 = cayclient.readStringUntil(': ');
      String date = cayclient.readStringUntil('\r');
      String h5 = cayclient.readStringUntil(': ');
      String type = cayclient.readStringUntil('\r');
      String h8 = cayclient.readStringUntil(': ');
      String leng = cayclient.readStringUntil('\r');
      String h10 = cayclient.readStringUntil('[');
      String json = cayclient.readStringUntil('}]');
      String h12 = cayclient.readStringUntil('\r');

      if (stat.toInt() == 200) {

        //  {"v":"1526700604","ts":"2018-05-19T03:30:05.283Z","unit":"seconds","device_type":"analog"}

        DynamicJsonBuffer  jsonBuffer(400);
        JsonObject& root = jsonBuffer.parseObject(json);

        if (!root.success()) {
          Serial.println("parseObject failed");

          cayclient.stop();
          return -1;
        } else {
          double xv  = root["v"];
          String xts = root["ts"];
          String xdevice_type = root["device_type"];
          String xunit = root["unit"];

          jsonBuffer.clear();
          cayclient.stop();

          v = xv;
          ts = xts;
          device_type = xdevice_type;
          unit = xunit;
          return 1;
        }
      } else {
        Serial.println("status=" + String(stat) + "=");
        Serial.println("date=" + String(date) + "=");
        Serial.println("type=" + String(type) + "=");
        Serial.println("length=" + String(leng) + "=");
        Serial.println("json=" + String(json) + "=");
        return -1;
      }
    }
  }
}

/*
 * REST_UPDATE_CAYENNE - update the cayenne database using REST, rather than MQTT
 *                     - REST update will send a confirmation to CAYENNE_IN_DEFAULT()
 *                     
 * rest_update_cayenne ( channel, v, type, unit)
 *   rest_update_cayenne2 ( channel, v, type, unit )
 *     - REST version of the virtualWrite command   
 *         Cayenne.virtualWrite(2, 25.2, "temp", "c")
 */

int rest_update_cayenne(int channel, double vv, char type[], char unit[]) {

  if (rest_update_cayenne2( channel, vv, type, unit) == 1) {
    return 1;

  } else if (rest_update_cayenne2( channel, vv, type, unit) == 1) {
    Serial.println("FIRST RETRY SUCCESSFUL");
    return 1;

  } else {
    Serial.println("SECOND RETRY FAILED -- GIVING UP");
    return -1;
  }
}

int rest_update_cayenne2(int channel, double vvv, char type[], char unit[]) {

  // parameters cayclientID and channel, with v,type,unit

  WiFiClientSecure cayclient;

  if (cayclient.connect(cayserver, 443)) {

    // {“channel”: 100,“value”: 27,“type”: “temp”,“unit”: “c”}

    String body1 = "{\"channel\":";
    body1 += String(channel);
    body1 += ",\"value\":";
    body1 += String(vvv);
    body1 += ",\"Type\":\"";
    body1 += type;
    body1 += "\",\"Unit\":\"";
    body1 += unit;
    body1 += "\"}";
    
    String post = "POST /v1.1/things/" + String(cayclientID) + "/cmd HTTP/1.1";
    cayclient.println(post);
    cayclient.println("Host: platform.mydevices.com");
    cayclient.println("User-Agent: ESP8266 (JZ)/1.0");
    cayclient.println("Connection: close");
    cayclient.println("authorization: Bearer " + String(access_token));
    cayclient.println("content-type: application/json; charset=UTF-8");
    cayclient.println("Content-Length: " + String(body1.length()));
    cayclient.println("");
    cayclient.print(body1);

    unsigned long timeout = millis();
    while (cayclient.available() == 0) {
      if (millis() - timeout > 5000) {
        Serial.println(">>> Client Timeout !");
        cayclient.stop();
        return -1;
      }
      delay(50);
    }

    while (cayclient.available()) {

      String h1 = cayclient.readStringUntil('HTTP/1.1 ');
      String stat = cayclient.readStringUntil(' ');
      String h3 = cayclient.readStringUntil(': ');
      String date = cayclient.readStringUntil('\r');
      String h5 = cayclient.readStringUntil(': ');
      String type = cayclient.readStringUntil('\r');
      String h8 = cayclient.readStringUntil(': ');
      String leng = cayclient.readStringUntil('\r');
      String h10 = cayclient.readStringUntil('{');
      String json = "{" + cayclient.readStringUntil('}') + "}";
      String h12 = cayclient.readStringUntil('\r');

      if (stat.toInt() == 200) {
        return 1;
      } else {
        Serial.println("status=" + String(stat) + "=");
        Serial.println("date=" + String(date) + "=");
        Serial.println("type=" + String(type) + "=");
        Serial.println("length=" + String(leng) + "=");
        Serial.println("json=" + String(json) + "=");
      }
      return 1;
    }
  } else {
    Serial.println("Cayserver failed to connect");
    return -1;
  }
  cayclient.stop();
  return -1;
}

/*
 * do_access_refresh
 *   - fetch OAuth2 access_token using the refresh_token, 
 *     then store the new refresh_token in the EPROM
 */
void do_access_refresh() {

  WiFiClientSecure cayclient;

  if (cayclient.connect(cayauthserver, 443)) {

    cayclient.println("POST /oauth/token HTTP/1.1");
    cayclient.println("Host: auth.mydevices.com");
    cayclient.println("cache-control: no-cache");
    cayclient.println("Content-Type: application/x-www-form-urlencoded");

    String body = "grant_type=refresh_token&refresh_token=" + String(refresh_token);
    cayclient.println("Content-Length: " + String(body.length()));
    cayclient.println("");
    cayclient.print(body);
    cayclient.println("");

    unsigned long timeout = millis();
    while (cayclient.available() == 0) {
      if (millis() - timeout > 15000) {
        Serial.println(">>> Client Timeout !");
        cayclient.stop();
        return;
      }
      Serial.print(".");
      delay(50);
    }
    Serial.println("");
   
    while (cayclient.available()) {
      String h1 = cayclient.readStringUntil('HTTP/1.1 ');
      String stat = cayclient.readStringUntil(' ');
      String h3 = cayclient.readStringUntil(': ');
      String date = cayclient.readStringUntil('\r');
      String h5 = cayclient.readStringUntil(': ');
      String type = cayclient.readStringUntil('\r');
      String h8 = cayclient.readStringUntil(': ');
      String leng = cayclient.readStringUntil('\r');
      String h10 = cayclient.readStringUntil('{');
      String json = "{" + cayclient.readStringUntil('}') + "}";
      String h12 = cayclient.readStringUntil('\r');

      if (stat.toInt() == 200) {

        DynamicJsonBuffer  jsonBuffer(800);
        JsonObject& root = jsonBuffer.parseObject(json);
        if (!root.success()) {
          Serial.println("parseObject failed");
          
          return ;
        } else {

          String at = root["access_token"];
          String rt = root["refresh_token"];
          Serial.println("at=" + at);
          Serial.println("rt=" + rt);

          at.toCharArray(access_token, 600);
          rt.toCharArray(refresh_token, 100);
        }

        lastreboot = time(nullptr);
        Serial.print("Refreshed access_token and refresh_token at "); Serial.println(lastreboot);
        Cayenne.virtualWrite(51, lastreboot , "analog_sensor", "unixtime");
        
        do_eprom_write();
        return ;
      } else {
        Serial.println("status=" + String(stat) + "=");
        Serial.println("date=" + String(date) + "=");
        Serial.println("type=" + String(type) + "=");
        Serial.println("length=" + String(leng) + "=");
        Serial.println("json=" + String(json) + "=");
      }
      return ;

    }
  } else {
    Serial.println("Cayserver failed to connect");
  }
  cayclient.stop();
}

void do_eprom_read() {

  eprom_data my_eprom_data;

  long x = millis();
  EEPROM.begin(2000);
  EEPROM.get(0, my_eprom_data);
  
  Serial.println("Get took " + String(millis() - x));
  Serial.println("Read from EEPROM: ");
  //Serial.println(my_eprom_data.at);
  Serial.println(my_eprom_data.rt );
  Serial.println(my_eprom_data.lastreboot );

  // is last reboot from eprom between dec 2017 and aug 2030 ... actually last eprom update
  // if so, use eprom refresh_token, rather than hardcoded refresh_token
  
  if (my_eprom_data.lastreboot > 1512345678 && my_eprom_data.lastreboot < 1912345678){
      //strncpy (access_token, my_eprom_data.at, 700);
      strncpy (refresh_token, my_eprom_data.rt, 100);   
  }
}

void do_eprom_write() {

  eprom_data my_eprom_data;

  //strncpy (my_eprom_data.at, access_token, 700);
  strncpy (my_eprom_data.rt, refresh_token, 100);
  
  my_eprom_data.lastreboot = lastreboot;

  Serial.println("Writing to EPROM ...");
  //Serial.println(my_eprom_data.at);
  Serial.println(my_eprom_data.rt );
  Serial.println(my_eprom_data.lastreboot );

  long x = millis();
  EEPROM.begin(2000);
  EEPROM.put(0, my_eprom_data);
  EEPROM.commit();
  EEPROM.end();

  Serial.println("Put took " + String(millis() - x) + " ms, bytes = " + String(sizeof(my_eprom_data)));

}
