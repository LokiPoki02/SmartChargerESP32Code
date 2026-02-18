#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "secrets.h"

const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASS;

const char* mqtt_server = SECRET_MQTT_SERVER;
const int   mqtt_port = 8883;
const char* mqtt_user = SECRET_MQTT_USER;
const char* mqtt_pass = SECRET_MQTT_PASS;
#define LED 8

// ================= ОБ'ЄКТИ =================
WiFiClientSecure espClient;
PubSubClient client(espClient);
Preferences preferences; // Для збереження налаштувань у флеш-пам'ять

// ================= ЗМІННІ СТАНУ (Як у додатку) =================
String currentMode = "OFF";   // Режими: ON, OFF, AUTO
float cutoffVoltage = 14.4;   // Напруга відсічки за замовчуванням

// Змінні для симуляції датчиків
float sim_v_psu = 14.2;
float sim_v_bat = 11.5;
float sim_current = 0.0;
float sim_temp = 35.0;

unsigned long lastTelemetryTime = 0;

// ================= ФУНКЦІЯ ПІДКЛЮЧЕННЯ ДО WI-FI =================
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

// ================= ОБРОБКА ВХІДНИХ КОМАНД ВІД ДОДАТКУ =================
void callback(char* topic, byte* payload, unsigned int length) {
  String messageTemp;
  for (int i = 0; i < length; i++) {
    messageTemp += (char)payload[i];
  }
  
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(messageTemp);

  // 1. Отримали команду зміни РЕЖИМУ
  if (String(topic) == "charger/cmd/mode") {
    currentMode = messageTemp;
    preferences.putString("mode", currentMode); // Зберігаємо в пам'ять
    
    // Відправляємо фактичний стан назад у додаток (Retained = true)
    client.publish("charger/state/mode", currentMode.c_str(), true);
    Serial.println("Mode changed to: " + currentMode);
  }
  
  // 2. Отримали команду зміни ВІДСІЧКИ
  if (String(topic) == "charger/cmd/cutoff") {
    cutoffVoltage = messageTemp.toFloat();
    preferences.putFloat("cutoff", cutoffVoltage); // Зберігаємо в пам'ять
    
    // Відправляємо фактичний стан назад у додаток (Retained = true)
    char cutoffStr[8];
    dtostrf(cutoffVoltage, 1, 1, cutoffStr);
    client.publish("charger/state/cutoff", cutoffStr, true);
    Serial.println("Cutoff changed to: " + String(cutoffVoltage));
  }
}

// ================= ПІДКЛЮЧЕННЯ ДО MQTT БРОКЕРА =================
void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    // Створюємо унікальний ID клієнта
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    
    espClient.setInsecure(); // Для HiveMQ Cloud потрібен SSL. Це спрощує сертифікати.

    // Заповіт (LWT): Якщо ESP32 зникне, брокер сам відправить "offline"
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass, "charger/status", 1, true, "offline")) {
      Serial.println("connected");
      
      // Ми в мережі!
      client.publish("charger/status", "online", true);
      
      // Публікуємо збережені стани, щоб додаток їх підхопив
      client.publish("charger/state/mode", currentMode.c_str(), true);
      
      char cutoffStr[8];
      dtostrf(cutoffVoltage, 1, 1, cutoffStr);
      client.publish("charger/state/cutoff", cutoffStr, true);

      // Підписуємося на команди від додатку
      client.subscribe("charger/cmd/#");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Ініціалізація пам'яті та читання збережених налаштувань
  preferences.begin("charger", false);
  currentMode = preferences.getString("mode", "OFF");
  cutoffVoltage = preferences.getFloat("cutoff", 14.4);

  setup_wifi();
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // Підтримка зв'язку з брокером

  // ================= СИМУЛЯЦІЯ ДАНИХ ТА ВІДПРАВКА (Кожні 2 секунди) =================
  unsigned long now = millis();
  if (now - lastTelemetryTime > 2000) {
    lastTelemetryTime = now;

    // 1. СИМУЛЯЦІЯ ЛОГІКИ РОБОТИ
    if (currentMode == "OFF") {
      sim_current = 0.0;
      sim_temp -= 0.5; // Охолоджується
      if(sim_temp < 35.0) sim_temp = 35.0;
    } 
    else if (currentMode == "ON" || currentMode == "AUTO") {
      // Симулюємо струм зарядки з невеликими коливаннями
      sim_current = 5.0 + random(-2, 3) / 10.0; 
      
      // Батарея потроху "заряджається" (напруга росте)
      sim_v_bat += 0.05; 
      
      // Температура потроху росте під навантаженням
      sim_temp += 0.2; 

      // Логіка AUTO режиму: якщо зарядилася до відсічки - вимикаємо
      if (currentMode == "AUTO" && sim_v_bat >= cutoffVoltage) {
        currentMode = "OFF";
        preferences.putString("mode", currentMode);
        client.publish("charger/state/mode", "OFF", true); // Кажемо додатку перемкнути кнопку!
        Serial.println("AUTO CUTOFF TRIGGERED! Mode -> OFF");
      }

      // Імітація ЗАХИСТУ ВІД ПЕРЕГРІВУ (як у вашому коді)
      if (sim_temp >= 75.0) {
         currentMode = "OFF";
         preferences.putString("mode", currentMode);
         client.publish("charger/state/mode", "OFF", true);
         Serial.println("OVERHEAT PROTECT TRIGGERED! Mode -> OFF");
      }
    }

    // Легкі коливання напруги блоку живлення (для реалістичності)
    sim_v_psu = 14.2 + random(-1, 2) / 10.0;
    
    float sim_power = sim_v_bat * sim_current;

    // 2. ВІДПРАВКА ДАНИХ ПО MQTT (ТЕЛЕМЕТРІЯ)
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

    Serial.println("Telemetry sent...");
  }
}