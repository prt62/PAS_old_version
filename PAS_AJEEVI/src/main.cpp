/*
========================================================
  AJEEVI AUDIO CONTROLLER – ESP32 ETHERNET FIRMWARE
  Based on original stable version (Core0/Core1)
  Added: Volume, Play/Pause over TCP & BLE and live pagging
  Ethernet reboot after 30 miutes of link loss
========================================================
FEATURES:
 Ethernet LAN8720
 BLE Control + telemetry 
 TCP Commands
 MP3 Playback
 SD Card Upload
 Auto Replay After Power Cut
 Heap Monitoring
 Ethernet Watchdog (30 minutes )
 24h Periodic Reboot
 SD Card Failure Recovery
 Thread Safe Audio
 Upload Safe Playback
 Industrial Stability 
 Audio live steaming

BLE COMMANDS:
*$101,IP,GW,SUBNET,PORT#
*$102,VOLUME#     0-100
*$103,1#   PLAY
*$103,0#   STOP

TCP COMMANDS:
PLAY
STOP
UPLD
volume
live streaming
*/

#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_task_wdt.h"

// ================= BLE =================
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ================= AUDIO =================
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h>

#include <WiFiUdp.h>
WiFiUDP udpAudio;          // UDP socket for live audio (same port 5500)

// =====================================================
// PINS (Olimex ESP32-POE)
// =====================================================
#define ETH_PHY_ADDR    0
#define ETH_PHY_POWER   12
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO17_OUT

#define I2S_LRC         32
#define I2S_BCLK        33
#define I2S_DOUT        16

#define STATUS_LED      5

// BLE UUIDs
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// =====================================================
// CONFIGURATION
// =====================================================
#define UPLOAD_MIN_SIZE     5000      // 5KB minimum file size
#define ETH_LOSS_REBOOT_SEC 1800  // Reboot after 30 inutes of link loss
// Each link check runs every 2 seconds, so threshold = (seconds / 2)
#define ETH_LOSS_THRESHOLD  (ETH_LOSS_REBOOT_SEC / 2)

// =====================================================
// GLOBALS
// =====================================================
AudioFileSourceFS     *file = nullptr;
AudioFileSourceID3    *id3  = nullptr;
AudioGeneratorMP3     *mp3  = nullptr;
AudioOutputI2S        *out  = nullptr;

BLEServer         *pServer         = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
WiFiServer        *server          = nullptr;
Preferences        preferences;

// Network settings (C‑strings)
char currentIP[16]      = "192.168.5.50";
char currentGateway[16] = "192.168.5.1";
char currentSubnet[16]  = "255.255.255.0";
int  currentPort        = 5500;

float currentGain          = 0.2f;
int   currentVolumePercent = 100;

// Cross‑core flags
volatile bool reqPlay        = false;
volatile bool reqStop        = false;
volatile bool isAudioPlaying = false;
volatile bool isLiveStreaming = false;

// Ethernet link loss counter
static int ethDisconnectCounter = 0;

// BLE connection flag
bool deviceConnected = false;
unsigned long bootTime = 0; 
TaskHandle_t NetworkTaskHandle = nullptr;

// =====================================================
// FORWARD DECLARATIONS
// =====================================================
void stopAudio();
void applyVolume(int percent);
void saveAutoReplay(bool state);
bool loadAutoReplay();

// =====================================================
// VOLUME & PERSISTENCE (NEW)
// =====================================================
void applyVolume(int percent) {
    percent = constrain(percent, 0, 100);
    currentVolumePercent = percent;
    currentGain = ((float)percent / 100.0f) * 0.25f;
    if (out) out->SetGain(currentGain);

    static int lastSavedPercent = -1;
    if (percent == lastSavedPercent) return;
    lastSavedPercent = percent;

    preferences.begin("audio", false);
    preferences.putFloat("gain", currentGain);
    preferences.putInt("vol_pct", percent);
    preferences.end();
}

void saveAutoReplay(bool state) {
    preferences.begin("audio", false);
    preferences.putBool("auto_play", state);
    preferences.end();
}

bool loadAutoReplay() {
    preferences.begin("audio", true);
    bool state = preferences.getBool("auto_play", false);
    preferences.end();
    return state;
}

// =====================================================
// AUDIO STOP (unchanged from original)
// =====================================================
void stopAudio() {
    if (mp3 && mp3->isRunning()) mp3->stop();
    if (id3) {
        delete id3;
        id3 = nullptr;
    }
    if (file && file->isOpen()) file->close();
    isAudioPlaying = false;
}

