#include <TFT_eSPI.h>
#include <SPI.h>
#include  <XPT2046_Touchscreen.h>
#include "segredos.h"



TFT_eSPI tft = TFT_eSPI();

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define BOTAO_A 22
#define BOTAO_B 27
#define BOTAO_C 35


SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

int telaAtual = 0;
unsigned long inicioTimer = 0;    // millis() de quando deu play
unsigned long tempoPausado = 0;   // tempo acumulado antes da última pausa
bool timerRodando = false;
int ultimoSegundo = -1;           // só redesenhar quando o segundo muda
int periodo = 1;                  // 1º ou 2º tempo
const unsigned long DURACAO_TEMPO = 45UL * 60 * 1000;   // 45 min em ms (para testar: 1UL * 60 * 1000)

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

// Escreve embaixo do relógio em qual parte do jogo estamos
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

  // Placar da Equipe A
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(30, 60);
  tft.println(timeA);

  tft.setTextSize(5);
  tft.setCursor(60, 110);
  tft.println(placarA);

  // Placar da Equipe B
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(200, 60);
  tft.println(timeB);

  tft.setTextSize(5);
  tft.setCursor(230, 110);
  tft.println(placarB);


}

void desenharPlacarBasquete(String timeA, String timeB, int placarA, int placarB, int set){

}

void desenharPlacarVolei(String timeA, String timeB, int set, int ){

}

void desenharPlacarTenis(String timeA, String timeB, int setsA, int setsB){

}
void desenharPlacarJiuJitsu(String lutadorA, String lutadorB, int punicoesA, int punicoesB, int vantagensA, int vantagensB){

}
void setup() {
  tft.init();
  tft.setRotation(1);        // 0 a 3. Use 1 ou 3 para paisagem (320x240)
  tft.fillScreen(TFT_BLACK); // limpa a tela
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1); // ajuste se a orientação sair errada
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);  // use a mesma rotação da tela

  pinMode(BOTAO_A, INPUT_PULLUP);
  pinMode(BOTAO_B, INPUT_PULLUP);
  pinMode(BOTAO_C, INPUT);   // pull-up é o resistor externo
  
  
  telaInicial();
}

int placarA = 0;
int placarB = 0;

// Redesenha só os números, sem limpar a tela toda
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
        desenharPlacarFutebol("gremio", "inter", placarA, placarB, 0, 0);
      } else if (tocouEm(x, y, 110, 80, 105, 40)) { // Basquete
        telaAtual = 2;
        desenharBola();
      }
    } else if (telaAtual == 1) {                    // no futebol: toque = play/pausa
      if (tempoAcabou()) {
        if (periodo == 1) {                         // no intervalo: toque prepara o 2º tempo
          periodo = 2;
          zerarTimer();
        }
      } else if (timerRodando) pausarTimer();
      else iniciarTimer();
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
}
