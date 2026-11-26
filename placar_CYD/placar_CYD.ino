// ============================================================================
// 1. BIBLIOTECAS, PINOS E VARIÁVEIS GLOBAIS
// ============================================================================
// WebServer/ElegantOTA PRECISAM vir antes da TFT_eSPI: ela inclui o FS.h
// com FS_NO_GLOBALS, e aí o WebServer.h não acha o tipo "FS" (erro de compilação).
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>   // biblioteca "ElegantOTA" (ayushsharma82)
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include "segredos.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#if !defined(OTA_USUARIO) || !defined(OTA_SENHA)
#error "Adicione OTA_USUARIO e OTA_SENHA no segredos.h (veja o segredos_exemplo.h)"
#endif

TFT_eSPI tft = TFT_eSPI();

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define BOTAO_A 22  // +1 Ponto Time A (Todos os esportes)
#define BOTAO_B 27  // +1 Ponto Time B (Todos os esportes)
#define BOTAO_C 35  // Reset de Quarto/Set (Basquete / Vôlei)

// D e E são botões de TOQUE na tela (não são pinos): o IO21 é a luz de fundo
// da tela e o IO16 é o LED RGB, não dá para ligar botão neles.
// Quando chegar o PCF8574, todos os botões passam para ele (ver apertou()).
#define BOTAO_D 100 // Falta A (Basquete) / -1 Ponto A (Vôlei)
#define BOTAO_E 101 // Falta B (Basquete) / -1 Ponto B (Vôlei)
bool toqueD = false;              // marcado pelo toque, consumido por apertou()
bool toqueE = false;

SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

// Controle de Tela e Tempo
int telaAtual = 0;                // 0=Menu, 1=Futebol, 2=Basquete, 3=Vôlei
unsigned long inicioTimer = 0;    // millis() de quando deu play
unsigned long tempoPausado = 0;   // tempo acumulado antes da última pausa
bool timerRodando = false;
int ultimoSegundo = -1;           // só redesenhar quando o segundo muda
int periodo = 1;                  // Tempo (Futebol), Quarto (Basquete) ou Set atual (Vôlei)

const unsigned long DURACAO_TEMPO = 45UL * 60 * 1000;   // 45 min em ms (Futebol)
const unsigned long DURACAO_QUARTO = 10UL * 60 * 1000;  // 10 min em ms (Basquete)

// Dados da Partida e Pontuações
String nomeA = "CASA";
String nomeB = "VISITANTE";
String modoAtual = "futebol";

int placarA = 0;
int placarB = 0;
int faltasA = 0;
int faltasB = 0;
int setsA = 0;
int setsB = 0;
bool setEncerrado = false;


// ============================================================================
// 2. REDE (WI-FI E SUPABASE)
// ============================================================================
void aplicarPartida(String modo, String timeA, String timeB); // Protótipo

// A rede roda numa tarefa separada, no núcleo 0 (tarefaRede), para o loop()
// nunca ficar esperando a internet. Regra: SÓ o loop() (núcleo 1) mexe na
// tela e nas variáveis do jogo. A tarefa de rede só deixa a partida nova
// nesta fila ("caixa de correio"), e o loop() decide quando aplicar.
struct Partida {
  bool primeira;                  // leitura ao ligar: só guarda os nomes
  bool substituir;                // true = aplica mesmo com jogo rodando
  char modo[12];
  char timeA[25];                 // 12 letras (até 2 bytes cada, se tiver acento)
  char timeB[25];
};
QueueHandle_t filaPartidas;       // tamanho 1: sempre guarda só a mais recente
long ultimoId = -1;               // id da última partida lida (só a tarefa de rede usa)
volatile bool atualizandoOTA = false; // durante a OTA a tarefa de rede fica quieta

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
        Partida nova;
        nova.primeira = (ultimoId == -1);   // ao ligar: não pula direto para o jogo
        nova.substituir = p["substituir"] | false;
        strlcpy(nova.modo,  p["modo"]   | "", sizeof(nova.modo));
        strlcpy(nova.timeA, p["time_a"] | "", sizeof(nova.timeA));
        strlcpy(nova.timeB, p["time_b"] | "", sizeof(nova.timeB));
        ultimoId = id;

        Serial.printf("%s %s x %s\n", nova.primeira ? "Partida atual:" : "Partida nova!",
                      nova.timeA, nova.timeB);
        xQueueOverwrite(filaPartidas, &nova);   // se já tinha uma esperando, troca pela nova
      }
    }
  }
  http.end();
}

