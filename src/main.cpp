#include <ESP8266WiFi.h>
#include <Wire.h>
#include <INA226_WE.h>
#include <WiFiUdp.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <Updater.h>
#include <WiFiManager.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
extern "C"
{
#include "user_interface.h"
}

// --- LLAVES MAESTRAS DE LOS LEDS (Lógica Inversa) ---
// D6 (Verde) y D7 (Rojo) están conectados a 3.3V.
// Para que se enciendan, el pin debe enviar LOW (cierra el circuito a masa).
#define LED_ON LOW
#define LED_OFF HIGH

ESP8266WebServer server(80);

bool isFirstBoot = true;
unsigned long firstBootMillis;
bool inaAvailable = false;
bool updateMode = false;
bool displayAvailable = false;
bool isDisplayPoweredOn = false;
bool apFallbackActive = false;
unsigned long wakeStartedAt = 0;
unsigned long lastInaRecoveryAttempt = 0;
const unsigned long inaRecoveryIntervalMs = 10000;
const unsigned long inaReadyTimeoutMs = 15000;
unsigned int inaI2cErrorCount = 0;
unsigned int wifiReconnectCount = 0;
unsigned int dnsFailureCount = 0;
unsigned int inaRecoveryCount = 0;
unsigned long updateModeStartedAt = 0;
unsigned long lastWifiRecoveryAttempt = 0;
unsigned long lastDnsResolutionAttempt = 0;

float filteredCurrent_A = 0.0;
bool filterInitialized = false;
bool inaTelemetryReady = false;

// Changelog:
// v1.9 (build 109)
// - Host SignalK configurable (IP u hostname) desde /config.
// - Modo AUTO diferenciado: no fuerza mantenimiento en arranque en frio.
// - En deep sleep se espera lectura INA valida antes de dormir (con timeout de seguridad).
// - Se evita publicar telemetria normal hasta tener muestra INA valida tras despertar.
const char *FIRMWARE_VERSION = "v1.9";
const uint16_t FIRMWARE_BUILD = 109;

enum OperationMode : uint8_t
{
    MODE_BATTERY = 0,
    MODE_MAINTENANCE = 1,
};

enum PreferredMode : uint8_t
{
    PREF_AUTO = 0,
    PREF_BATTERY = 1,
    PREF_MAINTENANCE = 2,
};

OperationMode operationMode = MODE_BATTERY;
bool maintenanceMode = false;
bool webServerActive = false;
const bool wifiFastConnectEnabled = true;

const unsigned long batteryWifiConnectTimeoutMs = 3500;
const unsigned long maintenanceWifiConnectTimeoutMs = 10000;
const uint8_t staFailuresForMaintenance = 3;
const bool autoMaintenanceOnColdBoot = false;

struct RtcState
{
    uint32_t magic;
    uint16_t version;
    uint8_t consecutiveStaFailures;
    uint8_t forceMaintenance;
    uint32_t crc32;
};

RtcState rtcState;
const uint32_t kRtcMagic = 0x494E4152UL; // "INAR"
const uint16_t kRtcVersion = 1;
const uint32_t kRtcStateAddress = 96;

enum DebugLevel : uint8_t
{
    DEBUG_ERROR = 0,
    DEBUG_WARN = 1,
    DEBUG_INFO = 2,
};

const DebugLevel kDebugLevel = DEBUG_INFO;

void debugLog(DebugLevel level, const char *format, ...)
{
    if (level > kDebugLevel)
        return;

    const char *tag = "INFO";
    if (level == DEBUG_ERROR)
        tag = "ERROR";
    else if (level == DEBUG_WARN)
        tag = "WARN";

    char message[192];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    Serial.printf("[%s] %s\n", tag, message);
}

#define LOG_ERROR(...) debugLog(DEBUG_ERROR, __VA_ARGS__)
#define LOG_WARN(...) debugLog(DEBUG_WARN, __VA_ARGS__)
#define LOG_INFO(...) debugLog(DEBUG_INFO, __VA_ARGS__)

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define I2C_ADDRESS 0x40

const unsigned int udpPort = 4210;
IPAddress udpAddress;
const char *kDefaultSignalKHost = "lysmarine.local";
const unsigned long timeToNormalDisplay = 30000;
const unsigned long updateModeTimeoutMs = 300000;
const unsigned long wifiRecoveryIntervalMs = 30000;
const unsigned long dnsRetryIntervalMs = 60000;
const unsigned long apFallbackTimeoutMs = 600000;
const bool allowBuiltinHeartbeat = false;
unsigned long apFallbackStartedAt = 0;
WiFiUDP udp;
INA226_WE ina226 = INA226_WE(I2C_ADDRESS);
WiFiManager wifiManager;

const int greenLEDPin = 12; // D6
const int redLEDPin = 13;   // D7
const int i2cSdaPin = 4;    // D2
const int i2cSclPin = 5;    // D1

struct Config
{
    uint8_t enableDisplay;
    uint8_t enableDeepSleep;
    uint8_t enableLEDs;
    uint8_t preferredMode;
    uint16_t sleepTime;
    char hostname[16];
    float shuntResistance;
    char sourceId[32];
    char basePath[48];
    char signalKHost[64];
};
Config config;

struct StoredConfig
{
    uint32_t magic;
    uint16_t version;
    uint16_t configSize;
    Config payload;
    uint32_t crc32;
};

const uint32_t kConfigMagic = 0x494E4132UL; // "INA2"
const uint16_t kConfigVersion = 4;
static_assert(sizeof(StoredConfig) <= 512, "StoredConfig exceeds EEPROM size");

// ── FUNCIONES DE MEMORIA, RED Y ACTUALIZACIÓN ────────────────────────────────

void saveConfiguration();

void saveRtcState();

const char *powerModeName()
{
    return operationMode == MODE_MAINTENANCE ? "maintenance" : "battery";
}

const char *preferredModeName()
{
    switch (config.preferredMode)
    {
    case PREF_BATTERY:
        return "battery";
    case PREF_MAINTENANCE:
        return "maintenance";
    default:
        return "auto";
    }
}

void buildDefaultHostname(char *buffer, size_t size)
{
    snprintf(buffer, size, "INA226-%06X", ESP.getChipId());
}

void buildDefaultSourceId(char *buffer, size_t size)
{
    snprintf(buffer, size, "INA226-%06X", ESP.getChipId());
}

size_t boundedStringLen(const char *value, size_t maxLen)
{
    size_t len = 0;
    while (len < maxLen && value[len] != '\0')
        len++;
    return len;
}

uint32_t calculateCrc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    }
    return ~crc;
}

void resetRtcState()
{
    memset(&rtcState, 0, sizeof(rtcState));
    rtcState.magic = kRtcMagic;
    rtcState.version = kRtcVersion;
    saveRtcState();
}

bool loadRtcState()
{
    if (!system_rtc_mem_read(kRtcStateAddress, &rtcState, sizeof(rtcState)))
        return false;

    const uint32_t expectedCrc = calculateCrc32(reinterpret_cast<const uint8_t *>(&rtcState), sizeof(rtcState) - sizeof(rtcState.crc32));
    if (rtcState.magic != kRtcMagic || rtcState.version != kRtcVersion || rtcState.crc32 != expectedCrc)
        return false;

    return true;
}

