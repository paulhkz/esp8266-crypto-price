#include <ESP8266WiFi.h>
#include <U8g2lib.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

// --- Configuration ---
// Using const char* instead of String objects to save memory and prevent fragmentation.
const char* ssid = "<YOUR WIFI SSID>";
const char* password = "<YOUR WIFI PASSWORD>";

const char* coin = "bitcoin"; // almost all coins are possible here, basically all the ones available in coingecko
const char* currency = "usd"; // almost all currencies are possible here
const unsigned int days = 31; // you can see up to the last 365 days

// --- OLED Display Setup ---
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/14, /* data=*/12, /* reset=*/U8X8_PIN_NONE);
const int displayWidth = 128;
const int displayHeight = 64;

// --- Global Variables ---
const int totalPrices = days > 90 ? days : days > 1 ? days * 24 : 285;

const unsigned long postingInterval = 60 * 1000; // Delay between updates
float priceHistory[displayWidth]; // Holds the sampled data for the display graph

// --- Forward Declarations ---
void getHistory();
float getPrice();
void displayPrice(float price, char update);
void displayHistory();
void updatePriceHistory(float newPrice);
bool makeApiRequest(const char* url, std::function<bool(WiFiClient& stream)> processStreamCallback);
void printError(const char* error);


void setup() {
  Serial.begin(115200);
  Serial.println();

  // Initialize display
  u8g2.begin();
  u8g2.setFontMode(0);
  u8g2.setFont(u8g2_font_likeminecraft_te);
  u8g2.setCursor(11, 62);
  u8g2.print("Connecting to WiFi");
  u8g2.sendBuffer();

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());

  // Initial data fetch
  getHistory();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    float newPrice = getPrice();
    if (newPrice > 0) { // Only update if we got a valid price
      float oldPrice = priceHistory[displayWidth - 1];

      char update = '=';
      if (oldPrice > newPrice) update = '-';
      else if (newPrice > oldPrice) update = '+';

      displayPrice(newPrice, update);
      updatePriceHistory(newPrice);
      displayHistory();
    }
    delay(postingInterval);
  }
}

/**
 * @brief Streams historical price data from the API without loading the whole JSON into memory.
 */
void getHistory() {
  // Allocate a temporary array on the heap to store all prices from the stream.
  float* fullPriceHistory = new float[totalPrices];
  if (!fullPriceHistory) {
    Serial.println("Failed to allocate memory for fullPriceHistory!");
    printError("Out of Memory!");
    return;
  }

  // Build the URL using snprintf for memory safety.
  char serverName[200];
  snprintf(serverName, sizeof(serverName),
           "https://api.coingecko.com/api/v3/coins/%s/market_chart?vs_currency=%s&days=%u",
           coin, currency, days);

  // This lambda contains the logic to stream the large history response.
  // It captures the variables it needs from the parent scope.
  auto processHistoryStream = [&](WiFiClient& stream) -> bool {
    if (stream.find("\"prices\":[[")) {
      Serial.println("Found prices array, starting to stream...");
      JsonDocument pricePairDoc;
      int priceCount = 0;

      DeserializationError error;

      while (stream.connected() && priceCount < totalPrices && stream.find(",")) {
        if (stream.peek() == '[') {
          error = deserializeJson(pricePairDoc, stream);
          if (!error) {
            fullPriceHistory[priceCount] = pricePairDoc[1].as<float>();
            priceCount++;
          } else {
            Serial.printf("deserializeJson() failed: %s\n", error.c_str());
          }
        }
      }
      Serial.printf("Finished streaming. Got %d prices.\n", priceCount);

      // Sample the full history into the smaller display history array.
      float step = (float)priceCount / (float)displayWidth;
      for (int i = 0, arrayIndex = 0; arrayIndex < displayWidth && i < priceCount; i = std::round((arrayIndex+1) * step) ) {
        priceHistory[arrayIndex] = fullPriceHistory[i];
        Serial.printf("%f ", fullPriceHistory[i]);
        arrayIndex++;
      }
      return true; // Success
    } else {
      Serial.println("Could not find 'prices' array in JSON response.");
      printError("API Error");
      return false; // Failure
    }
  };

  makeApiRequest(serverName, processHistoryStream);

  delete[] fullPriceHistory;
}