// Roda para sempre no núcleo 0: Wi-Fi + consulta ao Supabase a cada 3 s.
// Pode demorar à vontade (delay, HTTPS), porque não trava a tela nem os botões.
void tarefaRede(void *parametro) {
  conectarWiFi();
  for (;;) {
    if (atualizandoOTA) {         // não disputa rede/memória com a atualização
      delay(500);
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Sem Wi-Fi, tentando de novo...");
      WiFi.disconnect();
      WiFi.begin(WIFI_NOME, WIFI_SENHA);
      delay(10000);               // dá tempo de conectar antes de tentar de novo
      continue;
    }
    buscarPartida();
    delay(3000);
  }
}


// ============================================================================
// 2.1 ATUALIZAÇÃO PELO WI-FI (ELEGANTOTA)
// ============================================================================
// Abrir http://IP_DO_PLACAR/update (ou http://placar.local/update) no
// navegador, entrar com OTA_USUARIO/OTA_SENHA e enviar o .bin gerado em
// Sketch -> Export Compiled Binary. Só funciona na mesma rede do placar.
// O servidor roda no loop() (núcleo 1), por isso pode desenhar na tela.
WebServer servidor(80);
bool servidorIniciado = false;
int ultimoPorcento = -1;

void desenharIP() {
  if (telaAtual != 0 || WiFi.status() != WL_CONNECTED) return;
  tft.setTextSize(1);
  tft.setTextDatum(BR_DATUM);              // canto inferior direito
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(WiFi.localIP().toString(), 315, 235);
  tft.setTextDatum(TL_DATUM);
}

void inicioOTA() {
  atualizandoOTA = true;
  pausarTimer();
  ultimoPorcento = -1;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("ATUALIZANDO...", 160, 90);
  tft.drawRect(40, 120, 240, 20, TFT_WHITE);
  tft.setTextDatum(TL_DATUM);
}

