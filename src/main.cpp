#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h> 
#include <PubSubClient.h>
#include <Preferences.h>
#include "secrets.h" 

// ================= ПРОТОТИПИ ФУНКЦІЙ =================
float readVoltage();
float readCurrent();
float readTemp();
void updateDisplayBig(int fanPWM);
void autoModeLogic();
void setup_wifi();
void reconnect();

// ================= НАЛАШТУВАННЯ WI-FI ТА MQTT =================
const char* ssid = SECRET_WIFI_SSID;          
const char* password = SECRET_WIFI_PASS;      
const char* mqtt_server = SECRET_MQTT_SERVER; 
const int   mqtt_port = 8883;
const char* mqtt_user = SECRET_MQTT_USER;     
const char* mqtt_pass = SECRET_MQTT_PASS;

WiFiClientSecure espClient;
PubSubClient client(espClient);
Preferences preferences; 

String currentMode = "OFF";   

// ================= КАЛІБРУВАННЯ =================
const float CURRENT_CALIBRATION = 31.76; 
const float VOLTAGE_RATIO = 4.92; 
const float VCC_REF = 3.30;      
const float ADC_MAX = 4095.0;    

// Температурні пороги
const float TEMP_START = 40.0;     
const float TEMP_FULL = 60.0;      
const float TEMP_CRITICAL = 75.0;  
const float TEMP_RECOVERY = 50.0;  

// Налаштування кулера
const int FAN_MIN_SPEED = 50;     

// Термістор
const float R_SERIES = 1200.0; 
const float R_NOMINAL = 1000.0;
const float BETA = 3950.0; 

// ================= ПІНИ (ESP32-C3) =================
const int PIN_VOLT = 0;    
const int PIN_CURR = 1;    
const int PIN_NTC  = 4;    
const int PIN_FAN  = 7;    
const int PIN_OPTO = 5;    
const int PIN_TACHO = 6;   // <--- Пін зеленого дроту вентилятора (перевірте, який у вас зараз)

// Змінні для тахометра
volatile int pulseCount = 0; // Лічильник імпульсів (volatile обов'язково для переривань)
int rpm = 0;                 // Розраховані оберти
unsigned long lastRpmTime = 0;   

// ================= ЕКРАН =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Розділяємо напруги!
float voltage_supply = 0.0; // Напруга самого блоку живлення (те що міряємо зараз)
float voltage_battery = 0.0; // Напруга на клемах АКБ (алгоритм напишемо пізніше)
float current = 0.0;
float temperature = 0.0;
float power = 0.0;

bool isScreenOn = true;
bool isOverheated = false;
bool isOfflineMode = true;

unsigned long lastDisplayTime = 0;
unsigned long lastButtonTime = 0;
unsigned long lastTelemetryTime = 0;



// ================= ФУНКЦІЇ MQTT =================
void setup_wifi() {
Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int iter = 0;
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(2);
    display.setCursor(15, 20);
    
    // Щоб було зрозуміліше, що це за цифри, можна додати текст:
    display.print("WiFi: "); 
    // Просто передаємо змінну без макросу F()
    display.print(iter); 
    
    display.display();
    Serial.print(".");
    iter++;
    if(iter > 5){
      isOfflineMode = false;
      break;
    }
  }
  
  Serial.println("\nWiFi connected!");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  
  if (String(topic) == "charger/cmd/mode") {
    currentMode = msg;
    preferences.putString("mode", currentMode); 
    client.publish("charger/state/mode", currentMode.c_str(), true); 
    Serial.println("Mode set to: " + currentMode);
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    String clientId = "ESP32C3-Charger-";
    clientId += String(random(0xffff), HEX);
    espClient.setInsecure(); 

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass, "charger/status", 1, true, "offline")) {
      Serial.println("connected");
      client.publish("charger/status", "online", true);
      client.publish("charger/state/mode", currentMode.c_str(), true); 
      client.subscribe("charger/cmd/#");
    } else {
      delay(5000);
    }
  }
}

