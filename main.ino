#include <ESP8266WiFi.h>
#include <U8g2lib.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <StreamUtils.h>


// needed for oled
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/14, /* data=*/12, /* reset=*/U8X8_PIN_NONE);  // ESP32 Thing, pure SW emulated I2C
const int displayWidth = 128;
const int displayHeight = 64;

const unsigned long postingInterval = 30 * 1000; // delay between updates, in milliseconds

const String ssid = "<YOUR WIFI SSID>";
const String password = "<YOUR WIFI PASSWORD>";

const String coin = "<COIN NAME>"; // e.g. bitcoin
const String currency = "<CURRENCY NAME>"; // e.g. usd
const unsigned int days = 7; // price history in days


float priceHistory[displayWidth];


void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.println();

  u8g2.begin();
  u8g2.setFontMode(0);  // enable transparent mode, which is faster
  u8g2.setFont(u8g2_font_likeminecraft_te);
  u8g2.setCursor(11, 62);
  u8g2.print("Connecting to WiFi");
  u8g2.sendBuffer();  // transfer internal memory to the display

  WiFi.begin(ssid, password);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
  getHistory();
}


void getHistory() {
  JsonDocument doc;
  JsonDocument filter;
  filter["prices"] = true;
  String serverName = "https://api.coingecko.com/api/v3/coins/" + coin + "/market_chart?vs_currency=" + currency + "&days=" + String(days); // the interval on auto will be hourly

  if (getJsonFromUrl(serverName, doc, DeserializationOption::Filter(filter))) {
    JsonArray prices = doc["prices"];
    float step = (float)prices.size() / (float)displayWidth;

    Serial.println(step);

    int arrayIndex = 0;
    for (float priceIndex = 0; arrayIndex < displayWidth; priceIndex += step ) { // if the display is too short, we still want to display the right most values in the array
      int priceIndexFloored = std::floor(priceIndex);
      float price = String(prices[priceIndexFloored][1]).toFloat();
      priceHistory[arrayIndex] = price;
      arrayIndex++;
    }
  }
}

void loop() {
  // wait for WiFi connection
  if ((WiFi.status() == WL_CONNECTED)) {
    float newPrice = getPrice();
    float oldPrice = priceHistory[displayWidth-1];

    char update = '=';
    if (oldPrice > newPrice) update = '-';
    else if (newPrice > oldPrice) update = '+';

    displayPrice(newPrice, update);
    updatePriceHistory(newPrice);
    displayHistory();

    delay(postingInterval);
  }
}

float getPrice() {
  JsonDocument doc;
  JsonDocument filter;
  filter[coin] = true;
  String serverName = "https://api.coingecko.com/api/v3/simple/price?include_24hr_change=true&ids=" + coin + "&vs_currencies=" + currency + "&precision=full";
  // alternative: "https://api.coinbase.com/v2/prices/BTC-EUR/spot"
  
  if (getJsonFromUrl(serverName, doc, DeserializationOption::Filter(filter))) {
    float newPrice = String(doc[coin][currency]).toFloat();

    Serial.print(coin);
    Serial.print(" = ");
    Serial.printf("%.8g",newPrice);

    return newPrice;
  } else {
    return 0;
  }
}

// this is so that the current price is always the right most value to be seen as a comparison.
void updatePriceHistory(float newPrice) {
  priceHistory[displayWidth-1] = newPrice;
}

void displayPrice(float price, char update) {
  u8g2.clearBuffer();                 // clear the internal memory

  u8g2.setFont(u8g2_font_luBS08_te);
  u8g2.setCursor(0, 8);
  u8g2.print(coin + " in " + currency);  // this value must be lesser than 128 unless U8G2_16BIT is set
  u8g2.setFont(u8g2_font_luBS12_te);
  u8g2.setCursor(128-15, 10);
  u8g2.print(update);
  // u8g2.setFont(u8g2_font_bubble_tr);
  u8g2.setFont(u8g2_font_profont29_mn);
  u8g2.setCursor(0, 30);
  u8g2.printf("%.7g", price);
  u8g2.sendBuffer();  // transfer internal memory to the display
}

void displayHistory() {
  // getting maximum and minimum
  float *coinMin, *coinMax;
  std::tie(coinMin, coinMax) = std::minmax_element(std::begin(priceHistory), std::end(priceHistory));

  float coinDiff = *coinMax - *coinMin;

  int upperPixelY = 30; // 30 is the last height of the text (see function above)
  int pixelDiff = displayHeight - upperPixelY - 1; // 63 is the lowest value 

  int pricesPixels[displayWidth];

  for (int x = 0; x < displayWidth; x++) {
    float current = priceHistory[x];
    int y = std::round((float)(*coinMax - current) / (float)coinDiff * pixelDiff + upperPixelY); // you gotta visualize it

    pricesPixels[x] = y;
  }

  for (int i = 1; i < displayWidth; i++) {
    u8g2.drawLine(i-1, pricesPixels[i-1], i, pricesPixels[i]);
  }
  u8g2.sendBuffer();
}

bool getJsonFromUrl(const String& serverName, JsonDocument& doc, DeserializationOption::Filter filter) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;

  https.useHTTP10(true);

  if (https.begin(*client, serverName)) {
    Serial.printf("[HTTPS] GET... %s\n", serverName.c_str());

    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
      
      DeserializationError error = deserializeJson(doc, https.getStream(), filter);

      if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        https.end();
        return false; // Indicate failure
      }
      https.end();
      return true; // Indicate success!
    } else {
      Serial.printf("[HTTPS] GET failed, error: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.printf("[HTTPS] Unable to connect\n");
  }
  return false; // Indicate failure
}

void printError(String error) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_likeminecraft_te);
  u8g2.setCursor(0, 60);
  u8g2.print(error);
  u8g2.sendBuffer();  // transfer internal memory to the display
  delay(10000);
}
