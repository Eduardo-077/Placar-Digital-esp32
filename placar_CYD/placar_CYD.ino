#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include "segredos.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

TFT_eSPI tft = TFT_eSPI();

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define BOTAO_A 22  // Ponto Time A
#define BOTAO_B 27  // Ponto Time B
#define BOTAO_C 35  // Reset de Quarto (Faltas + Tempo)
#define BOTAO_D 21  // Falta Time A (ajustar pino conforme o hardware)
#define BOTAO_E 16  // Falta Time B (ajustar pino conforme o hardware)

SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

int telaAtual = 0;
unsigned long inicioTimer = 0;    // millis() de quando deu play
unsigned long tempoPausado = 0;   // tempo acumulado antes da última pausa
bool timerRodando = false;
int ultimoSegundo = -1;           // só redesenhar quando o segundo muda
int periodo = 1;                  // 1º ou 2º tempo (ou 1º ao 4º quarto no basquete)
const unsigned long DURACAO_TEMPO = 45UL * 60 * 1000;   // 45 min em ms (para testar: 1UL * 60 * 1000)
const unsigned long DURACAO_QUARTO = 10UL * 60 * 1000;  // 10 min em ms para o basquete

// Configuração da partida, vinda do Supabase
String nomeA = "CASA";
String nomeB = "VISITANTE";
String modoAtual = "futebol";
long ultimoId = -1;               // id da última partida lida (-1 = nenhuma ainda)
int placarA = 0;
int placarB = 0;
int faltasA = 0;
int faltasB = 0;
unsigned long ultimaConsulta = 0; 


void conectarWiFi(){
  Serial.println("Conectando no wifi");
  WiFi.begin(WIFI_NOME, WIFI_SENHA);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000){
    delay(500);
    Serial.println(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nConectado! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nSem Wi-Fi, seguindo offline.");
  }
}

void buscarPartida() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure cliente;
  cliente.setInsecure();          // não confere o certificado (melhorar na Etapa 5)

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/partidas?placar_id=eq." + PLACAR_ID
             + "&order=id.desc&limit=1";

  http.begin(cliente, url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(3000);          // no máximo 3 s esperando

  int codigo = http.GET();
  Serial.print("Codigo HTTP: ");
  Serial.println(codigo);
  if (codigo == 200) {
    JsonDocument doc;
    DeserializationError erro = deserializeJson(doc, http.getString());

    if (erro) {
      Serial.print("Erro no JSON: ");
      Serial.println(erro.c_str());
    } else if (doc.size() == 0) {
      Serial.println("Nenhuma partida no banco.");
    } else {
      JsonObject p = doc[0];
      long id = p["id"];

      if (id != ultimoId) {
        bool primeiraLeitura = (ultimoId == -1);
        ultimoId = id;

        if (primeiraLeitura) {
          // ao ligar: só guarda os nomes, não pula direto para o jogo
          nomeA = p["time_a"].as<String>();
          nomeB = p["time_b"].as<String>();
          Serial.println("Partida atual: " + nomeA + " x " + nomeB);
        } else {
          Serial.println("Partida nova!");
          aplicarPartida(p["modo"].as<String>(), p["time_a"].as<String>(), p["time_b"].as<String>());
        }
      }
    }
  }
  http.end();
}

unsigned long tempoDecorrido(){
  if(timerRodando) return tempoPausado + (millis() - inicioTimer);
  return tempoPausado;
}

void iniciarTimer() {
  if (!timerRodando) {
    inicioTimer = millis();
    timerRodando = true;
  }
}

void pausarTimer() {
  if (timerRodando) {
    tempoPausado += millis() - inicioTimer;
    timerRodando = false;
  }
}

void zerarTimer() {
  timerRodando = false;
  tempoPausado = 0;
  ultimoSegundo = -1;
}

bool tempoAcabou() {
  return tempoDecorrido() >= DURACAO_TEMPO;
}

bool tempoAcabouBasquete() {
  return tempoDecorrido() >= DURACAO_QUARTO;
}

