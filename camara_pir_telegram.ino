#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"    //Esto son todas las librerias para que el ESP32 pueda comunicarse con todos los elementos
#include "esp_camera.h"
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// ===== aqui pongo el nombre del wifi y la contraseña =====
const char* ssid     = "WIFI";
const char* password = "CONTRASEÑA";

// aqui definimos el token del bot de telegram y el chatID y tambien los pines del sensor pir,Led y el buzz
#define BOT_TOKEN "8844614783:AAFpAyv5ySLkWdB6K8r9X9skP3ZiNIFDYCc"
#define CHAT_ID   "7134259706"
#define PIR_PIN   13
#define LED_PIN   14
#define BUZZ_PIN  12

// Pines para la camara del  ESP32
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     21
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       19
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM        5
#define Y2_GPIO_NUM        4
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WiFiClientSecure clientTCP;
UniversalTelegramBot bot(BOT_TOKEN, clientTCP);

bool pirEstado = false;
bool sistemaActivo = true;
unsigned long ultimaFoto = 0;
unsigned long ultimoCheck = 0;
const int esperaEntrefotos = 3000;
const int checkMensajes = 2000;
const int NUM_FOTOS = 3;        // Número de fotos de la ráfaga
const int PAUSA_RAFAGA = 1500;  // Pausa entre cada fotos 

void pitido(int veces) {
  for (int i = 0; i < veces; i++) {
    digitalWrite(BUZZ_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZ_PIN, LOW);
    delay(100);
  }
}

void parpadeoLED(int veces) {
  for (int i = 0; i < veces; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
}

void iniciarCamara() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 10;
  config.fb_count     = 1;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Error al iniciar camara");
    return;
  }
  Serial.println("Camara lista");
}

void enviarUnaFoto(int numero) {
  // Descartar frames viejos
  camera_fb_t* fb;
  for (int i = 0; i < 3; i++) {
    fb = esp_camera_fb_get();
    esp_camera_fb_return(fb);
    delay(100);
  }
  fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Error capturando foto");
    return;
  }

  Serial.printf("Foto %d capturada: %d bytes\n", numero, fb->len);

  WiFiClientSecure clientFoto;
  clientFoto.setInsecure();

  if (!clientFoto.connect("api.telegram.org", 443)) {
    Serial.println("Error conectando a Telegram");
    esp_camera_fb_return(fb);
    return;
  }

  String boundary = "----ESP32Boundary";
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
                String(CHAT_ID) + "\r\n" +
                "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"photo\"; filename=\"foto.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  uint32_t totalLen = head.length() + fb->len + tail.length();

  clientFoto.println("POST /bot" + String(BOT_TOKEN) + "/sendPhoto HTTP/1.1");
  clientFoto.println("Host: api.telegram.org");
  clientFoto.println("Content-Type: multipart/form-data; boundary=" + boundary);
  clientFoto.println("Content-Length: " + String(totalLen));
  clientFoto.println("Connection: close");
  clientFoto.println();
  clientFoto.print(head);

  uint8_t* buf = fb->buf;
  size_t len   = fb->len;
  size_t chunk = 1024;
  for (size_t i = 0; i < len; i += chunk) {
    size_t c = (i + chunk < len) ? chunk : len - i;
    clientFoto.write(buf + i, c);
  }

  clientFoto.print(tail);

  unsigned long timeout = millis();
  while (clientFoto.connected() && millis() - timeout < 10000) {
    if (clientFoto.available()) {
      String line = clientFoto.readStringUntil('\n');
      if (line.indexOf("\"ok\":true") >= 0) {
        Serial.printf("Foto %d enviada!\n", numero);
        break;
      }
    }
  }

  esp_camera_fb_return(fb);
  clientFoto.stop();
}

