#include "Arduino.h"
#include <Encoder.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic

Encoder myEnc(D1, D2);

int channelId = -1;
int channels[8] = { 0,0,0,0,0,0,0,0 };
long lastAnnounceTime = 0;

long oldPosition  = -999;
boolean isButtonPressed = false;
long lastUpdateMillis = 0;

int requireAnnounce = 0;

void handleKey() {
  isButtonPressed = true;
}


void moveToNextChannel(){
  if(channelId == 7){
    channelId = 0;
    return ;
  }
  channelId++;
}


char mqtt_server[40] = "192.168.1.106";
char mqtt_port[6] = "1883";
char device_name[32] = "ESP Dimmer";

const int SUB_BUFFER_SIZE = JSON_OBJECT_SIZE(4) + JSON_ARRAY_SIZE(10);

bool shouldSaveConfig = false;  //flag for saving data

WiFiClient espClient;
PubSubClient client(espClient);
WiFiManager wifiManager;

WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
WiFiManagerParameter custom_device_name("device_name", "device name", device_name, 32);

int lightValues[8] = {0, 0, 0, 0, 0, 0, 0, 0};
int allowAnnounce = 1;

long lastMsg = 0;
char msg[2048];
int value = 0;

// DynamicJsonBuffer jsonBufferSub;

void resetOnDemand(){

  if(!digitalRead(D0)){
    return ;
  }

  Serial.println("Resetting everything");
  wifiManager.resetSettings();
  SPIFFS.format();
  Serial.println("Reboot in 2 seconds");
  delay(2000);
  ESP.restart();
}

bool doReadConfig(){
  if (!SPIFFS.exists("/config.json")) {
    // If Config file does not exist Force the Config page
    Serial.println("doReadConfig: config file does not exist");
    return false;
  }

  Serial.println("Reading config file");
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Failed to load json config file");
    // Force the Config page
    return false;
  }

  Serial.println("Opened config file");
  size_t size = configFile.size();
  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  configFile.readBytes(buf.get(), size);
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());
  json.printTo(Serial);
  Serial.println(" ...");
  if (!json.success()) {
    Serial.println("Failed to parse json config file");
    // Force the Config page
    return false;
  }

  strcpy(mqtt_server, json["mqtt_server"]);
  strcpy(mqtt_port, json["mqtt_port"]);
  strcpy(device_name, json["device_name"]);
  Serial.println("Json parsed and configuration loaded from file");
}

void doSaveConfig(){
  //save the custom parameters to FS
  if (shouldSaveConfig) {

    //read updated parameters
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(device_name, custom_device_name.getValue());

    Serial.println("Saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["device_name"] = device_name;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
      return ;
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
    Serial.println("Config Saved! Wait 5 seconds");
    delay(5000);
    ESP.restart();
  }
}


//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
  doSaveConfig();
}


String macAddress() {
  String formatted = "";
  char mac_address[20];
  WiFi.macAddress().toCharArray(mac_address, 20);
  for(int i = 0; i < 17; i++){
    if(i == 2 || i == 5 || i == 8 || i == 11 || i == 14){
      continue;
    }
    formatted = formatted + mac_address[i];
  }
  return formatted;
}

/*
char[] macAsConstChar(){
  char mac_address[20];
  WiFi.macAddress().toCharArray(mac_address, 20);
  return mac_address;
}
*/

void announce(){
  if( allowAnnounce == 0){
    return ;
  }
  lastMsg = millis();

  String mac_address = macAddress();
  char message[2048];
  DynamicJsonBuffer jsonBufferPub;

  JsonObject& json = jsonBufferPub.createObject();
  JsonArray& lights = json.createNestedArray("lights");

  Serial.println(mac_address);
  json["mac_address"] = mac_address;
  json["device_name"] = device_name;
  // json["subscribed_to"] = "/lights/" + WiFi.macAddress();

  for(int i = 1; i < 9; i++){
    lights.add(channels[i - 1]);
  }

  json.printTo(message);
  Serial.print("Publish message: ");
  Serial.println(message);
  client.publish("/device/announcement", message);
}


void notifyChanges(){

  lastMsg = millis();

  String mac_address = macAddress();
  char message[2048];
  DynamicJsonBuffer jsonBufferPub;

  JsonObject& json = jsonBufferPub.createObject();
  JsonArray& lights = json.createNestedArray("lights");

  Serial.println(mac_address);
  json["mac_address"] = mac_address;
  json["device_name"] = device_name;

  for(int i = 1; i < 9; i++){
    lights.add(channels[i - 1]);
  }

  json.printTo(message);
  Serial.print("NotifyChanges: ");
  Serial.println(message);
  client.publish("/controllers/", message);

}

void updatePinValues(){
  for(int i = 1; i < 8; i++){
    int pinNumber = 1;
    switch(i){
      case 1: pinNumber = 5; break;
      case 2: pinNumber = 4; break;
      case 3: pinNumber = 0; break;
      case 4: pinNumber = 2; break;
      case 5: pinNumber = 14; break;
      case 6: pinNumber = 12; break;
      case 7: pinNumber = 13; break;
      case 8: pinNumber = 15; break;
    }

    // pins[i - 1].setLevel(lightValues[i]);

    analogWrite(pinNumber, lightValues[i]);
  }
}