void progressoOTA(size_t atual, size_t total) {
  if (total == 0) return;
  int porcento = atual * 100 / total;
  if (porcento == ultimoPorcento) return;  // só redesenha quando muda
  ultimoPorcento = porcento;
  tft.fillRect(42, 122, 236 * porcento / 100, 16, TFT_GREEN);
  tft.setTextDatum(TC_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(tft.textWidth("100%"));
  tft.drawString(String(porcento) + "%", 160, 150);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

void fimOTA(bool sucesso) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.setTextPadding(320);
  if (sucesso) {                           // ElegantOTA reinicia o ESP sozinho
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("OK! REINICIANDO", 160, 190);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("ERRO NA ATUALIZACAO", 160, 190);
    atualizandoOTA = false;
  }
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

// Chamado no loop(): sobe o servidor assim que o Wi-Fi conectar
// (antes disso a rede do ESP ainda não existe) e depois atende os pedidos.
void cuidarOTA() {
  if (!servidorIniciado) {
    if (WiFi.status() != WL_CONNECTED) return;
    MDNS.begin("placar");                  // http://placar.local
    ElegantOTA.begin(&servidor, OTA_USUARIO, OTA_SENHA);
    ElegantOTA.onStart(inicioOTA);
    ElegantOTA.onProgress(progressoOTA);
    ElegantOTA.onEnd(fimOTA);
    servidor.begin();
    servidorIniciado = true;
    Serial.print("OTA pronta: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/update");
    desenharIP();
  }
  servidor.handleClient();
  ElegantOTA.loop();
}


// ============================================================================
// 3. CONTROLE GERAL DE CRONÔMETRO
// ============================================================================
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


// ============================================================================
// 4. MENU INICIAL E UTILITÁRIOS DE TELA
// ============================================================================
bool tocouEm(int x, int y, int bx, int by, int bw, int bh){
  return x >= bx && x <= bx + bw && y >= by && y <= by + bh;
}

// Retorna true uma vez por aperto (espera soltar o botão).
// É o ÚNICO lugar que lê os botões: para trocar pelo PCF8574, basta mudar
// esta função (ex.: digitalRead(botao) vira pcf.read(botao)).
bool apertou(int botao) {
  if (botao == BOTAO_D) { bool t = toqueD; toqueD = false; return t; }
  if (botao == BOTAO_E) { bool t = toqueE; toqueE = false; return t; }

  if (digitalRead(botao) != LOW) return false;
  while (digitalRead(botao) == LOW) delay(10);   // espera soltar
  delay(50);
  return true;
}

// Desenha os botões de toque D (esquerda) e E (direita) no rodapé da tela
void desenharBotoesToque(String texto) {
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);              // x, y = centro do texto
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRoundRect(40, 205, 80, 30, 8, TFT_DARKGREY);
  tft.drawString(texto, 80, 220);
  tft.drawRoundRect(200, 205, 80, 30, 8, TFT_DARKGREY);
  tft.drawString(texto, 240, 220);
  tft.setTextDatum(TL_DATUM);
}

// Toque no rodapé: metade esquerda = botão D, metade direita = botão E.
// Retorna true se o toque foi num desses botões.
bool tocouBotoesToque(int x, int y) {
  if (tocouEm(x, y, 0, 195, 160, 45))   { toqueD = true; return true; }
  if (tocouEm(x, y, 160, 195, 160, 45)) { toqueE = true; return true; }
  return false;
}

void desenharBola(){
   tft.fillCircle(250, 120, 80, 0x001F);
}

void telaInicial(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("Escolha o esporte OTA", 20, 10);
  int posy1 = 80;
  int posy2 = 140; 
  //tft.drawRoundRect(posição x, pos y, largura, altura, arredondamento, cor da borda);   
  tft.drawRoundRect(5, posy1, 105, 40, 20, 0x001F);
  tft.drawString("Futebol", 20, posy1 + 10);   //x+y=110
  tft.drawRoundRect(110, posy1, 105, 40, 20, 0x001F);
  tft.drawString("Basquete", 115, posy1 + 10); //x+y=205
  tft.drawRoundRect(215, posy1, 105, 40, 20, 0x001F);
  tft.drawString("Volei", 240, posy1 + 10);    //x+y=330

  tft.drawRoundRect(5, posy2, 105, 40, 20, 0x001F);
  tft.drawString("Tenis", 30, 150);            //x+y= 180
  tft.drawRoundRect(110, posy2, 105, 40, 20, 0x001F);
  tft.drawString("JiuJitsu", 115, posy2 + 10); //x+y= 265
  tft.drawRoundRect(215, posy2, 105, 40, 20, 0x001F);
  tft.drawString("Padel", 240, posy2 + 10);    //x+y=390
  desenharIP();                                // IP no canto, para achar a OTA
}


// ============================================================================
// 5. MÓDULO: FUTEBOL (telaAtual = 1) - Mantido intacto
// ============================================================================
bool tempoAcabou() {
  return tempoDecorrido() >= DURACAO_TEMPO;
}

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


// ============================================================================
// 6. MÓDULO: BASQUETE (telaAtual = 2)
// ============================================================================
bool tempoAcabouBasquete() {
  return tempoDecorrido() >= DURACAO_QUARTO;
}

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
  tft.drawString("FALTAS: " + String(faltasA), 80, 180);

  uint16_t corB = (faltasB >= 5) ? TFT_RED : TFT_WHITE;
  tft.setTextColor(corB, TFT_BLACK);
  tft.setTextPadding(tft.textWidth("FALTAS: 99"));
  tft.drawString("FALTAS: " + String(faltasB), 240, 180);

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
  desenharBotoesToque("FALTA");
  desenharTimerBasquete();
}


