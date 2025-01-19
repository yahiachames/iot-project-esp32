#include <WiFi.h>
#include <HTTPClient.h>
#include <FS.h>
#include <LittleFS.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <PicoMQTT.h>
#include <ArduinoJson.h>
#include <esp_random.h>

// Wi-Fi credentials
const char* ssid = "wifi name";
const char* password = "wifi password";

// Camera endpoint
const char* camera_url = "http://192.168.100.19/capture";
const char* azure_blob_base_url = "base blob url";
const char* saskey = "sas_key";

// File to save photo from camera
#define FILE_PHOTO_PATH "/photo.jpg"

// NTP Setup
const char* timeServer = "pool.ntp.org";
WiFiUDP udp;
NTPClient timeClient(udp, timeServer, 0);  // GMT offset 0, adjust as needed

// MQTT settings
PicoMQTT::Server mqttBroker;

// Generate UUID for unique RowKey in Azure Table Storage
String generateUUID() {
  char uuid[37];
  snprintf(uuid, sizeof(uuid), "%08X-%04X-%04X-%04X-%012X",
           esp_random(), esp_random() & 0xFFFF,
           esp_random() & 0x0FFF | 0x4000,
           esp_random() & 0x3FFF | 0x8000,
           esp_random());
  return String(uuid);
}

// Get Current Month as PartitionKey
String getCurrentMonth() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buffer[8];
  strftime(buffer, sizeof(buffer), "%Y%m", &timeinfo);  // Format: YYYYMM
  return String(buffer);
}

// Send Data to Azure Table Storage
void sendToAzureTable(const char* type, const char* value_str) {
  const String partitionKey = getCurrentMonth();
  const String rowKey = generateUUID();
  const float value_d = atof(value_str);
  const char* tableEndpoint = "https://planthealthcareiot.table.core.windows.net/tempHumidty";

  HTTPClient http;
  http.begin(String(tableEndpoint) + "?" + String(saskey));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json;odata=nometadata");
  http.addHeader("x-ms-version", "2020-04-08");

  String payload = "{"
                   "\"PartitionKey\": \""
                   + partitionKey + "\","
                                    "\"RowKey\": \""
                   + rowKey + "\","
                              "\"Topic\": \""
                   + String(type) + "\","
                                    "\"Value\": "
                   + String(value_d, 2) + "}";

  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Data sent to Azure!");
    Serial.println(response);
  } else {
    Serial.printf("Error sending data. HTTP code: %d\n", httpResponseCode);
  }
  http.end();
}

// Initialize Wi-Fi
void initWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting...");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  }
  Serial.println("Connected to WiFi!");
}

String getDateTime() {
  timeClient.update();
  time_t now = timeClient.getEpochTime();
  struct tm* timeinfo = localtime(&now);
  char date[11];
  strftime(date, sizeof(date), "%Y-%m-%d", timeinfo);  // Format: yyy-mm-dd
  char time[9];
  strftime(time, sizeof(time), "%H_%M_%S", timeinfo);  // Format: HH_MM_SS

  String azure_url = String(azure_blob_base_url) + date + "/image_" + date + "_" + time + ".jpeg";


  Serial.println(azure_url);
  return azure_url;
}

