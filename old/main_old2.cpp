#include <ESP8266WiFi.h>
#include <Wire.h>
#include <INA226_WE.h>
#include <WiFiUdp.h>
// #include <DNSServer.h>
#include <ESP8266MDNS.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
extern "C"
{
#include "user_interface.h"
}

// Definiciones para el servidor web
ESP8266WebServer server(80);

// Variables para controlar la lógica de arranque
bool isFirstBoot = true;
unsigned long firstBootMillis;

// Variables para almacenar configuraciones
bool enableDeepSleep = true;
bool enableDisplay = true;
const char *hostname = "INA226_X";

#define SCREEN_WIDTH 128 // ancho de la pantalla OLED
#define SCREEN_HEIGHT 64 // alto de la pantalla OLED
#define OLED_RESET -1    // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define I2C_ADDRESS 0x40

const unsigned int udpPort = 4210;
// FIX 1: udpAddress era const char* - udp.beginPacket() necesita IPAddress.
// Se resuelve con WiFi.hostByName() en setup() y se guarda aquí.
IPAddress udpAddress;
const char *udpHostname = "lysmarine.local"; // hostname a resolver
const char *topic = "electrical.batteries.fridge";
const char *host = "lysmarine.local";
char source[30] = "ESP8266.INA226-1";
char basePath[50] = "electrical.batteries.fridge";
const unsigned long timeToNormalDisplay = 30000; // 30 segundos (el comentario "5 minutos" en el loop era incorrecto)
const unsigned long sleepTime = 10e6;
unsigned long startMillis; // FIX 2: se inicializa al inicio de setup()
WiFiUDP udp;
INA226_WE ina226 = INA226_WE(I2C_ADDRESS);
WiFiManager wifiManager;

// Definición de pines para el LED de dos colores usando pines alternativos
const int greenLEDPin = 12; // GPIO 12 para el LED verde
const int redLEDPin = 13;   // GPIO 13 para el LED rojo

struct Config
{
    uint8_t enableDisplay;
    uint8_t enableDeepSleep;
    uint8_t enableLEDs;
    uint16_t sleepTime;
    char hostname[16];
};
Config config;

void sendPressureUpdate(IPAddress ip, int port, const char *path, float measure)
{
    char udpMessage[256]; // FIX 6: reducido de 1024 a 256, suficiente para el mensaje
    sprintf(udpMessage, "{\"updates\":[{\"$source\":\"ESP32.watermakerl\",\"values\":[{\"path\":\"%s\",\"value\":%f}]}]}", path, measure);
    udp.beginPacket(ip, port);
    udp.write(reinterpret_cast<uint8_t *>(udpMessage), strlen(udpMessage));
    udp.endPacket();
}

void readConfiguration()
{
    for (unsigned int i = 0; i < sizeof(config); ++i)
        *((char *)&config + i) = EEPROM.read(i);

    if (config.enableDisplay > 1)
    {
        Serial.println("asignando valores por defecto");
        config.enableDisplay = 1;
    }
    if (config.enableDeepSleep > 1)
        config.enableDeepSleep = 1;
    if (config.sleepTime > 300 || config.sleepTime < 15)
        config.sleepTime = 15;
    if (strlen(config.hostname) == 0 || strlen(config.hostname) > 15)
        strcpy(config.hostname, "INA226_X");
    if (config.enableLEDs > 1)
        config.enableLEDs = 1;
    Serial.println("Configuracion leida");
    Serial.print("enableDisplay: ");
    Serial.println(config.enableDisplay);
    Serial.print("enableDeepSleep: ");
    Serial.println(config.enableDeepSleep);
    Serial.print("sleepTime: ");
    Serial.println(config.sleepTime);
    Serial.print("hostname: ");
    Serial.println(config.hostname);
    Serial.print("enableLEDs: ");
    Serial.println(config.enableLEDs);
}