// ============================================================================
// 7. MÓDULO: VÔLEI (telaAtual = 3)
// ============================================================================
void desenharTopoVolei() {
  String texto;
  if (setsA == 3 || setsB == 3) {
    texto = "FIM DE JOGO";
  } else if (setEncerrado) {
    texto = "FIM DO SET";
  } else if (periodo == 5) {
    texto = "TIE-BREAK";
  } else {
    texto = String(periodo) + "o SET";
  }

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(3);
  tft.setTextPadding(tft.textWidth("FIM DE JOGO"));
  tft.drawString(texto, 160, 15);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void atualizarSetsVolei() {
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextPadding(tft.textWidth("SETS: 9"));

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("SETS: " + String(setsA), 80, 180);
  tft.drawString("SETS: " + String(setsB), 240, 180);

  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void atualizarPontosVolei() {
  tft.setTextSize(5);
  tft.setTextDatum(TC_DATUM);
  tft.setTextPadding(tft.textWidth("999"));

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String(placarA), 80, 110);

  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(String(placarB), 240, 110);

  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void verificarFimSetVolei() {
  if (setEncerrado || setsA == 3 || setsB == 3) return;

  int meta = (periodo == 5) ? 15 : 25; // 5º set é 15 pontos, demais são 25

  if (placarA >= meta && (placarA - placarB) >= 2) {
    setsA++;
    setEncerrado = true;
    atualizarSetsVolei();
    desenharTopoVolei();
  } else if (placarB >= meta && (placarB - placarA) >= 2) {
    setsB++;
    setEncerrado = true;
    atualizarSetsVolei();
    desenharTopoVolei();
  }
}

void desenharPlacarVolei(String timeA, String timeB, int setAtual){
  tft.fillScreen(TFT_BLACK);

  // Linha divisória
  tft.drawLine(160, 60, 160, 220, TFT_DARKGREY);

  // Nomes dos times
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(timeA, 80, 70);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(timeB, 240, 70);
  tft.setTextDatum(TL_DATUM);

  desenharTopoVolei();
  atualizarPontosVolei();
  atualizarSetsVolei();
  desenharBotoesToque("-1");
}


// ============================================================================
// 8. MÓDULOS FUTUROS (TÊNIS, JIU-JITSU, PADEL) E SELETOR DE PARTIDA
// ============================================================================
void desenharPlacarTenis(String timeA, String timeB, int setsA, int setsB){

}

void desenharPlacarJiuJitsu(String lutadorA, String lutadorB, int punicoesA, int punicoesB, int vantagensA, int vantagensB){

}

// Jogo sem nada a perder: dá para trocar de partida sem apagar um jogo rodando.
// Menu, relógio pausado (futebol/basquete) ou, no vôlei, set encerrado / 0 x 0.
bool jogoParado() {
  if (telaAtual == 0) return true;
  if (telaAtual == 3) return setEncerrado || (placarA == 0 && placarB == 0);
  return !timerRodando;
}

void aplicarPartida(String modo, String timeA, String timeB){
  modoAtual = modo;
  nomeA = timeA;
  nomeB = timeB;
  placarA = 0;
  placarB = 0;
  faltasA = 0;
  faltasB = 0;
  setsA = 0;
  setsB = 0;
  setEncerrado = false;
  periodo = 1;
  zerarTimer();

  if(modo == "futebol"){
    telaAtual = 1;
    desenharPlacarFutebol(nomeA, nomeB, placarA, placarB, 0, 0);
  } else if(modo == "basquete"){
    telaAtual = 2;
    desenharPlacarBasquete(nomeA, nomeB, placarA, placarB, periodo);
  } else if(modo == "volei"){
    telaAtual = 3;
    desenharPlacarVolei(nomeA, nomeB, periodo);
  } else{ // Colocar outras telas de jogo
    telaAtual = 0;
    telaInicial();
  }
}


// ============================================================================
// 9. SETUP E LOOP PRINCIPAL
// ============================================================================
void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);        // 0 a 3. Use 1 ou 3 para paisagem (320x240)
  tft.fillScreen(TFT_BLACK); // limpa a tela

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);  // use a mesma rotação da tela

  pinMode(BOTAO_A, INPUT_PULLUP);
  pinMode(BOTAO_B, INPUT_PULLUP);
  pinMode(BOTAO_C, INPUT);        // pull-up é o resistor externo
  // BOTAO_D e BOTAO_E são de toque: sem pinMode

  // Rede no núcleo 0 (o loop() roda no núcleo 1). 12 KB de pilha: o HTTPS usa bastante.
  filaPartidas = xQueueCreate(1, sizeof(Partida));
  xTaskCreatePinnedToCore(tarefaRede, "rede", 12288, NULL, 1, NULL, 0);
  
  telaInicial();
}