void mqttMessageCallback(char* topicParam, byte* payloadParam, unsigned int length) {
  allowAnnounce = 0;
  /*char topic[255];
  strcpy(topic, topicParam);
  */
  if( length > 254 ){
    return ;
  }

  char payload[255] = "";
  strncpy(payload, (char *)payloadParam, length);

  Serial.print("Payload: ");
  Serial.print(payload);

  StaticJsonBuffer<SUB_BUFFER_SIZE> jsonBufferSub;
  JsonObject& jsonPayload = jsonBufferSub.parseObject(payload);
  Serial.print(" - Parsed: ");
  jsonPayload.printTo(Serial);
  Serial.println("***");


  // TODO:: READ WHO SENT THE MESSAGE. IF IT'S MYSELF, IGNORE IT.
  if((String) topicParam == (String) "/controllers/"){
    JsonVariant sender = jsonPayload["mac_address"];
    if(!sender.success()){
      Serial.print("No sender. Reject.");
      return ;
    }

    Serial.print("My MAC: ");
    Serial.println(macAddress());
    Serial.print("Sender: ");
    Serial.println(sender.as<char*>());

    if(macAddress() == sender.as<char*>()){
      Serial.print("My own message. Reject.");
      return ;
    }

    Serial.print("Good message from ");
    Serial.println(sender.as<char*>());
    JsonArray& values = jsonPayload["lights"];

    if(!values.success()){
      Serial.println("No lights section.");
      return ;
    }

    for(int i = 0; i < values.size(); i++){
      channels[i] = values[i];
    }
    myEnc.write(channels[channelId]);
    return ;
  }

  Serial.print("Not a /controllers/ topic. Received in topic ");
  Serial.print(topicParam);

  allowAnnounce = 1;
  updatePinValues();
  // announce();
  return ;
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());

  Serial.println(myWiFiManager->getConfigPortalSSID());
}



void reconnect() {
  // Loop until we're reconnected
  // String topic = "/controllers/" + WiFi.macAddress();
  String topic = "/controllers/";
  char topicBuffer[255];
  topic.toCharArray(topicBuffer, 255);

  char mac_address[20];
  WiFi.macAddress().toCharArray(mac_address, 20);


  int tryes = 0;
  while (!client.connected() && tryes < 10) {
    Serial.print("Attempting MQTT connection...");
    tryes++;
    // Attempt to connect
    if (client.connect( mac_address )) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish("outTopic", "hello world");
      // ... and resubscribe
      client.subscribe(topicBuffer);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try #" + String(tryes) + " again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(1600);
    }
  }
}



void setupWifi(){


}

void cycle(){
  resetOnDemand();
  if (!client.connected()) {
    reconnect();
  }

  client.loop();

  long now = millis();
  if (now - lastAnnounceTime > 10000 || now < lastAnnounceTime) {
    lastAnnounceTime = now;
    announce();
  }

}


void setup() {
  Serial.begin(115200);
  Serial.println("Basic Encoder Test:");
  pinMode(D3, INPUT_PULLUP);
  attachInterrupt(D3, handleKey, RISING);

  delay(1000);
  Serial.begin(115200);
  Serial.println("Serial UP!");

  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount FS in Setup");
    delay(10000);
    ESP.restart();
    return ;
  }

  Serial.println("Mounted file system.");

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_device_name);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setAPCallback(configModeCallback);

  resetOnDemand();

  while(!wifiManager.autoConnect("AutoConnectAP", "password")){
    Serial.println("Can not connect to WiFi.");
    delay(2000);
    // return ;
  }

  Serial.println("Wifi up. Try to load device settings from JSON");

  if(!doReadConfig()){
    Serial.println("Either can not read config, or can not connect. Loading portal!");
    wifiManager.startConfigPortal("OnDemandAP");
    Serial.println("Portal loaded");
    return;
  }

  Serial.println("");
  Serial.println(WiFi.macAddress());
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttMessageCallback);
}


void loop() {

  // Call the Home automation cycle
  cycle();

  // software debounce
  if (isButtonPressed && millis() - lastUpdateMillis > 50) {
    isButtonPressed = false;
    lastUpdateMillis = millis();
    moveToNextChannel();
    myEnc.write(channels[channelId]);
  }


  long newPosition = myEnc.read();
  if (newPosition != oldPosition) {

    if(newPosition < 0){
        newPosition = 0;
        myEnc.write(newPosition);
        return ;
    }

    if(newPosition > 100){
        newPosition = 100;
        myEnc.write(newPosition);
        return ;
    }

    oldPosition = newPosition;
    channels[channelId] = newPosition;
    // Serial.println(newPosition);
    for(int i = 0; i < 8; i++){
        Serial.print(channels[i]);
        Serial.print(" ");
    }
    notifyChanges();
    Serial.println(" ");
  }
}