void saveRtcState()
{
    rtcState.magic = kRtcMagic;
    rtcState.version = kRtcVersion;
    rtcState.crc32 = calculateCrc32(reinterpret_cast<const uint8_t *>(&rtcState), sizeof(rtcState) - sizeof(rtcState.crc32));
    system_rtc_mem_write(kRtcStateAddress, &rtcState, sizeof(rtcState));
}

bool connectStaWithTimeout(unsigned long timeoutMs, bool showProgress)
{
    const unsigned long startedAt = millis();
    int wifiAttempts = 0;

    while (WiFi.status() != WL_CONNECTED && (millis() - startedAt) < timeoutMs)
    {
        delay(250);
        wifiAttempts++;

        if (showProgress && displayAvailable && (config.enableDisplay || isFirstBoot))
        {
            display.clearDisplay();
            display.setTextSize(1);
            display.setCursor(0, 0);
            display.println("Buscando WiFi...");
            display.setCursor(0, 20);
            display.print("Intento: ");
            display.println(wifiAttempts);
            display.setCursor(0, 40);
            display.println(config.hostname);
            display.display();
        }

        if (showProgress && config.enableLEDs && isFirstBoot)
        {
            digitalWrite(greenLEDPin, (wifiAttempts % 2 == 0) ? LED_ON : LED_OFF);
            digitalWrite(redLEDPin, LED_OFF);
        }
    }

    return WiFi.status() == WL_CONNECTED;
}