// ================= СМАРТ АЛГОРИТМ (Затичка) =================
void autoModeLogic() {
  digitalWrite(PIN_OPTO, HIGH); 
}

void IRAM_ATTR tachoInterrupt() {
  pulseCount++;
}
// ================= SETUP =================
void setup() {
Serial.begin(115200);

  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_OPTO, OUTPUT);
  
  // --- ДОДАЄМО НАЛАШТУВАННЯ ТАХОМЕТРА ---
  pinMode(PIN_TACHO, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_TACHO), tachoInterrupt, FALLING);
  // --------------------------------------

  digitalWrite(PIN_OPTO, LOW);

  preferences.begin("charger", false);
  currentMode = preferences.getString("mode", "OFF");

  Wire.begin(8, 9);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println("OLED failed");
  } else {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(2);
    display.setCursor(15, 20);
    display.print(F("Start"));
    display.display();
  }

  analogWrite(PIN_FAN, 255);
  delay(800); 
  analogWrite(PIN_FAN, FAN_MIN_SPEED);

  setup_wifi();
  if(isOfflineMode){
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);
  } 
}

// ================= LOOP =================
void loop() {
  if(isOfflineMode){
      if (!client.connected()) reconnect();
      client.loop(); 
  }


  unsigned long now = millis();



  // 2. Читання датчиків
  voltage_supply = readVoltage(); // Міряємо напругу БЖ
  // voltage_battery = ... // TODO: Тут пізніше буде алгоритм розрахунку напруги батареї!
  
  current = readCurrent();
  temperature = readTemp();
  power = voltage_supply * current; // Поки що рахуємо потужність по напрузі БЖ

  // 3. ЗАХИСТ ТА ЛОГІКА РЕЖИМІВ
  if (temperature >= TEMP_CRITICAL) isOverheated = true;
  else if (temperature <= TEMP_RECOVERY) isOverheated = false;

  if (isOverheated) {
    digitalWrite(PIN_OPTO, LOW); 
  } else {
    if (currentMode == "OFF") {
      digitalWrite(PIN_OPTO, LOW);
    } 
    else if (currentMode == "ON") {
      digitalWrite(PIN_OPTO, HIGH);
    } 
    else if (currentMode == "AUTO") {
      autoModeLogic();
    }
  }

  // === РОЗРАХУНОК ОБЕРТІВ (RPM) ===
  if (now - lastRpmTime >= 1000) {
    // Тимчасово вимикаємо переривання, щоб безпечно скопіювати цифру
    noInterrupts();
    int currentPulses = pulseCount;
    pulseCount = 0; // Скидаємо лічильник для наступної секунди
    interrupts();

    // 2 імпульси на оберт. (Pulses / 2) * 60 секунд = Pulses * 30
    rpm = currentPulses * 30; 
    lastRpmTime = now;
  }
// 4. Логіка КУЛЕРА
  int fanPWM; 
  if (isOverheated || temperature >= TEMP_FULL) {
    fanPWM = 255;
  } 
  else if (temperature >= TEMP_START) {
    fanPWM = map(temperature * 10, TEMP_START * 10, TEMP_FULL * 10, FAN_MIN_SPEED, 255);
  } 
  else {
    fanPWM = FAN_MIN_SPEED; 
  }

  // === ЗАХИСТ ВІД ЗАКЛИНЮВАННЯ ТА KICK-START ===
  // Якщо ми хочемо, щоб кулер крутився (fanPWM > 0), але датчик Холла бачить 0 обертів:
  Serial.println(rpm);
  if (fanPWM > 0 && rpm <50) {
    fanPWM = 255; // Даємо 100% потужності (максимальний крутний момент), щоб зірвати його з місця
    analogWrite(PIN_FAN, fanPWM);
    delay(5000);
  }

  analogWrite(PIN_FAN, fanPWM);

  // 5. Оновлення екрана
  if (isScreenOn && (now - lastDisplayTime > 250)) {
    lastDisplayTime = now;
    updateDisplayBig(fanPWM);
  }

  // 6. Відправка MQTT Телеметрії
  if (now - lastTelemetryTime > 2000 && isOfflineMode) {
    lastTelemetryTime = now;
    
    char strBuffer[10];
    
    // Відправляємо напругу БЖ
    dtostrf(voltage_supply, 1, 2, strBuffer);
    client.publish("charger/telemetry/v_supply", strBuffer);

    // Відправляємо напругу батареї (поки що там буде 0.00)
    dtostrf(voltage_battery, 1, 2, strBuffer);
    client.publish("charger/telemetry/v_bat", strBuffer);

    dtostrf(current, 1, 2, strBuffer);
    client.publish("charger/telemetry/current", strBuffer);

    dtostrf(power, 1, 0, strBuffer);
    client.publish("charger/telemetry/power", strBuffer);

    dtostrf(temperature, 1, 1, strBuffer);
    client.publish("charger/telemetry/temp", strBuffer);
  }
}

