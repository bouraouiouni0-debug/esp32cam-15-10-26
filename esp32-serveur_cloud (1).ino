
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
// ===== CONFIGURATION WIFI =====
const char* ssid = "WIFI-CHERCHEUR";
const char* password = "Cherch@Eniso@2022";

// ===== CONFIGURATION SERVEUR CLOUD (Render) =====
const char* SERVER  = "https://esp32photo.onrender.com";  // adresse de ton service Render
const char* API_KEY = "";   // laisse vide, ou mets la meme valeur que la variable API_KEY sur Render
const unsigned long POLL_INTERVAL_MS = 1500;

WiFiClientSecure secureClient;
HTTPClient http;
bool streaming = false;
bool streamMode = false;
unsigned long lastPoll = 0;
framesize_t photoSize = FRAMESIZE_VGA;
// ===== BROCHES AI-THINKER ESP32-CAM =====
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ===== INITIALISER LA CAMERA =====
bool initCamera() {
  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

 config.pin_sscb_sda = SIOD_GPIO_NUM;
config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA; // 800 x 600
    config.jpeg_quality = 10;
    config.fb_count = 2;
    
  } else {
    config.frame_size = FRAMESIZE_VGA; // 640 x 480
    config.jpeg_quality = 12;
    config.fb_count = 1;
    
  }

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("Erreur camera : 0x%x\n", err);
    return false;
  }

  return true;
}

// ===== CLOUD : ENVOYER UN JPEG AU SERVEUR =====
int postJpeg(const char* path, camera_fb_t *fb, String &response) {
  http.begin(secureClient, String(SERVER) + path);
  http.setReuse(true);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("x-api-key", API_KEY);
  int code = http.POST(fb->buf, fb->len);
  if (code > 0) response = http.getString();
  http.end();
  return code;
}

void setFrameSize(framesize_t size) {
  sensor_t *s = esp_camera_sensor_get();
  if (s) s->set_framesize(s, size);
}

// ===== CLOUD : PRENDRE ET ENVOYER UNE PHOTO =====
void sendPhoto() {
  setFrameSize(photoSize);

  // Ignorer une premiere image (elle peut etre ancienne)
  camera_fb_t *old = esp_camera_fb_get();
  if (old) esp_camera_fb_return(old);

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println("Erreur capture photo");
    return;
  }

  String resp;
  int code = postJpeg("/api/photo", fb, resp);
  Serial.printf("Photo envoyee (%u octets) -> HTTP %d\n", (unsigned int)fb->len, code);
  esp_camera_fb_return(fb);
}

// ===== CLOUD : ENVOYER UNE IMAGE DU STREAMING =====
void sendStreamFrame() {
  if (!streamMode) {
    setFrameSize(FRAMESIZE_QVGA); // petite image = streaming plus fluide
    streamMode = true;
    camera_fb_t *old = esp_camera_fb_get();
    if (old) esp_camera_fb_return(old);
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) {
    delay(50);
    return;
  }

  String resp;
  int code = postJpeg("/api/frame", fb, resp);
  esp_camera_fb_return(fb);

  if (code == 200) {
    streaming = resp.indexOf("\"stream\":true") >= 0;
  } else {
    Serial.printf("Erreur streaming HTTP %d\n", code);
    streaming = false;
    delay(500);
  }

  if (!streaming) {
    setFrameSize(photoSize);
    streamMode = false;
    Serial.println("Streaming arrete.");
  }
}

// ===== CLOUD : DEMANDER AU SERVEUR CE QU'IL FAUT FAIRE =====
void pollServer() {
  http.begin(secureClient, String(SERVER) + "/api/poll");
  http.setReuse(true);
  http.setTimeout(10000);
  http.addHeader("x-api-key", API_KEY);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    http.end();
    streaming = body.indexOf("\"stream\":true") >= 0;
    if (body.indexOf("\"photo\":true") >= 0) {
      sendPhoto();
    }
  } else {
    Serial.printf("Poll HTTP %d\n", code);
    http.end();
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Demarrage ESP32-CAM...");

  if (!initCamera()) {
    Serial.println("Impossible d'initialiser la camera.");
    return;
  }

  Serial.println("Camera initialisee.");

  photoSize = psramFound() ? FRAMESIZE_SVGA : FRAMESIZE_VGA;
  secureClient.setInsecure(); // HTTPS sans verification du certificat (simple pour Render)

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connexion Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connecte !");

  Serial.println("Pret : connexion au serveur cloud.");
}

// ===== BOUCLE PRINCIPALE =====
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
    return;
  }

  if (streaming) {
    sendStreamFrame();
    return;
  }

  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();
    pollServer();
  }
}