void enviarRafaga(bool automatico) {
  if (automatico) {
    bot.sendMessage(CHAT_ID, "🚨 ¡Movimiento detectado! Enviando ráfaga de fotos...", "");
  } else {
    bot.sendMessage(CHAT_ID, "📸 Enviando ráfaga manual de 3 fotos...", "");
  }
  
  for (int i = 1; i <= NUM_FOTOS; i++) {
    Serial.printf("Enviando foto %d de %d...\n", i, NUM_FOTOS);
    digitalWrite(LED_PIN, HIGH);
    enviarUnaFoto(i);
    digitalWrite(LED_PIN, LOW);
    
    if (i < NUM_FOTOS) {
      delay(PAUSA_RAFAGA);
    }
  }
  
  bot.sendMessage(CHAT_ID, "📸 Ráfaga completada (" + String(NUM_FOTOS) + " fotos)", "");
  Serial.println("Ráfaga completada!");
}

void procesarComandos() {
  int numMensajes = bot.getUpdates(bot.last_message_received + 1);
  while (numMensajes) {
    for (int i = 0; i < numMensajes; i++) {
      String texto = bot.messages[i].text;
      String chat  = bot.messages[i].chat_id;

      if (chat != CHAT_ID) {
        bot.sendMessage(chat, "No autorizado.", "");
        continue;
      }

      Serial.println("Comando recibido: " + texto);

      if (texto == "/on") {
        sistemaActivo = true;
        digitalWrite(LED_PIN, LOW);
        parpadeoLED(2);
        bot.sendMessage(CHAT_ID, "✅ Sistema ACTIVADO. Vigilando...", "");
        pitido(2);
      } else if (texto == "/off") {
        sistemaActivo = false;
        digitalWrite(LED_PIN, LOW);
        parpadeoLED(1);
        bot.sendMessage(CHAT_ID, "🔴 Sistema DESACTIVADO.", "");
        pitido(1);
      } else if (texto == "/estado") {
        String estado = sistemaActivo ? "✅ Activo" : "🔴 Desactivado";
        bot.sendMessage(CHAT_ID, "Estado actual: " + estado, "");
      } else if (texto == "/foto") {
        bot.sendMessage(CHAT_ID, "📸 Tomando foto manual...", "");
        parpadeoLED(2);
        enviarUnaFoto(1);
      } else if (texto == "/rafaga") {
        parpadeoLED(2);
        enviarRafaga(false);
      } else if (texto == "/help" || texto == "/start") {
        String ayuda = "🤖 Comandos disponibles:\n\n";
        ayuda += "/on - Activar vigilancia\n";
        ayuda += "/off - Desactivar vigilancia\n";
        ayuda += "/estado - Ver estado actual\n";
        ayuda += "/foto - Tomar una foto\n";
        ayuda += "/rafaga - Tomar 3 fotos seguidas\n";
        ayuda += "/help - Ver esta ayuda";
        bot.sendMessage(CHAT_ID, ayuda, "");
      }
    }
    numMensajes = bot.getUpdates(bot.last_message_received + 1);
  }
}
// Esto es el arranque del ESP32,al encenderse se conecta a wifi, enciende la cámara, condigura los pines del led, zumbador y envia el mensaje a telegram de que está encendido y listo
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZ_PIN, LOW);

  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado: " + WiFi.localIP().toString());

  clientTCP.setInsecure();
  iniciarCamara();

  parpadeoLED(3);
  pitido(3);

  String bienvenida = "✅ Sistema iniciado!\n\nEscribe /help para ver los comandos.";
  bot.sendMessage(CHAT_ID, bienvenida, "");
  Serial.println("Sistema listo!");
}
// Este es el bucle que ejecutara una vez este funcionando el ESP32, mira si le he mandado algun comando por telegram, si el sistema esta funcionando lee el sensor PIR y si detecta movimiento pita, enciende el led y manda las fotos
void loop() {
  if (millis() - ultimoCheck > checkMensajes) {
    ultimoCheck = millis();
    procesarComandos();
  }

  if (sistemaActivo) {
    bool movimiento = digitalRead(PIR_PIN);

    if (movimiento && !pirEstado) {
      pirEstado = true;
      unsigned long ahora = millis();
      if (ahora - ultimaFoto > esperaEntrefotos) {
        ultimaFoto = ahora;
        Serial.println("¡Movimiento detectado!");
        pitido(3);
        enviarRafaga(true);
      }
    }

    if (!movimiento) pirEstado = false;
  }

  delay(100);
}
