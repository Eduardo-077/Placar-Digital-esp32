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
#ifndef PLACAR_TOKEN
#error "Adicione PLACAR_TOKEN no segredos.h (veja o segredos_exemplo.h)"
#endif

TFT_eSPI tft = TFT_eSPI();

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define BOTAO_A 22  // +1 Ponto Time A (Todos os esportes)
#define BOTAO_B 27  // +1 Ponto Time B (Todos os esportes)
#define BOTAO_C 35  // Encerrar tempo / quarto / set, ou desfazer (depende do esporte)
#define SENSOR_LUZ 34 // LDR da própria CYD (telemetria)

// O IO35 NÃO tem pull-up interno: sem o resistor de 10k (IO35 -> 3.3V) o pino
// fica "flutuando", lê LOW sozinho e o apertou() trava esperando soltar.
// Deixe 0 até o resistor estar ligado; aí troque para 1.
#define BOTAO_C_LIGADO 0

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
int telaAtual = 0;                // 0=Menu, 1=Futebol, 2=Basquete, 3=Vôlei, 4=Tênis, 5=Jiu-Jitsu, 6=Padel
unsigned long inicioTimer = 0;    // millis() de quando deu play
unsigned long tempoPausado = 0;   // tempo acumulado antes da última pausa
bool timerRodando = false;
int ultimoSegundo = -1;           // só redesenhar quando o segundo muda
int periodo = 1;                  // Tempo (Futebol), Quarto (Basquete) ou Set atual (Vôlei)

const unsigned long DURACAO_TEMPO = 45UL * 60 * 1000;   // 45 min em ms (Futebol)
const unsigned long DURACAO_QUARTO = 10UL * 60 * 1000;  // 10 min em ms (Basquete)
const unsigned long DURACAO_LUTA = 5UL * 60 * 1000;     // 5 min em ms (Jiu-Jitsu)
bool periodoEncerrado = false;    // Futebol: botão C encerrou o tempo (intervalo / fim de jogo)

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

// Tênis / Padel (placarA/placarB = pontos do game: 0, 1, 2, 3 = 0, 15, 30, 40)
int gamesA = 0;
int gamesB = 0;
bool tiebreak = false;
char historicoSets[24] = "";      // sets já jogados, ex.: "6-4 3-6"

// Jiu-Jitsu (placarA/placarB = pontos)
int vantagensA = 0;
int vantagensB = 0;
int punicoesA = 0;
int punicoesB = 0;

// Desfazer (botão C no tênis, padel e jiu-jitsu): fotos do placar antes de cada ponto
struct Estado {
  int placarA, placarB, gamesA, gamesB, setsA, setsB;
  int vantagensA, vantagensB, punicoesA, punicoesB;
  bool tiebreak;
  char historicoSets[24];
};
const int MAX_HISTORICO = 30;
Estado historico[MAX_HISTORICO];
int nHistorico = 0;


// ============================================================================
// 2. REDE (WI-FI E SUPABASE)
// ============================================================================
void aplicarPartida(String modo, String timeA, String timeB); // Protótipos
void redesenharTela();

// A rede roda numa tarefa separada, no núcleo 0 (tarefaRede), para o loop()
// nunca ficar esperando a internet. Regra: SÓ o loop() (núcleo 1) mexe na
// tela e nas variáveis do jogo. A tarefa de rede só deixa a partida nova
// nesta fila ("caixa de correio"), e o loop() decide quando aplicar.
struct Partida {
  long id;                        // id na tabela partidas (vai junto nos eventos)
  bool primeira;                  // leitura ao ligar: só guarda os nomes
  bool substituir;                // true = aplica mesmo com jogo rodando
  char modo[12];
  char timeA[25];                 // 12 letras (até 2 bytes cada, se tiver acento)
  char timeB[25];
};
QueueHandle_t filaPartidas;       // tamanho 1: sempre guarda só a mais recente
long ultimoId = -1;               // id da última partida lida (só a tarefa de rede usa)
volatile bool atualizandoOTA = false; // durante a OTA a tarefa de rede fica quieta