// =====================================================
// BLE CALLBACKS (Extended for Volume & Play/Stop)
// =====================================================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("[BLE] Connected");
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        BLEDevice::startAdvertising();
        Serial.println("[BLE] Disconnected");
    }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        String rxValue = pChar->getValue().c_str();
        rxValue.trim();

        // ---- Network config: *$101,ip,gw,sub,port# (original) ----
        if (rxValue.startsWith("*$101,") && rxValue.endsWith("#")) {
            String payload = rxValue.substring(6, rxValue.length() - 1);
            int comma1 = payload.indexOf(',');
            int comma2 = payload.indexOf(',', comma1 + 1);
            int comma3 = payload.indexOf(',', comma2 + 1);
            if (comma1 != -1 && comma2 != -1 && comma3 != -1) {
                String nIP   = payload.substring(0, comma1);
                String nGW   = payload.substring(comma1 + 1, comma2);
                String nSN   = payload.substring(comma2 + 1, comma3);
                String nPort = payload.substring(comma3 + 1);
                nIP.trim(); nGW.trim(); nSN.trim(); nPort.trim();
                if (nIP.length() > 0 && nPort.toInt() > 0) {
                    preferences.begin("network", false);
                    preferences.putString("saved_ip", nIP);
                    preferences.putString("saved_gw", nGW);
                    preferences.putString("saved_sn", nSN);
                    preferences.putInt("saved_port", nPort.toInt());
                    preferences.end();
                    Serial.println("[BLE] Network config saved. Rebooting...");
                    delay(3000);
                    ESP.restart();
                }
            }
        }
        // ---- Volume: *$102,vol# (NEW) ----
        else if (rxValue.startsWith("*$102,") && rxValue.endsWith("#")) {
            int vol = rxValue.substring(6, rxValue.length() - 1).toInt();
            applyVolume(vol);
            Serial.printf("[BLE] Volume set to %d%%\n", vol);
        }
        // ---- Play/Stop: *$103,1# or *$103,0# (NEW) ----
       // ---- Play/Stop: *$103,1# or *$103,0# (NEW) ----
        else if (rxValue.startsWith("*$103,") && rxValue.endsWith("#")) {
            if (isLiveStreaming) {
                Serial.println("[BLE] Command Ignored - Live Announcement Active!");
                return; // Agar Live chal raha hai to command reject kar do
            }
            int state = rxValue.substring(6, rxValue.length() - 1).toInt();
            saveAutoReplay(state == 1);
            if (state == 1) {
                reqPlay = true;
                reqStop = false;
                Serial.println("[BLE] PLAY command");
            } else {
                reqStop = true;
                reqPlay = false;
                Serial.println("[BLE] STOP command");
            }
        }
    }
};

