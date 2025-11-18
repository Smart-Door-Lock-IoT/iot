#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#define WIFI_SSID "POCO F4"
#define WIFI_PASSWORD "123456788"
#define MQTT_BROKER "broker.emqx.io"
#define MQTT_PORT 1883

#define MQTT_BASE_TOPIC "smart-door-lock-iot"

#define LED_FINGERPRINT D5
#define LED_RFID D6
#define LED_DOOR D7

// prototypes
void connect_wifi();
void on_message_received(char* topic, byte* payload, unsigned int length);
void reconnect_mqtt_client();

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  Serial.begin(115200);
  pinMode(LED_FINGERPRINT, OUTPUT);
  pinMode(LED_RFID, OUTPUT);
  pinMode(LED_DOOR, OUTPUT);

  connect_wifi();

  client.setServer(MQTT_BROKER, MQTT_PORT);
  client.setCallback(on_message_received);
}

void loop() {
  if (!client.connected()) {
    reconnect_mqtt_client();
  }
  client.loop();
}

void connect_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi!");
}

bool led_fingerprint_state = false;
bool led_rfid_state = false;
bool led_door_state = false;
void on_message_received(char* topic, byte* payload, unsigned int length) {
  char message[24];
  for (unsigned int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  Serial.printf("Message arrived [%s]: %s\n", topic, message);

  if (strcmp(topic, MQTT_BASE_TOPIC "/open-door") == 0) {
    Serial.println("Open door command received");
    led_door_state = !led_door_state;
    digitalWrite(LED_DOOR, led_door_state ? HIGH : LOW);
  } else if (strcmp(topic, MQTT_BASE_TOPIC "/fingerprint-mode") == 0) {
    Serial.println("Fingerprint mode command received");
    led_fingerprint_state = !led_fingerprint_state;
    digitalWrite(LED_FINGERPRINT, led_fingerprint_state ? HIGH : LOW);
  } else if (strcmp(topic, MQTT_BASE_TOPIC "/rfid-mode") == 0) {
    Serial.println("RFID mode command received");
    led_rfid_state = !led_rfid_state;
    digitalWrite(LED_RFID, led_rfid_state ? HIGH : LOW);
  }
}

void reconnect_mqtt_client() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");

    String client_id = "ESP8266Client-";
    client_id += String(random(0xffff), HEX);

    if (client.connect(client_id.c_str())) {
      Serial.println("connected");
      client.subscribe(MQTT_BASE_TOPIC "/#");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(2000);
    }
  }
}