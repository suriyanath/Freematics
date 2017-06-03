/******************************************************************************
* Reference sketch for a vehicle data feed for Freematics Hub
* Works with Freematics ONE with ESP8266 WIFI module
* Developed by Stanley Huang https://www.facebook.com/stanleyhuangyc
* Distributed under BSD license
* Visit http://freematics.com/hub/api for Freematics Hub API reference
* Visit http://freematics.com/products/freematics-one for hardware information
* To obtain your Freematics Hub server key, contact support@freematics.com.au
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
******************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <FreematicsONE.h>
#include "config.h"
#include "datalogger.h"

// logger states
#define STATE_SD_READY 0x1
#define STATE_OBD_READY 0x2
#define STATE_GPS_READY 0x4
#define STATE_MEMS_READY 0x8
#define STATE_NET_READY 0x10

uint32_t nextConnTime = 0;
byte accCount = 0; // count of accelerometer readings
long accSum[3] = {0}; // sum of accelerometer data
int accCal[3] = {0}; // calibrated reference accelerometer data
byte deviceTemp = 0; // device temperature
int lastSpeed = 0;
uint32_t lastSpeedTime = 0;
uint32_t distance = 0;

char udpIP[16] = "172.104.40.203";
uint16_t udpPort = SERVER_PORT;

class COBDWIFI : public COBDSPI {
public:
    COBDWIFI() { buffer[0] = 0; }
    void netReset()
    {
      netSendCommand("AT+RST\r\n");
    }
    void netDisconnect()
    {
      netSendCommand("AT+CWQAP\r\n");
    }
    bool netInit()
    {
      // set xBee module serial baudrate
      bool success = false;
      // test the module by issuing AT command and confirming response of "OK"
      for (byte n = 0; !(success = netSendCommand("ATE0\n")) && n < 10; n++) {
        sleep(100);
      }
      if (success) {
        // send ESP8266 initialization commands
        //netSendCommand("AT+CWMODE=1\r\n", 100);
        netSendCommand("AT+CIPMUX=0\r\n");
        return true;
      } else {
        return false;
      }
    }
    bool netSetup()
    {
      // generate and send AT command for joining AP
      sprintf_P(buffer, PSTR("AT+CWJAP=\"%s\",\"%s\"\r\n"), WIFI_SSID, WIFI_PASSWORD);
      byte ret = netSendCommand(buffer, 10000, "OK");
      if (ret == 1) {
        // get IP address
        if (netSendCommand("AT+CIFSR\r\n", 1000, "OK") && !strstr(buffer, "0.0.0.0")) {
          char *p = strchr(buffer, '\r');
          if (p) *p = 0;
          Serial.println(buffer);
          // output IP address
          return true;
        } else {
          // output error message
          Serial.println(buffer); 
        }
      } else if (ret == 2) {
        Serial.println("Failed to join AP"); 
      }
      return false;
    }
    void udpClose()
    {
      netSendCommand("AT+CIPCLOSE\r\n", 1000, "Unlink");
    }
    bool udpOpen()
    {
      char *ip = queryIP(SERVER_URL);
      if (ip) {
        Serial.print(ip);
        Serial.print(' ');
        strncpy(udpIP, ip, sizeof(udpIP) - 1);
      }
      sprintf_P(buffer, PSTR("AT+CIPSTART=\"UDP\",\"%s\",%d,8000,0\r\n"), udpIP, udpPort);
      if (netSendCommand(buffer, 3000)) {
        return true;
      } else {
        // check if already connected
        if (strstr(buffer, "CONN")) return true;
        return false;
      }
    }
    bool udpSend(const char* data, unsigned int len)
    {
      sprintf_P(buffer, PSTR("AT+CIPSEND=%u\r\n"), len);
      if (netSendCommand(buffer, 100, ">")) {
        xbWrite(data, len);
        return netSendCommand(0, 1000);
      } else {
        Serial.print("UDP error:");
        Serial.println(buffer);
      }
      return false;
    }
    char* udpReceive(int* pbytes = 0)
    {
      if (netSendCommand(0, 3000, "+IPD,")) {
        char *p = strstr(buffer, "+IPD,");
        if (!p) return 0;
        p += 5;
        int len = atoi(p);
        if (pbytes) *pbytes = len;
        p = strchr(p, ':');
        if (p++) {
          *(p + len) = 0;
          return p;
        }
      }
      return 0;
    }
    char* queryIP(const char* host)
    {
      sprintf_P(buffer, PSTR("AT+CIPDOMAIN=\"%s\"\r\n"), host);
      if (netSendCommand(buffer, 2000, "+CIPDOMAIN")) {
        char *p = strstr(buffer, "+CIPDOMAIN");
        if (p) {
          p = strchr(p, ':');
          if (p) {
            char *ip = *(++p) == '\"' ? p + 1 : p;
            p = strchr(ip, '\"');
            if (p) *p = 0;
            return ip;
          }
        }
      }
      return 0;
    }
    bool netSendCommand(const char* cmd, unsigned int timeout = 2000, const char* expected = "OK\r\n", bool terminated = false)
    {
      if (cmd) {
        xbWrite(cmd);
      }
      buffer[0] = 0;
      byte ret = xbReceive(buffer, sizeof(buffer), timeout, expected);
      if (ret) {
        if (terminated) {
          char *p = strstr(buffer, expected);
          if (p) *p = 0;
        }
        return true;
      } else {
        return false;
      }
    }
    char buffer[192];
};

class CTeleClient : public COBDWIFI, public CDataLogger
{
public:
  bool verifyChecksum(const char* data)
  {
    uint8_t sum = 0;
    const char *s;
    for (s = data; *s && *s != '*'; s++) sum += *s;
    return (*s && hex2uint8(s + 1) == sum);
  }
  bool deregDataFeed()
  {
    for (byte n = 0; n < 3; n++) {
      Serial.print("Dereg...");
      // send request
      cacheBytes = sprintf_P(cache, PSTR("%u#OFFLINE"), feedid);
      transmitUDP();
      // receive reply
      int len;
      char *data = udpReceive(&len);
      if (!data) {
        Serial.println("failed");
        continue;
      }
      // verify checksum
      if (!verifyChecksum(data)) {
        Serial.println("checksum mismatch");
        continue;
      }
      Serial.println("OK");
      return true;
    }
    return false;
  }
  bool regDataFeed()
  {
    // retrieve VIN
    char vin[20] = {0};
    // retrieve VIN
    if (getVIN(buffer, sizeof(buffer))) {
      strncpy(vin, buffer, sizeof(vin) - 1);
      Serial.print("#VIN:");
      Serial.println(vin);
    } else {
      strcpy(vin, "DEFAULT_VIN");
    }

    Serial.print("#DTC:");
    uint16_t dtc[6];
    byte dtcCount = readDTC(dtc, sizeof(dtc) / sizeof(dtc[0]));
    Serial.println(dtcCount);

    connErrors = 0;
    connCount = 0;

    for (byte n = 0; n < 5; n++) {
      Serial.println("Registering...");
      // send request
      cacheBytes = sprintf_P(cache, PSTR("0#SK=%s,VIN=%s"), SERVER_KEY, vin);
      if (dtcCount > 0) {
        cacheBytes += sprintf_P(cache + cacheBytes, PSTR(",DTC="), dtcCount);
        for (byte i = 0; i < dtcCount; i++) {
          cacheBytes += sprintf_P(cache + cacheBytes, PSTR("%X;"), dtc[i]);
        }
        cacheBytes--;
      }
      transmitUDP();
      // receive reply
      int len;
      char *data = udpReceive(&len);
      if (!data) {
        Serial.println("failed");
        continue;
      }
      // verify checksum
      if (!verifyChecksum(data)) {
        Serial.println("checksum mismatch");
        continue;
      }
      feedid = atoi(data);
      Serial.print("#ID:");
      Serial.println(feedid);
      return true;
    }
    return false;
  }
  void transmitUDP()
  {
    // add checksum
    byte sum = 0;
    if (cacheBytes > 0 && cache[cacheBytes - 1] == ',')
      cacheBytes--; // last delimiter unneeded
    for (unsigned int i = 0; i < cacheBytes; i++) sum += cache[i];
    cacheBytes += sprintf_P(cache + cacheBytes, PSTR("*%X"), sum);
    // transmit data
    Serial.println(cache);
    Serial.print(connCount);
    Serial.print('#');
    Serial.print(cacheBytes);
    Serial.println('B');
    if (!udpSend(cache, cacheBytes)) {
      connErrors++;
    } else {
      connErrors = 0;
      connCount++;
    }
    // clear cache and write header for next transmission
    cacheBytes = sprintf_P(cache, PSTR("%u#"), feedid);
  }
  uint16_t feedid;
  uint16_t connCount;
  uint8_t connErrors;
};

class CTeleLogger : public CTeleClient
#if USE_MPU6050
,public CMPU6050
#elif USE_MPU9250
,public CMPU9250
#endif
{
public:
    CTeleLogger():state(0) {}
    void setup()
    {
        // this will init SPI communication
        begin();
 
#if USE_MPU6050 || USE_MPU9250
        // start I2C communication 
        Wire.begin();
        Serial.print("#MEMS...");
        if (memsInit()) {
          state |= STATE_MEMS_READY;
          Serial.println("OK");
        } else {
          Serial.println("NO");
        }
#endif

        for (;;) {
          // initialize OBD communication
          Serial.print("#OBD...");
          if (init()) {
            Serial.println("OK");
          } else {
            Serial.println("NO");
            reconnect();
            continue;
          }
          state |= STATE_OBD_READY;
  
#if USE_GPS
          // start serial communication with GPS receive
          Serial.print("#GPS...");
          if (initGPS(GPS_SERIAL_BAUDRATE)) {
            state |= STATE_GPS_READY;
            Serial.println("OK");
          } else {
            Serial.println("NO");
          }
#endif
  
          xbBegin(XBEE_BAUDRATE);
  
          // attempt to join AP with pre-defined credential
          // make sure to change WIFI_SSID and WIFI_PASSWORD to your own ones
          for (byte n = 0; ;n++) {
            // initialize ESP8266 xBee module (if present)
            Serial.print("#ESP8266...");
            if (netInit()) {
              state |= STATE_NET_READY;
              Serial.println("OK");
            } else {
              Serial.println(buffer);
              standby();
              continue;
            }
            sleep(100);
            Serial.print("#WIFI(SSID:");
            Serial.print(WIFI_SSID);
            Serial.print(")...");
            if (netSetup()) {
                Serial.print("#UDP...");
                if (udpOpen()) {
                  Serial.println("OK");
                  break;
                }
            }
            Serial.println("NO");
            if (n >= MAX_CONN_ERRORS) standby();
          }
          // connect with TeleServer (Freematics live data server)
          // will be stuck in the function if unsuccessful
          if (regDataFeed())
            break;

          standby();
        }

        Serial.println();
        sleep(1000);
    }
    void loop()
    {
        logTimestamp(millis());

        // process OBD data if connected
        if (state & STATE_OBD_READY) {
          processOBD();
        }

#if USE_MPU6050 || USE_MPU9250
        // process MEMS data if available
        if (state & STATE_MEMS_READY) {
            processMEMS();  
        }
#endif

#if USE_GPS
        // process GPS data if connected
        if (state & STATE_GPS_READY) {
          processGPS();
        }
#endif

        // read and log car battery voltage
        int v = getVoltage() * 100;
        logData(PID_BATTERY_VOLTAGE, v);

        if (deviceTemp >= COOLING_DOWN_TEMP && deviceTemp < 100) {
          // device too hot, slow down communication a bit
          Serial.print("Cooling (");
          Serial.print(deviceTemp);
          Serial.println("C)");
          sleep(5000);
        }
    }
    void standby()
    {
        if (state & STATE_NET_READY) {
          deregDataFeed();
          udpClose();
          netDisconnect(); // disconnect from AP
          sleep(500);
          netReset();
        }
#if USE_GPS
        if (state & STATE_GPS_READY) {
          Serial.print("#GPS:");
          initGPS(0); // turn off GPS power
          Serial.println("OFF");
        }
#endif
        state &= ~(STATE_OBD_READY | STATE_GPS_READY | STATE_NET_READY);
        Serial.println("Standby");
        enterLowPowerMode();
#if USE_MPU6050 || USE_MPU9250
        calibrateMEMS(3000);
        if (state & STATE_MEMS_READY) {
          for (;;) {
            // calculate relative movement
            unsigned long motion = 0;
            for (byte i = 0; i < 3; i++) {
              long n = accSum[i] / accCount - accCal[i];
              motion += n * n;
            }
            motion /= 100;
            Serial.println(motion);
            // check movement
            if (motion > WAKEUP_MOTION_THRESHOLD) {
              // try OBD reading
              leaveLowPowerMode();
              break;
            }
            // MEMS data collected while sleeping
            sleep(3000);
          }
        } else {
          while (!init()) sleepSec(10);
        }
#else
        while (!init()) sleepSec(10);
#endif
        Serial.println("Wakeup");
    }
private:
    void processOBD()
    {
        int speed = readSpeed();
        if (speed == -1) {
          return;
        }
        logData(0x100 | PID_SPEED, speed);
        logData(PID_TRIP_DISTANCE, distance);
        // poll more PIDs
        byte pids[]= {0, PID_RPM, PID_ENGINE_LOAD, PID_THROTTLE};
        const byte pidTier2[] = {PID_INTAKE_TEMP, PID_COOLANT_TEMP};
        static byte index2 = 0;
        int values[sizeof(pids)] = {0};
        // read multiple OBD-II PIDs, tier2 PIDs are less frequently read
        pids[0] = pidTier2[index2 = (index2 + 1) % sizeof(pidTier2)];
        byte results = readPID(pids, sizeof(pids), values);
        if (results == sizeof(pids)) {
          for (byte n = 0; n < sizeof(pids); n++) {
            logData(0x100 | pids[n], values[n]);
          }
        }
    }
    int readSpeed()
    {
        int value;
        if (readPID(PID_SPEED, value)) {
          uint32_t t = millis();    
          distance += (value + lastSpeed) * (t - lastSpeedTime) / 3600 / 2;
          lastSpeedTime = t;
          lastSpeed = value;
          return value;
        } else {
          return -1; 
        }
    }
#if USE_MPU6050 || USE_MPU9250
    void processMEMS()
    {
         // log the loaded MEMS data
        if (accCount) {
          logData(PID_ACC, accSum[0] / accCount / ACC_DATA_RATIO, accSum[1] / accCount / ACC_DATA_RATIO, accSum[2] / accCount / ACC_DATA_RATIO);
        }
    }
#endif
    void processGPS()
    {
        static uint16_t lastUTC = 0;
        static uint8_t lastGPSDay = 0;
        GPS_DATA gd = {0};
        // read parsed GPS data
        if (getGPSData(&gd)) {
            if (lastUTC != (uint16_t)gd.time) {
              byte day = gd.date / 10000;
              logData(PID_GPS_TIME, gd.time);
              if (lastGPSDay != day) {
                logData(PID_GPS_DATE, gd.date);
                lastGPSDay = day;
              }
              logCoordinate(PID_GPS_LATITUDE, gd.lat);
              logCoordinate(PID_GPS_LONGITUDE, gd.lng);
              logData(PID_GPS_ALTITUDE, gd.alt);
              logData(PID_GPS_SPEED, gd.speed);
              logData(PID_GPS_SAT_COUNT, gd.sat);
              lastUTC = (uint16_t)gd.time;
            }
            //Serial.print("#UTC:"); 
            //Serial.println(gd.time);
        } else {
          Serial.println("No GPS data");
        }
    }
    void reconnect()
    {
        // try to re-connect to OBD
        if (init()) return;
        sleep(1000);
        if (init()) return;
        standby();
    }
    void calibrateMEMS(unsigned int duration)
    {
        // MEMS data collected while sleeping
        if (duration > 0) {
          accCount = 0;
          accSum[0] = 0;
          accSum[1] = 0;
          accSum[2] = 0;
          sleep(duration);
        }
        // store accelerometer reference data
        if ((state & STATE_MEMS_READY) && accCount) {
          accCal[0] = accSum[0] / accCount;
          accCal[1] = accSum[1] / accCount;
          accCal[2] = accSum[2] / accCount;
        }
    }
    void readMEMS()
    {
        // load accelerometer and temperature data
        int16_t acc[3] = {0};
        int16_t temp; // device temperature (in 0.1 celcius degree)
        memsRead(acc, 0, 0, &temp);
        if (accCount >= 128) {
          accSum[0] >>= 1;
          accSum[1] >>= 1;
          accSum[2] >>= 1;
          accCount >>= 1;
        }
        accSum[0] += acc[0];
        accSum[1] += acc[1];
        accSum[2] += acc[2];
        accCount++;
        deviceTemp = temp / 10;
    }
    void dataIdleLoop()
    {
      // do something while waiting for data on SPI
      if (state & STATE_MEMS_READY) {
        readMEMS();
      }
    }
private:
    byte state;
};

CTeleLogger logger;

void setup()
{
    // initialize hardware serial (for USB and BLE)
    Serial.begin(115200);
    Serial.println("Freematics ONE");
    delay(500);
    // perform initializations
    logger.setup();
}

void loop()
{
    // error handling
    if (logger.errors > MAX_OBD_ERRORS) {
        // try to re-connect to OBD
        if (logger.init()) return;
        logger.standby();
        logger.setup();
    }
    if (logger.connErrors >= MAX_CONN_ERRORS) {
        logger.standby();
        logger.setup();
    }
   
#if DATASET_INTERVAL
    uint32_t t = millis();
#endif
    // collect data
    logger.loop();
    // transmit data
    logger.transmitUDP();
#if DATASET_INTERVAL
    // wait to reach preset data rate
    unsigned int elapsed = millis() - t;
    if (elapsed < DATASET_INTERVAL) logger.sleep(DATASET_INTERVAL - elapsed);
#endif
}