bool isValidCharsetToken(const String &value, size_t maxLen)
{
    if (value.length() == 0 || value.length() >= maxLen)
        return false;

    for (size_t i = 0; i < value.length(); ++i)
    {
        const char c = value[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

bool isValidHostname(const String &value)
{
    if (value.length() == 0 || value.length() >= sizeof(config.hostname))
        return false;
    if (value[0] == '-' || value[value.length() - 1] == '-')
        return false;

    for (size_t i = 0; i < value.length(); ++i)
    {
        const char c = value[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-'))
            return false;
    }
    return true;
}

bool isValidSourceId(const String &value)
{
    return isValidCharsetToken(value, sizeof(config.sourceId));
}

bool isValidPath(const String &value)
{
    return isValidCharsetToken(value, sizeof(config.basePath));
}

bool isValidSignalKHost(const String &value)
{
    if (value.length() == 0 || value.length() >= sizeof(config.signalKHost))
        return false;

    IPAddress ip;
    if (ip.fromString(value))
        return true;

    uint8_t labelLen = 0;
    for (size_t i = 0; i < value.length(); ++i)
    {
        const char c = value[i];

        if (c == '.')
        {
            if (labelLen == 0)
                return false;
            labelLen = 0;
            continue;
        }

        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-'))
            return false;

        if (c == '-' && labelLen == 0)
            return false;

        labelLen++;
        if (labelLen > 63)
            return false;

        const bool endOfLabel = (i + 1 == value.length()) || (value[i + 1] == '.');
        if (endOfLabel && c == '-')
            return false;
    }

    return labelLen > 0;
}

void setDefaultConfiguration()
{
    memset(&config, 0, sizeof(config));
    config.enableDisplay = 1;
    config.enableDeepSleep = 1;
    config.enableLEDs = 1;
    config.preferredMode = PREF_AUTO;
    config.sleepTime = 15;
    config.shuntResistance = 0.002f;
    buildDefaultHostname(config.hostname, sizeof(config.hostname));
    buildDefaultSourceId(config.sourceId, sizeof(config.sourceId));
    strncpy(config.basePath, "electrical.batteries.fridge", sizeof(config.basePath) - 1);
    config.basePath[sizeof(config.basePath) - 1] = '\0';
    strncpy(config.signalKHost, kDefaultSignalKHost, sizeof(config.signalKHost) - 1);
    config.signalKHost[sizeof(config.signalKHost) - 1] = '\0';
}

bool sanitizeConfiguration()
{
    bool changed = false;

    // Garantiza terminadores nulos para evitar lecturas fuera de límites.
    if (config.hostname[sizeof(config.hostname) - 1] != '\0')
        changed = true;
    config.hostname[sizeof(config.hostname) - 1] = '\0';
    if (config.sourceId[sizeof(config.sourceId) - 1] != '\0')
        changed = true;
    config.sourceId[sizeof(config.sourceId) - 1] = '\0';
    if (config.basePath[sizeof(config.basePath) - 1] != '\0')
        changed = true;
    config.basePath[sizeof(config.basePath) - 1] = '\0';
    if (config.signalKHost[sizeof(config.signalKHost) - 1] != '\0')
        changed = true;
    config.signalKHost[sizeof(config.signalKHost) - 1] = '\0';

    if (config.enableDisplay > 1)
    {
        config.enableDisplay = 1;
        changed = true;
    }
    if (config.enableDeepSleep > 1)
    {
        config.enableDeepSleep = 1;
        changed = true;
    }
    if (config.enableLEDs > 1)
    {
        config.enableLEDs = 1;
        changed = true;
    }
    if (config.preferredMode > PREF_MAINTENANCE)
    {
        config.preferredMode = PREF_AUTO;
        changed = true;
    }

    if (config.sleepTime > 300 || config.sleepTime < 15)
    {
        config.sleepTime = 15;
        changed = true;
    }

    if (boundedStringLen(config.hostname, sizeof(config.hostname)) == 0)
    {
        buildDefaultHostname(config.hostname, sizeof(config.hostname));
        changed = true;
    }

    if (isnan(config.shuntResistance) || config.shuntResistance <= 0.0f || config.shuntResistance > 1.0f)
    {
        config.shuntResistance = 0.002f;
        changed = true;
    }

    if (boundedStringLen(config.sourceId, sizeof(config.sourceId)) == 0)
    {
        buildDefaultSourceId(config.sourceId, sizeof(config.sourceId));
        changed = true;
    }

    if (boundedStringLen(config.basePath, sizeof(config.basePath)) == 0)
    {
        strncpy(config.basePath, "electrical.batteries.fridge", sizeof(config.basePath) - 1);
        changed = true;
    }

    for (size_t i = 0; i < sizeof(config.basePath) && config.basePath[i] != '\0'; ++i)
    {
        if (config.basePath[i] == '/')
        {
            config.basePath[i] = '.';
            changed = true;
        }
    }

    if (boundedStringLen(config.signalKHost, sizeof(config.signalKHost)) == 0 || !isValidSignalKHost(String(config.signalKHost)))
    {
        strncpy(config.signalKHost, kDefaultSignalKHost, sizeof(config.signalKHost) - 1);
        config.signalKHost[sizeof(config.signalKHost) - 1] = '\0';
        changed = true;
    }

    return changed;
}

void readConfiguration()
{
    StoredConfig storedConfig;
    for (unsigned int i = 0; i < sizeof(storedConfig); ++i)
        *((char *)&storedConfig + i) = EEPROM.read(i);

    const uint32_t expectedCrc = calculateCrc32(reinterpret_cast<const uint8_t *>(&storedConfig.payload), sizeof(storedConfig.payload));
    const bool isValid =
        storedConfig.magic == kConfigMagic &&
        storedConfig.version == kConfigVersion &&
        storedConfig.configSize == sizeof(Config) &&
        storedConfig.crc32 == expectedCrc;

    if (!isValid)
    {
        LOG_WARN("Configuracion EEPROM invalida o antigua. Restaurando defaults.");
        setDefaultConfiguration();
        sanitizeConfiguration();
        saveConfiguration();
    }
    else
    {
        memcpy(&config, &storedConfig.payload, sizeof(config));
        if (sanitizeConfiguration())
            saveConfiguration();
    }

    LOG_INFO("Configuracion leida");
    LOG_INFO("enableDisplay: %u", config.enableDisplay);
    LOG_INFO("enableDeepSleep: %u", config.enableDeepSleep);
    LOG_INFO("sleepTime: %u", config.sleepTime);
    LOG_INFO("hostname: %s", config.hostname);
    LOG_INFO("enableLEDs: %u", config.enableLEDs);
    LOG_INFO("preferredMode: %s", preferredModeName());
    LOG_INFO("shuntResistance: %.4f", config.shuntResistance);
    LOG_INFO("sourceId: %s", config.sourceId);
    LOG_INFO("basePath: %s", config.basePath);
    LOG_INFO("signalKHost: %s", config.signalKHost);
}

void saveConfiguration()
{
    StoredConfig storedConfig;
    storedConfig.magic = kConfigMagic;
    storedConfig.version = kConfigVersion;
    storedConfig.configSize = sizeof(Config);
    memcpy(&storedConfig.payload, &config, sizeof(config));
    storedConfig.crc32 = calculateCrc32(reinterpret_cast<const uint8_t *>(&storedConfig.payload), sizeof(storedConfig.payload));

    for (unsigned int i = 0; i < sizeof(storedConfig); ++i)
        EEPROM.write(i, *((char *)&storedConfig + i));
    EEPROM.commit();
}

void powerOnDisplay()
{
    if (!displayAvailable || isDisplayPoweredOn)
        return;

    display.ssd1306_command(SSD1306_DISPLAYON);
    isDisplayPoweredOn = true;
}

void powerOffDisplay()
{
    if (!displayAvailable || !isDisplayPoweredOn)
        return;

    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    isDisplayPoweredOn = false;
}

void configureIna()
{
    ina226.setAverage(INA226_AVERAGE_16);
    ina226.setConversionTime(INA226_CONV_TIME_1100);
    ina226.setMeasureMode(INA226_CONTINUOUS);
    ina226.setResistorRange(config.shuntResistance, 10.0);
}

void resetWireBus()
{
    Wire.begin(i2cSdaPin, i2cSclPin);
    Wire.setClock(100000);
    Wire.setClockStretchLimit(150000);
}

void recoverI2cBus()
{
    LOG_WARN("Intentando recuperar bus I2C...");

    pinMode(i2cSdaPin, INPUT_PULLUP);
    pinMode(i2cSclPin, INPUT_PULLUP);
    delayMicroseconds(5);

    if (digitalRead(i2cSdaPin) == LOW)
    {
        pinMode(i2cSclPin, OUTPUT_OPEN_DRAIN);
        digitalWrite(i2cSclPin, HIGH);

        for (uint8_t pulse = 0; pulse < 9 && digitalRead(i2cSdaPin) == LOW; ++pulse)
        {
            digitalWrite(i2cSclPin, LOW);
            delayMicroseconds(5);
            digitalWrite(i2cSclPin, HIGH);
            delayMicroseconds(5);
        }
    }

    pinMode(i2cSdaPin, OUTPUT_OPEN_DRAIN);
    digitalWrite(i2cSdaPin, LOW);
    delayMicroseconds(5);
    pinMode(i2cSclPin, INPUT_PULLUP);
    delayMicroseconds(5);
    digitalWrite(i2cSdaPin, HIGH);
    pinMode(i2cSdaPin, INPUT_PULLUP);

    resetWireBus();
}

bool resolveUdpTarget(bool force = false)
{
    const unsigned long now = millis();
    if (!force && now - lastDnsResolutionAttempt < dnsRetryIntervalMs)
        return udpAddress != IPAddress(0, 0, 0, 0);

    lastDnsResolutionAttempt = now;
    udpAddress = IPAddress(0, 0, 0, 0);

    if (WiFi.status() != WL_CONNECTED)
        return false;

    LOG_INFO("Resolviendo IP de %s", config.signalKHost);
    if (!WiFi.hostByName(config.signalKHost, udpAddress))
    {
        dnsFailureCount++;
        LOG_WARN("DNS resolution failed - se reintentara automaticamente");
        return false;
    }

    LOG_INFO("SignalK IP: %s", udpAddress.toString().c_str());
    return true;
}

void tryRecoverWifi(unsigned long currentMillis)
{
    if (WiFi.status() == WL_CONNECTED)
        return;
    if (currentMillis - lastWifiRecoveryAttempt < wifiRecoveryIntervalMs)
        return;

    lastWifiRecoveryAttempt = currentMillis;
    udpAddress = IPAddress(0, 0, 0, 0);
    wifiReconnectCount++;
    LOG_WARN("Reintentando enlace WiFi STA...");
    WiFi.reconnect();
}

void sendChunkedResponse(const char *contentType)
{
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, contentType, "");
}

void finishChunkedResponse()
{
    server.sendContent("");
}

void sendStatusJson()
{
    char buffer[256];
    const WiFiMode_t wifiMode = WiFi.getMode();
    const bool softApEnabled = (wifiMode & WIFI_AP) != 0;
    const char *wifiModeStr = "UNKNOWN";
    if (wifiMode == WIFI_STA)
        wifiModeStr = "STA";
    else if (wifiMode == WIFI_AP)
        wifiModeStr = "AP";
    else if (wifiMode == WIFI_AP_STA)
        wifiModeStr = "AP_STA";
    else if (wifiMode == WIFI_OFF)
        wifiModeStr = "OFF";

    sendChunkedResponse("application/json");
    server.sendContent("{");
    snprintf(buffer, sizeof(buffer), "\"firmware\":\"%s\",", FIRMWARE_VERSION);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"firmwareBuild\":%u,", FIRMWARE_BUILD);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"powerMode\":\"%s\",", powerModeName());
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"maintenanceMode\":%s,", maintenanceMode ? "true" : "false");
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"preferredMode\":\"%s\",", preferredModeName());
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"wifiFastConnectEnabled\":%s,", wifiFastConnectEnabled ? "true" : "false");
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"hostname\":\"%s\",", config.hostname);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"sourceId\":\"%s\",", config.sourceId);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"basePath\":\"%s\",", config.basePath);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"signalKHost\":\"%s\",", config.signalKHost);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"wifiConnected\":%s,", WiFi.status() == WL_CONNECTED ? "true" : "false");
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"ip\":\"%s\",", WiFi.localIP().toString().c_str());
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"apIp\":\"%s\",", WiFi.softAPIP().toString().c_str());
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"udpTarget\":\"%s\",", udpAddress.toString().c_str());
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"wifiMode\":\"%s\",", wifiModeStr);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"softApEnabled\":%s,", softApEnabled ? "true" : "false");
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"apFallbackActive\":%s,", apFallbackActive ? "true" : "false");
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"apFallbackUptimeMs\":%lu,", apFallbackActive ? (millis() - apFallbackStartedAt) : 0);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"inaAvailable\":%s,", inaAvailable ? "true" : "false");
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"i2cErrors\":%u,", inaI2cErrorCount);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"inaRecoveries\":%u,", inaRecoveryCount);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"dnsFailures\":%u,", dnsFailureCount);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"wifiReconnects\":%u,", wifiReconnectCount);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"displayAvailable\":%s,", displayAvailable ? "true" : "false");
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"updateMode\":%s,", updateMode ? "true" : "false");
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"uptimeMs\":%lu,", millis());
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"freeHeap\":%u,", ESP.getFreeHeap());
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"rssi\":%d,", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "\"resetReason\":\"%s\"", ESP.getResetReason().c_str());
    server.sendContent(buffer);
    server.sendContent("}");
    finishChunkedResponse();
}

