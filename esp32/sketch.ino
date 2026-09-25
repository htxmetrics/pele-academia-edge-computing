#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <math.h>

const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_SENHA = "";
const int   WIFI_CANAL = 6;

const char* TS_WRITE_API_KEY = "1QA5UAK2ICPLZFUK";
const char* TS_URL           = "http://api.thingspeak.com/update";

#define PINO_DHT          15
#define TIPO_DHT          DHT22
#define PINO_LED_VERDE    25
#define PINO_LED_AMARELO  26
#define PINO_LED_VERMELHO 27
#define PINO_BOTAO        4

#define OLED_LARGURA  128
#define OLED_ALTURA   64
#define OLED_ENDERECO 0x3C

const unsigned long INTERVALO_LEITURA_MS   = 2000;
const unsigned long INTERVALO_ENVIO_MS     = 20000;
const unsigned long INTERVALO_TELA_MS      = 6000;
const unsigned long INTERVALO_WIFI_MS      = 10000;
const unsigned long INTERVALO_PISCA_MS     = 250;
const unsigned long DEBOUNCE_MS            = 200;

const int N_AMOSTRAS = 10;

const float WBGT_ATENCAO  = 25.0;
const float WBGT_ALERTA   = 28.0;
const float WBGT_CRITICO  = 32.0;
const float TEMP_LIMITE   = 32.0;
const float UMID_MAX      = 80.0;
const float UMID_MIN      = 30.0;

enum Estado { NORMAL = 0, ATENCAO = 1, ALERTA = 2, CRITICO = 3, ERRO_SENSOR = 4 };
const char* NOME_ESTADO[] = { "NORMAL", "ATENCAO", "ALERTA", "CRITICO", "ERRO SENSOR" };
const char* RECOMENDACAO[] = {
  "Treino liberado",
  "Pausas p/ hidratacao",
  "Reduzir intensidade",
  "Suspender treino",
  "Verificar sensor"
};

DHT dht(PINO_DHT, TIPO_DHT);
Adafruit_SSD1306 display(OLED_LARGURA, OLED_ALTURA, &Wire, -1);

float         bufTemp[N_AMOSTRAS];
float         bufUmid[N_AMOSTRAS];
unsigned long bufTempo[N_AMOSTRAS];
int           bufIndice = 0;
int           bufQtd    = 0;

float tempAtual = NAN;
float umidAtual = NAN;

struct Indicadores {
  float  tempMedia, umidMedia;
  float  tempMin, tempMax;
  float  indiceCalor;
  float  wbgt;
  float  tendencia;
  int    excedencias;
  Estado estado;
} ind;

int falhasSensorSeguidas = 0;

unsigned long enviosOk = 0, enviosFalha = 0;
unsigned long ultimoEnvioOk = 0;

unsigned long tUltimaLeitura = 0, tUltimoEnvio = 0, tUltimaTela = 0;
unsigned long tUltimaTentativaWifi = 0, tUltimoPisca = 0, tUltimoBotao = 0;
bool estadoPisca = false;
int  telaAtual = 0;
const int TOTAL_TELAS = 4;
bool botaoAnterior = HIGH;
bool displayOk = false;

float pressaoVapor(float t, float ur) {
  return (ur / 100.0) * 6.105 * exp((17.27 * t) / (237.7 + t));
}

float calcularWBGT(float t, float ur) {
  return 0.567 * t + 0.393 * pressaoVapor(t, ur) + 3.94;
}

float calcularTendencia() {
  if (bufQtd < 3) return 0;

  int inicio = (bufQtd < N_AMOSTRAS) ? 0 : bufIndice;
  unsigned long t0 = bufTempo[inicio];
  float sx = 0, sy = 0, sxy = 0, sxx = 0;
  for (int i = 0; i < bufQtd; i++) {
    int k = (inicio + i) % N_AMOSTRAS;
    float x = (bufTempo[k] - t0) / 60000.0;
    float y = bufTemp[k];
    sx += x; sy += y; sxy += x * y; sxx += x * x;
  }
  float den = bufQtd * sxx - sx * sx;
  if (fabs(den) < 1e-6) return 0;
  return (bufQtd * sxy - sx * sy) / den;
}