// Escreve embaixo do relógio em qual parte do jogo estamos (Futebol)
void desenharPeriodo() {
  const char* texto;
  if (periodo == 1) texto = tempoAcabou() ? "INTERVALO" : "1o TEMPO";
  else              texto = tempoAcabou() ? "FIM DE JOGO" : "2o TEMPO";

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextPadding(tft.textWidth("FIM DE JOGO"));
  tft.drawString(texto, 160, 35);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void desenharTimer() {
  unsigned long decorrido = tempoDecorrido();
  if (decorrido >= DURACAO_TEMPO) {        // chegou nos 45: para o relógio
    pausarTimer();
    tempoPausado = DURACAO_TEMPO;
    decorrido = DURACAO_TEMPO;
  }

  // No 2º tempo o relógio continua de 45:00 até 90:00
  int segundos = ((periodo - 1) * DURACAO_TEMPO + decorrido) / 1000;
  if (segundos == ultimoSegundo) return;   // nada mudou, não redesenha
  ultimoSegundo = segundos;
  desenharPeriodo();

  char texto[6];
  snprintf(texto, sizeof(texto), "%02d:%02d", segundos / 60, segundos % 60);

  tft.setTextDatum(TC_DATUM);              // x, y = centro do topo do texto
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);
  tft.setTextPadding(tft.textWidth("00:00"));
  tft.drawString(texto, 160, 5);
  tft.setTextDatum(TL_DATUM);              // volta ao padrão
  tft.setTextPadding(0);
}

// --- FUNÇÕES DO BASQUETE ---

void desenharPeriodoBasquete() {
  String texto;
  if (tempoAcabouBasquete()) {
    texto = (periodo >= 4) ? "FIM DE JOGO" : "FIM " + String(periodo) + "o Q";
  } else {
    texto = (periodo <= 4) ? String(periodo) + "o QUARTO" : "PRORROG.";
  }

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextPadding(tft.textWidth("FIM DE JOGO"));
  tft.drawString(texto, 160, 35);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void desenharTimerBasquete() {
  unsigned long decorrido = tempoDecorrido();
  if (decorrido >= DURACAO_QUARTO) {
    pausarTimer();
    tempoPausado = DURACAO_QUARTO;
    decorrido = DURACAO_QUARTO;
  }

  // Cronômetro regressivo (10:00 até 00:00)
  unsigned long restante = DURACAO_QUARTO - decorrido;
  int segundos = restante / 1000;
  if (segundos == ultimoSegundo) return;
  ultimoSegundo = segundos;
  desenharPeriodoBasquete();

  char texto[6];
  snprintf(texto, sizeof(texto), "%02d:%02d", segundos / 60, segundos % 60);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);
  tft.setTextPadding(tft.textWidth("00:00"));
  tft.drawString(texto, 160, 5);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void atualizarFaltasBasquete() {
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);

  // Fica vermelho ao atingir 5 faltas coletivas
  uint16_t corA = (faltasA >= 5) ? TFT_RED : TFT_WHITE;
  tft.setTextColor(corA, TFT_BLACK);
  tft.setTextPadding(tft.textWidth("FALTAS: 99"));
  tft.drawString("FALTAS: " + String(faltasA), 80, 195);

  uint16_t corB = (faltasB >= 5) ? TFT_RED : TFT_WHITE;
  tft.setTextColor(corB, TFT_BLACK);
  tft.setTextPadding(tft.textWidth("FALTAS: 99"));
  tft.drawString("FALTAS: " + String(faltasB), 240, 195);

  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void atualizarPlacarBasquete() {
  tft.setTextSize(5);
  tft.setTextDatum(TC_DATUM);
  tft.setTextPadding(tft.textWidth("999")); // Suporta até 3 dígitos centralizados

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String(placarA), 80, 110);

  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(String(placarB), 240, 110);

  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

// ---------------------------

void desenharBola(){
   tft.fillCircle(250, 120, 80, 0x001F);
}

void telaInicial(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("Selecione o modo de jogo", 20, 10);
  int posy1 = 80;
  int posy2 = 140; 
  //tft.drawRoundRect(posição x, pos y, largura, altura, arredondamento(n pode ser maior q metade da largura ou altura), cor da borda);   
  tft.drawRoundRect(5, posy1, 105, 40, 20, 0x001F);
  tft.drawString("Futebol", 20, posy1 + 10);   //x+y=110
  tft.drawRoundRect(110, posy1, 105, 40, 20, 0x001F);
  tft.drawString("Basquete", 115, posy1 + 10); //x+y=205
  tft.drawRoundRect(215, posy1, 105, 40, 20, 0x001F);
  tft.drawString("Volei", 240, posy1 + 10); //x+y=330

  tft.drawRoundRect(5, posy2, 105, 40, 20, 0x001F);
  tft.drawString("Tenis", 30, 150);   //x+y= 180
  tft.drawRoundRect(110, posy2, 105, 40, 20, 0x001F);
  tft.drawString("JiuJitsu", 115, posy2 + 10); //x+y= 265
  tft.drawRoundRect(215, posy2, 105, 40, 20, 0x001F);
  tft.drawString("Padel", 240, posy2 + 10); //x+y=390
}

bool tocouEm(int x, int y, int bx, int by, int bw, int bh){
  return x >= bx && x <= bx + bw && y >= by && y <= by + bh;
}

void desenharPlacarFutebol(String timeA, String timeB, int placarA, int placarB, int faltasA, int faltasB) { 
  tft.fillScreen(TFT_BLACK);

  // Título
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  //tft.setCursor(100, 10);
  //tft.println("PLACAR ESP32");

  // Linha divisória
  tft.drawLine(160, 60, 160, 220, TFT_DARKGREY); // inicio em x, incio em y, final x, final ym cor

  // Nomes dos times, centralizados em cada metade
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);              // x = centro do texto
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(timeA, 80, 70);           // centro da metade esquerda
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(timeB, 240, 70);          // centro da metade direita
  tft.setTextDatum(TL_DATUM);

  // Placar
  tft.setTextSize(5);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(60, 110);
  tft.println(placarA);

  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setCursor(230, 110);
  tft.println(placarB);
}