bool checkForI2cErrors()
{
    byte errorCode = ina226.getI2cErrorCode();
    if (errorCode)
    {
        LOG_ERROR("I2C error: %u", errorCode);
        switch (errorCode)
        {
        case 1:
            LOG_ERROR("Data too long to fit in transmit buffer");
            break;
        case 2:
            LOG_ERROR("Received NACK on transmit of address");
            break;
        case 3:
            LOG_ERROR("Received NACK on transmit of data");
            break;
        case 4:
            LOG_ERROR("Other error");
            break;
        case 5:
            LOG_ERROR("Timeout");
            break;
        default:
            LOG_ERROR("Can't identify the error");
        }
        inaI2cErrorCount++;
        inaAvailable = false;
        filterInitialized = false;
        inaTelemetryReady = false;
        lastInaRecoveryAttempt = millis();
        LOG_WARN("INA226 marcado como no disponible. Reintentando sin reiniciar MCU.");
        return false;
    }
    return true;
}

void tryRecoverIna(unsigned long currentMillis)
{
    if (inaAvailable)
        return;
    if (currentMillis - lastInaRecoveryAttempt < inaRecoveryIntervalMs)
        return;

    lastInaRecoveryAttempt = currentMillis;
    LOG_WARN("Reintentando inicializacion INA226...");
    recoverI2cBus();

    if (ina226.init())
    {
        configureIna();
        inaAvailable = true;
        inaI2cErrorCount = 0;
        inaRecoveryCount++;
        inaTelemetryReady = false;
        LOG_INFO("INA226 recuperado correctamente");
    }
}

void handlePreUpdate()
{
    updateMode = true;
    updateModeStartedAt = millis();
    if (config.enableLEDs)
    {
        digitalWrite(redLEDPin, LED_ON);
        digitalWrite(greenLEDPin, LED_OFF);
    }

    // Forzamos el encendido del panel para dar feedback visual durante la OTA
    if (displayAvailable)
    {
        powerOnDisplay();
        display.clearDisplay();
        display.setCursor(0, 10);
        display.println("MODO UPDATE");
        display.setCursor(0, 30);
        display.println("Esperando archivo...");
        display.display();
    }

    server.sendHeader("Location", "/update");
    server.send(303);
}

void handleRoot()
{
    char shuntStr[12];
    dtostrf(config.shuntResistance, 6, 4, shuntStr);
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    sendChunkedResponse("text/html");
    server.sendContent("<html><head><meta http-equiv='Content-Type' content='text/html; charset=UTF-8'>");
    server.sendContent("<style>body{font-family:sans-serif;max-width:400px;margin:20px auto;padding:0 10px}label{display:block;margin-top:12px;font-weight:bold}input[type=text],input[type=number],select{width:100%;padding:8px;box-sizing:border-box}input[type=submit]{margin-top:16px;padding:12px;background:#007bff;color:white;border:none;width:100%;cursor:pointer}</style></head><body>");
    server.sendContent("<h2>Configuración INA226</h2><form action='/config' method='POST'>");
    server.sendContent(config.enableDisplay ? "<label><input type='checkbox' name='pantalla' value='1' checked> Pantalla Activa</label>" : "<label><input type='checkbox' name='pantalla' value='1'> Pantalla Activa</label>");
    server.sendContent(config.enableDeepSleep ? "<label><input type='checkbox' name='deepSleep' value='1' checked> Deep Sleep</label>" : "<label><input type='checkbox' name='deepSleep' value='1'> Deep Sleep</label>");
    server.sendContent(config.enableLEDs ? "<label><input type='checkbox' name='leds' value='1' checked> LEDs Activos</label>" : "<label><input type='checkbox' name='leds' value='1'> LEDs Activos</label>");
    server.sendContent("<label>Modo de operacion:</label><select name='preferredMode'>");
    server.sendContent(config.preferredMode == PREF_AUTO ? "<option value='auto' selected>Auto</option>" : "<option value='auto'>Auto</option>");
    server.sendContent(config.preferredMode == PREF_BATTERY ? "<option value='battery' selected>Bateria</option>" : "<option value='battery'>Bateria</option>");
    server.sendContent(config.preferredMode == PREF_MAINTENANCE ? "<option value='maintenance' selected>Mantenimiento</option>" : "<option value='maintenance'>Mantenimiento</option>");
    server.sendContent("</select><p style='font-size:12px;color:#444'>Auto: usa bateria y solo entra a mantenimiento por fallos/forzado RTC. Bateria: siempre ciclo rapido sin AP. Mantenimiento: siempre habilita web/AP para diagnostico.</p>");

    char buffer[256];
    snprintf(buffer, sizeof(buffer), "<label>Tiempo Sleep (seg):</label><input type='number' name='sleepTime' value='%u'>", config.sleepTime);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "<label>Hostname:</label><input type='text' name='hostname' value='%s'>", config.hostname);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "<label>Shunt (Ohm):</label><input type='number' name='shuntResistance' step='0.0001' value='%s'>", shuntStr);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "<label>SignalK Source ID:</label><input type='text' name='sourceId' value='%s'>", config.sourceId);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "<label>SignalK Path:</label><input type='text' name='basePath' value='%s'>", config.basePath);
    server.sendContent(buffer);
    snprintf(buffer, sizeof(buffer), "<label>SignalK Host/IP:</label><input type='text' name='signalKHost' value='%s'>", config.signalKHost);
    server.sendContent(buffer);
    server.sendContent("<input type='submit' value='GUARDAR CONFIGURACIÓN'></form><hr>");
    server.sendContent("<h3>Mantenimiento Rapido</h3>");
    server.sendContent("<form action='/maintenance' method='POST'><input type='hidden' name='enable' value='1'><input type='submit' value='Entrar en mantenimiento ahora (reinicia)'></form>");
    server.sendContent("<form action='/maintenance' method='POST'><input type='hidden' name='enable' value='0'><input type='submit' value='Quitar mantenimiento forzado'></form>");
    server.sendContent("<hr><h3>Actualizar</h3><button onclick=\"window.location.href='/pre-update'\">Modo Actualización</button><hr><h3>Estado</h3><p><a href='/status'>Ver estado JSON</a></p></body></html>");
    finishChunkedResponse();
}