// =====================================================
// CORE 0 – NETWORK & TCP SERVER TASK
// =====================================================
void networkTaskCode(void * parameter) {
    static char cmdBuf[16];   // enough for "VOL:100"
    int cmdIdx = 0;

    for (;;) {
        esp_task_wdt_reset();

        // ---- Ethernet link watchdog (every 2 seconds) ----
        static unsigned long lastDiag = 0;
        if (millis() - lastDiag > 2000) {
            lastDiag = millis();
            bool linkUp = ETH.linkUp();
            if (!linkUp) {
                ethDisconnectCounter++;
                if (ethDisconnectCounter >= ETH_LOSS_THRESHOLD) {
                    Serial.printf("[ETH] Link down for %d seconds – rebooting\n", ETH_LOSS_REBOOT_SEC);
                    delay(100);
                    ESP.restart();
                }
            } else {
                ethDisconnectCounter = 0;
            }

            // BLE telemetry (C‑string to avoid fragmentation)
            if (deviceConnected && pCharacteristic) {
                char telemetry[300];
                snprintf(telemetry, sizeof(telemetry),
                    " IP :%s :%d\n GW : %s\n MASK : %s\n VOL : %d%%\n STATE : %s\n ETH : %s\n SD : %s\n HEAP : %u Bytes\n MIN : %u Bytes\n\n",
                    currentIP, currentPort ,
                    currentGateway ,
                    currentSubnet ,
                    currentVolumePercent ,
                    isAudioPlaying  ?  "PLAY" : "STOP" ,
                    linkUp  ?  "UP" : "DOWN",
                    SD_MMC.cardType()  != CARD_NONE ? "OK" : "FAIL" ,
                    ESP.getFreeHeap() ,
                    ESP.getMinFreeHeap()

                );
                pCharacteristic->setValue(telemetry);
                pCharacteristic->notify();
            }
        }

      // ---- TCP command handling (Backward Compatible Logic) ----
        WiFiClient client = server->available();
        if (client) {
            client.setNoDelay(true);
            unsigned long timeout = millis();
            
            // Wait for exactly 4 bytes (Purana solid logic)
            while (client.available() < 4 && (millis() - timeout < 1000)) {
                vTaskDelay(1);
            }

            if (client.available() >= 4) {
                char cmdBuf[16] = {0};
                
                // Sirf 4 byte padho (Isse \n ka koi lafda hi nahi rahega)
                client.read((uint8_t*)cmdBuf, 4);

                // ---- PLAY ----
               // ---- PLAY ----
                if (strcmp(cmdBuf, "PLAY") == 0) 
                {
                    if (isLiveStreaming) {
                        client.println("BUSY"); // Python ko batao abhi busy hai
                    } else {
                        Serial.println("[TCP] PLAY");
                        client.println("ACK_PLAY");
                        saveAutoReplay(true);
                        reqPlay = true;
                        reqStop = false;
                    }
                }
                // ---- STOP ----
                else if (strcmp(cmdBuf, "STOP") == 0)
                 {
                    Serial.println("[TCP] STOP");
                    client.println("ACK_STOP");
                    saveAutoReplay(false);
                    reqStop = true;
                    reqPlay = false;
                }
                // ---- VOLUME (NEW) ----
                else if (strncmp(cmdBuf, "VOL:", 4) == 0) 
                {
                    // "VOL:" whatever number it comes after it will read (range 0-100)
                    int i = 4;
                    unsigned long volTimeout = millis();
                    while(client.connected() && i < 15 && (millis() - volTimeout < 500)) {
                        if (client.available()) {
                            char c = client.read();
                            if (c == '\n' || c == '\r') break; // if /n comes loop will break;
                            cmdBuf[i++] = c;
                        } else {
                            vTaskDelay(1);
                        }
                    }
                    
                    int vol = atoi(cmdBuf + 4);
                    if (vol >= 0 && vol <= 100) {
                        applyVolume(vol);
                        client.println("ACK");
                        Serial.printf("[TCP] Volume set to %d%%\n", vol);
                    } else {
                        client.println("ERR_VOL");
                    }
                }
               // ---- UPLOAD (direct to /song.mp3, no temp file) ----
                else if (strcmp(cmdBuf, "UPLD") == 0) 
                {
                    Serial.println("[TCP] UPLOAD");
                    
                    reqStop = true;
                    while (reqStop || isAudioPlaying) vTaskDelay(10);
                    while (client.available()) client.read(); 

                    if (SD_MMC.exists("/song.mp3")) SD_MMC.remove("/song.mp3");
                    File uploadFile = SD_MMC.open("/song.mp3", FILE_WRITE);
                    
                    if (uploadFile) {
                        uint32_t totalBytes = 0;
                        uint32_t lastDataTime = millis();
                        client.println("READY");   

                        while (client.connected()) {
                            if (client.available()) {
                                uint8_t buf[4096];
                                int len = client.read(buf, sizeof(buf));
                                if (len > 0) {
                                    uploadFile.write(buf, len);
                                    totalBytes += len;
                                    lastDataTime = millis();
                                    
                                    // FIX: let WDT and Ethernet MAC breathe
                                    esp_task_wdt_reset(); 
                                    vTaskDelay(2 / portTICK_PERIOD_MS); 
                                }
                            } else {
                                if (millis() - lastDataTime > 15000) break; 
                                vTaskDelay(1);
                            }
                        }
                        uploadFile.close();

                        if (totalBytes >= UPLOAD_MIN_SIZE) {
                            client.println("UPLOAD_OK");
                            Serial.printf("[TCP] Upload success: %u bytes\n", totalBytes);
                            if (loadAutoReplay()) {
                                vTaskDelay(500);
                                reqPlay = true;
                                reqStop = false;
                            }
                        } else {
                            SD_MMC.remove("/song.mp3");
                            client.printf("ERR_SIZE_MIN_%d", UPLOAD_MIN_SIZE);
                        }
                    } else {
                        client.println("ERR_OPEN");
                    }
                    client.stop(); 
                }
                
  // ---- LIVE ANNOUNCEMENT (PURE UDP STREAMING) ----
                else if (strcmp(cmdBuf, "LIVE") == 0) {
                    Serial.println("[TCP] LIVE COMMAND RECEIVED. SWITCHING TO UDP...");
                    bool wasPlaying = isAudioPlaying; 
                    
                    reqStop = true;
                    while (reqStop || isAudioPlaying) vTaskDelay(10);
                    
                    isLiveStreaming = true; 
                    
                    while (client.available()) client.read(); 
                    client.println("ACK_LIVE");
                    client.flush();
                    vTaskDelay(100 / portTICK_PERIOD_MS); // 100ms ruko taaki packet laptop tak pahunch jaye
                    
                    // Client IP aur Port note kar lo Hole Punching ke liye
                    IPAddress clientIP = client.remoteIP();
                    client.stop(); // TCP Band

                    if (out) {
                        out->stop();
                        out->SetRate(16000); 
                        out->SetBitsPerSample(16);
                        out->SetChannels(1);
                        out->SetGain(currentGain); 
                        out->begin();
                    }
                    
                    // UDP Shuru
                    udpAudio.begin(5500);
                    
                    // 🔥 UDP HOLE PUNCHING: Windows Firewall ka rasta kholne ke liye
                    // ESP32 ek dummy packet wapas Python ko bhejega
                    Serial.println("[UDP] Punching Firewall Hole...");
                    udpAudio.beginPacket(clientIP, 5500);
                    udpAudio.write((const uint8_t*)"READY", 5);
                    udpAudio.endPacket();

                    uint8_t rxBuf[2048];
                    unsigned long lastVoiceTime = millis();
                    bool hasLeftover = false;
                    uint8_t leftoverByte = 0;
                    
                    Serial.println("[UDP] Waiting for Audio Stream...");
                    
                    while (isLiveStreaming) {
                        esp_task_wdt_reset();
                        
                        int packetSize = udpAudio.parsePacket();
                        if (packetSize > 0) {
                            
                            int maxToRead = sizeof(rxBuf) - (hasLeftover ? 1 : 0);
                            int toRead = (packetSize > maxToRead) ? maxToRead : packetSize;
                            
                            int len = udpAudio.read(rxBuf + (hasLeftover ? 1 : 0), toRead);
                            
                            if (len > 0) {
                                int totalBytes = len + (hasLeftover ? 1 : 0);
                                
                                for (int i = 0; i < totalBytes - 1; i += 2) {
                                    int16_t val = (int16_t)((rxBuf[i+1] << 8) | rxBuf[i]);
                                    int16_t sample[2] = {val, val};
                                    if (out) out->ConsumeSample(sample);
                                }
                                
                                if (totalBytes % 2 != 0) {
                                    leftoverByte = rxBuf[totalBytes - 1];
                                    rxBuf[0] = leftoverByte; 
                                    hasLeftover = true;
                                } else {
                                    hasLeftover = false;
                                }
                                
                                lastVoiceTime = millis();
                            }
                        } else {
                            if (millis() - lastVoiceTime > 4000) { // 4 Second timeout
                                Serial.println("[UDP] AUDIO TIMEOUT. NO DATA RECEIVED.");
                                break;
                            }
                            vTaskDelay(1);
                        }
                    }
                    
                    Serial.println("[UDP] END LIVE STREAM");
                    if (out) out->stop();
                    udpAudio.stop();
                    
                    isLiveStreaming = false; 
                    if (wasPlaying) reqPlay = true;
                }
                // ---- Unknown command ----
                else {
                    client.println("ERR_CMD");
                }
            }

            // last cleanup: clean the remaining wastage in tcp buffer and stop
            while (client.available()) client.read();
            client.stop();
        }

        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

// =====================================================
// SETUP (unchanged from original + volume init)
// =====================================================
void setup() {
    Serial.begin(115200);
    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, LOW);

    Serial.println("\n========================================");
    Serial.println("  AJEEVI AUDIO CONTROLLER (Stable+)");
    Serial.println("========================================\n");

     bootTime = millis();
    esp_task_wdt_init(90, false);
    esp_task_wdt_add(NULL);

    // Load network config
    preferences.begin("network", true);
    String ip   = preferences.getString("saved_ip", "192.168.5.50");
    String gw   = preferences.getString("saved_gw", "192.168.5.1");
    String sn   = preferences.getString("saved_sn", "255.255.255.0");
    currentPort = preferences.getInt("saved_port", 5500);
    preferences.end();
    strncpy(currentIP, ip.c_str(), sizeof(currentIP)-1);
    strncpy(currentGateway, gw.c_str(), sizeof(currentGateway)-1);
    strncpy(currentSubnet, sn.c_str(), sizeof(currentSubnet)-1);

    // Load audio settings (volume, auto‑replay)
    preferences.begin("audio", true);
    currentGain = preferences.getFloat("gain", 0.2f);
    currentVolumePercent = preferences.getInt("vol_pct", 100);
    preferences.end();

    // BLE
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char bleName[32];
    sprintf(bleName, "AJEEVI_PAS_%02X%02X%02X", mac[3], mac[4], mac[5]);

    BLEDevice::init(bleName);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pCharacteristic->addDescriptor(new BLE2902());
    pCharacteristic->setCallbacks(new MyCallbacks());
    pService->start();
    BLEDevice::getAdvertising()->start();
    Serial.printf("[BLE] Name: %s\n", bleName);

    // Ethernet
    IPAddress localIP, gatewayIP, subnetMask, dns(8,8,8,8);
    localIP.fromString(currentIP);
    gatewayIP.fromString(currentGateway);
    subnetMask.fromString(currentSubnet);
    ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE);
    delay(1000);
    ETH.config(localIP, gatewayIP, subnetMask, dns);
    Serial.printf("[ETH] IP: %s\n", currentIP);

    // SD card
    if (!SD_MMC.begin("/sdcard", true, false, 20000) || SD_MMC.cardType() == CARD_NONE) {
        Serial.println("[SD] ERROR – card missing or failed");
    } else {
        Serial.printf("[SD] OK, size: %u MB\n", SD_MMC.totalBytes() / (1024*1024));
    }

    // Audio
    out = new AudioOutputI2S();
    out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    out->SetGain(currentGain);
    file = new AudioFileSourceFS(SD_MMC);
    mp3  = new AudioGeneratorMP3();

    // TCP server
    server = new WiFiServer(currentPort);
    server->begin();
    Serial.printf("[TCP] Listening on port %d\n", currentPort);

    // Auto‑play after power loss (if previously playing)
    if (loadAutoReplay() && SD_MMC.exists("/song.mp3")) {
        reqPlay = true;
        Serial.println("[AUDIO] Auto‑play enabled");
    }

    // Start network task on Core 0
    xTaskCreatePinnedToCore(networkTaskCode, "NetTask", 20000, NULL, 1, &NetworkTaskHandle, 0);
    Serial.println("[SYSTEM] Ready\n");
}

