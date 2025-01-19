#include <DHT.h>
#include <WiFi.h>
#include <PicoMQTT.h>

// DHT sensor configuration
DHT dht(26, DHT11);

// Wi-Fi credentials
const char* ssid = "wifi name";    // Gateway Wi-Fi SSID
const char* password = "wifi password";  // Gateway Wi-Fi password

// MQTT configuration
const char* mqttBrokerIP = "Gateway local ip"; // Replace with your gateway MQTT broker's IP address
const int mqttPort = 1883;
PicoMQTT::Client mqtt("Gateway local ip"); // MQTT client instance

void setup() {
    Serial.begin(115200);

    // Initialize DHT sensor
    dht.begin();
    delay(2000);

    // Connect to the gateway's Wi-Fi
    Serial.print("Connecting to Wi-Fi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected to Wi-Fi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Connect to the MQTT broker
    mqtt.begin();

    Serial.println("Connected to MQTT broker");
}

void loop() {
  mqtt.loop();
    // Read temperature and humidity from the DHT sensor
    float temp = dht.readTemperature();
    float humidity = dht.readHumidity();

    // Check for valid readings
    if (isnan(temp) || isnan(humidity)) {
        Serial.println("Failed to read from DHT sensor!");
        delay(2000);
        return;
    }

    // Publish temperature and humidity to MQTT broker
    char tempStr[8], humidityStr[8];
    dtostrf(temp, 4, 2, tempStr);      // Convert temperature to string
    dtostrf(humidity, 4, 2, humidityStr); // Convert humidity to string

    mqtt.publish("dht/temp", tempStr);      // Publish temperature to topic "dht/temp"
    mqtt.publish("dht/humidity", humidityStr); // Publish humidity to topic "dht/humidity"

    // Print published values
    Serial.printf("Published: Temperature = %s, Humidity = %s\n", tempStr, humidityStr);

    delay(30000); // Delay before the next reading
}
