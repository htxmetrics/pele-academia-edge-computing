/*
 * ============================================================================
 *  PELÉ ACADEMIA — Monitor Ambiental de Treino (Edge Computing)
 *  FIAP · Engenharia de Software · Challenge 2026 · Sprint 3 · ECCS
 * ============================================================================
 *
 *  O ESP32 lê temperatura e umidade (DHT22) do local de treino e, NA BORDA:
 *    1. valida cada leitura (descarta falhas e valores fora da faixa do sensor);
 *    2. guarda as últimas N leituras num buffer circular;
 *    3. calcula média móvel, mínimo e máximo;
 *    4. calcula o índice de calor (sensação térmica) e o IBUTG/WBGT estimado,
 *       indicador usado no esporte para avaliar o risco de estresse térmico;
 *    5. calcula a tendência da temperatura (°C/min) por regressão linear;
 *    6. conta as leituras que ultrapassaram os limites configurados;
 *    7. classifica o ambiente em NORMAL / ATENCAO / ALERTA / CRITICO;
 *    8. mostra tudo na IHM local (OLED + LEDs de sinalização + botão).
 *
 *  Só DEPOIS disso os dados (já processados) vão para o ThingSpeak, que fica
 *  responsável pelo armazenamento, histórico, gráficos e acesso remoto.
 *  Se o Wi-Fi ou a nuvem caírem, o monitoramento local continua funcionando.
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <math.h>

// ----------------------------------------------------------------------------
//  Configuração de rede e nuvem
// ----------------------------------------------------------------------------
const char* WIFI_SSID  = "Wokwi-GUEST";   // rede aberta do simulador Wokwi
const char* WIFI_SENHA = "";
const int   WIFI_CANAL = 6;               // canal do Wokwi-GUEST (acelera a conexão)

// Write API Key do canal ThingSpeak (Channels > API Keys)
const char* TS_WRITE_API_KEY = "1QA5UAK2ICPLZFUK";
const char* TS_URL           = "http://api.thingspeak.com/update";

// ----------------------------------------------------------------------------
//  Pinos
// ----------------------------------------------------------------------------
#define PINO_DHT          15
#define TIPO_DHT          DHT22
#define PINO_LED_VERDE    25
#define PINO_LED_AMARELO  26
#define PINO_LED_VERMELHO 27
#define PINO_BOTAO        4     // troca a tela da IHM (INPUT_PULLUP)
// OLED SSD1306 no barramento I2C padrão: SDA = GPIO21, SCL = GPIO22

// ----------------------------------------------------------------------------
//  Display
// ----------------------------------------------------------------------------
#define OLED_LARGURA  128
#define OLED_ALTURA   64
#define OLED_ENDERECO 0x3C

// ----------------------------------------------------------------------------
//  Temporização (tudo com millis(), sem delay() no loop)
// ----------------------------------------------------------------------------
const unsigned long INTERVALO_LEITURA_MS   = 2000;   // DHT22 aceita 1 leitura a cada 2 s
const unsigned long INTERVALO_ENVIO_MS     = 20000;  // ThingSpeak gratuito: mínimo 15 s
const unsigned long INTERVALO_TELA_MS      = 6000;   // rotação automática das telas
const unsigned long INTERVALO_WIFI_MS      = 10000;  // nova tentativa de conexão
const unsigned long INTERVALO_PISCA_MS     = 250;    // pisca do LED em estado crítico
const unsigned long DEBOUNCE_MS            = 200;

// ----------------------------------------------------------------------------
//  Processamento local
// ----------------------------------------------------------------------------
const int N_AMOSTRAS = 10;   // janela da média móvel (10 x 2 s = últimos 20 s)

// Limites de referência (ver README, seção "Critérios de atenção")
const float WBGT_ATENCAO  = 25.0;  // °C: pausas para hidratação
const float WBGT_ALERTA   = 28.0;  // °C: reduzir intensidade / volume do treino
const float WBGT_CRITICO  = 32.0;  // °C: suspender ou adiar atividade intensa
const float TEMP_LIMITE   = 32.0;  // °C: temperatura do ar elevada
const float UMID_MAX      = 80.0;  // %: dificulta a evaporação do suor
const float UMID_MIN      = 30.0;  // %: ar seco, desidratação e desconforto respiratório

enum Estado { NORMAL = 0, ATENCAO = 1, ALERTA = 2, CRITICO = 3, ERRO_SENSOR = 4 };
const char* NOME_ESTADO[] = { "NORMAL", "ATENCAO", "ALERTA", "CRITICO", "ERRO SENSOR" };
const char* RECOMENDACAO[] = {
  "Treino liberado",
  "Pausas p/ hidratacao",
  "Reduzir intensidade",
  "Suspender treino",
  "Verificar sensor"
};

// ----------------------------------------------------------------------------
//  Objetos e variáveis globais
// ----------------------------------------------------------------------------
DHT dht(PINO_DHT, TIPO_DHT);
Adafruit_SSD1306 display(OLED_LARGURA, OLED_ALTURA, &Wire, -1);

// Buffer circular com as últimas leituras válidas
float         bufTemp[N_AMOSTRAS];
float         bufUmid[N_AMOSTRAS];
unsigned long bufTempo[N_AMOSTRAS];
int           bufIndice = 0;
int           bufQtd    = 0;

// Última leitura bruta
float tempAtual = NAN;
float umidAtual = NAN;

// Resultados do processamento local
struct Indicadores {
  float  tempMedia, umidMedia;
  float  tempMin, tempMax;
  float  indiceCalor;      // °C (sensação térmica, NOAA)
  float  wbgt;             // °C (IBUTG estimado)
  float  tendencia;        // °C por minuto
  int    excedencias;      // leituras na janela fora dos limites
  Estado estado;
} ind;

int falhasSensorSeguidas = 0;

// Estatísticas da nuvem
unsigned long enviosOk = 0, enviosFalha = 0;
unsigned long ultimoEnvioOk = 0;

// Controle de tempo
unsigned long tUltimaLeitura = 0, tUltimoEnvio = 0, tUltimaTela = 0;
unsigned long tUltimaTentativaWifi = 0, tUltimoPisca = 0, tUltimoBotao = 0;
bool estadoPisca = false;
int  telaAtual = 0;
const int TOTAL_TELAS = 4;
bool botaoAnterior = HIGH;
bool displayOk = false;

// ============================================================================
//  PROCESSAMENTO NA BORDA
// ============================================================================

// Pressão de vapor (hPa) a partir de temperatura e umidade relativa (Magnus)
float pressaoVapor(float t, float ur) {
  return (ur / 100.0) * 6.105 * exp((17.27 * t) / (237.7 + t));
}

// WBGT/IBUTG estimado para ambiente sombreado (fórmula do Australian Bureau
// of Meteorology). Não considera radiação solar direta nem vento.
float calcularWBGT(float t, float ur) {
  return 0.567 * t + 0.393 * pressaoVapor(t, ur) + 3.94;
}

// Inclinação da reta (mínimos quadrados) da temperatura em função do tempo
float calcularTendencia() {
  if (bufQtd < 3) return 0;
  // Índice da amostra mais antiga dentro do buffer circular
  int inicio = (bufQtd < N_AMOSTRAS) ? 0 : bufIndice;
  unsigned long t0 = bufTempo[inicio];
  float sx = 0, sy = 0, sxy = 0, sxx = 0;
  for (int i = 0; i < bufQtd; i++) {
    int k = (inicio + i) % N_AMOSTRAS;
    float x = (bufTempo[k] - t0) / 60000.0;   // minutos
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

  // A classificação usa a MÉDIA, e não a leitura instantânea, para evitar que
  // um pico isolado (ruído) mude o estado do ambiente.
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

  // Validação: falha de leitura ou valor fora da faixa física do DHT22
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

// ============================================================================
//  IHM LOCAL — LEDs
// ============================================================================

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
    case CRITICO:     vermelho = estadoPisca;  break;   // vermelho piscando
    case ERRO_SENSOR: amarelo = estadoPisca;   break;   // amarelo piscando
  }
  digitalWrite(PINO_LED_VERDE, verde);
  digitalWrite(PINO_LED_AMARELO, amarelo);
  digitalWrite(PINO_LED_VERMELHO, vermelho);
}

// ============================================================================
//  IHM LOCAL — Display OLED (4 telas)
// ============================================================================

void desenharCabecalho(const char* titulo) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(titulo);
  // Indicadores de conectividade no canto direito
  display.setCursor(104, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "W" : "-");
  display.print((ultimoEnvioOk && millis() - ultimoEnvioOk < 60000) ? "C" : "-");
  display.setCursor(122, 0);
  display.print(telaAtual + 1);
  display.drawFastHLine(0, 9, OLED_LARGURA, SSD1306_WHITE);
}

void desenharFaixaEstado() {
  // Faixa invertida na parte de baixo com o estado atual
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
    tUltimaTela = millis();   // reinicia a rotação automática
  }
  botaoAnterior = leitura;
}

// ============================================================================
//  CONECTIVIDADE — Wi-Fi e ThingSpeak
// ============================================================================

void conectarWifi() {
  Serial.printf("[WIFI] Conectando a %s...\r\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA, WIFI_CANAL);
  tUltimaTentativaWifi = millis();
}

// Reconexão não bloqueante: o loop continua lendo o sensor enquanto isso
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

  // Mensagem de status: aparece no canal junto de cada registro
  String status = String(NOME_ESTADO[ind.estado]) + " | " + RECOMENDACAO[ind.estado] +
                  " | exced " + ind.excedencias + "/" + bufQtd;

  String url = String(TS_URL) + "?api_key=" + TS_WRITE_API_KEY +
               "&field1=" + String(tempAtual, 1) +       // temperatura instantânea
               "&field2=" + String(umidAtual, 1) +       // umidade instantânea
               "&field3=" + String(ind.tempMedia, 2) +   // temperatura média (borda)
               "&field4=" + String(ind.umidMedia, 2) +   // umidade média (borda)
               "&field5=" + String(ind.indiceCalor, 2) + // índice de calor (borda)
               "&field6=" + String(ind.wbgt, 2) +        // WBGT estimado (borda)
               "&field7=" + String((int)ind.estado) +    // estado 0..3 (borda)
               "&field8=" + String(ind.excedencias) +    // leituras fora do limite (borda)
               "&status=" + codificarUrl(status);

  HTTPClient http;
  http.setTimeout(4000);
  http.begin(url);
  int codigo = http.GET();
  String resposta = http.getString();
  http.end();

  // O ThingSpeak devolve o número do registro criado, ou "0" se recusou
  if (codigo == 200 && resposta.toInt() > 0) {
    enviosOk++;
    ultimoEnvioOk = millis();
    Serial.printf("[NUVEM] Enviado ao ThingSpeak — registro #%s\r\n", resposta.c_str());
  } else {
    enviosFalha++;
    Serial.printf("[NUVEM] Falha no envio (HTTP %d, resposta '%s')\r\n", codigo, resposta.c_str());
  }
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================

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

  // Teste rápido dos LEDs na inicialização
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