bool fetchImageFromCamera() {
  HTTPClient http;
  http.begin(camera_url);

  // Set a longer timeout for the HTTPClient
  http.setTimeout(60000);  // 60 seconds

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    File file = LittleFS.open(FILE_PHOTO_PATH, FILE_WRITE);
    if (!file) {
      Serial.println("Failed to open file for writing");
      http.end();
      return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    const size_t chunkSize = 4 * 1024;  // 4 KB chunk size
    uint8_t* buffer = (uint8_t*)malloc(chunkSize);
    if (!buffer) {
      Serial.println("Failed to allocate memory for buffer");
      file.close();
      http.end();
      return false;
    }

    size_t bytesRead;
    size_t totalBytes = 0;
    while (stream->connected() && (bytesRead = stream->read(buffer, chunkSize)) > 0) {
      file.write(buffer, bytesRead);
      totalBytes += bytesRead;
      Serial.printf("Bytes read: %zu, Total bytes: %zu\n", bytesRead, totalBytes);
    }

    free(buffer);
    file.close();
    Serial.printf("Image saved to LittleFS with size: %zu bytes\n", totalBytes);

    http.end();
    return true;
  } else {
    Serial.printf("Failed to fetch image from camera. HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }
}

// Function to upload image to Azure Blob Storage

bool uploadToAzureBlob() {
  File file = LittleFS.open(FILE_PHOTO_PATH, FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return false;
  }

  HTTPClient http;
  const size_t chunkSize = 28 * 1024;  // 28 KB chunk size
  uint8_t* buffer = (uint8_t*)malloc(chunkSize);
  if (!buffer) {
    Serial.println("Failed to allocate memory for buffer");
    file.close();
    return false;
  }

  size_t bytesRead;
  size_t totalBytesUploaded = 0;

  // Get the current date and time for the `x-ms-date` header
  time_t now = time(NULL);
  struct tm* timeinfo = gmtime(&now);  // Use GMT/UTC time
  char dateBuffer[50];
  strftime(dateBuffer, sizeof(dateBuffer), "%a, %d %b %Y %H:%M:%S GMT", timeinfo);

  const int maxRetries = 3;  // Maximum retries for each chunk
  String azure_blob_url = getDateTime() ;
  // Check if the blob exists by sending a HEAD request
  http.begin(azure_blob_url + "?" + saskey);  // Initialize with the blob URL (without comp=appendblock)

  int httpCode = http.sendRequest("HEAD");
  if (httpCode == 404) {
    // If the blob does not exist, create it using a PUT request (without comp=appendblock)
    Serial.println("Blob does not exist. Creating the blob...");
    http.begin(azure_blob_url + "?" + saskey );  
    Serial.println(azure_blob_url + "?" + saskey);// Reinitialize with the same URL for creation
    http.addHeader("Content-Type", "application/octet-stream");
   

    http.addHeader("x-ms-blob-type", "AppendBlob");
    http.addHeader("x-ms-version", "2015-02-21");
    http.addHeader("Content-Length", "0");  // No content for creating the blob

    httpCode = http.sendRequest("PUT");  // Create the blob
    if (httpCode == HTTP_CODE_CREATED) {
      Serial.println("Blob created successfully.");
    } else {
      Serial.printf("Failed to create the blob. HTTP error: %d\n", httpCode);
      String responseBody = http.getString();
      Serial.println("Response body:");
      Serial.println(responseBody);
      free(buffer);
      file.close();
      return false;  // Exit if blob creation fails
    }
  } else if (httpCode != HTTP_CODE_OK) {
    // If the blob exists but some other error occurs, print the error and exit
    Serial.printf("Error: Unable to access the blob. HTTP error: %d\n", httpCode);
    String responseBody = http.getString();
    Serial.println("Response body:");
    Serial.println(responseBody);
    free(buffer);
    file.close();
    return false;
  }

  // Now proceed with uploading the chunks after ensuring the blob exists
  while ((bytesRead = file.read(buffer, chunkSize)) > 0) {
    bool success = false;
    for (int attempt = 1; attempt <= maxRetries; attempt++) {
      String appendBlobUrl = String(azure_blob_url) + "?comp=appendblock&" + saskey;  // Add comp=appendblock for append operation

      http.begin(appendBlobUrl);  // Use the URL with comp=appendblock for appending

      // Add the required headers for appending
      http.addHeader("Content-Type", "application/octet-stream");
   
      http.addHeader("x-ms-blob-type", "AppendBlob");
     // http.addHeader("x-ms-version", "2015-02-21");
   //   http.addHeader("x-ms-date", dateBuffer);  // Add the current UTC date
      http.addHeader("x-ms-blob-condition-appendpos", String(totalBytesUploaded));  // Set the append position to the total size uploaded so far
     http.addHeader("Content-Length", String(bytesRead));  // Set Content-Length to bytesRead for the actual chunk size

      // Debug: Log header values to ensure correctness
      Serial.printf("Uploading chunk of size: %zu bytes\n", bytesRead);
      Serial.printf("Content-Length: %zu\n", bytesRead);

      int httpResponseCode = http.sendRequest("PUT", buffer, bytesRead);

      // Log HTTP response and content
      String responseBody = http.getString();
      Serial.printf("HTTP response code: %d\n", httpResponseCode);
      Serial.println("Response body:");
      Serial.println(responseBody);

      if (httpResponseCode == HTTP_CODE_CREATED) {
        Serial.printf("Uploaded chunk %d (attempt %d) successfully.\n", totalBytesUploaded, attempt);
        totalBytesUploaded += bytesRead;  // Increment total bytes uploaded
        success = true;
        break;
      } else {
        Serial.printf("Error uploading chunk (attempt %d). HTTP error: %d\n", attempt, httpResponseCode);
      }
    }
    if (!success) {
      Serial.println("Failed to upload chunk after multiple attempts.");
      free(buffer);
      file.close();
      return false;
    }
  }

  free(buffer);
  file.close();
  return true;
}




void setup() {
  Serial.begin(115200);
  LittleFS.begin();
  initWiFi();

  timeClient.begin();
  timeClient.update();  // Sync time to the current epoch

  fetchImageFromCamera();  // Fetch image from camera
  uploadToAzureBlob();     // Upload image to Azure Blob

       mqttBroker.begin();

    // Subscribe to topics
    mqttBroker.subscribe("dht/humidity", [](const char* topic, const char* payload) {
        float humidity = atof(payload); // Convert payload to float
        Serial.printf("Received message in topic %s: Humidity = %.2f\n", topic, humidity);
        // sendToAzureTable(topic,humidity);
    });

    mqttBroker.subscribe("dht/temp", [](const char* topic, const char* payload) {
        float temperature = atof(payload); // Convert payload to float
        Serial.printf("Receivedd message in topic %s: Temperature = %.2f\n", topic, temperature);
        // sendToAzureTable(topic,temperature);
    });

    // Catch-all subscription for debugging
    mqttBroker.subscribe("#", [](const char* topic, const char* payload) {
        Serial.printf("Catch-all: Topic = %s, Payload = %f\n", topic, payload);
        sendToAzureTable(topic,payload);
    });
}

void loop() {
    mqttBroker.loop(); // Keep the broker running
  // Reattempt image capture and upload periodically
  fetchImageFromCamera();
  uploadToAzureBlob();
  delay(6000);  // Wait for 60 seconds before the next cycle
}
