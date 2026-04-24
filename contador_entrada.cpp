#include <Arduino.h>

#define TRIG 5
#define ECHO 18
#define SENSOR_IMA 34   // A49E (analógico)

#define DISTANCIA_LIMITE 30   // limite para detectar pessoa em cm
#define LIMIAR_DETECCAO 300   // ajuste depois testando
#define LIMIAR_RESET 100      // evita ruído magnetico

enum Estado {
  ESPERANDO_PESSOA,
  AGUARDANDO_GIRO
};

Estado estadoAtual = ESPERANDO_PESSOA;

int contador = 0;
bool jaContou = false;
int valorBase = 0;


float readDistance() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);

  float duration = pulseIn(ECHO, HIGH, 30000); // timeout
  if (duration == 0) return -1;

  float distance = duration * 0.034 / 2;
  return distance;
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  Serial.println("Iniciando sistema...");
  Serial.println("Calibrando sensor do ima...");

  delay(2000); // tempo pra estabilizar

  // média de várias leituras para base mais estável
  long soma = 0;
  for (int i = 0; i < 50; i++) {
    soma += analogRead(SENSOR_IMA);
    delay(10);
  }
  valorBase = soma / 50;

  Serial.print("Valor base: ");
  Serial.println(valorBase);

  Serial.println("Sistema pronto!");
}

void loop() {

  float distancia = readDistance();
  int leituraIma = analogRead(SENSOR_IMA);
  int variacao = abs(leituraIma - valorBase);

  // debug opcional
  // Serial.println(leituraIma);

  switch (estadoAtual) {


    case ESPERANDO_PESSOA:

      if (distancia > 0 && distancia < DISTANCIA_LIMITE) {
        Serial.println("Pessoa detectada!");
        estadoAtual = AGUARDANDO_GIRO;
        delay(200); // debounce
      }
      break;

    case AGUARDANDO_GIRO:


      // detecta giro pelo ímã
      if (variacao > LIMIAR_DETECCAO && !jaContou) {
        contador++;

        Serial.print("Pessoa Entrou! Total: ");
        Serial.println(contador);

        jaContou = true;
      }

      // libera para próxima detecção
      if (variacao < LIMIAR_RESET) {
        jaContou = false;
      }

      // se a pessoa sair sem girar, reseta
      if (distancia == -1 || distancia > DISTANCIA_LIMITE) {
        Serial.println("Pessoa não detectada.");
        estadoAtual = ESPERANDO_PESSOA;
      }

      break;
  }

  delay(50);
}
