// ======================================================================
// ESP32-S3 SMART HOME HUB - MODERN VERSION (2025)
// Совместим с: ESP-IDF v5.5, ArduinoJson 7.x, ESPAsyncWebServer
// ======================================================================

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "SPIFFS.h"

// ------------------- КОНФИГУРАЦИЯ -------------------
#define HUB_ID 1
#define HTTP_PORT 80
#define WS_PORT 81
#define NODE_TIMEOUT 30000

// Настройки WiFi точки доступа
const char* AP_SSID = "SmartHomeHub-S3";
const char* AP_PASSWORD = "admin1234";
IPAddress localIP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// Структура для ESP-NOW (совместима со старыми узлами)
#pragma pack(push, 1)
typedef struct {
  char command[32];
  uint8_t sender_id;
  uint8_t target_id;
  uint32_t timestamp;
} espnow_message_t;
#pragma pack(pop)

// Данные узла
typedef struct {
  uint8_t id;
  bool online;
  int rssi;
  String lastSeen;
  unsigned long lastUpdate;
  float temperature;
  float humidity;
  bool relay1;
  bool relay2;
  bool led;
} node_data_t;

// ------------------- ГЛОБАЛЬНЫЕ ОБЪЕКТЫ -------------------
AsyncWebServer server(HTTP_PORT);
WebSocketsServer webSocket(WS_PORT);
node_data_t nodes[10];
uint8_t nodeCount = 0;
bool espNowInitialized = false;

// MAC-адреса для примера (замените на реальные)
uint8_t nodeMacs[][6] = {
  {0x9C, 0x9C, 0x1F, 0xC7, 0x2D, 0x94}, // Узел 1
  {0x24, 0x6F, 0x28, 0x8A, 0x10, 0x3C}  // Узел 2
};

// ------------------- ПРОТОТИПЫ ФУНКЦИЙ -------------------
void initESPNow();
void onESPNowRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
void onESPNowSend(const wifi_tx_info_t *info, esp_now_send_status_t status);
bool sendESPNowCommand(uint8_t targetId, const char* command);
void updateNode(uint8_t nodeId, const char* command);
void checkNodeTimeouts();
String getTimeString();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void sendNodeListWS();
void sendNodeUpdateWS(uint8_t nodeId, const char* command);
void sendSystemInfoWS();
void initWebServer();

// ------------------- ESP-NOW ФУНКЦИИ -------------------
void initESPNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Ошибка инициализации ESP-NOW");
    return;
  }
  
  esp_now_register_recv_cb(onESPNowRecv);
  esp_now_register_send_cb(onESPNowSend);
  
  // Добавляем известные узлы как пиров
  esp_now_peer_info_t peerInfo = {};
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  
  for (int i = 0; i < sizeof(nodeMacs)/6; i++) {
    memcpy(peerInfo.peer_addr, nodeMacs[i], 6);
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.printf("❌ Не удалось добавить узел %d\n", i+1);
    }
  }
  
  espNowInitialized = true;
  Serial.println("✅ ESP-NOW инициализирован");
}

void onESPNowRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  if (len < sizeof(espnow_message_t)) return;
  
  espnow_message_t msg;
  memcpy(&msg, data, sizeof(msg));
  
  // Извлекаем MAC-адрес из новой структуры
  const uint8_t* mac = recv_info->src_addr;
  
  // Обновляем данные узла
  updateNode(msg.sender_id, msg.command);
  
  // Отправляем обновление через WebSocket
  sendNodeUpdateWS(msg.sender_id, msg.command);
  
  // Логирование
  Serial.printf("📥 ESP-NOW от ");
  for(int i=0; i<6; i++) Serial.printf("%02X:", mac[i]);
  Serial.printf(" команда: %s\n", msg.command);
}

void onESPNowSend(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("⚠️ ESP-NOW отправка не удалась");
  }
}

bool sendESPNowCommand(uint8_t targetId, const char* command) {
  if (!espNowInitialized) {
    Serial.println("❌ ESP-NOW не инициализирован");
    return false;
  }
  
  if (targetId == 0 || targetId > sizeof(nodeMacs)/6) {
    Serial.printf("❌ Неверный ID узла: %d\n", targetId);
    return false;
  }
  
  espnow_message_t msg;
  strlcpy(msg.command, command, sizeof(msg.command));
  msg.sender_id = HUB_ID;
  msg.target_id = targetId;
  msg.timestamp = millis();
  
  esp_err_t result = esp_now_send(nodeMacs[targetId-1], (uint8_t*)&msg, sizeof(msg));
  
  Serial.printf("📤 Команда к узлу %d: %s (%s)\n", 
                targetId, command, 
                result == ESP_OK ? "OK" : "FAIL");
  return result == ESP_OK;
}

