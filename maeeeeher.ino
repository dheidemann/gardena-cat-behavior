#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ctype.h>

// WiFi
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

// Open-Meteo weather API
const char* OPEN_METEO_LAT  = "";
const char* OPEN_METEO_LON  = "";
const char* OPEN_METEO_TZ   = "auto";

// Husqvarna / Gardena mower cloud API
const char* HUSQVARNA_APP_KEY    = "";
const char* HUSQVARNA_APP_SECRET = "";

// Action to send when rain is detected.
// Use "PARK_UNTIL_NEXT_TASK" for "send home" behavior.
// Use "PARK_UNTIL_FURTHER_NOTICE" if you want it to stay docked until manually resumed.
const char* MOWER_ACTION_TYPE = "PARK_UNTIL_NEXT_TASK";

const char* GARDENA_BASE_URL = "https://api.smart.gardena.dev/v2";

// Open-Meteo allows up to 10000 calls/day.
const unsigned long WEATHER_CHECK_INTERVAL_MS = 2UL * 60UL * 1000UL;

// The Gardena/Husqvarna cloud API is limited to one call every 15 minutes.
const unsigned long GARDENA_MIN_INTERVAL_MS = 15UL * 60UL * 1000UL;

unsigned long lastWeatherCheckTime = 0;
unsigned long lastGardenaApiCallMs = 0;
bool hasCalledGardenaApi = false;

bool rainActionSentWhileRaining = false;

String mowerAccessToken;
unsigned long mowerTokenExpiresAtMs = 0;

String cachedMowerId;

bool gardenaRateLimitOk() {
  if (!hasCalledGardenaApi) return true;
  return (millis() - lastGardenaApiCallMs) >= GARDENA_MIN_INTERVAL_MS;
}

void markGardenaApiCalled() {
  lastGardenaApiCallMs = millis();
  hasCalledGardenaApi = true;
}

String urlEncode(const String& input) {
  String out;
  out.reserve(input.length() * 3);

  for (size_t i = 0; i < input.length(); i++) {
    const char c = input[i];
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
      out += buf;
    }
  }
  return out;
}