void saveConfiguration()
{
    for (unsigned int i = 0; i < sizeof(config); ++i)
        EEPROM.write(i, *((char *)&config + i));
    EEPROM.commit();
}

void checkForI2cErrors()
{
    byte errorCode = ina226.getI2cErrorCode();
    if (errorCode)
    {
        Serial.print("I2C error: ");
        Serial.println(errorCode);
        switch (errorCode)
        {
        case 1:
            Serial.println("Data too long to fit in transmit buffer");
            break;
        case 2:
            Serial.println("Received NACK on transmit of address");
            break;
        case 3:
            Serial.println("Received NACK on transmit of data");
            break;
        case 4:
            Serial.println("Other error");
            break;
        case 5:
            Serial.println("Timeout");
            break;
        default:
            Serial.println("Can't identify the error");
        }
        // FIX 3: while(1){} bloqueaba para siempre - ahora reinicia tras 5s
        delay(5000);
        ESP.restart();
    }
}

void handleRoot()
{
    readConfiguration();

    String checkedDisplay  = config.enableDisplay  ? "checked" : "";
    String checkedDeepSleep = config.enableDeepSleep ? "checked" : "";
    String checkedLEDs     = config.enableLEDs     ? "checked" : "";

    String html = "<html><head><meta http-equiv='Content-Type' content='text/html; charset=UTF-8'></head><body>"
                  "<h2>Configuración del Dispositivo</h2>"
                  "<form action='/config' method='POST'>"
                  "Pantalla Activa:<br><input type='checkbox' name='pantalla' value='1' " +
                  checkedDisplay + "><br>"
                  "Deep Sleep:<br><input type='checkbox' name='deepSleep' value='1' " +
                  checkedDeepSleep + "><br>"
                  "Tiempo de Sleep (segundos):<br><input type='number' name='sleepTime' min='15' max='300' value='" +
                  String(config.sleepTime) + "'><br>"
                  "Hostname:<br><input type='text' name='hostname' value='" +
                  String(config.hostname) + "' maxlength='15'><br>"
                  "LEDs Activos:<br><input type='checkbox' name='leds' value='1' " +
                  checkedLEDs + "><br>"
                  "<input type='submit' value='Guardar'>"
                  "</form></body></html>";

    server.send(200, "text/html", html);
}

void handleConfig()
{
    config.enableDeepSleep = server.hasArg("deepSleep") ? 1 : 0;
    config.enableDisplay   = server.hasArg("pantalla")  ? 1 : 0;
    config.enableLEDs      = server.hasArg("leds")      ? 1 : 0;
    config.sleepTime       = server.arg("sleepTime").toInt();
    if (config.sleepTime < 15 || config.sleepTime > 300)
        config.sleepTime = 15;

    String newHostname = server.arg("hostname");
    newHostname.toCharArray(config.hostname, sizeof(config.hostname) - 1);
    config.hostname[sizeof(config.hostname) - 1] = '\0';

    saveConfiguration();
    server.sendHeader("Content-Type", "text/plain; charset=utf-8");
    server.send(200, "text/plain", "Configuración guardada. Reinicie el dispositivo para aplicar los cambios.");
}

void setupServer()
{
    server.on("/", HTTP_GET, handleRoot);
    server.on("/config", HTTP_POST, handleConfig);
    server.begin();
    Serial.println("HTTP server started");
}

