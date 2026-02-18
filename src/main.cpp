#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "secrets.h"

// ================= НАЛАШТУВАННЯ WI-FI ТА MQTT =================
const char* ssid = SECRET_WIFI_SSID;          
const char* password = SECRET_WIFI_PASS;      

const char* mqtt_server = SECRET_MQTT_SERVER; 
const int   mqtt_port = 8883;
const char* mqtt_user = SECRET_MQTT_USER;     
const char* mqtt_pass = SECRET_MQTT_PASS;

// ================= ОБ'ЄКТИ =================
WiFiClientSecure espClient;
PubSubClient client(espClient);
Preferences preferences; 

// ================= ЗМІННІ СТАНУ =================
String currentMode = "OFF";   
float cutoffVoltage = 12.0;   

// Змінні для симуляції датчиків
float sim_v_psu = 14.2;
float sim_v_bat = 11.5;
float sim_current = 0.0;
float sim_temp = 35.0;

unsigned long lastTelemetryTime = 0;

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String messageTemp;
  for (int i = 0; i < length; i++) {
    messageTemp += (char)payload[i];
  }
  
  // 1. Отримуємо команду РЕЖИМУ від телефону
  if (String(topic) == "charger/cmd/mode") {
    currentMode = messageTemp;
    preferences.putString("mode", currentMode); 
    
    // Просто підтверджуємо брокеру, що ми отримали команду (щоб телефон знав статус при перепідключенні)
    client.publish("charger/state/mode", currentMode.c_str(), true);
    Serial.println("Mode set by phone: " + currentMode);
  }
  
  // 2. Отримуємо команду ВІДСІЧКИ від телефону
  if (String(topic) == "charger/cmd/cutoff") {
    cutoffVoltage = messageTemp.toFloat();
    preferences.putFloat("cutoff", cutoffVoltage); 
    // МОВЧИМО. Нічого не відправляємо назад у телефон!
    Serial.println("Cutoff set by phone: " + String(cutoffVoltage));
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    
    espClient.setInsecure(); 

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass, "charger/status", 1, true, "offline")) {
      Serial.println("connected");
      client.publish("charger/status", "online", true);
      
      // При старті публікуємо лише режим (щоб кнопка в додатку стала правильно)
      client.publish("charger/state/mode", currentMode.c_str(), true);
      
      // ВІДСІЧКУ НЕ ПУБЛІКУЄМО! Телефон сам знає, де стоїть його повзунок.

      client.subscribe("charger/cmd/#");
    } else {
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  preferences.begin("charger", false);
  currentMode = preferences.getString("mode", "OFF");
  cutoffVoltage = preferences.getFloat("cutoff", 12.0);

  setup_wifi();
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); 

  unsigned long now = millis();
  if (now - lastTelemetryTime > 2000) {
    lastTelemetryTime = now;

    // СИМУЛЯЦІЯ (Тільки фізичні показники, жодного втручання в логіку управління)
    if (currentMode == "OFF") {
      sim_current = 0.0;
      sim_temp -= 0.5; 
      if(sim_temp < 35.0) sim_temp = 35.0;
    } 
    else { // Режими ON та AUTO
      sim_current = 5.0 + random(-2, 3) / 10.0; 
      sim_v_bat += 0.05; 
      sim_temp += 0.2; 
      // Всі автоматичні вимкнення ПРИБРАНО. Плата слухає тільки телефон.
    }

    sim_v_psu = 14.2 + random(-1, 2) / 10.0;
    float sim_power = sim_v_psu * sim_current;

    char strBuffer[10];
    dtostrf(sim_v_psu, 1, 2, strBuffer);
    client.publish("charger/telemetry/v_psu", strBuffer);

    dtostrf(sim_v_bat, 1, 2, strBuffer);
    client.publish("charger/telemetry/v_bat", strBuffer);

    dtostrf(sim_current, 1, 2, strBuffer);
    client.publish("charger/telemetry/current", strBuffer);

    dtostrf(sim_power, 1, 0, strBuffer);
    client.publish("charger/telemetry/power", strBuffer);

    dtostrf(sim_temp, 1, 1, strBuffer);
    client.publish("charger/telemetry/temp", strBuffer);
  }
}