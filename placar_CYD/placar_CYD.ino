#include <TFT_eSPI.h>
#include <SPI.h>

TFT_eSPI tft = TFT_eSPI();

int placarA = 0;
int placarB = 0;


void escolherTipoJogo(int modo, String timeA, String timeB, int placarA, int placarB, int set, ){
  switch (modo){
    case 0:
      voidesenharPlacarFutebol(String timeA, String timeB, int placarA, int placarB);
      break;

    case 1:
      desenharPlacarBasquete(String timeA, String timeB, int placarA, int placarB);
      break;

    case 2:

  }
}
//Futebol:
//Gols
//Faltas
void desenharPlacarFutebol(String timeA, String timeB, int placarA, int placarB, int faltasA, int faltasB) { 
  tft.fillScreen(TFT_BLACK);

  // Título
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(100, 10);
  tft.println("PLACAR ESP32");

  // Linha divisória
  tft.drawLine(160, 40, 160, 220, TFT_DARKGREY); // inicio em x, incio em y, final x, final ym cor

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
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1); // ajuste se a orientação sair errada

  desenhaPlacar();
}

void loop() {
  // Simulação automática: alterna pontos entre os times a cada 5s
  delay(5000);



  desenhaPlacarFutebol();
}
