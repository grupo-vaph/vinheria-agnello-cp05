/*
 * ==========================================================================
 *  Vinheria Agnello - Checkpoint 05 (IoT + FIWARE)
 *  FIAP - Engenharia de Software - Edge Computing & Computer Systems
 *  Prof. Lucas Demetrius Augusto
 *
 *  Grupo:
 *    Victor Ulisses de Morais Silva  - RM 572634
 *    Enzo Gabriel Pereira            - RM 570659
 *    Pedro Henrique Moura Aguiar     - RM 570715
 *    Henrique Giusti de Souza        - RM 570766
 *    Pietro Alexandre Guerato Objetivo - RM 571232
 *
 *  O que este programa faz:
 *   - Lê temperatura e umidade (DHT22) e luminosidade (LDR) no ESP32;
 *   - Mostra o estado da adega no LCD 16x2 e sinaliza com LEDs e buzzer;
 *   - Conecta no Wi-Fi e publica as leituras no FIWARE via MQTT
 *     (protocolo UltraLight 2.0 do IoT Agent), com o timestamp de cada leitura;
 *   - Recebe comandos pelo tópico /cmd (on / off do alarme sonoro),
 *     que podem ser enviados pelo MyMQTT ou pelo Orion (NGSIv2).
 * ==========================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include <math.h>

// ======================= CONFIGURAÇÕES EDITÁVEIS ==========================
// Wi-Fi (no Wokwi use sempre a rede Wokwi-GUEST, sem senha)
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// Broker MQTT do servidor FIWARE
const char* BROKER_MQTT = "3.135.234.101";
const int   BROKER_PORT = 1883;

// Identificação no IoT Agent.
// IMPORTANTE: o servidor é compartilhado com outros grupos, então o DEVICE_ID
// precisa ser único. Se trocar aqui, troque também na coleção do Postman.
#define APIKEY    "TEF"
#define DEVICE_ID "vinharia027"

const char* ID_MQTT          = "fiware_" DEVICE_ID;              // id da sessão MQTT
const char* TOPICO_PUBLISH   = "/" APIKEY "/" DEVICE_ID "/attrs";  // envio de leituras
const char* TOPICO_SUBSCRIBE = "/" APIKEY "/" DEVICE_ID "/cmd";    // recebimento de comandos
const char* TOPICO_CMDEXE    = "/" APIKEY "/" DEVICE_ID "/cmdexe"; // confirmação de comandos

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";
// ==========================================================================

// ------------------------------- Pinos ------------------------------------
#define PINO_DHT      15
#define PINO_LDR      34   // entrada analógica (ADC1)
#define LED_VERDE     25
#define LED_AMARELO   26
#define LED_VERMELHO  27
#define BUZZER        14
// LCD I2C: SDA = GPIO 21, SCL = GPIO 22 (padrão do ESP32)

#define DHTTYPE DHT22
DHT dht(PINO_DHT, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient espClient;
PubSubClient MQTT(espClient);

// ----------------- Faixas ideais (slide 4 do enunciado) -------------------
// Temperatura ideal ~13 °C, tolerando variação de 3 °C
const float TEMP_MIN = 10.0;
const float TEMP_MAX = 16.0;
// Umidade ideal ~70 %, variação aceitável de 60 % a 80 %
const float UMID_MIN = 60.0;
const float UMID_MAX = 80.0;
// Luminosidade em % (0 = escuro, 100 = muito claro)
const int LUX_ESCURO = 33;
const int LUX_MEIA   = 66;

// ----------------------------- Temporização -------------------------------
const unsigned long INTERVALO = 5000;  // intervalo entre ciclos de leitura (ms)
const int N_LEITURAS    = 5;           // leituras usadas para calcular a média
const int DELAY_LEITURA = 800;         // espera entre leituras (ms)
unsigned long tempoAnterior = 0;
unsigned long ultimaTentativaMQTT = 0;
int telaAtual = 0;

bool alarmeAtivo = true;   // alterado remotamente pelos comandos on / off

// ==========================================================================
//  Utilidades
// ==========================================================================

// Espera sem "travar" o MQTT (mantém o cliente processando mensagens)
void esperar(unsigned long ms) {
  unsigned long inicio = millis();
  while (millis() - inicio < ms) {
    MQTT.loop();
    delay(10);
  }
}

// Monta o timestamp ISO 8601 em UTC (ex.: 2026-10-23T14:05:31Z)
bool obterTimestamp(char* buf, size_t len) {
  struct tm t;
  if (!getLocalTime(&t, 50)) return false;
  strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &t);
  return true;
}

// Estimativa de lux para o módulo LDR do Wokwi (alimentado em 3,3 V, ADC de 12 bits)
float calcularLux(int leituraBruta) {
  const float GAMMA = 0.7;
  const float RL10  = 50;
  const float VCC   = 3.3;
  float tensao = leituraBruta / 4095.0 * VCC;
  tensao = constrain(tensao, 0.01f, VCC - 0.01f);
  float resistencia = 2000 * tensao / (1 - tensao / VCC);
  return pow(RL10 * 1e3 * pow(10, GAMMA) / resistencia, (1 / GAMMA));
}

// ==========================================================================
//  Conectividade (Wi-Fi, NTP e MQTT)
// ==========================================================================

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("[WiFi] Conectando em ");
  Serial.println(WIFI_SSID);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Conectando WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);  // canal 6 acelera a conexão no Wokwi
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
    // Se não conectar em ~10 s, reinicia a tentativa (o Wi-Fi do Wokwi às vezes trava)
    if (++tentativas >= 40) {
      tentativas = 0;
      Serial.println("\n[WiFi] Sem resposta, tentando de novo...");
      WiFi.disconnect();
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
  Serial.println();
  Serial.print("[WiFi] Conectado! IP: ");
  Serial.println(WiFi.localIP());
}

void iniciarRelogio() {
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);  // horário em UTC
  Serial.print("[NTP] Sincronizando relogio");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sincronizando");
  lcd.setCursor(0, 1);
  lcd.print("relogio (NTP)");

  struct tm t;
  for (int i = 0; i < 20 && !getLocalTime(&t, 500); i++) {
    Serial.print(".");
  }
  Serial.println();

  char ts[25];
  if (obterTimestamp(ts, sizeof(ts))) {
    Serial.print("[NTP] Horario UTC: ");
    Serial.println(ts);
  } else {
    Serial.println("[NTP] Falha ao sincronizar; leituras sairao sem 'ts' ate sincronizar.");
  }
}

void publicarEstadoAlarme() {
  MQTT.publish(TOPICO_PUBLISH, alarmeAtivo ? "s|on" : "s|off");
}

bool conectarMQTT() {
  if (MQTT.connected()) return true;
  if (ultimaTentativaMQTT != 0 && millis() - ultimaTentativaMQTT < 3000) return false;
  ultimaTentativaMQTT = millis();

  Serial.printf("[MQTT] Conectando ao broker %s:%d ...\n", BROKER_MQTT, BROKER_PORT);
  if (MQTT.connect(ID_MQTT)) {
    Serial.println("[MQTT] Conectado!");
    MQTT.subscribe(TOPICO_SUBSCRIBE);
    Serial.printf("[MQTT] Inscrito em %s\n", TOPICO_SUBSCRIBE);
    publicarEstadoAlarme();
    return true;
  }

  Serial.printf("[MQTT] Falha (rc=%d). Nova tentativa em 3 s.\n", MQTT.state());
  return false;
}

// Mensagens recebidas no tópico /cmd.
// Formato UltraLight: <device_id>@<comando>|<parametros>
// Exemplos: vinharia027@on|   ou   vinharia027@off|
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] Recebido em %s: %s\n", topic, msg.c_str());

  int arroba = msg.indexOf('@');
  int barra  = msg.indexOf('|', arroba + 1);
  String comando = (arroba >= 0)
                     ? msg.substring(arroba + 1, barra > arroba ? barra : msg.length())
                     : msg;
  comando.trim();

  if (comando == "on") {
    alarmeAtivo = true;
  } else if (comando == "off") {
    alarmeAtivo = false;
    noTone(BUZZER);
  } else {
    Serial.printf("[MQTT] Comando desconhecido: %s\n", comando.c_str());
    return;
  }

  Serial.printf("[CMD] Alarme sonoro %s\n", alarmeAtivo ? "ATIVADO" : "SILENCIADO");

  // Confirma a execução para o IoT Agent (atualiza o status do comando no Orion)
  char resposta[96];
  snprintf(resposta, sizeof(resposta), "%s@%s|%s", DEVICE_ID, comando.c_str(),
           alarmeAtivo ? "alarme ativado" : "alarme silenciado");
  MQTT.publish(TOPICO_CMDEXE, resposta);

  // Atualiza o atributo de estado do alarme
  publicarEstadoAlarme();
}

// ==========================================================================
//  Sinalização local (LEDs, buzzer e LCD) - lógica da fase 1
// ==========================================================================

void atualizarLEDs(bool escuro, bool meiaLuz, bool muitoClaro,
                   bool tempForaFaixa, bool umidForaFaixa) {
  digitalWrite(LED_VERDE,    (escuro && !tempForaFaixa && !umidForaFaixa));
  digitalWrite(LED_AMARELO,  (meiaLuz || tempForaFaixa));
  digitalWrite(LED_VERMELHO, (muitoClaro || umidForaFaixa));

  if (alarmeAtivo && (muitoClaro || tempForaFaixa || umidForaFaixa)) {
    tone(BUZZER, 1000);
  } else {
    noTone(BUZZER);
  }
}

void mostrarDisplay(bool escuro, bool meiaLuz, bool tempForaFaixa,
                    bool umidForaFaixa, float temperatura, float umidade,
                    int luminosidade) {
  lcd.clear();

  switch (telaAtual) {
    case 0:  // Luminosidade
      lcd.setCursor(0, 0);
      if (escuro) {
        lcd.print("Ambiente Escuro");
      } else if (meiaLuz) {
        lcd.print("Ambiente MeiaLuz");
      } else {
        lcd.print("Ambiente CLARO");
      }
      lcd.setCursor(0, 1);
      lcd.print("Luz = ");
      lcd.print(luminosidade);
      lcd.print("%");
      break;

    case 1:  // Temperatura
      lcd.setCursor(0, 0);
      if (!tempForaFaixa)              lcd.print("Temperatura OK");
      else if (temperatura > TEMP_MAX) lcd.print("Temp. Alta");
      else                             lcd.print("Temp. Baixa");
      lcd.setCursor(0, 1);
      lcd.print("Temp. = ");
      lcd.print(temperatura, 1);
      lcd.print("C");
      break;

    case 2:  // Umidade
      lcd.setCursor(0, 0);
      if (!umidForaFaixa)          lcd.print("Umidade OK");
      else if (umidade > UMID_MAX) lcd.print("Umidade Alta");
      else                         lcd.print("Umidade Baixa");
      lcd.setCursor(0, 1);
      lcd.print("Umidade = ");
      lcd.print((int)umidade);
      lcd.print("%");
      break;

    case 3:  // Conexão com o FIWARE
      lcd.setCursor(0, 0);
      lcd.print(MQTT.connected() ? "FIWARE: online" : "FIWARE: offline");
      lcd.setCursor(0, 1);
      lcd.print(alarmeAtivo ? "Alarme: ligado" : "Alarme: mudo");
      break;
  }

  telaAtual = (telaAtual + 1) % 4;
}

// ==========================================================================
//  Envio das leituras para o FIWARE
// ==========================================================================
// Payload UltraLight 2.0 (o IoT Agent converte para NGSIv2):
//   t  -> temperature         (°C)
//   h  -> relativeHumidity    (%)
//   i  -> illuminance         (lux, estimado)
//   l  -> luminosity          (% de luz, escala usada na fase 1)
//   a  -> alertStatus         (OK ou lista de alertas)
//   s  -> alarmStatus         (on/off)
//   ts -> dateObserved        (timestamp ISO 8601 da leitura)
void publicarLeituras(float temperatura, float umidade, float lux,
                      int luminosidade, const String& alerta) {
  char ts[25];
  bool temHora = obterTimestamp(ts, sizeof(ts));

  char payload[256];
  int n = snprintf(payload, sizeof(payload), "t|%.1f|h|%.1f|i|%.1f|l|%d|a|%s|s|%s",
                   temperatura, umidade, lux, luminosidade, alerta.c_str(),
                   alarmeAtivo ? "on" : "off");
  if (temHora && n > 0 && n < (int)sizeof(payload)) {
    snprintf(payload + n, sizeof(payload) - n, "|ts|%s", ts);
  }

  if (MQTT.connected()) {
    bool ok = MQTT.publish(TOPICO_PUBLISH, payload);
    Serial.printf("[MQTT] %s %s -> %s\n", ok ? "Publicado" : "FALHA ao publicar",
                  TOPICO_PUBLISH, payload);
  } else {
    Serial.printf("[MQTT] Desconectado, leitura nao enviada: %s\n", payload);
  }
}

// ==========================================================================
//  Setup e loop
// ==========================================================================

void setup() {
  Serial.begin(115200);
  dht.begin();
  lcd.init();
  lcd.backlight();

  pinMode(LED_VERDE,    OUTPUT);
  pinMode(LED_AMARELO,  OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  pinMode(BUZZER,       OUTPUT);
  digitalWrite(LED_VERDE,    LOW);
  digitalWrite(LED_AMARELO,  LOW);
  digitalWrite(LED_VERMELHO, LOW);
  noTone(BUZZER);

  lcd.setCursor(0, 0);
  lcd.print("Vinheria Agnello");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");
  delay(1500);

  conectarWiFi();
  iniciarRelogio();

  MQTT.setServer(BROKER_MQTT, BROKER_PORT);
  MQTT.setCallback(mqttCallback);
  MQTT.setBufferSize(512);
  conectarMQTT();

  lcd.clear();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  conectarMQTT();
  MQTT.loop();

  // Ressincroniza o relógio a cada 1 min. No Wokwi a simulação pode rodar
  // mais devagar que o tempo real e o relógio interno vai ficando atrasado.
  static unsigned long ultimaSincronia = 0;
  if (millis() - ultimaSincronia >= 60000) {
    ultimaSincronia = millis();
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  }

  if (millis() - tempoAnterior < INTERVALO) return;
  tempoAnterior = millis();

  // ---- Leitura dos sensores (média de N leituras) ----
  float somaTemp = 0, somaUmid = 0;
  long  somaLdr  = 0;
  int   leituras = 0;

  for (int i = 0; i < N_LEITURAS; i++) {
    float t = dht.readTemperature();
    float u = dht.readHumidity();
    int   l = analogRead(PINO_LDR);

    if (!isnan(t) && !isnan(u)) {
      somaTemp += t;
      somaUmid += u;
      somaLdr  += l;
      leituras++;
    }
    esperar(DELAY_LEITURA);
  }

  if (leituras == 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Erro sensor DHT");
    Serial.println("ERRO: DHT sem leitura valida");
    MQTT.publish(TOPICO_PUBLISH, "a|ERRO_SENSOR_DHT");
    return;
  }

  float temperatura  = somaTemp / leituras;
  float umidade      = somaUmid / leituras;
  int   ldrBruto     = somaLdr / leituras;
  int   luminosidade = map(ldrBruto, 0, 4095, 100, 0);  // mais luz = leitura menor
  float lux          = calcularLux(ldrBruto);

  bool tempForaFaixa = (temperatura < TEMP_MIN || temperatura > TEMP_MAX);
  bool umidForaFaixa = (umidade < UMID_MIN || umidade > UMID_MAX);
  bool muitoClaro    = (luminosidade > LUX_MEIA);
  bool meiaLuz       = (luminosidade > LUX_ESCURO && luminosidade <= LUX_MEIA);
  bool escuro        = (luminosidade <= LUX_ESCURO);

  // ---- Texto de alerta enviado ao FIWARE ----
  String alerta = "";
  if (temperatura > TEMP_MAX)      alerta += "TEMP_ALTA-";
  else if (temperatura < TEMP_MIN) alerta += "TEMP_BAIXA-";
  if (umidade > UMID_MAX)          alerta += "UMID_ALTA-";
  else if (umidade < UMID_MIN)     alerta += "UMID_BAIXA-";
  if (muitoClaro)                  alerta += "LUZ_EXCESSIVA-";
  if (alerta.length() == 0) alerta = "OK";
  else alerta.remove(alerta.length() - 1);  // tira o último "-"

  atualizarLEDs(escuro, meiaLuz, muitoClaro, tempForaFaixa, umidForaFaixa);
  mostrarDisplay(escuro, meiaLuz, tempForaFaixa, umidForaFaixa,
                 temperatura, umidade, luminosidade);

  Serial.printf("Luz: %d%% (%.0f lux) | Temp: %.1fC | Umid: %.0f%% | Status: %s\n",
                luminosidade, lux, temperatura, umidade, alerta.c_str());

  publicarLeituras(temperatura, umidade, lux, luminosidade, alerta);
}