void desenharPlacarBasquete(String timeA, String timeB, int placarA, int placarB, int set){
  tft.fillScreen(TFT_BLACK);

  // Linha divisória
  tft.drawLine(160, 60, 160, 220, TFT_DARKGREY);

  // Nomes dos times, centralizados em cada metade
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(timeA, 80, 70);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(timeB, 240, 70);
  tft.setTextDatum(TL_DATUM);

  atualizarPlacarBasquete();
  atualizarFaltasBasquete();
  desenharTimerBasquete();
}

void desenharPlacarVolei(String timeA, String timeB, int set, int ){

}

void desenharPlacarTenis(String timeA, String timeB, int setsA, int setsB){

}

void desenharPlacarJiuJitsu(String lutadorA, String lutadorB, int punicoesA, int punicoesB, int vantagensA, int vantagensB){

}

void aplicarPartida(String modo, String timeA, String timeB){
  modoAtual = modo;
  nomeA = timeA;
  nomeB = timeB;
  placarA = 0;
  placarB = 0;
  faltasA = 0;
  faltasB = 0;
  periodo = 1;
  zerarTimer();

  if(modo == "futebol"){
    telaAtual = 1;
    desenharPlacarFutebol(nomeA, nomeB, placarA, placarB, 0, 0);
  } else if(modo == "basquete"){
    telaAtual = 2;
    desenharPlacarBasquete(nomeA, nomeB, placarA, placarB, periodo);
  } else{ //Colocar outras telas de jogo
    telaAtual = 0;
    telaInicial();
  }
}

void setup() {
  Serial.begin(115200);
  conectarWiFi();
  buscarPartida();
  tft.init();
  tft.setRotation(1);        // 0 a 3. Use 1 ou 3 para paisagem (320x240)
  tft.fillScreen(TFT_BLACK); // limpa a tela

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);  // use a mesma rotação da tela

  pinMode(BOTAO_A, INPUT_PULLUP);
  pinMode(BOTAO_B, INPUT_PULLUP);
  pinMode(BOTAO_C, INPUT);        // pull-up é o resistor externo
  pinMode(BOTAO_D, INPUT_PULLUP); // Falta Time A
  pinMode(BOTAO_E, INPUT_PULLUP); // Falta Time B
  
  telaInicial();
}