void setup()
{
    startMillis = millis(); // FIX 2: inicializar antes de cualquier otra cosa

    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(74880);
    EEPROM.begin(512);
    readConfiguration();
    pinMode(greenLEDPin, OUTPUT);
    pinMode(redLEDPin, OUTPUT);
    digitalWrite(redLEDPin, LOW);
    digitalWrite(greenLEDPin, LOW);

    const rst_info *reset_info = system_get_rst_info();
    isFirstBoot = (reset_info->reason != REASON_DEEP_SLEEP_AWAKE);

    if (isFirstBoot)
    {
        Serial.println("This is a cold start or a power-on reset.");
        firstBootMillis = millis();
        if (config.enableLEDs)
        {
            digitalWrite(greenLEDPin, HIGH);
            Serial.println("LED verde encendido");
            digitalWrite(redLEDPin, LOW);
            Serial.println("LED rojo apagado");
        }
    }
    else
    {
        Serial.println("Waking up from deep sleep.");
        if (config.enableLEDs)
        {
            digitalWrite(greenLEDPin, LOW);
            Serial.println("LED verde apagado");
            digitalWrite(redLEDPin, HIGH);
            Serial.println("LED rojo encendido");
        }
    }

    if (config.enableDisplay || isFirstBoot)
    {
        Serial.println("Pantalla activa");
        if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
        {
            Serial.println(F("SSD1306 allocation failed"));
            for (;;);
        }
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
    }

    enableDisplay   = config.enableDisplay;
    enableDeepSleep = config.enableDeepSleep;
    hostname        = config.hostname; // FIX 5: apunta al char array de config en RAM - válido mientras config exista

    if (enableDeepSleep)
    {
        // Configuraciones específicas para deep sleep
    }

    digitalWrite(greenLEDPin, HIGH);
    Serial.println("led verde encendido");
    digitalWrite(redLEDPin, HIGH);
    Serial.println("led rojo encendido");

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

    WiFi.hostname(hostname);
    if (!wifiManager.autoConnect("ShuntConnectAP", "12345678"))
    {
        Serial.println("failed to connect and hit timeout");
        delay(500);
        ESP.restart();
    }

    // FIX 1: resolver hostname mDNS a IPAddress una sola vez tras conectar WiFi
    Serial.print("Resolviendo IP de ");
    Serial.println(udpHostname);
    if (!WiFi.hostByName(udpHostname, udpAddress))
    {
        Serial.println("DNS resolution failed - UDP no funcionará esta sesión");
        // No reiniciamos: el dispositivo puede seguir midiendo y mostrando en pantalla
        // aunque no pueda enviar a SignalK
    }
    else
    {
        Serial.print("SignalK IP: ");
        Serial.println(udpAddress);
    }

    setupServer();
    Serial.print("tiempo para conectar: ");
    Serial.println(millis() - startMillis); // FIX 2: ahora startMillis tiene valor válido
    Serial.println("Connected to WiFi");
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Host Name: ");

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Modo Client");
    display.setCursor(0, 0);
    display.println(WiFi.localIP());
    display.setCursor(0, 20);

    if (!MDNS.begin(hostname))
    {
        Serial.println("Error al iniciar mDNS");
        return;
    }
    Serial.println(WiFi.getHostname());

    if (isFirstBoot)
    {
        digitalWrite(greenLEDPin, LOW);
        Serial.println("LED verde apagado");
    }
    else
    {
        digitalWrite(redLEDPin, LOW);
        Serial.println("LED rojo apagado");
    }

    Wire.begin();
    const int maxAttempts = 5;
    const int delayBetweenAttempts = 2000;
    bool inaInitialized = false;

    for (int attempt = 1; attempt <= maxAttempts; attempt++)
    {
        Serial.print("Intento de inicialización INA226 #");
        Serial.println(attempt);

        if (ina226.init())
        {
            inaInitialized = true;
            Serial.println("INA226 inicializado correctamente");
            break;
        }
        else
        {
            Serial.println("Fallo al inicializar INA226. Revisar conexiones.");
            if (config.enableLEDs)
            {
                digitalWrite(redLEDPin, HIGH);
                digitalWrite(greenLEDPin, LOW);
                Serial.println("LED rojo encendido para indicar fallo INA226");
            }
            if (config.enableDisplay)
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

    if (!inaInitialized)
    {
        Serial.println("No se pudo inicializar INA226 después de todos los intentos.");
        if (config.enableDisplay)
        {
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("Fallo INA226");
            display.setCursor(0, 10);
            display.println("Reiniciando...");
            display.display();
        }
        if (config.enableLEDs)
        {
            digitalWrite(redLEDPin, HIGH);
            digitalWrite(greenLEDPin, LOW);
            Serial.println("LED rojo encendido antes de reiniciar");
        }
        Serial.println("Esperando 10 segundos antes de reiniciar...");
        delay(10000);
        ESP.restart();
    }

    ina226.setAverage(AVERAGE_4);
    ina226.setMeasureMode(TRIGGERED);
    /* Para R002 el valor es 0.002
       para r010 el valor es 0.010
    */
    ina226.setResistorRange(0.002, 10.0);
    udp.begin(udpPort);
}

void loop()
{
    unsigned long currentMillis = millis();
    if (isFirstBoot && (currentMillis - firstBootMillis < timeToNormalDisplay))
    {
        server.handleClient();
        if (config.enableLEDs)
        {
            digitalWrite(greenLEDPin, HIGH);
            Serial.println("LED verde encendido");
            digitalWrite(redLEDPin, LOW);
            Serial.println("LED rojo apagado");
        }
    }

    float busVoltage_V = 0.0;
    float current_mA   = 0.0;
    char udpMessage[256]; // FIX 6: reducido de 1024 a 256
    char path1[100] = "";
    char path2[100] = "";

    ina226.startSingleMeasurement();
    ina226.readAndClearFlags();
    busVoltage_V = ina226.getBusVoltage_V();
    current_mA   = ina226.getCurrent_mA();
    checkForI2cErrors();

    snprintf(path1, sizeof(path1), "%s%s", basePath, ".voltage");
    snprintf(path2, sizeof(path2), "%s%s", basePath, ".current");

    if (config.enableDisplay || (isFirstBoot && (currentMillis - firstBootMillis < timeToNormalDisplay)))
    {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 10);
        display.println(WiFi.localIP());
        display.setCursor(0, 20);
        display.setTextSize(2);
        display.print(busVoltage_V);
        display.println("V");
        display.print(current_mA / 1000, 5);
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
        display.display();
    }
    else
    {
        Serial.println("Pantalla desactivada");
    }

    Serial.print("Bus Voltage: ");
    Serial.print(busVoltage_V);
    Serial.println(" V");
    Serial.print("Current: ");
    Serial.print(current_mA / 1000);
    Serial.println(" A");

    sprintf(udpMessage,
            "{"
            "\"updates\":["
            "{"
            "\"$source\":\"INA226-1\","
            "\"values\":["
            "{"
            "\"path\":\"%s\","
            "\"value\":%f"
            "},"
            "{"
            "\"path\":\"%s\","
            "\"value\":%f"
            "}"
            "]"
            "}"
            "]"
            "}",
            path1, busVoltage_V, path2, current_mA / 1000);

    // FIX 1: usar udpAddress (IPAddress resuelto en setup) en vez del hostname string
    // y comprobar WiFi antes de enviar
    if (WiFi.status() == WL_CONNECTED && udpAddress != IPAddress(0, 0, 0, 0))
    {
        udp.beginPacket(udpAddress, udpPort);
        udp.print(udpMessage);
        udp.endPacket();
    }
    else
    {
        Serial.println("UDP no enviado: WiFi no conectado o IP no resuelta");
    }

    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
    digitalWrite(LED_BUILTIN, HIGH);

    if (config.enableDeepSleep && config.sleepTime > 0 && (!isFirstBoot || (currentMillis - firstBootMillis >= timeToNormalDisplay)))
    {
        Serial.println("Going to sleep");
        delay(2000);
        display.ssd1306_command(SSD1306_DISPLAYOFF);
        ESP.deepSleep(config.sleepTime * 1e6, WAKE_RF_DEFAULT);
    }

    delay(1000);
}