void loop() {
  cuidarOTA();
  if (atualizandoOTA) return;      // tela é da barra de progresso

  // Partida nova deixada na fila pela tarefa de rede (não espera nada)
  Partida nova;
  if (xQueuePeek(filaPartidas, &nova, 0) == pdTRUE) {
    if (nova.primeira) {                           // ao ligar: só guarda os nomes
      xQueueReceive(filaPartidas, &nova, 0);
      nomeA = nova.timeA;
      nomeB = nova.timeB;
    } else if (nova.substituir || jogoParado()) {  // senão, fica esperando na fila
      xQueueReceive(filaPartidas, &nova, 0);
      aplicarPartida(nova.modo, nova.timeA, nova.timeB);
    }
  }

  // --- LEITURA DO TOUCH (Menu Inicial e Play/Pause nos cronômetros) ---
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, 200, 3700, 1, 320);
    int y = map(p.y, 240, 3800, 1, 240);

    if (telaAtual == 0) {
      if (tocouEm(x, y, 5, 80, 105, 40)) {           // Futebol
        telaAtual = 1;
        placarA = 0;
        placarB = 0;
        periodo = 1;
        zerarTimer();
        desenharPlacarFutebol(nomeA, nomeB, placarA, placarB, 0, 0);
      } else if (tocouEm(x, y, 110, 80, 105, 40)) {  // Basquete
        telaAtual = 2;
        placarA = 0;
        placarB = 0;
        faltasA = 0;
        faltasB = 0;
        periodo = 1;
        zerarTimer();
        desenharPlacarBasquete(nomeA, nomeB, placarA, placarB, periodo);
      } else if (tocouEm(x, y, 215, 80, 105, 40)) {  // Vôlei
        telaAtual = 3;
        placarA = 0;
        placarB = 0;
        setsA = 0;
        setsB = 0;
        periodo = 1;
        setEncerrado = false;
        desenharPlacarVolei(nomeA, nomeB, periodo);
      }
    } else if (telaAtual == 1) {                     // Futebol: toque = play/pausa
      if (tempoAcabou()) {
        if (periodo == 1) {                          // no intervalo: toque prepara o 2º tempo
          periodo = 2;
          zerarTimer();
        }
      } else if (timerRodando) pausarTimer();
      else iniciarTimer();
    } else if (telaAtual == 2) {                     // Basquete: botões FALTA ou play/pausa
      if (!tocouBotoesToque(x, y) && !tempoAcabouBasquete()) {
        if (timerRodando) pausarTimer();
        else iniciarTimer();
      }
    } else if (telaAtual == 3) {                     // Vôlei: botões -1
      tocouBotoesToque(x, y);
    }

    while (ts.touched()) delay(10);   // espera tirar o dedo
    delay(200);                       // evita que um toque conte várias vezes
  }

  // --- CONTROLES: FUTEBOL (telaAtual == 1) ---
  if (telaAtual == 1) {
    desenharTimer();
    if (apertou(BOTAO_A)) {
      placarA++;
      atualizarPlacar();
    }
    if (apertou(BOTAO_B)) {
      placarB++;
      atualizarPlacar();
    }
  }

  // --- CONTROLES: BASQUETE (telaAtual == 2) ---
  if (telaAtual == 2) {
    desenharTimerBasquete();

    if (apertou(BOTAO_A)) {
      placarA++;
      atualizarPlacarBasquete();
    }
    if (apertou(BOTAO_B)) {
      placarB++;
      atualizarPlacarBasquete();
    }
    if (apertou(BOTAO_C)) {
      faltasA = 0;
      faltasB = 0;
      periodo++;
      zerarTimer();
      atualizarFaltasBasquete();
      desenharTimerBasquete();
    }
    if (apertou(BOTAO_D)) {
      faltasA++;
      atualizarFaltasBasquete();
    }
    if (apertou(BOTAO_E)) {
      faltasB++;
      atualizarFaltasBasquete();
    }
  }

  // --- CONTROLES: VÔLEI (telaAtual == 3) ---
  if (telaAtual == 3) {
    // +1 Ponto Time A
    // apertou() vem primeiro no if: assim o aperto é sempre consumido,
    // mesmo quando a ação não vale (ex.: set já encerrado)
    if (apertou(BOTAO_A) && !setEncerrado && setsA < 3 && setsB < 3) {
      placarA++;
      atualizarPontosVolei();
      verificarFimSetVolei();
    }
    // +1 Ponto Time B
    if (apertou(BOTAO_B) && !setEncerrado && setsA < 3 && setsB < 3) {
      placarB++;
      atualizarPontosVolei();
      verificarFimSetVolei();
    }
    // BOTAO_C: Iniciar próximo set (zera pontos do set e avança para o próximo)
    if (apertou(BOTAO_C) && setsA < 3 && setsB < 3) {
      if (periodo < 5) periodo++;
      placarA = 0;
      placarB = 0;
      setEncerrado = false;
      atualizarPontosVolei();
      desenharTopoVolei();
    }
    // BOTAO_D (toque): -1 Ponto Time A (Correção)
    if (apertou(BOTAO_D) && placarA > 0) {
      if (setEncerrado && placarA > placarB) {
        setsA--;
        setEncerrado = false;
        atualizarSetsVolei();
        desenharTopoVolei();
      }
      placarA--;
      atualizarPontosVolei();
    }
    // BOTAO_E (toque): -1 Ponto Time B (Correção)
    if (apertou(BOTAO_E) && placarB > 0) {
      if (setEncerrado && placarB > placarA) {
        setsB--;
        setEncerrado = false;
        atualizarSetsVolei();
        desenharTopoVolei();
      }
      placarB--;
      atualizarPontosVolei();
    }
  }
}