// Redesenha só os números, sem limpar a tela toda (Futebol)
void atualizarPlacar() {
  tft.setTextSize(5);

  tft.fillRect(60, 110, 90, 40, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(60, 110);
  tft.println(placarA);

  tft.fillRect(230, 110, 90, 40, TFT_BLACK);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setCursor(230, 110);
  tft.println(placarB);
}

void loop() {
  // Touch: escolher o modo na tela inicial
  if(millis() - ultimaConsulta > 3000 && (telaAtual == 0 || !timerRodando)){
      ultimaConsulta = millis();
      buscarPartida();
  }
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, 200, 3700, 1, 320);
    int y = map(p.y, 240, 3800, 1, 240);

    if (telaAtual == 0) {
      if (tocouEm(x, y, 5, 80, 105, 40)) {          // Futebol
        telaAtual = 1;
        placarA = 0;
        placarB = 0;
        periodo = 1;
        zerarTimer();
        desenharPlacarFutebol(nomeA, nomeB, placarA, placarB, 0, 0);
      } else if (tocouEm(x, y, 110, 80, 105, 40)) { // Basquete
        telaAtual = 2;
        placarA = 0;
        placarB = 0;
        faltasA = 0;
        faltasB = 0;
        periodo = 1;
        zerarTimer();
        desenharPlacarBasquete(nomeA, nomeB, placarA, placarB, periodo);
      }
    } else if (telaAtual == 1) {                    // no futebol: toque = play/pausa
      if (tempoAcabou()) {
        if (periodo == 1) {                         // no intervalo: toque prepara o 2º tempo
          periodo = 2;
          zerarTimer();
        }
      } else if (timerRodando) pausarTimer();
      else iniciarTimer();
    } else if (telaAtual == 2) {                    // no basquete: toque = play/pausa
      if (!tempoAcabouBasquete()) {
        if (timerRodando) pausarTimer();
        else iniciarTimer();
      }
    }

    while (ts.touched()) delay(10);   // espera tirar o dedo
    delay(200);   // evita que um toque conte várias vezes
  }

  // Botões físicos: marcar gol no futebol
  if (telaAtual == 1) {
    desenharTimer();
    if (digitalRead(BOTAO_A) == LOW) {
      placarA++;
      atualizarPlacar();
      while (digitalRead(BOTAO_A) == LOW) delay(10);   // espera soltar
      delay(50);
    }
    if (digitalRead(BOTAO_B) == LOW) {
      placarB++;
      atualizarPlacar();
      while (digitalRead(BOTAO_B) == LOW) delay(10);
      delay(50);
    }
  }

  // Botões físicos: Basquete (mesma lógica de pontos + botões de falta e reset de quarto)
  if (telaAtual == 2) {
    desenharTimerBasquete();

    // Ponto Time A
    if (digitalRead(BOTAO_A) == LOW) {
      placarA++;
      atualizarPlacarBasquete();
      while (digitalRead(BOTAO_A) == LOW) delay(10);
      delay(50);
    }
    // Ponto Time B
    if (digitalRead(BOTAO_B) == LOW) {
      placarB++;
      atualizarPlacarBasquete();
      while (digitalRead(BOTAO_B) == LOW) delay(10);
      delay(50);
    }
    // Reset de Quarto (reseta tempo e faltas, avança o quarto e mantém os pontos)
    if (digitalRead(BOTAO_C) == LOW) {
      faltasA = 0;
      faltasB = 0;
      periodo++;
      zerarTimer();
      atualizarFaltasBasquete();
      desenharTimerBasquete();
      while (digitalRead(BOTAO_C) == LOW) delay(10);
      delay(50);
    }
    // Falta Time A
    if (digitalRead(BOTAO_D) == LOW) {
      faltasA++;
      atualizarFaltasBasquete();
      while (digitalRead(BOTAO_D) == LOW) delay(10);
      delay(50);
    }
    // Falta Time B
    if (digitalRead(BOTAO_E) == LOW) {
      faltasB++;
      atualizarFaltasBasquete();
      while (digitalRead(BOTAO_E) == LOW) delay(10);
      delay(50);
    }
  }
}