/**
 * @brief Gets the current price of the coin.
 * @return The current price as a float, or 0 on failure.
 */
float getPrice() {
  char serverName[200];
  snprintf(serverName, sizeof(serverName),
           "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=%s&precision=full",
           coin, currency);

  float newPrice = 0;

  // This lambda contains the logic to parse the simple current-price response.
  auto processPriceStream = [&](WiFiClient& stream) -> bool {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, stream);
    if (error) {
      Serial.printf("deserializeJson() failed: %s\n", error.c_str());
      return false;
    }
    newPrice = doc[coin][currency].as<float>();
    Serial.printf("%s = %.8g\n", coin, newPrice);
    return true;
  };

  makeApiRequest(serverName, processPriceStream);
  
  return newPrice;
}

/**
 * @brief Handles the HTTPS connection and calls a lambda function to process the response stream.
 * This centralizes the connection logic and removes code duplication.
 * @param url The API endpoint to request.
 * @param processStreamCallback A lambda function that takes a WiFiClient stream and returns true on success.
 * @return True if the request and the stream processing were successful.
 */
bool makeApiRequest(const char* url, std::function<bool(WiFiClient& stream)> processStreamCallback) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  HTTPClient https;
  https.useHTTP10(true);

  bool success = false;
  Serial.printf("[HTTPS] GET... %s\n", url);
  if (https.begin(*client, url)) {
    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
      WiFiClient& stream = https.getStream();
      // Call the provided lambda to process the stream.
      success = processStreamCallback(stream);
    } else {
      Serial.printf("[HTTPS] GET failed, error: %s\n", https.errorToString(httpCode).c_str());
      printError("HTTP Error");
    }
    https.end();
  } else {
    Serial.printf("[HTTPS] Unable to connect\n");
    printError("Connection Failed");
  }
  return success;
}

/**
 * @brief Updates the price history array to show the latest price on the far right.
 */
void updatePriceHistory(float newPrice) {
  // Add the new price at the very end
  priceHistory[displayWidth - 1] = newPrice;
}

void displayPrice(float price, char update) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_luBS08_te);
  u8g2.setCursor(0, 8);
  u8g2.printf("%s in %s", coin, currency);
  u8g2.setFont(u8g2_font_luBS12_te);
  u8g2.setCursor(128 - 15, 10);
  u8g2.print(update);
  u8g2.setFont(u8g2_font_profont29_mn);
  u8g2.setCursor(0, 30);
  u8g2.printf("%.7g", price);
  // The buffer is sent in displayHistory() after the graph is drawn.
}

void displayHistory() {
  float minPrice = priceHistory[0];
  float maxPrice = priceHistory[0];
  for (int i = 1; i < displayWidth; i++) {
    if (priceHistory[i] < minPrice) minPrice = priceHistory[i];
    if (priceHistory[i] > maxPrice) maxPrice = priceHistory[i];
  }

  float priceDiff = maxPrice - minPrice;
  if (priceDiff == 0) priceDiff = 1; // Avoid division by zero if all prices are the same

  int graphBottomY = displayHeight - 1;
  int graphTopY = 32;
  int graphHeight = graphBottomY - graphTopY;

  int prevX = 0;
  int prevY = std::round(((maxPrice - priceHistory[0]) / priceDiff) * graphHeight) + graphTopY;

  for (int x = 1; x < totalPrices; x++) {
    int y = std::round(((maxPrice - priceHistory[x]) / priceDiff) * graphHeight) + graphTopY;
    u8g2.drawLine(prevX, prevY, x, y);
    prevX = x;
    prevY = y;
  }
  u8g2.sendBuffer(); // Send the price and the graph to the display at the same time
}

void printError(const char* error) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_likeminecraft_te);
  u8g2.setCursor(0, 60);
  u8g2.print(error);
  u8g2.sendBuffer();
  delay(20000);
}