void handleConfig()
{
    Config newConfig = config;
    newConfig.enableDeepSleep = server.hasArg("deepSleep") ? 1 : 0;
    newConfig.enableDisplay = server.hasArg("pantalla") ? 1 : 0;
    newConfig.enableLEDs = server.hasArg("leds") ? 1 : 0;

    const String preferredModeArg = server.arg("preferredMode");
    if (preferredModeArg == "battery")
        newConfig.preferredMode = PREF_BATTERY;
    else if (preferredModeArg == "maintenance")
        newConfig.preferredMode = PREF_MAINTENANCE;
    else if (preferredModeArg == "auto" || preferredModeArg.length() == 0)
        newConfig.preferredMode = PREF_AUTO;
    else
    {
        server.send(400, "text/plain", "preferredMode invalido. Usa auto, battery o maintenance.");
        return;
    }

    const uint16_t newSleepTime = server.arg("sleepTime").toInt();
    if (newSleepTime < 15 || newSleepTime > 300)
    {
        server.send(400, "text/plain", "sleepTime debe estar entre 15 y 300 segundos.");
        return;
    }
    newConfig.sleepTime = newSleepTime;

    const String newHostname = server.arg("hostname");
    if (!isValidHostname(newHostname))
    {
        server.send(400, "text/plain", "hostname invalido. Usa solo letras, numeros y '-' (sin guion al inicio o final).");
        return;
    }
    newHostname.toCharArray(newConfig.hostname, sizeof(newConfig.hostname));

    const float newShunt = server.arg("shuntResistance").toFloat();
    if (!(newShunt > 0.0f && newShunt <= 1.0f))
    {
        server.send(400, "text/plain", "shuntResistance debe estar entre 0.0001 y 1.0 ohmios.");
        return;
    }
    newConfig.shuntResistance = newShunt;

    const String newSourceId = server.arg("sourceId");
    if (!isValidSourceId(newSourceId))
    {
        server.send(400, "text/plain", "sourceId invalido. Usa letras, numeros, '.', '-' o '_'.");
        return;
    }
    newSourceId.toCharArray(newConfig.sourceId, sizeof(newConfig.sourceId));

    const String newBasePath = server.arg("basePath");
    if (!isValidPath(newBasePath))
    {
        server.send(400, "text/plain", "basePath invalido. Usa letras, numeros, '.', '-' o '_'.");
        return;
    }
    newBasePath.toCharArray(newConfig.basePath, sizeof(newConfig.basePath));

    const String newSignalKHost = server.arg("signalKHost");
    if (!isValidSignalKHost(newSignalKHost))
    {
        server.send(400, "text/plain", "signalKHost invalido. Usa IPv4 o hostname valido (ej: lysmarine.local).");
        return;
    }
    newSignalKHost.toCharArray(newConfig.signalKHost, sizeof(newConfig.signalKHost));

    config = newConfig;
    sanitizeConfiguration();
    saveConfiguration();
    server.send(200, "text/plain",
                "Configuracion guardada. El modo de operacion se aplica en el siguiente arranque; hostname y mDNS requieren reinicio.");
}

void handleUpdateStart()
{
    updateMode = true;
    updateModeStartedAt = millis();
    if (config.enableLEDs)
    {
        digitalWrite(greenLEDPin, LED_ON);
        digitalWrite(redLEDPin, LED_ON);
    }
    if (displayAvailable)
    {
        powerOnDisplay();
        display.clearDisplay();
        display.setCursor(0, 10);
        display.println(">> ACTUALIZANDO <<");
        display.setCursor(0, 30);
        display.println("No apagues el");
        display.println("dispositivo...");
        display.display();
    }
}

void handleUpdateEnd()
{
    if (displayAvailable)
    {
        powerOnDisplay();
        display.clearDisplay();
        display.setCursor(0, 10);
        display.println("ACTUALIZADO OK!");
        display.setCursor(0, 30);
        display.println("Reiniciando en");
        display.println("5 segundos...");
        display.display();
    }
    LOG_INFO("Actualizacion: finalizada con exito");
}

void handleStatus()
{
    sendStatusJson();
}

void handleMaintenanceControl()
{
    if (!server.hasArg("enable"))
    {
        server.send(400, "text/plain", "Falta parametro enable (1 o 0).");
        return;
    }

    const String enableArg = server.arg("enable");
    if (enableArg == "1")
    {
        rtcState.forceMaintenance = 1;
        saveRtcState();
        server.send(200, "text/plain", "Mantenimiento forzado en RTC. Reiniciando...");
        delay(500);
        ESP.restart();
        return;
    }

    if (enableArg == "0")
    {
        rtcState.forceMaintenance = 0;
        saveRtcState();
        server.send(200, "text/plain", "Mantenimiento forzado desactivado en RTC.");
        return;
    }

    server.send(400, "text/plain", "Valor enable invalido. Usa 1 o 0.");
}

void setupServer()
{
    server.on("/", HTTP_GET, handleRoot);
    server.on("/config", HTTP_POST, handleConfig);
    server.on("/pre-update", HTTP_GET, handlePreUpdate);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/maintenance", HTTP_POST, handleMaintenanceControl);

    // --- 1. MOSTRAR LA WEB DE SUBIDA ---
    server.on("/update", HTTP_GET, []()
              {
        updateMode = true;
        updateModeStartedAt = millis();
        server.sendHeader("Connection", "close");
        sendChunkedResponse("text/html");
        server.sendContent("<html><head><meta charset='UTF-8'><style>body{font-family:sans-serif;max-width:400px;margin:20px auto;padding:0 10px}input[type=submit]{margin-top:16px;padding:12px;background:#007bff;color:white;border:none;width:100%;cursor:pointer}</style></head><body><h2>Actualizar Firmware</h2><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update' accept='.bin'><br><input type='submit' value='Subir y Actualizar'></form></body></html>");
        finishChunkedResponse(); });

    // --- 2. RECIBIR EL ARCHIVO Y ACTUALIZAR (OTA Personalizado) ---
    server.on("/update", HTTP_POST, []()
              {
        // Esto se ejecuta justo al finalizar la petición web
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
            updateMode = false;
            updateModeStartedAt = 0;
            LOG_ERROR("Fallo OTA: la imagen no se aplico.");
            Update.printError(Serial);
            if (displayAvailable)
            {
                powerOnDisplay();
                display.clearDisplay();
                display.setCursor(0, 10);
                display.println("FALLO OTA");
                display.setCursor(0, 30);
                display.println("Reintenta carga");
                display.display();
            }
            server.send(500, "text/plain", "Fallo en la actualizacion");
        } else {
            server.send(200, "text/plain", "OK. Reiniciando el dispositivo...");
            delay(2000);
            ESP.restart();
        } }, []()
              {
        // Esto se ejecuta DURANTE la subida del archivo, integrando los mensajes en pantalla
        HTTPUpload& upload = server.upload();
        
        if (upload.status == UPLOAD_FILE_START) {
            handleUpdateStart(); 
            uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
            if (!Update.begin(maxSketchSpace)) { 
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) { 
                handleUpdateEnd(); 
            } else {
                Update.printError(Serial);
            }
        }
        yield(); });

    server.begin();
    LOG_INFO("HTTP server started");
}