Estado classificarPorWBGT(float wbgt) {
  if (wbgt >= WBGT_CRITICO) return CRITICO;
  if (wbgt >= WBGT_ALERTA)  return ALERTA;
  if (wbgt >= WBGT_ATENCAO) return ATENCAO;
  return NORMAL;
}

void adicionarAoBuffer(float t, float ur) {
  bufTemp[bufIndice]  = t;
  bufUmid[bufIndice]  = ur;
  bufTempo[bufIndice] = millis();
  bufIndice = (bufIndice + 1) % N_AMOSTRAS;
  if (bufQtd < N_AMOSTRAS) bufQtd++;
}

void processarLeituras() {
  float somaT = 0, somaU = 0;
  float minT = 1000, maxT = -1000;
  int exced = 0;

  for (int i = 0; i < bufQtd; i++) {
    somaT += bufTemp[i];
    somaU += bufUmid[i];
    if (bufTemp[i] < minT) minT = bufTemp[i];
    if (bufTemp[i] > maxT) maxT = bufTemp[i];
    if (bufTemp[i] > TEMP_LIMITE || bufUmid[i] > UMID_MAX || bufUmid[i] < UMID_MIN) exced++;
  }

  ind.tempMedia   = somaT / bufQtd;
  ind.umidMedia   = somaU / bufQtd;
  ind.tempMin     = minT;
  ind.tempMax     = maxT;
  ind.excedencias = exced;
  ind.indiceCalor = dht.computeHeatIndex(ind.tempMedia, ind.umidMedia, false);
  ind.wbgt        = calcularWBGT(ind.tempMedia, ind.umidMedia);
  ind.tendencia   = calcularTendencia();

  Estado e = classificarPorWBGT(ind.wbgt);
  bool foraDosLimites = ind.tempMedia > TEMP_LIMITE ||
                        ind.umidMedia > UMID_MAX ||
                        ind.umidMedia < UMID_MIN;
  if (foraDosLimites && e < ATENCAO) e = ATENCAO;
  ind.estado = e;
}

void lerSensor() {
  float t  = dht.readTemperature();
  float ur = dht.readHumidity();

  bool valida = !isnan(t) && !isnan(ur) && t >= -40 && t <= 80 && ur >= 0 && ur <= 100;
  if (!valida) {
    falhasSensorSeguidas++;
    Serial.println("[SENSOR] Leitura invalida descartada");
    if (falhasSensorSeguidas >= 3) ind.estado = ERRO_SENSOR;
    return;
  }

  falhasSensorSeguidas = 0;
  tempAtual = t;
  umidAtual = ur;
  adicionarAoBuffer(t, ur);
  processarLeituras();

  Serial.printf("[BORDA] T=%.1fC UR=%.0f%% | media T=%.1fC UR=%.0f%% | IC=%.1fC WBGT=%.1fC | "
                "tend=%+.2fC/min | exced=%d/%d | %s\r\n",
                t, ur, ind.tempMedia, ind.umidMedia, ind.indiceCalor, ind.wbgt,
                ind.tendencia, ind.excedencias, bufQtd, NOME_ESTADO[ind.estado]);
}

void atualizarLeds() {
  if (millis() - tUltimoPisca >= INTERVALO_PISCA_MS) {
    tUltimoPisca = millis();
    estadoPisca = !estadoPisca;
  }
  bool verde = false, amarelo = false, vermelho = false;
  switch (ind.estado) {
    case NORMAL:      verde = true;            break;
    case ATENCAO:     amarelo = true;          break;
    case ALERTA:      vermelho = true;         break;
    case CRITICO:     vermelho = estadoPisca;  break;
    case ERRO_SENSOR: amarelo = estadoPisca;   break;
  }
  digitalWrite(PINO_LED_VERDE, verde);
  digitalWrite(PINO_LED_AMARELO, amarelo);
  digitalWrite(PINO_LED_VERMELHO, vermelho);
}

