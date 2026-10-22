
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// ===== GitHub repository =====
const char* VERSION_URL =
  "https://raw.githubusercontent.com/esp2323/ESP32-OTA/main/version.txt";

const char* FIRMWARE_URL =
  "https://github.com/esp2323/ESP32-OTA/releases/latest/download/firmware.bin";

// ===== Installed firmware version =====
const char* CURRENT_VERSION = "1.0.1";

// ===== Hardware =====
const int LED_PIN = 2;

// ===== Timers =====
const unsigned long CHECK_INTERVAL = 120000UL;
const unsigned long BLINK_INTERVAL = 200UL;
const unsigned long WIFI_RETRY_INTERVAL = 10000UL;
const unsigned long PORTAL_DELAY = 20000UL;

// ===== Objects =====
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

// ===== State =====
String savedSSID;
String savedPassword;

bool portalRunning = false;
bool ledState = false;

unsigned long lastCheck = 0;
unsigned long lastBlink = 0;
unsigned long disconnectedSince = 0;
unsigned long lastReconnect = 0;

// ===== Wi-Fi configuration portal =====
void startPortal() {
  if (portalRunning) return;

  Serial.println("Starting Wi-Fi setup portal...");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32-Setup");

  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);

  server.on("/", HTTP_GET, []() {
    int count = WiFi.scanNetworks();

    String html =
      "<!DOCTYPE html><html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>ESP32 Setup</title></head><body>"
      "<h2>ESP32 Wi-Fi Setup</h2>"
      "<form method='POST' action='/save'>"
      "<label>Wi-Fi network</label><br>"
      "<select name='ssid' required>";

    for (int i = 0; i < count; i++) {
      String s = WiFi.SSID(i);
      String safe = s;
      safe.replace("&", "&amp;");
      safe.replace("<", "&lt;");
      safe.replace(">", "&gt;");
      safe.replace("\"", "&quot;");

      html += "<option value=\"" + safe + "\">";
      html += safe + " (" + String(WiFi.RSSI(i)) + " dBm)";
      html += "</option>";
    }

    WiFi.scanDelete();

    html +=
      "</select><br><br>"
      "<label>Password</label><br>"
      "<input name='password' type='password'>"
      "<br><br><button type='submit'>Connect</button>"
      "</form></body></html>";

    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String password = server.arg("password");

    if (ssid.length() == 0) {
      server.send(400, "text/plain", "SSID is required");
      return;
    }

    prefs.putString("ssid", ssid);
    prefs.putString("password", password);

    savedSSID = ssid;
    savedPassword = password;

    WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

    disconnectedSince = millis();
    lastReconnect = millis();

    server.send(200, "text/html",
      "<h2>Connecting...</h2>"
      "<p>Wait a moment and check Serial Monitor.</p>");
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  portalRunning = true;

  Serial.print("Connect to ESP32-Setup. Open http://");
  Serial.println(apIP);
}

// ===== Stop configuration portal =====
void stopPortal() {
  if (!portalRunning) return;

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalRunning = false;

  WiFi.mode(WIFI_STA);

  Serial.println("Setup portal stopped.");
}

// ===== Check GitHub and update =====
bool checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected.");
    return false;
  }

  Serial.println("Checking GitHub version...");

  WiFiClientSecure client;

  // Prototype only: TLS certificate verification is disabled.
  // Configure certificate validation before production use.
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  if (!http.begin(client, VERSION_URL)) {
    Serial.println("Could not start version request.");
    return false;
  }

  int status = http.GET();

  if (status != HTTP_CODE_OK) {
    Serial.printf("Version request failed: HTTP %d\n", status);
    http.end();
    return false;
  }

  String remoteVersion = http.getString();
  remoteVersion.trim();
  http.end();

  Serial.printf("Installed version: %s\n", CURRENT_VERSION);
  Serial.printf("GitHub version: %s\n", remoteVersion.c_str());

  if (remoteVersion.length() == 0) {
    Serial.println("Empty version file.");
    return false;
  }

  if (remoteVersion == CURRENT_VERSION) {
    Serial.println("Already up to date.");
    return true;
  }

  Serial.println("Different version detected. Starting OTA...");

  WiFiClientSecure updateClient;
  updateClient.setInsecure();

  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  t_httpUpdate_return result =
    httpUpdate.update(
      updateClient,
      FIRMWARE_URL,
      CURRENT_VERSION
    );

  if (result == HTTP_UPDATE_OK) {
    Serial.println("OTA successful. Rebooting...");
    delay(1000);
    ESP.restart();
  }

  if (result == HTTP_UPDATE_NO_UPDATES) {
    Serial.println("No update available.");
    return true;
  }

  Serial.printf("OTA failed: %s\n",
    httpUpdate.getLastErrorString().c_str());

  return false;
}

// ===== Connect to saved Wi-Fi =====
void connectWiFi() {
  if (savedSSID.length() == 0) {
    startPortal();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

  Serial.printf("Connecting to %s\n", savedSSID.c_str());

  unsigned long started = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < 15000UL) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    disconnectedSince = 0;
  } else {
    Serial.println("Connection failed.");
    disconnectedSince = millis();
    startPortal();
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  prefs.begin("wifi-config", false);

  savedSSID = prefs.getString("ssid", "");
  savedPassword = prefs.getString("password", "");

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    checkForUpdate();
    lastCheck = millis();
  }
}

// ===== Main loop =====
void loop() {
  unsigned long now = millis();

  if (now - lastBlink >= BLINK_INTERVAL) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }

  if (portalRunning) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  if (WiFi.status() == WL_CONNECTED) {
    disconnectedSince = 0;

    if (portalRunning) {
      Serial.print("Wi-Fi connected. IP: ");
      Serial.println(WiFi.localIP());
      stopPortal();
    }

    if (now - lastCheck >= CHECK_INTERVAL) {
      lastCheck = now;

      if (!checkForUpdate()) {
        Serial.println("Update check failed; will retry.");
      }
    }
  } else {
    if (disconnectedSince == 0) {
      disconnectedSince = now;
      Serial.println("Wi-Fi disconnected.");
    }

    if (!portalRunning &&
        now - lastReconnect >= WIFI_RETRY_INTERVAL) {
      lastReconnect = now;
      Serial.println("Trying Wi-Fi reconnect...");
      WiFi.reconnect();
    }

    if (!portalRunning &&
        now - disconnectedSince >= PORTAL_DELAY) {
      startPortal();
    }
  }
}
