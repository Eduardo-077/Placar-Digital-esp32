#include <TFT_eSPI.h>
#include <SPI.h>
#include  <XPT2046_Touchscreen.h>




TFT_eSPI tft = TFT_eSPI();

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

void telaInicial(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("Selecione o modo de jogo", 20, 10);
                      
  //tft.drawRoundRect(posição x, pos y, largura, altura, arredondamento(n pode ser maior q metade da largura ou altura), cor da borda);   
  tft.drawRoundRect(5, 60, 105, 40, 20, 0x001F);
  tft.drawString("Futebol", 20, 70);   
  tft.drawRoundRect(110, 60, 105, 40, 20, 0x001F);
  tft.drawString("Basquete", 115, 70);
  tft.drawRoundRect(215, 60, 105, 40, 20, 0x001F);
  tft.drawString("Volei", 240, 70);

  tft.drawRoundRect(5, 100, 105, 40, 20, 0x001F);
  tft.drawString("Jiu-jitsu", 20, 110);   
  tft.drawRoundRect(110, 100, 105, 40, 20, 0x001F);
  tft.drawString("Basquete", 115, 110);
  tft.drawRoundRect(215, 100, 105, 40, 20, 0x001F);
  tft.drawString("Volei", 240, 110);


}
//void escolherTipoJogo(int modo, String timeA, String timeB, int placarA, int placarB, int set){
  //switch (modo){
    //case 0:
      //voidesenharPlacarFutebol(String timeA, String timeB, int placarA, int placarB);
      //break;

    //case 1:
      //desenharPlacarBasquete(String timeA, String timeB, int placarA, int placarB);
      //break;

    //case 2:

  //}
//}
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
  tft.init();
  tft.setRotation(1);        // 0 a 3. Use 1 ou 3 para paisagem (320x240)
  tft.fillScreen(TFT_BLACK); // limpa a tela
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1); // ajuste se a orientação sair errada
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);  // use a mesma rotação da tela

    telaInicial();
}

void loop() {
  // Simulação automática: alterna pontos entre os times a cada 5s
  delay(5000);



}
