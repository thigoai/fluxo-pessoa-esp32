#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

#define MEU_PISO  2

#if MEU_PISO == 1
  uint8_t MAC_VIZINHO[] = {0x94, 0xE6, 0x86, 0x05, 0xA3, 0x68};
#else
  uint8_t MAC_VIZINHO[] = {0x3C, 0x71, 0xBF, 0x45, 0xB2, 0x94};
#endif

#define PIN_PIR    13
#define PIN_TRIG    5
#define PIN_ECHO   18
#define PIN_LED     2

#define DIST_MAX_CM       35
#define TIMEOUT_CONFIRMA  6000
#define JANELA_FLUXO      35000
#define TEMPO_COMMIT      5000
#define COOLDOWN_MS       1000   

#define PKT_DETECCAO  0
#define PKT_CANCELAR  1

//  FILA DE DETECÇÕES DO VIZINHO
//  Cada entrada guarda o timestamp de quando recebemos o pacote.
//  Ao confirmar local, consumimos a entrada mais antiga válida.
#define FILA_MAX 8

unsigned long filaVizinho[FILA_MAX];
uint8_t       filaTamanho = 0;

int pessoasNoAndar2 = 0;

void filaAdicionar() {
    if (filaTamanho < FILA_MAX) {
        filaVizinho[filaTamanho++] = millis();
    } else {
        Serial.println("[FILA] Cheia — entrada descartada");
    }
}

// Remove a entrada mais antiga (usada ao confirmar direção)
void filaConsumirAntiga() {
    if (filaTamanho == 0) return;
    for (uint8_t i = 0; i < filaTamanho - 1; i++)
        filaVizinho[i] = filaVizinho[i + 1];
    filaTamanho--;
}

// Remove a entrada mais recente (usada em cancelamento)
void filaRemoverRecente() {
    if (filaTamanho > 0) filaTamanho--;
}

// Retorna true se há entrada válida (dentro da janela de fluxo)
bool filaTemValida(unsigned long agora) {
    if (filaTamanho == 0) return false;
    // A mais antiga é a [0]; se ainda está dentro da janela, é válida
    if (agora - filaVizinho[0] >= JANELA_FLUXO) {
        // Entrada expirada — descarta e tenta próxima recursivamente
        Serial.println("[FILA] Entrada expirada, descartando.");
        filaConsumirAntiga();
        return filaTemValida(agora);
    }
    return true;
}


typedef struct {
    uint8_t  piso;
    uint8_t  tipo;
    uint32_t millis_local;
} PacoteEspNow;


enum Estado { AGUARDANDO, CONFIRMANDO, MONITORANDO, COOLDOWN };
Estado estado = AGUARDANDO;

// PIR como contador — nunca perde eventos mesmo durante COOLDOWN
volatile uint8_t contadorPIR = 0;
void IRAM_ATTR isrPIR() { contadorPIR++; }

unsigned long tEntradaEstado = 0;
bool zonaLivre = false;

long lerDistancia() {
    digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    long pulso = pulseIn(PIN_ECHO, HIGH, 30000);
    return (pulso == 0) ? 999 : (pulso * 0.034 / 2);
}

void enviarPacote(uint8_t tipo) {
    PacoteEspNow pkt = { (uint8_t)MEU_PISO, tipo, (uint32_t)millis() };
    esp_err_t r = esp_now_send(MAC_VIZINHO, (uint8_t*)&pkt, sizeof(pkt));
    Serial.printf("[ESP-NOW] %s → %s\n",
        tipo == PKT_DETECCAO ? "Deteccao" : "CANCELAMENTO",
        r == ESP_OK ? "OK" : "FALHOU");
}

void aoReceber(const uint8_t *mac, const uint8_t *dados, int len) {
    if (len != sizeof(PacoteEspNow)) return;

    PacoteEspNow pkt;
    memcpy(&pkt, dados, sizeof(pkt));

    if (pkt.tipo == PKT_CANCELAR) {
        filaRemoverRecente();
        Serial.printf("[ESP-NOW] Cancelamento do piso %d (fila: %d)\n",
                      pkt.piso, filaTamanho);
    } else {
        filaAdicionar();
        Serial.printf("[ESP-NOW] Deteccao do piso %d (fila: %d)\n",
                      pkt.piso, filaTamanho);
    }
}