// Eventos do jogo (gol, ponto, falta...): o loop() deixa na fila e a tarefa
// de rede envia para o Supabase. Sem internet eles esperam na fila e saem
// quando a rede voltar. Se a fila encher, os eventos novos se perdem.
struct Evento {
  long partidaId;                 // 0 = sem partida do banco
  char modo[12];
  char tipo[12];                  // ver o check da tabela eventos (etapa3_eventos.sql)
  char time;                      // 'A', 'B' ou 0 (nenhum)
  int placarA, placarB, setsA, setsB, periodo;
  int tempoJogoS;
};
const int TAMANHO_FILA_EVENTOS = 60;
QueueHandle_t filaEventos;
long partidaAtualId = 0;          // id da partida na tela (só o loop() usa)

const unsigned long INTERVALO_TELEMETRIA = 60000;   // 60 s
unsigned long ultimaTelemetria = 0;                 // só a tarefa de rede usa

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
        nova.id = id;
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

// Chama uma função do Supabase (/rest/v1/rpc/...) com o corpo em JSON.
// Retorna o código HTTP (negativo = sem conexão / tempo esgotado).
int chamarFuncao(const char* funcao, JsonDocument &corpo) {
  WiFiClientSecure cliente;
  cliente.setInsecure();

  HTTPClient http;
  http.begin(cliente, String(SUPABASE_URL) + "/rest/v1/rpc/" + funcao);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);

  String texto;
  serializeJson(corpo, texto);
  int codigo = http.POST(texto);
  if (codigo >= 300) {                     // mostra o erro que o Supabase devolveu
    Serial.printf("%s: HTTP %d %s\n", funcao, codigo, http.getString().c_str());
  }
  http.end();
  return codigo;
}

// Chamado só pelo loop(): tira uma "foto" do jogo e deixa na fila.
// tipo: inicio, ponto, correcao, falta, vantagem, punicao, play, pausa, fim_periodo, fim
// time: 'A', 'B' ou 0
void registrarEvento(const char* tipo, char time) {
  Evento e;
  e.partidaId = partidaAtualId;
  strlcpy(e.modo, modoAtual.c_str(), sizeof(e.modo));
  strlcpy(e.tipo, tipo, sizeof(e.tipo));
  e.time = time;
  e.placarA = placarA;
  e.placarB = placarB;
  e.setsA = setsA;
  e.setsB = setsB;
  e.periodo = periodo;
  e.tempoJogoS = tempoDecorrido() / 1000;

  if (xQueueSend(filaEventos, &e, 0) != pdTRUE) {
    Serial.printf("Fila de eventos cheia, perdeu: %s\n", tipo);
  }
}

// Envia os eventos da fila, do mais antigo para o mais novo.
// Só tira da fila depois que o Supabase aceitou.
void enviarEventos() {
  Evento e;
  while (xQueuePeek(filaEventos, &e, 0) == pdTRUE) {
    JsonDocument corpo;
    corpo["p_token"] = PLACAR_TOKEN;
    if (e.partidaId > 0) corpo["p_partida_id"] = e.partidaId;
    else                 corpo["p_partida_id"] = nullptr;
    corpo["p_modo"] = e.modo;
    corpo["p_tipo"] = e.tipo;
    if (e.time) corpo["p_time"] = String(e.time);
    else        corpo["p_time"] = nullptr;
    corpo["p_placar_a"] = e.placarA;
    corpo["p_placar_b"] = e.placarB;
    corpo["p_sets_a"] = e.setsA;
    corpo["p_sets_b"] = e.setsB;
    corpo["p_periodo"] = e.periodo;
    corpo["p_tempo_jogo_s"] = e.tempoJogoS;

    int codigo = chamarFuncao("registrar_evento", corpo);
    if (codigo >= 200 && codigo < 300) {
      xQueueReceive(filaEventos, &e, 0);
      Serial.printf("Evento enviado: %s %c %d x %d\n", e.tipo, e.time ? e.time : '-',
                    e.placarA, e.placarB);
    } else if (codigo >= 400 && codigo < 500) {
      // O Supabase recusou (token errado, tipo inválido...): tentar de novo
      // não adianta e travaria a fila. Joga fora.
      xQueueReceive(filaEventos, &e, 0);
      Serial.println("Evento recusado pelo Supabase, descartado.");
    } else {
      return;                              // sem internet: tenta na próxima volta
    }
  }
}

