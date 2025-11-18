#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <MFRC522.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <string.h>

#include <vector>

#define WIFI_SSID "Tamaristo_Bawah_Barat"
#define WIFI_PASSWORD "Tamaristo123"
#define MQTT_BROKER "broker.emqx.io"
#define MQTT_PORT 1883

#define MQTT_BASE_TOPIC "smart-door-lock-iot"

#define RELAY_PIN D3

// #define DOOR_PIN D1
// #define BUZZER_ALARM_PIN D0

// prototypes
void connect_wifi();
void on_message_received(char* topic, byte* payload, unsigned int length);
void reconnect_mqtt_client();
void read_rfid_card();
// UID storage helpers (defined later)
String uidToHex(const byte* uid, byte size);

// Storage for up to 3 allowed UID slots (index 1..3)
String allowed_uid_slots[4];

// When >0, next scanned UID will be stored into this slot (1..3)
int add_rfid_slot = 0;

// Relay timer (non-blocking). If relay_end != 0, relay is on until millis() >
// relay_end
unsigned long relay_end = 0;

WiFiClient espClient;
PubSubClient client(espClient);
MFRC522 rfid_reader(D2, D1);

void setup() {
  Serial.begin(115200);
  SPI.begin();

  // mount filesystem for storing RFID list
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  } else {
    Serial.println("LittleFS mounted");
  }

  rfid_reader.PCD_Init();

  // load allowed UIDs from per-slot files (rfid_1.txt .. rfid_3.txt)
  {
    unsigned loaded = 0;
    for (int slot = 1; slot <= 3; ++slot) {
      char path[16];
      snprintf(path, sizeof(path), "/rfid_%d.txt", slot);
      File f = LittleFS.open(path, "r");
      if (f) {
        String line = f.readStringUntil('\n');
        line.trim();
        allowed_uid_slots[slot] = line;
        if (line.length()) ++loaded;
        f.close();
      }
    }
    Serial.printf("Loaded %u allowed UID slots\n", loaded);
  }

  // setup relay pin
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // pinMode(DOOR_PIN, OUTPUT);
  // pinMode(BUZZER_ALARM_PIN, OUTPUT);

  connect_wifi();

  client.setServer(MQTT_BROKER, MQTT_PORT);
  client.setCallback(on_message_received);
}