// ── SETUP (INICIALIZACIÓN DEL SISTEMA) ───────────────────────────────────────


void setup()
{
    const unsigned long setupStartedAt = millis();
    wakeStartedAt = setupStartedAt;

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.begin(74880);
    LOG_INFO("--- Iniciando Firmware: %s (build %u) ---", FIRMWARE_VERSION, FIRMWARE_BUILD);
    EEPROM.begin(512);
    readConfiguration();
    pinMode(greenLEDPin, OUTPUT);
    pinMode(redLEDPin, OUTPUT);
    digitalWrite(redLEDPin, LED_OFF);
    digitalWrite(greenLEDPin, LED_OFF);

    // Verificamos si es un encendido físico o viene de un ciclo de Deep Sleep
    const rst_info *reset_info = system_get_rst_info();
    isFirstBoot = (reset_info->reason != REASON_DEEP_SLEEP_AWAKE);

    if (!loadRtcState())
    {
        LOG_WARN("RTC state invalido. Inicializando estado RTC.");
        resetRtcState();
    }

    if (isFirstBoot)
    {
        LOG_INFO("This is a cold start or a power-on reset.");
        firstBootMillis = millis();
        rtcState.consecutiveStaFailures = 0;
        rtcState.forceMaintenance = 0;
        saveRtcState();
    }
    else
    {
        LOG_INFO("Waking up from deep sleep.");
        if (config.enableLEDs)
        {
            // En modo barco, el despertar del Deep Sleep es siempre a oscuras (sigiloso)
            digitalWrite(greenLEDPin, LED_OFF);
            digitalWrite(redLEDPin, LED_OFF);
        }
    }

    if (config.enableDisplay || isFirstBoot)
    {
        displayAvailable = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
        if (!displayAvailable)
        {
            LOG_WARN("SSD1306 allocation failed");
            LOG_WARN("Continuando sin pantalla OLED.");
        }
        else
        {
            isDisplayPoweredOn = true;
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(0, 0);
            display.print(WiFi.localIP());
            display.setCursor(0, 8);
            display.print(FIRMWARE_VERSION);
            display.print(" #");
            display.println(FIRMWARE_BUILD);
        }
    }
   

    if (config.preferredMode == PREF_MAINTENANCE)
        maintenanceMode = true;
    else if (config.preferredMode == PREF_BATTERY)
        maintenanceMode = false;
    else
        maintenanceMode = (autoMaintenanceOnColdBoot && isFirstBoot) || rtcState.forceMaintenance || rtcState.consecutiveStaFailures >= staFailuresForMaintenance;

    operationMode = maintenanceMode ? MODE_MAINTENANCE : MODE_BATTERY;

    if (maintenanceMode && !isFirstBoot && rtcState.consecutiveStaFailures >= staFailuresForMaintenance)
        LOG_WARN("Entrada en modo mantenimiento por fallos repetidos de STA (%u).", rtcState.consecutiveStaFailures);

    WiFi.persistent(false);
    WiFi.hostname(config.hostname);
    WiFi.mode(WIFI_STA);
    WiFi.begin();

    const bool staConnected = connectStaWithTimeout(
        maintenanceMode ? maintenanceWifiConnectTimeoutMs : batteryWifiConnectTimeoutMs,
        maintenanceMode);

    if (staConnected)
    {
        if (rtcState.consecutiveStaFailures != 0 || rtcState.forceMaintenance)
        {
            rtcState.consecutiveStaFailures = 0;
            rtcState.forceMaintenance = 0;
            saveRtcState();
        }
    }
    else
    {
        if (rtcState.consecutiveStaFailures < 255)
            rtcState.consecutiveStaFailures++;

        if (rtcState.consecutiveStaFailures >= staFailuresForMaintenance)
            rtcState.forceMaintenance = 1;

        saveRtcState();

        if (maintenanceMode)
            LOG_WARN("Fallo de conexion STA en mantenimiento (%u consecutivos).", rtcState.consecutiveStaFailures);
        else
            LOG_WARN("Fallo de conexion en ciclo normal (%u consecutivos). Se omitira envio y se volvera a deep sleep.", rtcState.consecutiveStaFailures);
    }

    // Si no hay red en mantenimiento, preparamos UI para modo AP.
    if (maintenanceMode && WiFi.status() != WL_CONNECTED)
    {
        if (config.enableLEDs && isFirstBoot)
        {
            digitalWrite(greenLEDPin, LED_ON);
            digitalWrite(redLEDPin, LED_ON); // Ambos fijos = Modo AP activo (Atención requerida)
        }

        if (displayAvailable && (config.enableDisplay || isFirstBoot))
        {
            display.clearDisplay();
            display.setTextSize(1);
            display.setCursor(0, 0);
            display.println("Modo AP");
            display.setCursor(0, 10);
            display.println("Conecta a red");
            display.setCursor(0, 20);
            display.println("ShuntConnectAP");
            display.setCursor(0, 30);
            display.println("para configurar");
            display.setCursor(0, 40);
            display.println("la red WiFi");
            display.setCursor(0, 50);
            display.print("IP: ");
            display.println("192.168.4.1");
            display.display();
        }
    }

    // En mantenimiento permitimos WiFiManager/AP fallback; en bateria no.
    if (maintenanceMode && WiFi.status() != WL_CONNECTED)
    {
        const bool wifiConnected = wifiManager.autoConnect("ShuntConnectAP", "12345678");
        if (!wifiConnected)
        {
            LOG_WARN("WiFiManager timeout. Continuando en modo mantenimiento con AP local.");
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAP("ShuntConnectAP", "12345678");
            apFallbackActive = true;
            apFallbackStartedAt = millis();
        }
        else
        {
            rtcState.consecutiveStaFailures = 0;
            rtcState.forceMaintenance = 0;
            saveRtcState();
        }
    }

    udpAddress = IPAddress(0, 0, 0, 0);
    if (WiFi.status() == WL_CONNECTED)
    {
        resolveUdpTarget(true);
    }
    else
    {
        if (maintenanceMode)
            LOG_WARN("Sin WiFi STA. Se mantiene AP local para mantenimiento.");
        else
            LOG_WARN("Sin WiFi STA en modo bateria. Ciclo sin envio.");
    }

    webServerActive = maintenanceMode || apFallbackActive;
    if (webServerActive)
        setupServer();

    LOG_INFO("tiempo para conectar: %lu", millis() - setupStartedAt);
    if (WiFi.status() == WL_CONNECTED)
    {
        LOG_INFO("WiFi connected");
        LOG_INFO("IP address: %s", WiFi.localIP().toString().c_str());
    }
    else
    {
        LOG_INFO("AP IP address: %s", WiFi.softAPIP().toString().c_str());
    }

    if (displayAvailable && (config.enableDisplay || isFirstBoot))
    {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        if (WiFi.status() == WL_CONNECTED)
            display.print(WiFi.localIP());
        else
            display.print(WiFi.softAPIP());
        display.setCursor(0, 8);
        display.print(FIRMWARE_VERSION);
        display.print(" #");
        display.println(FIRMWARE_BUILD);
        display.display();
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        if (!MDNS.begin(config.hostname))
            LOG_WARN("Error al iniciar mDNS");
        else
            LOG_INFO("mDNS hostname: %s", WiFi.getHostname());
    }
    else
    {
        LOG_WARN("mDNS omitido: no hay conectividad STA.");
    }

    // Aseguramos apagado de luces tras conectar exitosamente
    digitalWrite(greenLEDPin, LED_OFF);
    digitalWrite(redLEDPin, LED_OFF);

    resetWireBus();
    const int maxAttempts = 5;
    const int delayBetweenAttempts = 2000;

    for (int attempt = 1; attempt <= maxAttempts; attempt++)
    {
        LOG_INFO("Intento de inicializacion INA226 #%d", attempt);

        if (ina226.init())
        {
            inaAvailable = true;
            configureIna();
            LOG_INFO("INA226 inicializado correctamente");
            break;
        }
        else
        {
            LOG_WARN("Fallo al inicializar INA226.");
            if (config.enableLEDs && isFirstBoot)
            {
                digitalWrite(redLEDPin, LED_ON);
                digitalWrite(greenLEDPin, LED_OFF);
            }
            if (displayAvailable && (config.enableDisplay || isFirstBoot))
            {
                display.clearDisplay();
                display.setCursor(0, 0);
                display.print("Fallo INA226 #");
                display.println(attempt);
                display.display();
            }
            delay(delayBetweenAttempts);
        }
    }

    if (!inaAvailable)
    {
        LOG_WARN("INA226 no disponible - continuando sin sensor.");
        if (displayAvailable && (config.enableDisplay || isFirstBoot))
        {
            display.clearDisplay();
            display.setTextSize(2);
            display.setCursor(0, 0);
            display.println("NO INA");
            display.setTextSize(1);
            display.setCursor(0, 20);
            display.println("Sin sensor.");
            display.setCursor(0, 30);
            display.println("Configura en:");
            display.setCursor(0, 40);
            if (WiFi.status() == WL_CONNECTED)
                display.println(WiFi.localIP());
            else
                display.println(WiFi.softAPIP());
            display.display();
        }
        digitalWrite(greenLEDPin, LED_OFF);
        digitalWrite(redLEDPin, LED_OFF);
    }
    lastInaRecoveryAttempt = millis();

    udp.begin(udpPort);
}
// ── BUCLE PRINCIPAL (LOOP) ───────────────────────────────────────────────────