// Sinal do Wi-Fi, memória livre, tempo ligado e luz ambiente, a cada 60 s
void enviarTelemetria() {
  if (ultimaTelemetria != 0 && millis() - ultimaTelemetria < INTERVALO_TELEMETRIA) return;
  ultimaTelemetria = millis();

  JsonDocument corpo;
  corpo["p_token"] = PLACAR_TOKEN;
  corpo["p_rssi"] = WiFi.RSSI();
  corpo["p_heap_livre"] = ESP.getFreeHeap();
  corpo["p_uptime_s"] = millis() / 1000;
  corpo["p_luz"] = analogRead(SENSOR_LUZ);
  chamarFuncao("registrar_telemetria", corpo);
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
    enviarEventos();
    enviarTelemetria();
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
  }
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);

  if (!sucesso) {                          // mostra o erro e volta para o jogo
    delay(3000);
    atualizandoOTA = false;
    redesenharTela();
  }
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

// Toque de play/pausa nos esportes com relógio
void alternarTimer() {
  if (timerRodando) {
    pausarTimer();
    registrarEvento("pausa", 0);
  } else {
    iniciarTimer();
    registrarEvento("play", 0);
  }
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
  if (botao == BOTAO_C && !BOTAO_C_LIGADO) return false;

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

// Faltas do futebol e do basquete, acima dos botões de toque.
// limite: a partir de quantas faltas fica vermelho (0 = nunca).
void atualizarFaltas(int limite) {
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextPadding(tft.textWidth("FALTAS: 99"));

  uint16_t corA = (limite > 0 && faltasA >= limite) ? TFT_RED : TFT_WHITE;
  tft.setTextColor(corA, TFT_BLACK);
  tft.drawString("FALTAS: " + String(faltasA), 80, 180);

  uint16_t corB = (limite > 0 && faltasB >= limite) ? TFT_RED : TFT_WHITE;
  tft.setTextColor(corB, TFT_BLACK);
  tft.drawString("FALTAS: " + String(faltasB), 240, 180);

  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

// Toque no rodapé: metade esquerda = botão D, metade direita = botão E.
// Retorna true se o toque foi num desses botões.
bool tocouBotoesToque(int x, int y) {
  if (tocouEm(x, y, 0, 195, 160, 45))   { toqueD = true; return true; }
  if (tocouEm(x, y, 160, 195, 160, 45)) { toqueE = true; return true; }
  return false;
}

// Botão MENU no canto superior esquerdo de todas as telas de jogo
void desenharBotaoMenu() {
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRoundRect(4, 4, 52, 26, 6, TFT_DARKGREY);
  tft.drawString("MENU", 30, 18);
  tft.setTextDatum(TL_DATUM);
}

// A área de toque é maior que o desenho, para facilitar acertar o canto
bool tocouMenu(int x, int y) {
  return tocouEm(x, y, 0, 0, 70, 40);
}

void voltarMenu() {
  telaAtual = 0;
  zerarTimer();
  telaInicial();
}

// Guarda uma foto do placar antes de cada ponto, para o botão C desfazer.
// Se encher, joga fora a foto mais antiga.
void salvarEstado() {
  if (nHistorico == MAX_HISTORICO) {
    memmove(historico, historico + 1, sizeof(Estado) * (MAX_HISTORICO - 1));
    nHistorico--;
  }
  Estado &e = historico[nHistorico++];
  e.placarA = placarA;       e.placarB = placarB;
  e.gamesA = gamesA;         e.gamesB = gamesB;
  e.setsA = setsA;           e.setsB = setsB;
  e.vantagensA = vantagensA; e.vantagensB = vantagensB;
  e.punicoesA = punicoesA;   e.punicoesB = punicoesB;
  e.tiebreak = tiebreak;
  strlcpy(e.historicoSets, historicoSets, sizeof(e.historicoSets));
}

void desfazer() {
  if (nHistorico == 0) return;
  Estado &e = historico[--nHistorico];
  placarA = e.placarA;       placarB = e.placarB;
  gamesA = e.gamesA;         gamesB = e.gamesB;
  setsA = e.setsA;           setsB = e.setsB;
  vantagensA = e.vantagensA; vantagensB = e.vantagensB;
  punicoesA = e.punicoesA;   punicoesB = e.punicoesB;
  tiebreak = e.tiebreak;
  strlcpy(historicoSets, e.historicoSets, sizeof(historicoSets));
  redesenharTela();
  registrarEvento("correcao", 0);
}

void desenharBola(){
   tft.fillCircle(250, 120, 80, 0x001F);
}

void telaInicial(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("Selecione o modo", 20, 10);
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
// 5. MÓDULO: FUTEBOL (telaAtual = 1)
// ============================================================================
// Ao chegar em 45:00 o relógio NÃO para: o tempo extra aparece como acréscimo
// ("1o T +2:13"). Quem encerra o tempo é o botão C.
void desenharPeriodo() {
  String texto;
  unsigned long decorrido = tempoDecorrido();
  if (periodoEncerrado) {
    texto = (periodo == 1) ? "INTERVALO" : "FIM DE JOGO";
  } else if (decorrido > DURACAO_TEMPO) {
    int extra = (decorrido - DURACAO_TEMPO) / 1000;
    char acrescimo[8];
    snprintf(acrescimo, sizeof(acrescimo), "+%d:%02d", extra / 60, extra % 60);
    texto = String(periodo) + "o T " + acrescimo;
  } else {
    texto = String(periodo) + "o TEMPO";
  }

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
  int agora = decorrido / 1000;
  if (agora == ultimoSegundo) return;      // nada mudou, não redesenha
  ultimoSegundo = agora;
  desenharPeriodo();

  // O relógio grande para em 45:00 (ou 90:00); o que passar é acréscimo.
  // No 2º tempo o relógio continua de 45:00 até 90:00
  int segundos = ((periodo - 1) * DURACAO_TEMPO + min(decorrido, DURACAO_TEMPO)) / 1000;

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

  atualizarFaltas(0);                      // no futebol a falta nunca fica vermelha
  desenharBotoesToque("FALTA");
  desenharTimer();
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
    bool acabouAgora = timerRodando;       // só avisa uma vez
    pausarTimer();
    tempoPausado = DURACAO_QUARTO;
    decorrido = DURACAO_QUARTO;
    // Empate no 4º quarto (ou depois) vai para a prorrogação
    if (acabouAgora) registrarEvento(periodo >= 4 && placarA != placarB ? "fim" : "fim_periodo", 0);
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
  atualizarFaltas(5);                   // vermelho com 5 faltas coletivas
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
    registrarEvento(setsA == 3 ? "fim" : "fim_periodo", 'A');
  } else if (placarB >= meta && (placarB - placarA) >= 2) {
    setsB++;
    setEncerrado = true;
    atualizarSetsVolei();
    desenharTopoVolei();
    registrarEvento(setsB == 3 ? "fim" : "fim_periodo", 'B');
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
// 8. MÓDULO: TÊNIS E PADEL (telaAtual = 4 e 6)
// ============================================================================
// Games de 0-15-30-40, set até 6 games com 2 de diferença, tie-break no 6-6
// (até 7 pontos com 2 de diferença), melhor de 3 sets.
// Padel igual, mas com ponto de ouro: no 40-40 quem fizer o próximo ponto leva.
// A/B = ponto, C = desfazer o último ponto.
bool ehPadel() {
  return telaAtual == 6;
}

bool fimTenis() {
  return setsA == 2 || setsB == 2;
}

String textoPontoTenis(int meu, int outro) {
  if (tiebreak) return String(meu);
  if (meu >= 3 && outro >= 3) return (meu > outro) ? "AD" : "40";  // iguais / vantagem
  const char* nomes[] = {"0", "15", "30", "40"};
  return nomes[meu];
}

void desenharTopoTenis() {
  String texto;
  if (fimTenis())                                          texto = "FIM DE JOGO";
  else if (tiebreak)                                       texto = "TIE-BREAK";
  else if (placarA == 3 && placarB == 3 && ehPadel())      texto = "PONTO DE OURO";
  else if (placarA >= 3 && placarA == placarB)             texto = "IGUAIS";
  else                                                     texto = String(setsA + setsB + 1) + "o SET";

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextPadding(tft.textWidth("PONTO DE OURO"));
  tft.drawString(texto, 160, 12);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void atualizarTenis() {
  desenharTopoTenis();

  tft.setTextDatum(TC_DATUM);
  tft.setTextSize(5);
  tft.setTextPadding(tft.textWidth("AD"));
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(textoPontoTenis(placarA, placarB), 80, 75);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(textoPontoTenis(placarB, placarA), 240, 75);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(tft.textWidth("GAMES: 9"));
  tft.drawString("GAMES: " + String(gamesA), 80, 130);
  tft.drawString("GAMES: " + String(gamesB), 240, 130);
  tft.drawString("SETS: " + String(setsA), 80, 155);
  tft.drawString("SETS: " + String(setsB), 240, 155);

  tft.setTextPadding(320);                 // sets já jogados, embaixo no meio
  tft.drawString(historicoSets, 160, 190);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void ganharGameTenis(bool paraA) {
  if (paraA) gamesA++; else gamesB++;
  placarA = 0;
  placarB = 0;

  int meus = paraA ? gamesA : gamesB;
  int outros = paraA ? gamesB : gamesA;
  bool ganhouSet = tiebreak || (meus >= 6 && meus - outros >= 2);   // tie-break vale o set (7-6)
  tiebreak = false;

  if (ganhouSet) {
    char set[8];
    snprintf(set, sizeof(set), "%d-%d ", gamesA, gamesB);
    strlcat(historicoSets, set, sizeof(historicoSets));
    if (paraA) setsA++; else setsB++;
    gamesA = 0;
    gamesB = 0;
  } else if (gamesA == 6 && gamesB == 6) {
    tiebreak = true;
  }
}

void pontoTenis(bool paraA) {
  if (fimTenis()) return;
  salvarEstado();
  int setsAntes = setsA + setsB;
  if (paraA) placarA++; else placarB++;

  int meu = paraA ? placarA : placarB;
  int outro = paraA ? placarB : placarA;
  bool ganhouGame;
  if (tiebreak)                                ganhouGame = meu >= 7 && meu - outro >= 2;
  else if (ehPadel() && meu == 4 && outro == 3) ganhouGame = true;   // ponto de ouro
  else                                         ganhouGame = meu >= 4 && meu - outro >= 2;

  if (ganhouGame) ganharGameTenis(paraA);
  atualizarTenis();

  char time = paraA ? 'A' : 'B';
  registrarEvento("ponto", time);
  if (setsA + setsB != setsAntes) registrarEvento(fimTenis() ? "fim" : "fim_periodo", time);
}

void desenharPlacarTenis(String timeA, String timeB){
  tft.fillScreen(TFT_BLACK);

  // Linha divisória
  tft.drawLine(160, 40, 160, 175, TFT_DARKGREY);

  // Nomes dos times
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(timeA, 80, 45);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(timeB, 240, 45);

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("BOTAO C = DESFAZER", 160, 225);
  tft.setTextDatum(TL_DATUM);

  atualizarTenis();
}


// ============================================================================
// 9. MÓDULO: JIU-JITSU (telaAtual = 5)
// ============================================================================
// Pontos IBJJF: A/B = +2 (queda, raspagem, joelho na barriga),
// toque = +3 (passagem de guarda), +4 (montada, costas), V (vantagem), P (punição).
// Toque fora dos botões = play/pausa. C = desfazer.
const char* BOTOES_JIU[] = {"+3", "+4", "V", "P"};

bool tempoAcabouJiu() {
  return tempoDecorrido() >= DURACAO_LUTA;
}

void desenharTimerJiu() {
  unsigned long decorrido = tempoDecorrido();
  if (decorrido >= DURACAO_LUTA) {
    bool acabouAgora = timerRodando;       // só avisa uma vez
    pausarTimer();
    tempoPausado = DURACAO_LUTA;
    decorrido = DURACAO_LUTA;
    if (acabouAgora) registrarEvento("fim", 0);
  }

  // Cronômetro regressivo (05:00 até 00:00)
  int segundos = (DURACAO_LUTA - decorrido) / 1000;
  if (segundos == ultimoSegundo) return;
  ultimoSegundo = segundos;

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextPadding(tft.textWidth("FIM DA LUTA"));
  tft.drawString(tempoAcabouJiu() ? "FIM DA LUTA" : "LUTA", 160, 35);

  char texto[6];
  snprintf(texto, sizeof(texto), "%02d:%02d", segundos / 60, segundos % 60);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);
  tft.setTextPadding(tft.textWidth("00:00"));
  tft.drawString(texto, 160, 5);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void atualizarJiu() {
  tft.setTextDatum(TC_DATUM);
  tft.setTextSize(5);
  tft.setTextPadding(tft.textWidth("99"));
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String(placarA), 80, 80);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(String(placarB), 240, 80);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(tft.textWidth("V:9  P:9"));
  tft.drawString("V:" + String(vantagensA) + "  P:" + String(punicoesA), 80, 132);
  tft.drawString("V:" + String(vantagensB) + "  P:" + String(punicoesB), 240, 132);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

// Posição do botão de toque i (0 a 3) do lado 0 (A, esquerda) ou 1 (B, direita):
// 2 linhas x 2 colunas em cada metade da tela
int botaoJiuX(int lado, int i) { return 8 + lado * 160 + (i % 2) * 74; }
int botaoJiuY(int i)           { return 165 + (i / 2) * 36; }

void desenharBotoesJiu() {
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int lado = 0; lado < 2; lado++) {
    for (int i = 0; i < 4; i++) {
      int bx = botaoJiuX(lado, i);
      int by = botaoJiuY(i);
      tft.drawRoundRect(bx, by, 70, 30, 8, TFT_DARKGREY);
      tft.drawString(BOTOES_JIU[i], bx + 35, by + 15);
    }
  }
  tft.setTextDatum(TL_DATUM);
}

// acao: 0 = +2 (botões A/B), 1 = +3, 2 = +4, 3 = vantagem, 4 = punição
void pontoJiu(bool paraA, int acao) {
  salvarEstado();
  int &pontos = paraA ? placarA : placarB;
  if (acao == 0) pontos += 2;
  if (acao == 1) pontos += 3;
  if (acao == 2) pontos += 4;
  if (acao == 3) (paraA ? vantagensA : vantagensB)++;
  if (acao == 4) (paraA ? punicoesA : punicoesB)++;
  atualizarJiu();

  const char* tipos[] = {"ponto", "ponto", "ponto", "vantagem", "punicao"};
  registrarEvento(tipos[acao], paraA ? 'A' : 'B');
}

// Retorna true se o toque foi num dos botões +3 / +4 / V / P
bool tocouBotoesJiu(int x, int y) {
  for (int lado = 0; lado < 2; lado++) {
    for (int i = 0; i < 4; i++) {
      if (tocouEm(x, y, botaoJiuX(lado, i), botaoJiuY(i), 70, 30)) {
        pontoJiu(lado == 0, i + 1);
        return true;
      }
    }
  }
  return false;
}

void desenharPlacarJiuJitsu(String lutadorA, String lutadorB){
  tft.fillScreen(TFT_BLACK);

  // Linha divisória
  tft.drawLine(160, 60, 160, 235, TFT_DARKGREY);

  // Nomes dos lutadores
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(lutadorA, 80, 58);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(lutadorB, 240, 58);
  tft.setTextDatum(TL_DATUM);

  atualizarJiu();
  desenharBotoesJiu();
  desenharTimerJiu();
}


// ============================================================================
// 10. SELETOR DE PARTIDA
// ============================================================================

// Jogo sem nada a perder: dá para trocar de partida sem apagar um jogo rodando.
// Menu, relógio pausado (futebol/basquete/jiu-jitsu), no vôlei set encerrado / 0 x 0,
// no tênis/padel entre um game e outro.
bool jogoParado() {
  if (telaAtual == 0) return true;
  if (telaAtual == 3) return setEncerrado || (placarA == 0 && placarB == 0);
  if (telaAtual == 4 || telaAtual == 6) return fimTenis() || (placarA == 0 && placarB == 0);
  return !timerRodando;
}

// Desenha a tela inteira do esporte atual (troca de partida, desfazer, erro na OTA)
void redesenharTela() {
  ultimoSegundo = -1;                      // força o relógio a ser redesenhado
  switch (telaAtual) {
    case 1: desenharPlacarFutebol(nomeA, nomeB, placarA, placarB, faltasA, faltasB); break;
    case 2: desenharPlacarBasquete(nomeA, nomeB, placarA, placarB, periodo); break;
    case 3: desenharPlacarVolei(nomeA, nomeB, periodo); break;
    case 4:
    case 6: desenharPlacarTenis(nomeA, nomeB); break;
    case 5: desenharPlacarJiuJitsu(nomeA, nomeB); break;
    default: telaInicial(); return;        // menu não tem botão MENU
  }
  desenharBotaoMenu();
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
  gamesA = 0;
  gamesB = 0;
  tiebreak = false;
  historicoSets[0] = '\0';
  vantagensA = 0;
  vantagensB = 0;
  punicoesA = 0;
  punicoesB = 0;
  nHistorico = 0;
  periodoEncerrado = false;
  periodo = 1;
  zerarTimer();

  if (modo == "futebol")       telaAtual = 1;
  else if (modo == "basquete") telaAtual = 2;
  else if (modo == "volei")    telaAtual = 3;
  else if (modo == "tenis")    telaAtual = 4;
  else if (modo == "jiujitsu") telaAtual = 5;
  else if (modo == "padel")    telaAtual = 6;
  else                         telaAtual = 0;
  redesenharTela();
  if (telaAtual != 0) registrarEvento("inicio", 0);
}


// ============================================================================
// 11. SETUP E LOOP PRINCIPAL
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
  pinMode(BOTAO_C, INPUT);        // pull-up é o resistor externo  // BOTAO_D e BOTAO_E são de toque: sem pinMode

  // Rede no núcleo 0 (o loop() roda no núcleo 1). 12 KB de pilha: o HTTPS usa bastante.
  filaPartidas = xQueueCreate(1, sizeof(Partida));
  filaEventos = xQueueCreate(TAMANHO_FILA_EVENTOS, sizeof(Evento));
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
      partidaAtualId = nova.id;
      nomeA = nova.timeA;
      nomeB = nova.timeB;
    } else if (nova.substituir || jogoParado()) {  // senão, fica esperando na fila
      xQueueReceive(filaPartidas, &nova, 0);
      partidaAtualId = nova.id;
      aplicarPartida(nova.modo, nova.timeA, nova.timeB);
    }
  }

  // --- LEITURA DO TOUCH (Menu Inicial e Play/Pause nos cronômetros) ---
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, 200, 3700, 1, 320);
    int y = map(p.y, 240, 3800, 1, 240);

    if (telaAtual == 0) {
      // 6 botões do menu, na mesma ordem do desenho: 3 em cima, 3 embaixo
      const char* modos[] = {"futebol", "basquete", "volei", "tenis", "jiujitsu", "padel"};
      for (int i = 0; i < 6; i++) {
        int bx = 5 + (i % 3) * 105;
        int by = (i < 3) ? 80 : 140;
        if (tocouEm(x, y, bx, by, 105, 40)) aplicarPartida(modos[i], nomeA, nomeB);
      }
    } else if (tocouMenu(x, y)) {                    // botão MENU (todas as telas de jogo)
      voltarMenu();
    } else if (telaAtual == 1) {                     // Futebol: botões FALTA ou play/pausa
      if (tocouBotoesToque(x, y)) {
        // falta: tratada em CONTROLES: FUTEBOL
      } else if (periodoEncerrado) {
        if (periodo == 1) {                          // no intervalo: toque prepara o 2º tempo
          periodo = 2;
          periodoEncerrado = false;
          zerarTimer();
        }
      } else alternarTimer();
    } else if (telaAtual == 2) {                     // Basquete: botões FALTA ou play/pausa
      if (!tocouBotoesToque(x, y) && !tempoAcabouBasquete()) {
        alternarTimer();
      }
    } else if (telaAtual == 3) {                     // Vôlei: botões -1
      tocouBotoesToque(x, y);
    } else if (telaAtual == 5) {                     // Jiu-Jitsu: +3/+4/V/P ou play/pausa
      if (!tocouBotoesJiu(x, y) && !tempoAcabouJiu()) {
        alternarTimer();
      }
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
      registrarEvento("ponto", 'A');
    }
    if (apertou(BOTAO_B)) {
      placarB++;
      atualizarPlacar();
      registrarEvento("ponto", 'B');
    }
    // BOTAO_C: encerra o tempo (fim do 1º = intervalo, fim do 2º = fim de jogo)
    if (apertou(BOTAO_C) && !periodoEncerrado && tempoDecorrido() > 0) {
      pausarTimer();
      periodoEncerrado = true;
      ultimoSegundo = -1;                  // redesenha "INTERVALO" / "FIM DE JOGO"
      registrarEvento(periodo == 1 ? "fim_periodo" : "fim", 0);
    }
    if (apertou(BOTAO_D)) {
      faltasA++;
      atualizarFaltas(0);
      registrarEvento("falta", 'A');
    }
    if (apertou(BOTAO_E)) {
      faltasB++;
      atualizarFaltas(0);
      registrarEvento("falta", 'B');
    }
  }

  // --- CONTROLES: BASQUETE (telaAtual == 2) ---
  if (telaAtual == 2) {
    desenharTimerBasquete();

    if (apertou(BOTAO_A)) {
      placarA++;
      atualizarPlacarBasquete();
      registrarEvento("ponto", 'A');
    }
    if (apertou(BOTAO_B)) {
      placarB++;
      atualizarPlacarBasquete();
      registrarEvento("ponto", 'B');
    }
    if (apertou(BOTAO_C)) {
      faltasA = 0;
      faltasB = 0;
      periodo++;
      zerarTimer();
      atualizarFaltas(5);
      desenharTimerBasquete();
    }
    if (apertou(BOTAO_D)) {
      faltasA++;
      atualizarFaltas(5);
      registrarEvento("falta", 'A');
    }
    if (apertou(BOTAO_E)) {
      faltasB++;
      atualizarFaltas(5);
      registrarEvento("falta", 'B');
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
      registrarEvento("ponto", 'A');
      verificarFimSetVolei();
    }
    // +1 Ponto Time B
    if (apertou(BOTAO_B) && !setEncerrado && setsA < 3 && setsB < 3) {
      placarB++;
      atualizarPontosVolei();
      registrarEvento("ponto", 'B');
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
      registrarEvento("correcao", 'A');
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
      registrarEvento("correcao", 'B');
    }
  }

  // --- CONTROLES: TÊNIS E PADEL (telaAtual == 4 / 6) ---
  if (telaAtual == 4 || telaAtual == 6) {
    if (apertou(BOTAO_A)) pontoTenis(true);
    if (apertou(BOTAO_B)) pontoTenis(false);
    if (apertou(BOTAO_C)) desfazer();
  }

  // --- CONTROLES: JIU-JITSU (telaAtual == 5) ---
  if (telaAtual == 5) {
    desenharTimerJiu();
    if (apertou(BOTAO_A)) pontoJiu(true, 0);     // +2
    if (apertou(BOTAO_B)) pontoJiu(false, 0);
    if (apertou(BOTAO_C)) desfazer();
  }
}
