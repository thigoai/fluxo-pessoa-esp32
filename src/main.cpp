#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <PubSubClient.h>

//  CONFIGURAÇÃO DO SISTEMA
#define MEU_PISO  1 // Mude para 1 no ESP do piso inferior, e 2 no do piso superior

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

//  CONFIGURAÇÃO WI-FI E MQTT (ADAFRUIT IO)
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";

const char* MQTT_SERVER = "io.adafruit.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = ""; 
const char* MQTT_PASS   = ""; 

const char* FEED_FLUXO  = "";

WiFiClient espClient;
PubSubClient mqtt(espClient);

//  FILA DE DETECÇÕES DO VIZINHO
#define FILA_MAX 8
unsigned long filaVizinho[FILA_MAX];
uint8_t       filaTamanho = 0;

void filaAdicionar() {
    if (filaTamanho < FILA_MAX) {
        filaVizinho[filaTamanho++] = millis();
    } else {
        Serial.println("[FILA] Cheia — entrada descartada");
    }
}

void filaConsumirAntiga() {
    if (filaTamanho == 0) return;
    for (uint8_t i = 0; i < filaTamanho - 1; i++)
        filaVizinho[i] = filaVizinho[i + 1];
    filaTamanho--;
}

void filaRemoverRecente() {
    if (filaTamanho > 0) filaTamanho--;
}

bool filaTemValida(unsigned long agora) {
    if (filaTamanho == 0) return false;
    if (agora - filaVizinho[0] >= JANELA_FLUXO) {
        Serial.println("[FILA] Entrada expirada, descartando.");
        filaConsumirAntiga();
        return filaTemValida(agora);
    }
    return true;
}

//  ESTRUTURA ESP-NOW
typedef struct {
    uint8_t  piso;
    uint8_t  tipo;
    uint32_t millis_local;
} PacoteEspNow;

//  MÁQUINA DE ESTADOS
enum Estado { AGUARDANDO, CONFIRMANDO, MONITORANDO, COOLDOWN };
Estado estado = AGUARDANDO;

volatile uint8_t contadorPIR = 0;
void IRAM_ATTR isrPIR() { contadorPIR++; }

unsigned long tEntradaEstado = 0;
bool zonaLivre = false;

//  CONEXÃO MQTT
void manterConexaoMQTT() {
    if (!mqtt.connected()) {
        Serial.print("[MQTT] Conectando ao Adafruit IO... ");
        String clientId = "ESP32_Piso_" + String(MEU_PISO);
        
        if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
            Serial.println("Conectado!");
        } else {
            Serial.print("Falhou, rc=");
            Serial.print(mqtt.state());
            Serial.println(". Tentando novamente na proxima iteracao.");
        }
    }
    mqtt.loop();
}

//  SENSORES E COMUNICAÇÃO

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

void aoReceber(const esp_now_recv_info *info, const uint8_t *dados, int len) {
    if (len != sizeof(PacoteEspNow)) return;
    PacoteEspNow pkt;
    memcpy(&pkt, dados, sizeof(pkt));

    if (pkt.tipo == PKT_CANCELAR) {
        filaRemoverRecente();
        Serial.printf("[ESP-NOW] Cancelamento do piso %d (fila: %d)\n", pkt.piso, filaTamanho);
    } else {
        filaAdicionar();
        Serial.printf("[ESP-NOW] Deteccao do piso %d (fila: %d)\n", pkt.piso, filaTamanho);
    }
}

//  DIREÇÃO (REPORTA AO MQTT)
void verificarEReportarDirecao() {
    unsigned long agora = millis();
    if (filaTemValida(agora)) {
        Serial.println();
        Serial.println("╔══════════════════════════════════════════╗");
        if (MEU_PISO == 2) {
            Serial.println("║   ▲  SUBIDA DETECTADA     (1 → 2)       ║");
            mqtt.publish(FEED_FLUXO, "Subida (1 -> 2)");
        } else {
            Serial.println("║   ▼  DESCIDA DETECTADA    (2 → 1)       ║");
            mqtt.publish(FEED_FLUXO, "Descida (2 -> 1)");
        }
        Serial.printf ("║   Deteccoes vizinho na fila: %d           ║\n", filaTamanho);
        Serial.println("╚══════════════════════════════════════════╝");
        Serial.println();
        filaConsumirAntiga();
    } else {
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
    Serial.print("\n[WIFI] Conectando a rede... ");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WIFI] Conectado!");
    Serial.printf("[WIFI] Canal em uso: %d\n", WiFi.channel());

    // Configurar MQTT
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);

    // Inicializar ESP-NOW
    if (esp_now_init() != ESP_OK) { Serial.println("[ERRO] ESP-NOW"); return; }
    esp_now_register_recv_cb(aoReceber);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MAC_VIZINHO, 6);
    peer.channel = 0; // 0 significa que vai usar o canal atual (o mesmo do Wi-Fi)
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
    // Mantém a conexão com o Adafruit IO viva
    if (WiFi.status() == WL_CONNECTED) {
        manterConexaoMQTT();
    }

    unsigned long agora = millis();

    switch (estado) {
        case AGUARDANDO:
            if (contadorPIR > 0) {
                contadorPIR--;
                tEntradaEstado = agora;
                zonaLivre      = false;
                estado         = CONFIRMANDO;
                Serial.printf("[PIR] Evento (restantes na fila: %d). Confirmando...\n", contadorPIR);
            }
            break;

        case CONFIRMANDO: {
            long d = lerDistancia();

            if (d > 0 && d <= DIST_MAX_CM) {
                digitalWrite(PIN_LED, HIGH);
                Serial.printf("[CONFIRMADO] Piso %d — %ld cm. Monitorando retorno...\n", MEU_PISO, d);
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

        case MONITORANDO: {
            long d = lerDistancia();
            unsigned long tempo = agora - tEntradaEstado;

            if (!zonaLivre && d > DIST_MAX_CM) {
                zonaLivre = true;
                Serial.println("[MONIT] Zona livre. Aguardando commit...");
            }

            if (tempo >= TEMPO_COMMIT) {
                Serial.println("[COMMIT] Passagem definitiva.");
                verificarEReportarDirecao();
                tEntradaEstado = agora;
                estado         = COOLDOWN;
            }
            break;
        }

        case COOLDOWN:
            if (agora - tEntradaEstado >= COOLDOWN_MS) {
                digitalWrite(PIN_LED, LOW);
                estado = AGUARDANDO;
                Serial.printf("[PRONTO] Reiniciado. PIR na fila: %d\n\n", contadorPIR);
            }
            break;
    }
}