// ------------------- УПРАВЛЕНИЕ УЗЛАМИ -------------------
void updateNode(uint8_t nodeId, const char* command) {
  // Поиск существующего узла
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].id == nodeId) {
      nodes[i].online = true;
      nodes[i].lastUpdate = millis();
      nodes[i].lastSeen = getTimeString();
      
      // Парсим команды-подтверждения
      if (strstr(command, "ACK_ON")) nodes[i].led = true;
      else if (strstr(command, "ACK_OFF")) nodes[i].led = false;
      else if (strstr(command, "ACK_RELAY1_ON")) nodes[i].relay1 = true;
      else if (strstr(command, "ACK_RELAY1_OFF")) nodes[i].relay1 = false;
      else if (strstr(command, "ACK_RELAY2_ON")) nodes[i].relay2 = true;
      else if (strstr(command, "ACK_RELAY2_OFF")) nodes[i].relay2 = false;
      
      return;
    }
  }
  
  // Добавление нового узла
  if (nodeCount < 10) {
    nodes[nodeCount].id = nodeId;
    nodes[nodeCount].online = true;
    nodes[nodeCount].lastUpdate = millis();
    nodes[nodeCount].lastSeen = getTimeString();
    nodeCount++;
    
    Serial.printf("🆕 Новый узел: #%d\n", nodeId);
    Serial.printf("Всего узлов: %d\n", nodeCount);
  }
}

void checkNodeTimeouts() {
  bool changed = false;
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].online && (millis() - nodes[i].lastUpdate > NODE_TIMEOUT)) {
      nodes[i].online = false;
      changed = true;
      Serial.printf("⏰ Узел #%d: таймаут\n", nodes[i].id);
    }
  }
  if (changed) sendNodeListWS();
}

String getTimeString() {
  unsigned long seconds = millis() / 1000;
  uint8_t hours = (seconds / 3600) % 24;
  uint8_t minutes = (seconds / 60) % 60;
  uint8_t secs = seconds % 60;
  
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, minutes, secs);
  return String(buf);
}

// ------------------- WEB SOCKET -------------------
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("🔗 WS[%u] подключен\n", num);
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("IP клиента: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
      sendNodeListWS();
      break;
    }
      
    case WStype_TEXT: {
      // Парсим JSON сообщение
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload, length);
      
      if (error) {
        Serial.printf("❌ Ошибка JSON: %s\n", error.c_str());
        return;
      }
      
      const char* action = doc["action"];
      if (!action) return;
      
      if (strcmp(action, "command") == 0) {
        uint8_t nodeId = doc["nodeId"];
        const char* command = doc["command"];
        if (nodeId && command) {
          bool success = sendESPNowCommand(nodeId, command);
          
          // Отправляем подтверждение клиенту
          JsonDocument response;
          response["type"] = "commandResult";
          response["nodeId"] = nodeId;
          response["command"] = command;
          response["success"] = success;
          
          String json;
          serializeJson(response, json);
          webSocket.sendTXT(num, json);
        }
      }
      else if (strcmp(action, "getSystem") == 0) {
        sendSystemInfoWS();
      }
      break;
    }
      
    case WStype_DISCONNECTED:
      Serial.printf("🔌 WS[%u] отключен\n", num);
      break;
      
    default:
      break;
  }
}

void sendNodeListWS() {
  JsonDocument doc;
  doc["type"] = "nodeList";
  
  JsonArray nodesArray = doc["nodes"].to<JsonArray>();
  for (int i = 0; i < nodeCount; i++) {
    JsonObject node = nodesArray.add<JsonObject>();
    node["id"] = nodes[i].id;
    node["online"] = nodes[i].online;
    node["rssi"] = nodes[i].rssi;
    node["lastSeen"] = nodes[i].lastSeen;
    node["temperature"] = nodes[i].temperature;
    node["humidity"] = nodes[i].humidity;
    node["relay1"] = nodes[i].relay1;
    node["relay2"] = nodes[i].relay2;
    node["led"] = nodes[i].led;
  }
  
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}

void sendNodeUpdateWS(uint8_t nodeId, const char* command) {
  JsonDocument doc;
  doc["type"] = "nodeUpdate";
  doc["nodeId"] = nodeId;
  doc["command"] = command;
  doc["timestamp"] = millis();
  
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}

void sendSystemInfoWS() {
  JsonDocument doc;
  doc["type"] = "system";
  doc["uptime"] = millis() / 1000;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["wifiClients"] = WiFi.softAPgetStationNum();
  
  int online = 0;
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].online) online++;
  }
  doc["nodesOnline"] = online;
  doc["totalNodes"] = nodeCount;
  doc["espNowActive"] = espNowInitialized;
  
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}