void loop() {
  if (!client.connected()) {
    reconnect_mqtt_client();
  }
  client.loop();
  read_rfid_card();

  // Non-blocking relay timeout handling
  if (relay_end != 0 && millis() > relay_end) {
    digitalWrite(RELAY_PIN, LOW);
    relay_end = 0;
    Serial.println("Relay turned OFF");
  }
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

bool door_state = false;
bool buzzer_alarm_state = false;

// registration slot flag (use `add_rfid_slot` instead)

// Globals to replace function-static state (no `static` usage)
byte last_uid[10] = {0};
byte last_size = 0;
uint8_t no_card_count = 0;
const uint8_t NO_CARD_THRESHOLD = 3;  // consecutive no-card loops to reset

// Helper: format and publish UID to MQTT
void publish_rfid_uid(const byte* uid, byte size) {
  char uid_str[32];
  int pos = 0;
  for (byte i = 0; i < size && pos < (int)sizeof(uid_str) - 3; ++i) {
    pos += snprintf(uid_str + pos, sizeof(uid_str) - pos, "%02X", uid[i]);
  }
  uid_str[pos] = '\0';
  if (client.connected()) {
    client.publish(MQTT_BASE_TOPIC "/rfid", uid_str);
  } else {
    Serial.println("MQTT not connected, cannot publish RFID UID");
  }
}

void on_message_received(char* topic, byte* payload, unsigned int length) {
  char message[24];
  for (unsigned int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  // ensure null-terminated string for safe string operations
  if (length < sizeof(message))
    message[length] = '\0';
  else
    message[sizeof(message) - 1] = '\0';
  Serial.printf("Message arrived [%s]: %s\n", topic, message);

  if (strcmp(topic, MQTT_BASE_TOPIC "/open-door") == 0) {
    // Serial.println("Open door command received");
    // door_state = !door_state;
    // digitalWrite(DOOR_PIN, door_state ? HIGH : LOW);
  } else if (strcmp(topic, MQTT_BASE_TOPIC "/fingerprint-mode") == 0) {
  } else if (strcmp(topic, MQTT_BASE_TOPIC "/rfid-mode") == 0) {
    // payload should be "1", "2", or "3" to select slot to register
    int slot = atoi(message);
    if (slot >= 1 && slot <= 3) {
      add_rfid_slot = slot;
      Serial.printf("RFID registration mode enabled for slot %d\n", slot);
    } else {
      Serial.println(
          "Invalid rfid-mode payload: send 1, 2 or 3 to select slot");
    }
  } else if (strcmp(topic, MQTT_BASE_TOPIC "/buzzer-alarm") == 0) {
    // Serial.println("Buzzer alarm command received");
    // buzzer_alarm_state = !buzzer_alarm_state;
    // digitalWrite(BUZZER_ALARM_PIN, buzzer_alarm_state ? HIGH : LOW);
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

void read_rfid_card() {
  // Check presence first (separate calls for clarity)
  if (!rfid_reader.PICC_IsNewCardPresent()) {
    if (no_card_count < NO_CARD_THRESHOLD) ++no_card_count;
    if (no_card_count >= NO_CARD_THRESHOLD) last_size = 0;
    return;
  }

  if (!rfid_reader.PICC_ReadCardSerial()) {
    // card not readable this loop; treat as transient no-card
    if (no_card_count < NO_CARD_THRESHOLD) ++no_card_count;
    if (no_card_count >= NO_CARD_THRESHOLD) last_size = 0;
    return;
  }

  // card is present and readable; reset no-card counter
  no_card_count = 0;

  // If same UID as last read, skip printing (prevents continuous prints)
  if (rfid_reader.uid.size == last_size && last_size > 0 &&
      memcmp(rfid_reader.uid.uidByte, last_uid, last_size) == 0) {
    return;
  }

  // Save UID to last_uid and print it once
  last_size = rfid_reader.uid.size;
  memcpy(last_uid, rfid_reader.uid.uidByte, last_size);
  for (byte i = 0; i < last_size; ++i) {
    Serial.print(last_uid[i] < 0x10 ? " 0" : " ");
    Serial.print(last_uid[i], HEX);
  }
  Serial.println();

  // If RFID mode enabled, publish UID to MQTT topic (then disable once)
  if (add_rfid_slot > 0) {
    // register UID into selected slot file (overwrite)
    String uid = uidToHex(last_uid, last_size);
    char path[16];
    snprintf(path, sizeof(path), "/rfid_%d.txt", add_rfid_slot);
    File f = LittleFS.open(path, "w");
    if (!f) {
      Serial.println("Failed to open slot file for writing");
    } else {
      f.println(uid);
      f.close();
      allowed_uid_slots[add_rfid_slot] = uid;
      Serial.printf("Saved UID %s to %s\n", uid.c_str(), path);
    }
    add_rfid_slot = 0;  // done
  }

  // Check if UID is in allowed list and print status
  {
    String uid = uidToHex(last_uid, last_size);
    bool allowed = false;
    for (int slot = 1; slot <= 3; ++slot) {
      if (allowed_uid_slots[slot].length() && allowed_uid_slots[slot] == uid) {
        allowed = true;
        break;
      }
    }
    Serial.printf("UID %s => %s\n", uid.c_str(),
                  allowed ? "ALLOWED" : "NOT ALLOWED");
    if (allowed) {
      // Activate relay for 10 seconds (non-blocking)
      digitalWrite(RELAY_PIN, HIGH);
      relay_end = millis() + 10000UL;  // 10 seconds
      Serial.println("Relay activated for 10 seconds");
    }
  }
}

// Helper: convert UID bytes to hex string
String uidToHex(const byte* uid, byte size) {
  char buf[32];
  int pos = 0;
  for (byte i = 0; i < size && pos < (int)sizeof(buf) - 3; ++i) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", uid[i]);
  }
  buf[pos] = '\0';
  return String(buf);
}

// Append UID to file and vector if not already present
// (removed previous append_allowed_uid - now using per-slot files)