bool isRainCondition(String code) {
  code.toLowerCase();

  return code == "rain" ||
         code == "drizzle" ||
         code == "heavyrain" ||
         code == "sunshowers" ||
         code == "freezingrain" ||
         code == "isolatedthunderstorms" ||
         code == "scatteredthunderstorms";
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

bool isRainWeatherCode(int code) {
  // WMO codes from Open-Meteo docs:
  // drizzle: 51,53,55
  // freezing drizzle: 56,57
  // rain: 61,63,65
  // freezing rain: 66,67
  // rain showers: 80,81,82
  // thunderstorms: 95,96,99
  return (code >= 51 && code <= 57) ||
         (code >= 61 && code <= 67) ||
         (code >= 80 && code <= 82) ||
         (code >= 95 && code <= 99);
}

bool checkIfRaining() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = String("https://api.open-meteo.com/v1/forecast")
             + "?latitude=" + OPEN_METEO_LAT
             + "&longitude=" + OPEN_METEO_LON
             + "&current=weather_code,precipitation,rain,showers,snowfall"
             + "&timezone=" + OPEN_METEO_TZ;

  Serial.print("Requesting Open-Meteo... ");
  Serial.println(url);

  if (!http.begin(client, url)) {
    Serial.println("Open-Meteo begin() failed.");
    return false;
  }

  http.addHeader("Accept", "application/json");

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("Open-Meteo HTTP error: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("Open-Meteo JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonObject current = doc["current"];
  if (current.isNull()) {
    Serial.println("Open-Meteo response missing current section.");
    return false;
  }

  int weatherCode = current["weather_code"] | -1;
  float precipitation = current["precipitation"] | 0.0;
  float rain = current["rain"] | 0.0;
  float showers = current["showers"] | 0.0;
  float snowfall = current["snowfall"] | 0.0;

  Serial.print("weather_code: ");
  Serial.println(weatherCode);
  Serial.print("precipitation: ");
  Serial.println(precipitation);

  bool rainyCode = (weatherCode >= 0) ? isRainWeatherCode(weatherCode) : false;

  // Treat any measurable precip as rain-like enough to park.
  return rainyCode || precipitation > 0.0 || rain > 0.0 || showers > 0.0 || snowfall > 0.0;
}

bool refreshMowerToken() {
  if (mowerAccessToken.length() > 0 && millis() < mowerTokenExpiresAtMs) {
    return true;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const String tokenUrl = "https://api.authentication.husqvarnagroup.dev/v1/oauth2/token";

  if (!http.begin(client, tokenUrl)) {
    Serial.println("Token request begin() failed.");
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Accept", "application/json");

  String body = "grant_type=client_credentials";
  body += "&client_id=" + urlEncode(HUSQVARNA_APP_KEY);
  body += "&client_secret=" + urlEncode(HUSQVARNA_APP_SECRET);

  int httpCode = http.POST(body);
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("Token request failed. HTTP: ");
    Serial.println(httpCode);
    String response = http.getString();
    if (response.length()) {
      Serial.println(response);
    }
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("Token JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  mowerAccessToken = doc["access_token"] | "";
  const long expiresInSec = doc["expires_in"] | 0;

  if (mowerAccessToken.isEmpty() || expiresInSec <= 0) {
    Serial.println("Token response missing access_token or expires_in.");
    return false;
  }

  const unsigned long safetyMarginSec = 300;
  unsigned long usableSec = (expiresInSec > static_cast<long>(safetyMarginSec))
                              ? static_cast<unsigned long>(expiresInSec - safetyMarginSec)
                              : static_cast<unsigned long>(expiresInSec);

  mowerTokenExpiresAtMs = millis() + (usableSec * 1000UL);

  Serial.println("Husqvarna token acquired.");
  return true;
}

String findMowerId() {
  if (!refreshMowerToken()) return "";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client, String(GARDENA_BASE_URL) + "/locations");
  http.addHeader("Authorization", "Bearer " + mowerAccessToken);
  http.addHeader("X-Api-Key", HUSQVARNA_APP_KEY);
  http.addHeader("Accept", "application/vnd.api+json");

  int httpCode = http.GET();
  markGardenaApiCalled();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return "";
  }

  JsonDocument doc;
  deserializeJson(doc, http.getString());
  String locId = doc["data"][0]["id"];
  http.end();

  http.begin(client, String(GARDENA_BASE_URL) + "/locations/" + locId);
  http.addHeader("Authorization", "Bearer " + mowerAccessToken);
  http.addHeader("X-Api-Key", HUSQVARNA_APP_KEY);

  http.GET();
  markGardenaApiCalled();
  deserializeJson(doc, http.getString());
  http.end();

  JsonArray included = doc["included"];
  for (JsonVariant item : included) {
    if (item["type"] == "MOWER") {
      return item["id"].as<String>();
    }
  }
  return "";
}

bool sendMowerAction(const String& serviceId, const char* actionType) {
  if (!refreshMowerToken()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(GARDENA_BASE_URL) + "/command/" + serviceId;
  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + mowerAccessToken);
  http.addHeader("X-Api-Key", HUSQVARNA_APP_KEY);
  http.addHeader("Content-Type", "application/vnd.api+json");

  JsonDocument doc;
  doc["data"]["id"] = "req-1"; 
  doc["data"]["type"] = "MOWER_CONTROL";
  doc["data"]["attributes"]["command"] = actionType;

  String body;
  serializeJson(doc, body);

  int httpCode = http.PUT(body);
  markGardenaApiCalled();
  http.end();

  return (httpCode == 202);
}

void setup() {
  Serial.begin(115200);
  delay(10);

  Serial.println();
  Serial.println("Initializing weather-to-mower automation...");

  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
    connectWiFi();
    return;
  }

  if (lastWeatherCheckTime == 0 || (millis() - lastWeatherCheckTime >= WEATHER_CHECK_INTERVAL_MS)) {
    lastWeatherCheckTime = millis();

    Serial.println();

    const bool raining = checkIfRaining();

    if (raining) {
      Serial.println("Rain detected.");

      if (!rainActionSentWhileRaining) {
        if (!gardenaRateLimitOk()) {
          unsigned long waitMs = GARDENA_MIN_INTERVAL_MS - (millis() - lastGardenaApiCallMs);
          Serial.print("Gardena API rate limit in effect, retrying in ~");
          Serial.print(waitMs / 1000UL);
          Serial.println("s.");
        } else {
          if (cachedMowerId.isEmpty()) {
            cachedMowerId = findMowerId();
          }

          if (cachedMowerId.length() > 0) {
            if (gardenaRateLimitOk()) {
              if (sendMowerAction(cachedMowerId, MOWER_ACTION_TYPE)) {
                rainActionSentWhileRaining = true;
                Serial.println("Mower command sent successfully.");
              } else {
                Serial.println("Mower command failed.");
              }
            } else {
              Serial.println("Gardena API rate limit hit while resolving mower ID; will send command on next check.");
            }
          } else {
            Serial.println("Could not resolve mower ID.");
          }
        }
      } else {
        Serial.println("Rain action already sent for this rain event.");
      }
    } else {
      Serial.println("No rain detected.");
      rainActionSentWhileRaining = false;
    }
  }

  delay(1000);
}