// ================= ФУНКЦІЇ ДАТЧИКІВ =================
float readVoltage() {
  long sum = 0;
  for(int i=0; i<15; i++) sum += analogRead(PIN_VOLT);
  return ((sum / 15.0) / ADC_MAX) * VCC_REF * VOLTAGE_RATIO;
}

float readCurrent() {
  long sum = 0;
  // Робимо 20 замірів з мікропаузами для стабільності
  for(int i=0; i<20; i++) {
    sum += analogRead(PIN_CURR);
    delay(1);
  }
  
  float vSense = ((sum / 20.0) / ADC_MAX) * VCC_REF;
  
  // 1. Віднімаємо "фантомні" 19 мілівольт похибки (0.019 В)
  vSense = vSense - 0.019; 
  
  // 2. Відсікаємо дрібний шум (все що менше ~0.15 А стає нулем)
  if (vSense < 0.005) {
    return 0.0;
  }
  
  return vSense * CURRENT_CALIBRATION;
}

float readTemp() {
  int raw = analogRead(PIN_NTC);
  if (raw == 0 || raw == 4095) return 25.0; 
  
  float r_ntc = R_SERIES * (ADC_MAX / (float)raw - 1.0);
  float steinhart = r_ntc / R_NOMINAL;      
  steinhart = log(steinhart);                 
  steinhart /= BETA;                          
  steinhart += 1.0 / (25.0 + 273.15); 
  steinhart = 1.0 / steinhart;        
  steinhart -= 273.15;                
  return steinhart;
}

// ================= МАЛЮВАННЯ НА ЕКРАНІ =================
void updateDisplayBig(int fanPWM) {
  display.clearDisplay();
  
  if (isOverheated) {
    display.setTextSize(3);
    display.setCursor(20, 15);
    display.print("ALARM");
    display.setTextSize(2);
    display.setCursor(25, 45);
    display.print("HOT!");
  } 
  else {
    display.setTextSize(3);      
    display.setCursor(0, 0);     
    if(voltage_supply < 10.0) display.print(" "); 
    display.print(voltage_supply, 1);   // Виводимо напругу БЖ на екран  
    display.setTextSize(2);      
    display.setCursor(85, 8);
    display.print("V");

    display.setTextSize(3);      
    display.setCursor(0, 28);    
    if(current < 10.0) display.print(current, 2); 
    else display.print(current, 1);
    
    display.setTextSize(2);      
    display.setCursor(85, 36);
    display.print("A");
  }

  display.setTextSize(1);
  display.setCursor(0, 56);    
  display.print("P:");
  display.print(power, 1); 
  display.print("W   ");
  display.print("T:"); 
  display.print((int)temperature);     
  display.print("C");
  
  if(fanPWM > FAN_MIN_SPEED) display.print("*");
  if(currentMode == "AUTO") display.print(" A"); 

  display.display();
}