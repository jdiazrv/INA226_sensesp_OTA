#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <unity.h>

void test_basic_math_is_stable()
{
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

void test_string_literal_not_empty()
{
    const char *name = "INA226";
    TEST_ASSERT_TRUE(name[0] != '\0');
}

void setup()
{
    delay(1000);
    UNITY_BEGIN();
    RUN_TEST(test_basic_math_is_stable);
    RUN_TEST(test_string_literal_not_empty);
    UNITY_END();
}

void loop()
{
}