void desenharCabecalho(const char* titulo) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(titulo);

  display.setCursor(104, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "W" : "-");
  display.print((ultimoEnvioOk && millis() - ultimoEnvioOk < 60000) ? "C" : "-");
  display.setCursor(122, 0);
  display.print(telaAtual + 1);
  display.drawFastHLine(0, 9, OLED_LARGURA, SSD1306_WHITE);
}

void desenharFaixaEstado() {

  display.fillRect(0, 54, OLED_LARGURA, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  const char* txt = NOME_ESTADO[ind.estado];
  int x = (OLED_LARGURA - strlen(txt) * 6) / 2;
  display.setCursor(x, 55);
  display.print(txt);
  display.setTextColor(SSD1306_WHITE);
}

void telaPrincipal() {
  desenharCabecalho("PELE ACADEMIA");
  if (bufQtd == 0) {
    display.setCursor(0, 24);
    display.print("Aguardando leituras");
  } else {
    display.setTextSize(2);
    display.setCursor(0, 14);
    display.printf("%.1f", tempAtual);
    display.setTextSize(1);
    display.print(" C");
    display.setTextSize(2);
    display.setCursor(0, 34);
    display.printf("%.0f", umidAtual);
    display.setTextSize(1);
    display.print(" %UR");

    display.setCursor(80, 16);
    display.print("WBGT");
    display.setCursor(80, 26);
    display.printf("%.1fC", ind.wbgt);
    display.setCursor(80, 40);
    display.print(ind.tendencia > 0.3 ? "subindo" : ind.tendencia < -0.3 ? "caindo" : "estavel");
  }
  desenharFaixaEstado();
}

void telaMedias() {
  desenharCabecalho("MEDIAS LOCAIS");
  display.setCursor(0, 13);
  display.printf("Janela: %d/%d leituras", bufQtd, N_AMOSTRAS);
  display.setCursor(0, 24);
  display.printf("T media : %.1f C", ind.tempMedia);
  display.setCursor(0, 34);
  display.printf("UR media: %.0f %%", ind.umidMedia);
  display.setCursor(0, 44);
  display.printf("Min %.1f  Max %.1f", ind.tempMin, ind.tempMax);
  desenharFaixaEstado();
}

void telaConforto() {
  desenharCabecalho("CONFORTO TERMICO");
  display.setCursor(0, 13);
  display.printf("WBGT est.: %.1f C", ind.wbgt);
  display.setCursor(0, 23);
  display.printf("Sens. term: %.1f C", ind.indiceCalor);
  display.setCursor(0, 33);
  display.printf("Tend: %+.2f C/min", ind.tendencia);
  display.setCursor(0, 43);
  display.print(RECOMENDACAO[ind.estado]);
  desenharFaixaEstado();
}

void telaConexao() {
  desenharCabecalho("NUVEM/REDE");
  display.setCursor(0, 13);
  if (WiFi.status() == WL_CONNECTED) {
    display.printf("WiFi OK %ddBm", WiFi.RSSI());
    display.setCursor(0, 23);
    display.print(WiFi.localIP());
  } else {
    display.print("WiFi desconectado");
    display.setCursor(0, 23);
    display.print("Modo local ativo");
  }
  display.setCursor(0, 33);
  display.printf("TS ok:%lu falha:%lu", enviosOk, enviosFalha);
  display.setCursor(0, 43);
  if (ultimoEnvioOk) display.printf("Ultimo envio: %lus", (millis() - ultimoEnvioOk) / 1000);
  else               display.print("Nenhum envio ainda");
  desenharFaixaEstado();
}

void atualizarDisplay() {
  if (!displayOk) return;
  display.clearDisplay();
  switch (telaAtual) {
    case 0: telaPrincipal(); break;
    case 1: telaMedias();    break;
    case 2: telaConforto();  break;
    case 3: telaConexao();   break;
  }
  display.display();
}

void verificarBotao() {
  bool leitura = digitalRead(PINO_BOTAO);
  if (leitura == LOW && botaoAnterior == HIGH && millis() - tUltimoBotao > DEBOUNCE_MS) {
    tUltimoBotao = millis();
    telaAtual = (telaAtual + 1) % TOTAL_TELAS;
    tUltimaTela = millis();
  }
  botaoAnterior = leitura;
}

void conectarWifi() {
  Serial.printf("[WIFI] Conectando a %s...\r\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA, WIFI_CANAL);
  tUltimaTentativaWifi = millis();
}

void manterWifi() {
  static bool estavaConectado = false;
  bool conectado = WiFi.status() == WL_CONNECTED;
  if (conectado && !estavaConectado) {
    Serial.print("[WIFI] Conectado. IP: ");
    Serial.println(WiFi.localIP());
  }
  if (!conectado && estavaConectado) Serial.println("[WIFI] Conexao perdida — seguindo em modo local");
  estavaConectado = conectado;

  if (!conectado && millis() - tUltimaTentativaWifi >= INTERVALO_WIFI_MS) {
    WiFi.disconnect();
    conectarWifi();
  }
}

String codificarUrl(const String& s) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.') out += c;
    else { out += '%'; out += hex[(c >> 4) & 0xF]; out += hex[c & 0xF]; }
  }
  return out;
}