// ------------------- ВЕБ-СЕРВЕР -------------------
void initWebServer() {
  // Главная страница
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (SPIFFS.exists("/index.html")) {
      request->send(SPIFFS, "/index.html", "text/html");
    } else {
      // Простая заглушка если index.html не загружен
      String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
      html += "<title>ESP32-S3 Smart Home Hub</title>";
      html += "<style>body{font-family:Arial;padding:20px;}</style>";
      html += "</head><body>";
      html += "<h1>🏠 Умный Дом Хаб (ESP32-S3)</h1>";
      html += "<p><strong>Статус:</strong> ✅ Активен</p>";
      html += "<p><strong>WiFi:</strong> " + String(AP_SSID) + "</p>";
      html += "<p><strong>IP:</strong> " + WiFi.softAPIP().toString() + "</p>";
      html += "<p><strong>Узлов онлайн:</strong> <span id='onlineCount'>0</span></p>";
      html += "<hr>";
      html += "<p>Для полного интерфейса загрузите index.html через esptool:</p>";
      html += "<code>python -m esptool --chip esp32s3 --port COMx write_flash 0x310000 data/index.html</code>";
      html += "</body></html>";
      request->send(200, "text/html", html);
    }
  });
  
  // API: список узлов (JSON)
  server.on("/api/nodes", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray array = doc["nodes"].to<JsonArray>();
    
    for (int i = 0; i < nodeCount; i++) {
      JsonObject node = array.add<JsonObject>();
      node["id"] = nodes[i].id;
      node["online"] = nodes[i].online;
      node["lastSeen"] = nodes[i].lastSeen;
      node["temperature"] = nodes[i].temperature;
      node["humidity"] = nodes[i].humidity;
    }
    
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });
  
  // API: отправка команды
  server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("node", true) && request->hasParam("cmd", true)) {
      int nodeId = request->getParam("node", true)->value().toInt();
      String command = request->getParam("cmd", true)->value();
      
      bool success = sendESPNowCommand(nodeId, command.c_str());
      
      JsonDocument doc;
      doc["success"] = success;
      doc["node"] = nodeId;
      doc["command"] = command;
      doc["timestamp"] = millis();
      
      String json;
      serializeJson(doc, json);
      request->send(200, "application/json", json);
    } else {
      request->send(400, "application/json", "{\"error\":\"Missing parameters\"}");
    }
  });
  
  // API: системная информация
  server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["version"] = "2025.01 Modern";
    doc["chipModel"] = ESP.getChipModel();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["wifiClients"] = WiFi.softAPgetStationNum();
    doc["uptime"] = millis() / 1000;
    
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });
  
  // Статические файлы из SPIFFS
  server.serveStatic("/", SPIFFS, "/");
  
  server.begin();
  Serial.println("✅ HTTP сервер запущен на порту 80");
}

// ------------------- ОСНОВНЫЕ ФУНКЦИИ -------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n" + String(ESP.getChipModel()) + " Smart Home Hub");
  Serial.println("Версия прошивки: 2025.01 Modern (ESP-IDF v5.5)");
  Serial.println("==============================================");
  
  // Инициализация файловой системы
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ Ошибка SPIFFS! Форматируем...");
    SPIFFS.format();
    SPIFFS.begin(true);
  }
  Serial.println("✅ Файловая система SPIFFS");
  
  // Список файлов в SPIFFS (для отладки)
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  Serial.println("📁 Содержимое SPIFFS:");
  while(file){
    Serial.printf("  %s (%d байт)\n", file.name(), file.size());
    file = root.openNextFile();
  }
  
  // Настройка WiFi точки доступа
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.softAPConfig(localIP, gateway, subnet);
  delay(100);
  
  Serial.println("\n✅ WiFi Точка доступа:");
  Serial.print("   SSID:     "); Serial.println(AP_SSID);
  Serial.print("   IP:       "); Serial.println(WiFi.softAPIP());
  Serial.print("   Клиенты:  "); Serial.println(WiFi.softAPgetStationNum());
  
  // Настройка веб-сервера
  initWebServer();
  
  // Настройка WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("✅ WebSocket сервер на порту 81");
  
  // Настройка ESP-NOW
  initESPNow();
  
  Serial.println("\n✅ Хаб готов к работе!");
  Serial.println("   Веб-интерфейс: http://192.168.4.1");
  Serial.println("   WebSocket:     ws://192.168.4.1:81");
  Serial.println("   API:           http://192.168.4.1/api/*");
  Serial.println("   Жду подключения узлов...\n");
}

void loop() {
  webSocket.loop();
  
  // Периодические задачи
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 5000) {
    lastUpdate = millis();
    
    checkNodeTimeouts();
    
    if (webSocket.connectedClients() > 0) {
      // Отправляем обновления только если есть подключенные клиенты
      sendSystemInfoWS();
    }
    
    // Раз в 30 секунд отправляем полный список узлов
    static unsigned long lastFullUpdate = 0;
    if (millis() - lastFullUpdate > 30000) {
      lastFullUpdate = millis();
      if (webSocket.connectedClients() > 0) {
        sendNodeListWS();
      }
    }
  }
  
  delay(10);
}
