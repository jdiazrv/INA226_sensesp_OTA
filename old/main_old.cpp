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
const char *udpAddress = "lysmarine.local";
const char *topic = "electrical.batteries.fridge";
const char *host = "lysmarine.local";
char source[30] = "ESP8266.INA226-1";
char basePath[50] = "electrical.batteries.fridge";
const unsigned long timeToNormalDisplay = 30000;
const unsigned long sleepTime = 10e6;
unsigned long startMillis;
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
    uint8_t enableLEDs; // Nueva variable para controlar los LEDs
    uint16_t sleepTime;
    char hostname[16];
};
Config config;

void sendPressureUpdate(IPAddress ip, int port, const char *path, float measure)
{
    char udpMessage[1024];
    sprintf(udpMessage, "{\"updates\":[{\"$source\":\"ESP32.watermakerl\",\"values\":[{\"path\":\"%s\",\"value\":%f}]}]}", path, measure);
    udp.beginPacket(ip, port);
    udp.write(reinterpret_cast<uint8_t *>(udpMessage), strlen(udpMessage));
    udp.endPacket();
}

void readConfiguration()
{
    // Leer la estructura de configuración desde la EEPROM
    for (unsigned int i = 0; i < sizeof(config); ++i)
        *((char *)&config + i) = EEPROM.read(i);

    // Valores predeterminados si es la primera vez o hay datos corruptos
    if (config.enableDisplay > 1)
    {
        Serial.println("asignando valores por defecto");
        config.enableDisplay = 1;
    }
    if (config.enableDeepSleep > 1)
        config.enableDeepSleep = 1;
    if (config.sleepTime > 300 || config.sleepTime < 15)
        config.sleepTime = 15; // 15 segundos como predeterminado
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
    // Escribir la estructura de configuración en la EEPROM
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
        while (1)
        {
        }
    }
}
void handleRoot()
{
    // Leer la configuración actual antes de generar el formulario
    readConfiguration();

    String checkedDisplay = config.enableDisplay ? "checked" : "";
    String checkedDeepSleep = config.enableDeepSleep ? "checked" : "";
    String checkedLEDs = config.enableLEDs ? "checked" : ""; // Estado del checkbox para los LEDs

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
                  checkedLEDs + "><br>" // Campo para los LEDs al final
                                "<input type='submit' value='Guardar'>"
                                "</form></body></html>";

    server.send(200, "text/html", html);
}