void verificarEReportarDirecao() {
    unsigned long agora = millis();
    if (filaTemValida(agora)) {
        // Vizinho detectou antes → pessoa veio de lá para cá
        Serial.println();
        Serial.println("╔══════════════════════════════════════════╗");
        if (MEU_PISO == 2) {
            Serial.println("║   ▲  SUBIDA DETECTADA     (1 → 2)       ║");
            pessoasNoAndar2++;
        } else {
            Serial.println("║   ▼  DESCIDA DETECTADA    (2 → 1)       ║");
            pessoasNoAndar2--;
            if (pessoasNoAndar2 < 0) pessoasNoAndar2 = 0;
        }
        Serial.printf ("║   Deteccoes vizinho na fila: %d           ║\n", filaTamanho);
        Serial.println("╚══════════════════════════════════════════╝");
        Serial.printf("Pessoas no andar 2: %d\n", pessoasNoAndar2);
        Serial.println();
        filaConsumirAntiga();   // consome apenas UMA entrada
        // TODO IoT: publicar via MQTT aqui
    } else {
        // Nós fomos os primeiros — o vizinho vai reportar quando confirmar
        Serial.printf("[LOCAL] Piso %d committed. Vizinho reportara a direcao.\n", MEU_PISO);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);

    pinMode(PIN_PIR,  INPUT);
    pinMode(PIN_LED,  OUTPUT);
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR), isrPIR, RISING);

    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) { Serial.println("[ERRO] ESP-NOW"); return; }
    esp_now_register_recv_cb(aoReceber);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MAC_VIZINHO, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) { Serial.println("[ERRO] Peer"); return; }

    Serial.println();
    Serial.println("══════════════════════════════════════════");
    Serial.printf ("   PISO %d ONLINE\n", MEU_PISO);
    Serial.printf ("   Dist. limite   : %d cm\n", DIST_MAX_CM);
    Serial.printf ("   Janela de fluxo: %d s\n",  JANELA_FLUXO / 1000);
    Serial.printf ("   Tempo commit   : %d s\n",  TEMPO_COMMIT / 1000);
    Serial.printf ("   Capacidade fila: %d\n",    FILA_MAX);
    Serial.println("══════════════════════════════════════════\n");
}

void loop() {
    unsigned long agora = millis();

    switch (estado) {

        // Aguarda evento do PIR 
        case AGUARDANDO:
            if (contadorPIR > 0) {
                contadorPIR--;
                tEntradaEstado = agora;
                zonaLivre      = false;
                estado         = CONFIRMANDO;
                Serial.printf("[PIR] Evento (restantes na fila: %d). Confirmando...\n",
                              contadorPIR);
            }
            break;

        // Confirma com ultrassônico 
        case CONFIRMANDO: {
            long d = lerDistancia();

            if (d > 0 && d <= DIST_MAX_CM) {
                digitalWrite(PIN_LED, HIGH);
                Serial.printf("[CONFIRMADO] Piso %d — %ld cm. Monitorando retorno...\n",
                              MEU_PISO, d);
                // Avisa vizinho — direção só é reportada no COMMIT
                enviarPacote(PKT_DETECCAO);
                zonaLivre      = false;
                tEntradaEstado = agora;
                estado         = MONITORANDO;
            }
            else if (agora - tEntradaEstado > TIMEOUT_CONFIRMA) {
                Serial.println("[TIMEOUT] Ultrassonico nao confirmou.\n");
                estado = AGUARDANDO;
            }
            break;
        }

        // Monitora retorno pelo ultrassônico 
        // Retorno = zona fica livre e então volta a ficar ocupada
        // Novo PIR = segunda pessoa (mantido no contador, processado depois)
        case MONITORANDO: {
            long d = lerDistancia();
            unsigned long tempo = agora - tEntradaEstado;

            // Passo 1: detecta quando a pessoa saiu da zona
            if (!zonaLivre && d > DIST_MAX_CM) {
                zonaLivre = true;
                Serial.println("[MONIT] Zona livre. Aguardando commit...");
            }

            // Passo 2: zona fica ocupada de novo antes do commit → voltou
            //if (zonaLivre && d <= DIST_MAX_CM && tempo < TEMPO_COMMIT) {
            //    Serial.println("[RETORNO] Pessoa voltou! Cancelando.\n");
            //    enviarPacote(PKT_CANCELAR);
            //    digitalWrite(PIN_LED, LOW);
            //    tEntradaEstado = agora;
            //    estado         = COOLDOWN;
            //    break;
            //}

            // Passo 3: tempo esgotado sem retorno → commit
            if (tempo >= TEMPO_COMMIT) {
                Serial.println("[COMMIT] Passagem definitiva.");
                verificarEReportarDirecao();
                tEntradaEstado = agora;
                estado         = COOLDOWN;
            }
            break;
        }

        // Pausa entre detecções (não bloqueante)
        // contadorPIR NÃO é zerado — pessoas na fila são processadas
        case COOLDOWN:
            if (agora - tEntradaEstado >= COOLDOWN_MS) {
                digitalWrite(PIN_LED, LOW);
                estado = AGUARDANDO;
                Serial.printf("[PRONTO] Reiniciado. PIR na fila: %d\n\n", contadorPIR);
            }
            break;
    }
}