void enviarThingSpeak() {
  if (bufQtd == 0 || ind.estado == ERRO_SENSOR) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[NUVEM] Sem Wi-Fi: envio adiado, processamento local continua");
    return;
  }

  String status = String(NOME_ESTADO[ind.estado]) + " | " + RECOMENDACAO[ind.estado] +
                  " | exced " + ind.excedencias + "/" + bufQtd;

  String url = String(TS_URL) + "?api_key=" + TS_WRITE_API_KEY +
               "&field1=" + String(tempAtual, 1) +
               "&field2=" + String(umidAtual, 1) +
               "&field3=" + String(ind.tempMedia, 2) +
               "&field4=" + String(ind.umidMedia, 2) +
               "&field5=" + String(ind.indiceCalor, 2) +
               "&field6=" + String(ind.wbgt, 2) +
               "&field7=" + String((int)ind.estado) +
               "&field8=" + String(ind.excedencias) +
               "&status=" + codificarUrl(status);

  HTTPClient http;
  http.setTimeout(4000);
  http.begin(url);
  int codigo = http.GET();
  String resposta = http.getString();
  http.end();

  if (codigo == 200 && resposta.toInt() > 0) {
    enviosOk++;
    ultimoEnvioOk = millis();
    Serial.printf("[NUVEM] Enviado ao ThingSpeak — registro #%s\r\n", resposta.c_str());
  } else {
    enviosFalha++;
    Serial.printf("[NUVEM] Falha no envio (HTTP %d, resposta '%s')\r\n", codigo, resposta.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Pele Academia — Monitor Ambiental de Treino ===");

  pinMode(PINO_LED_VERDE, OUTPUT);
  pinMode(PINO_LED_AMARELO, OUTPUT);
  pinMode(PINO_LED_VERMELHO, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);

  dht.begin();

  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ENDERECO);
  if (!displayOk) {
    Serial.println("[IHM] OLED nao encontrado — seguindo com LEDs e Serial");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(16, 8);
    display.print("PELE");
    display.setCursor(16, 26);
    display.print("ACADEMIA");
    display.setTextSize(1);
    display.setCursor(16, 50);
    display.print("Monitor de treino");
    display.display();
  }

  digitalWrite(PINO_LED_VERDE, HIGH);
  digitalWrite(PINO_LED_AMARELO, HIGH);
  digitalWrite(PINO_LED_VERMELHO, HIGH);
  delay(1500);

  ind.estado = NORMAL;
  conectarWifi();
}

void loop() {
  unsigned long agora = millis();

  if (agora - tUltimaLeitura >= INTERVALO_LEITURA_MS) {
    tUltimaLeitura = agora;
    lerSensor();
  }

  verificarBotao();

  if (agora - tUltimaTela >= INTERVALO_TELA_MS) {
    tUltimaTela = agora;
    telaAtual = (telaAtual + 1) % TOTAL_TELAS;
  }

  atualizarLeds();
  atualizarDisplay();
  manterWifi();

  if (agora - tUltimoEnvio >= INTERVALO_ENVIO_MS) {
    tUltimoEnvio = agora;
    enviarThingSpeak();
  }
}