void loop()
{
    unsigned long currentMillis = millis();

    if (webServerActive)
        server.handleClient();

    // Protección operativa: cerramos la ventana OTA tras timeout para volver
    // al comportamiento normal (incluyendo posible deep sleep si está habilitado).
    if (updateMode && (currentMillis - updateModeStartedAt >= updateModeTimeoutMs))
    {
        updateMode = false;
        updateModeStartedAt = 0;
        LOG_WARN("Timeout de modo update. Volviendo a operacion normal.");
    }

    if (maintenanceMode && apFallbackActive && !updateMode && (currentMillis - apFallbackStartedAt >= apFallbackTimeoutMs))
    {
        LOG_WARN("Timeout de AP local. Apagando AP fallback para reducir consumo.");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        apFallbackActive = false;
    }

    if (maintenanceMode && WiFi.status() == WL_CONNECTED && apFallbackActive)
    {
        WiFi.softAPdisconnect(true);
        apFallbackActive = false;
    }

    if (maintenanceMode)
    {
        tryRecoverWifi(currentMillis);
        if (WiFi.status() == WL_CONNECTED && udpAddress == IPAddress(0, 0, 0, 0))
            resolveUdpTarget();
    }

    // ── LÓGICA DE LEDs EN MODO ARMARIO/BARCO ─────────────────────────────────
    if (!config.enableLEDs)
    {
        // Forzamos el apagado continuo si están desactivados en web para máximo ahorro
        digitalWrite(greenLEDPin, LED_OFF);
        digitalWrite(redLEDPin, LED_OFF);
    }
    else
    {
        // Solo encendemos LEDs los primeros 30 seg del arranque manual para diagnóstico
        if (isFirstBoot && (currentMillis - firstBootMillis < timeToNormalDisplay))
        {
            if (inaAvailable)
            {
                digitalWrite(greenLEDPin, LED_ON);
                digitalWrite(redLEDPin, LED_OFF);
            }
            else
            {
                digitalWrite(greenLEDPin, LED_OFF);
                digitalWrite(redLEDPin, LED_ON); // D7 encendido fijo si hay fallo al arrancar
            }
        }
        else
        {
            // Pasados 30 seg o si viene de Deep Sleep, modo sigilo absoluto
            digitalWrite(greenLEDPin, LED_OFF);
            digitalWrite(redLEDPin, LED_OFF);
        }
    }

    // ── TIMERS NO BLOQUEANTES DE DESTELLOS DE LED ────────────────────────────
    static unsigned long heartbeatTimer = 0;
    static bool heartbeatOn = false;

    static unsigned long udpFlashTimer = 0;
    static bool udpFlashOn = false;
    static int udpFlashPin = greenLEDPin;

    // Apagado del Heartbeat tras 100ms
    if (heartbeatOn && (currentMillis - heartbeatTimer >= 100))
    {
        digitalWrite(LED_BUILTIN, HIGH);
        heartbeatOn = false;
    }

    // Apagado del destello UDP tras 30ms (Sustituye al antiguo delay(30))
    if (udpFlashOn && (currentMillis - udpFlashTimer >= 30))
    {
        digitalWrite(udpFlashPin, LED_OFF);
        udpFlashOn = false;
    }

    // ── TAREAS DE MEDICIÓN (1 VEZ POR SEGUNDO) ───────────────────────────────
    static unsigned long lastMainTask = 0;
    if (currentMillis - lastMainTask >= 1000)
    {
        lastMainTask = currentMillis;

        if (allowBuiltinHeartbeat && config.enableLEDs)
        {
            digitalWrite(LED_BUILTIN, LOW);
            heartbeatOn = true;
            heartbeatTimer = currentMillis;
        }

        float busVoltage_V = 0.0;
        bool hasFreshInaSample = false;

        tryRecoverIna(currentMillis);

        if (inaAvailable)
        {
            busVoltage_V = ina226.getBusVoltage_V();
            float current_A = ina226.getCurrent_mA() / 1000.0;
            bool i2cOk = checkForI2cErrors();

            // Solo actualizamos filtro y telemetría si la lectura I2C fue válida.
            if (i2cOk && !filterInitialized)
            {
                filteredCurrent_A = current_A;
                filterInitialized = true;
            }
            else if (i2cOk)
            {
                filteredCurrent_A = 0.6 * filteredCurrent_A + 0.4 * current_A;
            }

            if (i2cOk && busVoltage_V > 0.01f)
            {
                hasFreshInaSample = true;
                inaTelemetryReady = true;
            }
            else if (i2cOk)
            {
                LOG_WARN("Lectura INA invalida o no estabilizada (V=%.4f). Se omite envio este ciclo.", busVoltage_V);
            }
        }

        // ── PANTALLA (MÁQUINA DE ESTADOS PARA APAGADO REAL) ──────────────────
        // Controlamos internamente la energía del panel OLED para eliminar consumos residuales
        bool shouldShowDisplay = config.enableDisplay || (isFirstBoot && (currentMillis - firstBootMillis < timeToNormalDisplay));

        if (displayAvailable && !updateMode && shouldShowDisplay)
        {
            // Reencender el panel si venía de un estado apagado
            powerOnDisplay();

            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);

            int screenIndex = (currentMillis / 5000) % 2;

            if (screenIndex == 0)
            {
                display.setCursor(0, 0);
                if (WiFi.status() == WL_CONNECTED)
                    display.print(WiFi.localIP());
                else
                    display.print(WiFi.softAPIP());
                display.setCursor(0, 8);
                display.print(FIRMWARE_VERSION);
                display.print(" #");
                display.println(FIRMWARE_BUILD);
                display.setCursor(0, 56);
                if (maintenanceMode)
                    display.println("MNT: Web/AP config");
                else
                    display.println("BAT: rapido ahorro");

                if (!inaAvailable)
                {
                    display.setTextSize(2);
                    display.setCursor(0, 16);
                    display.println("NO INA");
                    display.setTextSize(1);
                    display.setCursor(0, 48);
                    display.print("B");
                    display.print(isFirstBoot);
                    display.print(" D");
                    display.print(config.enableDisplay);
                    display.print(" L");
                    display.print(config.enableLEDs);
                    display.print(" S");
                    display.println(config.enableDeepSleep);
                }
                else
                {
                    display.setCursor(0, 16);
                    display.setTextSize(2);
                    display.print(busVoltage_V);
                    display.println("V");
                    display.print(filteredCurrent_A, 2);
                    display.println("A");
                    display.setTextSize(1);
                    display.print("B");
                    display.print(isFirstBoot);
                    display.print(" D");
                    display.print(config.enableDisplay);
                    display.print(" L");
                    display.print(config.enableLEDs);
                    display.print(" S");
                    display.print(config.enableDeepSleep);
                    display.print(" T");
                    display.println((currentMillis - firstBootMillis) / 1000);
                }
            }
            else
            {
                display.setCursor(0, 0);
                display.println("--- ESTADO RED ---");
                if (WiFi.status() == WL_CONNECTED)
                {
                    display.print("IP: ");
                    display.println(WiFi.localIP());
                }
                else if (WiFi.getMode() & WIFI_AP)
                {
                    display.print("AP: ");
                    display.println(WiFi.softAPIP());
                }
                else
                {
                    display.println("WiFi: DESCONECTADO");
                }
                display.println("------------------");
                display.print("Host: ");
                display.println(config.signalKHost);

                if (udpAddress != IPAddress(0, 0, 0, 0))
                {
                    display.print("Dest: ");
                    display.println(udpAddress);
                    display.print("Port: ");
                    display.println(udpPort);
                }
                else
                {
                    display.println("Error: Sin DNS");
                }
            }
            display.display();
        }
        else if (displayAvailable && !updateMode && !shouldShowDisplay && isDisplayPoweredOn)
        {
            // Apagado físico del panel OLED para eliminar consumos residuales
            powerOffDisplay();
        }

        // ── PUERTO SERIAL ────────────────────────────────────────────────────
        if (inaAvailable)
        {
            LOG_INFO("Bus Voltage: %.3f V", busVoltage_V);
            LOG_INFO("Current: %.3f A", filteredCurrent_A);
        }
        else
        {
            LOG_WARN("INA226 no disponible - enviando alarma.");
        }

        // ── ENVÍO UDP (DATOS NORMALES O ALARMAS SIGNALK) ─────────────────────
        if (WiFi.status() == WL_CONNECTED && udpAddress != IPAddress(0, 0, 0, 0))
        {
            char udpMessage[1024];

            if (inaAvailable && inaTelemetryReady && hasFreshInaSample)
            {
                // MODO NORMAL: Envío optimizado sin path1 ni path2
                snprintf(udpMessage, sizeof(udpMessage),
                         "{\"updates\":[{"
                         "\"$source\":\"%s\","
                         "\"values\":["
                         "{\"path\":\"%s.voltage\",\"value\":%f},"
                         "{\"path\":\"%s.current\",\"value\":%f}"
                         "]}]}",
                         config.sourceId, config.basePath, busVoltage_V, config.basePath, filteredCurrent_A);
            }
            else if (!inaAvailable)
            {
                // MODO FALLO: Notificación de Alarma estándar de SignalK
                char alarmPath[100];
                snprintf(alarmPath, sizeof(alarmPath), "notifications.%s.sensor_error", config.basePath);

                snprintf(udpMessage, sizeof(udpMessage),
                         "{\"updates\":[{"
                         "\"$source\":\"%s\","
                         "\"values\":[{"
                         "\"path\":\"%s\","
                         "\"value\":{\"state\":\"alarm\",\"message\":\"Sensor INA226 fallando o desconectado\",\"method\":[\"visual\"]}"
                         "}]}]}",
                         config.sourceId, alarmPath);
            }
            else
            {
                LOG_WARN("UDP omitido: esperando primera lectura valida de INA226.");
                goto skipUdpSend;
            }

            udp.beginPacket(udpAddress, udpPort);
            udp.println(udpMessage);

            if (udp.endPacket() && config.enableLEDs)
            {
                // Activación del temporizador de 30ms (no bloqueante)
                udpFlashPin = inaAvailable ? greenLEDPin : redLEDPin;
                digitalWrite(udpFlashPin, LED_ON);
                udpFlashTimer = currentMillis;
                udpFlashOn = true;
            }

        skipUdpSend:
            ;
        }
        else
        {
            LOG_WARN("UDP no enviado: WiFi no conectado o IP no resuelta");
        }

        // ── DEEP SLEEP (MODO AHORRO DE BATERÍA) ──────────────────────────────
        // ATENCIÓN: Solo entra en reposo absoluto si se habilitó explícitamente en la configuración web
        const bool sleepingWindowReady = !isFirstBoot || (currentMillis - firstBootMillis >= timeToNormalDisplay);
        const bool waitingInaStabilization = inaAvailable && !inaTelemetryReady;
        const bool inaStabilizationTimeout = waitingInaStabilization && (currentMillis - wakeStartedAt >= inaReadyTimeoutMs);

        if (config.enableDeepSleep && config.sleepTime > 0 &&
            sleepingWindowReady &&
            !updateMode &&
            (!waitingInaStabilization || inaStabilizationTimeout))
        {
            if (inaStabilizationTimeout)
                LOG_WARN("INA no estabilizo antes del timeout (%lu ms). Entrando en deep sleep.", inaReadyTimeoutMs);

            LOG_INFO("Going to sleep");

            // Si la pantalla debe mostrarse, esperamos para su lectura. Si no, latencia mínima.
            if (displayAvailable && (config.enableDisplay || (isFirstBoot && (currentMillis - firstBootMillis < timeToNormalDisplay))))
            {
                delay(2000);
                powerOffDisplay();
            }
            else
            {
                delay(100);
            }

            ESP.deepSleep(config.sleepTime * 1e6, WAKE_RF_DEFAULT);
        }

    } // Fin del bloque de tareas de 1 segundo
}