// =====================================================
// LOOP (Core 1 – Audio Playback)
// =====================================================
void loop() 
{
    esp_task_wdt_reset();
    
     // ===== AUTO-RESTART EVERY 24 HOURS =====
    if (bootTime != 0 && (millis() - bootTime > 86400000UL)) {
        Serial.println("[SYS] 24h uptime reached – restarting");
        saveAutoReplay(isAudioPlaying);
        delay(100);
        ESP.restart();
    }

    if (reqStop)
     {
        stopAudio();
        reqStop = false;
    }

    if (reqPlay && !isAudioPlaying) {
        stopAudio(); // cleanup previous objects
        if (file->open("/song.mp3")) {
            id3 = new AudioFileSourceID3(file);
            mp3->begin(id3, out);
            isAudioPlaying = true;
            Serial.println("[AUDIO] Playing");
        } else {
            Serial.println("[AUDIO] song.mp3 not found");
        }
        reqPlay = false;
    }

   if (mp3 && mp3->isRunning()) 
    {
        // --- 1-HOUR PAUSE FIX: DATA FLOW WATCHDOG ---
        static uint32_t lastFilePos = 0;
        static unsigned long lastPosTime = millis();
        
        uint32_t currentPos = file->getPos();
        if (currentPos != lastFilePos) {
            lastFilePos = currentPos;
            lastPosTime = millis(); // Track time only if data moves
        }

        // Agar 4 seconds tak position nahi badhi = System Hang ho gaya!
        if (millis() - lastPosTime > 4000) {
            Serial.println("[AUDIO] SD Card Stuck! Restarting MP3...");
            stopAudio();
            reqPlay = true; // Auto-resume trigger
            lastPosTime = millis();
        }
        // ---------------------------------------------

        if (!mp3->loop()) {
            // Song finished naturally
            stopAudio();
            if (loadAutoReplay() && SD_MMC.exists("/song.mp3")) {
                // Restart automatically
                if (file->open("/song.mp3")) {
                    id3 = new AudioFileSourceID3(file);
                    mp3->begin(id3, out);
                    isAudioPlaying = true;
                }
            }
        }
    } else 
    {
        vTaskDelay(1);
    }

    // Status LED: blink when playing, solid when idle + Ethernet up
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 1000) {
        lastBlink = millis();
        if (isAudioPlaying) {
            digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
        } else {
            digitalWrite(STATUS_LED, ETH.linkUp() ? HIGH : LOW);
        }
    }

    vTaskDelay(5 / portTICK_PERIOD_MS);
}