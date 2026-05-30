Sistema de Monitoramento de Fluxo no Terceiro Piso do nPITI

## 1. Visão Geral do Sistema
O projeto consiste em um sistema de IoT distribuído para monitoramento da ocupação no terceiro piso/andar do nPITI, utilizando-se do fluxo bidirecional de pessoas na escada entre o 2º e o 3º andar). O sistema utiliza a técnica de **Sensor Fusion** (combinação de sensores infravermelhos passivos e ultrassônicos) para garantir alta precisão, diminuindo o risco de falsos positivos. 

A arquitetura de rede é híbrida: utiliza **ESP-NOW** para comunicação de baixíssima latência entre os nós da borda (sincronismo dos degraus) e **Wi-Fi/MQTT** para reporte de dados à nuvem (Adafruit IO).

## 2. Arquitetura de Hardware

<img width="500" alt="Arquitetura de Sistema" src="Diagrama_ Arquitetura_de_Sistema.png" />

Cada pavimento (nó) é composto pelo seguinte conjunto de hardware:
* **Microcontrolador:** ESP32 (atuando simultaneamente como Emissor/Receptor ESP-NOW e Cliente MQTT).
* **Sensor de Gatilho (Interrupção):** Sensor PIR. Responsável por acordar o sistema e registrar o início do movimento de forma assíncrona.
* **Sensor de Confirmação:** Sensor Ultrassônico HC-SR04. Afere fisicamente a presença do usuário a uma distância configurada (<= 35cm).
* **Sinalização Visual:** LED de Statuspara validação de passagem.

## 3. Topologia de Rede e Comunicação
O sistema implementa uma rede de topologia dupla:
1. **Comunicação P2P (Edge-to-Edge):** Utiliza o protocolo **ESP-NOW** operando no mesmo canal do Wi-Fi para enviar pacotes de detecção (`PKT_DETECCAO`) e cancelamento (`PKT_CANCELAR`) entre o 2º e o 3º piso. Essa comunicação resolve o problema de sincronismo físico.
2. **Comunicação Cloud (Edge-to-Cloud):** O nó que finaliza a lógica de direcionalidade publica o evento finalizado ("Subida" ou "Descida") em um broker MQTT (`io.adafruit.com`) no tópico `Thiago_F/feeds/projeto`.

## 4. Lógica de Funcionamento (Máquina de Estados)
Para evitar bloqueios no processador e suportar o tráfego de rede, o *firmware* foi desenhado sobre uma Máquina de Estados Finitos com 4 estágios locais:

1. **`AGUARDANDO`:** Estado ocioso. O sistema não consome processamento do ultrassônico. Fica à espera de uma interrupção de hardware (RISING) do sensor PIR.
2. **`CONFIRMANDO`:** Ao receber o sinal do PIR, o ultrassônico é ativado. Se um obstáculo for detectado a menos de 35cm, a presença é confirmada, um pacote ESP-NOW é enviado ao vizinho e o estado avança. Se ocorrer um *timeout* (6000ms), o sistema assume que foi um falso positivo e retorna ao estado inicial.
3. **`MONITORANDO`:** Aguarda a pessoa liberar a zona de detecção (distância > 35cm) por um tempo mínimo de *commit* (5000ms), garantindo que a pessoa concluiu a travessia no degrau e não ficou parada na frente do sensor.
4. **`COOLDOWN`:** Período de resfriamento e *debounce* (1000ms) para reiniciar as variáveis e apagar o LED antes de aceitar a próxima leitura.

## 5. Algoritmo de Direcionalidade
O grande desafio de monitorar escadas é a assincronia dos eventos e a latência da rede. O sistema resolve isso com uma **Fila de Eventos**.

* Quando um nó detecta uma presença, ele notifica o outro via ESP-NOW.
* O nó receptor armazena o *timestamp* (`millis()`) dessa detecção externa em uma fila local (`filaVizinho`), suportando até 8 eventos simultâneos.
* Quando um nó confirma a sua própria passagem local (no estado `MONITORANDO`), ele verifica sua fila:
  * Se há um evento do vizinho dentro da **Janela de Fluxo (35 segundos)**, isso significa que a pessoa passou primeiro no outro andar e finalizou no andar atual. A direção é calculada e reportada ao MQTT. A fila é então consumida.
  * Se a fila está vazia, o nó sabe que ele foi o *primeiro* a detectar a pessoa, e delega ao vizinho a responsabilidade de reportar a direção no futuro.

## 6. Configuração e Deploy
Para configurar o sistema nas placas físicas, o código conta com um compilador diretivo condicional. O desenvolvedor deve apenas alterar a constante macro na linha 7 do arquivo `main.cpp` antes do *upload*:

```cpp
// Alterar para 1 ao gravar no ESP do andar inferior (2º Piso)
// Alterar para 2 ao gravar no ESP do andar superior (3º Piso)
#define MEU_PISO  1
A mudança da constante ajusta automaticamente qual MAC Address vizinho o nó tentará contatar, evitando erros manuais de hardcode.