void handleConfig()
{
    config.enableDeepSleep = server.hasArg("deepSleep") ? 1 : 0; // Si "deepSleep" está marcado, entonces 1, sino 0
    config.enableDisplay = server.hasArg("pantalla") ? 1 : 0;    // Si "pantalla" está marcado, entonces 1, sino 0
    config.enableLEDs = server.hasArg("leds") ? 1 : 0;           // Si "leds" está marcado, entonces 1, sino 0
    config.sleepTime = server.arg("sleepTime").toInt();          // Convertir el tiempo de sleep a entero
    if (config.sleepTime < 15 || config.sleepTime > 300)
        config.sleepTime = 15; // Validar el rango de sleepTime

    String newHostname = server.arg("hostname");
    newHostname.toCharArray(config.hostname, sizeof(config.hostname) - 1);
    config.hostname[sizeof(config.hostname) - 1] = '\0'; // Asegurar terminación nula

    saveConfiguration(); // Guardar los cambios en la EEPROM
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

// Función para leer la configuración de la EEPROM

// Función para guardar la configuración en la EEPROM

void setup()
{
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
        { // Enciende el LED verde si los LEDs están habilitados

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
        { // Cambia al LED rojo después del reinicio si los LEDs están habilitados
            digitalWrite(greenLEDPin, LOW);
            Serial.println("LED verde apagado");
            digitalWrite(redLEDPin, HIGH);
            Serial.println("LED rojo encendido");
        }
    }
    if (config.enableDisplay || isFirstBoot)
    {
        Serial.println("Pantalla activa");
        // Inicializar la pantalla OLED
        if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
        {
            Serial.println(F("SSD1306 allocation failed"));
            for (;;)
                ; // Bucle infinito si falla la pantalla
        }
        display.clearDisplay();              // Limpia la pantalla para el próximo dibujo
        display.setTextSize(1);              // Tamaño de texto normal
        display.setTextColor(SSD1306_WHITE); // Color del texto
    }
    enableDisplay = config.enableDisplay;
    enableDeepSleep = config.enableDeepSleep;
    hostname = config.hostname; // Convertir char array a String para uso en el código

    if (enableDeepSleep)
    {
        // Configuraciones específicas para deep sleep
    }

    //WiFi.mode(WIFI_AP_STA); // explicitly set mode, esp defaults to STA+AP

    // Encender ambos LEDs para indicar que el proceso de conexión está en curso
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
    display.display(); // Actualiza la pantalla con todo el texto

    // Asigna el hostname configurado en la estructura Config antes de la conexión WiFi
   WiFi.hostname(hostname); // Set the device hostname
   // wifiManager.setTimeout(120);
    if (!wifiManager.autoConnect("ShuntConnectAP", "12345678"))
    {
        Serial.println("failed to connect and hit timeout");
        delay(500);
        // reset and try again, or maybe put it to deep sleep
        ESP.restart();
    }
    setupServer();
    Serial.print("tiempo para conectar: ");
    Serial.println(millis() - startMillis);
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
        // En el primer arranque, mantener el LED rojo encendido
        digitalWrite(greenLEDPin, LOW);
        Serial.println("LED verde apagado");
    }
    else
    {
        // En arranques sucesivos, apagar el LED rojo y dejar el verde encendido
        digitalWrite(redLEDPin, LOW);
        Serial.println("LED rojo apagado");
    }
    Wire.begin();
    const int maxAttempts = 5;             // Número máximo de intentos
    const int delayBetweenAttempts = 2000; // 2 segundos entre intentos
    bool inaInitialized = false;

    for (int attempt = 1; attempt <= maxAttempts; attempt++)
    {
        Serial.print("Intento de inicialización INA226 #");
        Serial.println(attempt);

        if (ina226.init())
        {
            inaInitialized = true;
            Serial.println("INA226 inicializado correctamente");
            break; // Salir del bucle si la inicialización es exitosa
        }
        else
        {
            Serial.println("Fallo al inicializar INA226. Revisar conexiones.");
            if (config.enableLEDs)
            {
                digitalWrite(redLEDPin, HIGH);  // Enciende LED rojo para indicar fallo
                digitalWrite(greenLEDPin, LOW); // Apaga LED verde
                Serial.println("LED rojo encendido para indicar fallo INA226");
            }
            if (config.enableDisplay)
            {
                display.clearDisplay();
                display.setCursor(0, 0);
                display.print("Fallo INA226 #");
                display.println(attempt);
                display.display(); // Mostrar el intento fallido en la pantalla
            }
            delay(delayBetweenAttempts); // Esperar antes del próximo intento
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
            display.display(); // Mostrar mensaje final
        }
        if (config.enableLEDs)
        {
            digitalWrite(redLEDPin, HIGH);  // Mantener LED rojo encendido
            digitalWrite(greenLEDPin, LOW); // Mantener LED verde apagado
            Serial.println("LED rojo encendido antes de reiniciar");
        }
        Serial.println("Esperando 10 segundos antes de reiniciar...");
        delay(10000);  // Pausa de 10 segundos
        ESP.restart(); // Reinicia el ESP8266
    }
    ina226.setAverage(AVERAGE_4);
    ina226.setMeasureMode(TRIGGERED);
    /* Para R002 el valor es 0.002
    para r010 el valoer es 0.010
*/
  //  ina226.setResistorRange(0.002, 10.0); // choose resistor  and gain
    ina226.setResistorRange(0.002, 10.0);
    udp.begin(udpPort);
}

void loop()
{
    // Verificar si han pasado los primeros 5 minutos desde el arranque
    unsigned long currentMillis = millis();
    if (isFirstBoot and (currentMillis - firstBootMillis < timeToNormalDisplay))
    { // 300,000 ms = 5 minutos
        // Manejar el servidor web para permitir la configuración
        server.handleClient();
        if (config.enableLEDs)
        { // Enciende el LED verde si los LEDs están habilitados
            digitalWrite(greenLEDPin, HIGH);
            Serial.println("LED verde encendido");
            digitalWrite(redLEDPin, LOW);
            Serial.println("LED rojo apagado");
        }
    }

    // Procesamiento normal
    float busVoltage_V = 0.0;
    float current_mA = 0.0;
    char udpMessage[1024];
    char path1[100] = "";
    char path2[100] = "";

    // Iniciar la medición con el INA226
    ina226.startSingleMeasurement();
    ina226.readAndClearFlags();
    busVoltage_V = ina226.getBusVoltage_V();
    current_mA = ina226.getCurrent_mA();
    checkForI2cErrors();

    // Formatear las rutas para los mensajes UDP
    snprintf(path1, sizeof(path1), "%s%s", basePath, ".voltage");
    snprintf(path2, sizeof(path2), "%s%s", basePath, ".current");

    // Solo mostrar en la pantalla si está activada
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
        display.display(); // Actualiza la pantalla con todo el texto
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

    // Preparar y enviar el mensaje UDP
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

    udp.beginPacket(udpAddress, udpPort);
    udp.print(udpMessage);
    udp.endPacket();

    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
    digitalWrite(LED_BUILTIN, HIGH);

    // Solo entrar en Deep Sleep si está habilitado y después de timeToNormalDisplay
    if (config.enableDeepSleep && config.sleepTime > 0 && (!isFirstBoot || (currentMillis - firstBootMillis >= timeToNormalDisplay)))
    {
        Serial.println("Going to sleep");
        delay(2000);
        display.ssd1306_command(SSD1306_DISPLAYOFF);            // Apagar la pantalla si está activada
        ESP.deepSleep(config.sleepTime * 1e6, WAKE_RF_DEFAULT); // Convertir segundos a microsegundos
    }

    delay(1000); // Pequeña pausa para estabilizar y no saturar el bucle
}
