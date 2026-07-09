// Firmware do Cardputer para o robô experimental.
// Versão: v2 (adiciona camada de descoberta UDP + challenge HMAC)
//
// Controles:
// - R: inicia / para a gravação circular
// - S: para a gravação, salva snapshot em SD e envia ao servidor
// - P: envia PING para o Raspberry
//
// Observações:
// - manter o pacote M5Stack em 3.2.2 enquanto o mic estiver em uso;
// - não usar 3.3.7 para este fluxo;
// - áudio do microfone e do speaker é tratado como half-duplex.
//
// MUDANÇAS DA v2 (isoladas; áudio intocado):
// - descoberta do servidor por UDP broadcast antes do WebSocket;
// - validação por HMAC-SHA256 sobre um nonce (mbedtls, já no core);
// - fallback para WS_URL fixo se a descoberta falhar;
// - descoberta NUNCA roda durante a gravação do microfone.

#include "M5Cardputer.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoWebsockets.h>
#include <SPI.h>
#include <SD.h>
#include <mbedtls/md.h>

using namespace websockets;

// ============================================================
// CONFIGURAÇÕES EDITÁVEIS
// ============================================================

const char* WIFI_SSID = "TROQUE_SEU_SSID";
const char* WIFI_PASS = "TROQUE_SUA_SENHA";
const char* WS_URL = "ws://192.168.1.100:8765";  // fallback fixo

// Segredo compartilhado com o servidor Python. TROQUE por um valor próprio.
// Deve ser idêntico ao ROBOT_SECRET do servidor.
const char* ROBOT_SECRET = "TROQUE_ESTE_SEGREDO_COMPARTILHADO";

// ============================================================
// CONSTANTES DO FLUXO
// ============================================================

static const uint32_t MIC_SAMPLE_RATE = 17000;
static const uint16_t RECORD_LENGTH = 240;
static const uint16_t RECORD_BLOCKS = 384;
static const uint16_t MIC_BLOCK_BYTES = RECORD_LENGTH * sizeof(int16_t);
static const uint32_t DISPLAY_UPDATE_MS = 1000;
static const uint32_t SEND_DELAY_MS = 3;
static const uint32_t CONNECT_RETRY_MS = 5000;
static const uint16_t SD_SCK_PIN = 40;
static const uint16_t SD_MISO_PIN = 39;
static const uint16_t SD_MOSI_PIN = 14;
static const uint16_t SD_CS_PIN = 12;
static const char* MIC_RING_PATH = "/mic_ring.raw";
static const char* RX_AUDIO_PATH = "/rx_audio.raw";

// ---- Descoberta UDP (v2) ----
static const uint16_t DISCO_SERVER_PORT = 8766;  // porta que o Raspberry escuta
static const uint16_t DISCO_LOCAL_PORT = 8767;   // porta local para receber a resposta
static const uint32_t DISCO_TIMEOUT_MS = 350;    // espera por resposta a cada tentativa
static const uint8_t  DISCO_MAX_TRIES = 4;       // tentativas antes do fallback
static const uint8_t  DISCO_HMAC_HEX_LEN = 16;   // 8 bytes -> 16 hex

// ============================================================
// ESTADO GLOBAL
// ============================================================

WebsocketsClient client;
WiFiUDP disco;
File rxAudioFile;
bool wifiReady = false;
bool wsReady = false;
bool micRecording = false;
bool receivingPlayAudio = false;
bool rxAudioReadyToPlay = false;

String resolvedWsUrl = "";  // preenchido pela descoberta; vazio => usa WS_URL

uint16_t micWriteBlockIndex = 0;
uint16_t micBlocksWritten = 0;
uint32_t lastDisplayUpdateMs = 0;
uint32_t lastConnectAttemptMs = 0;
uint32_t lastPingMs = 0;

int16_t micRing[RECORD_BLOCKS][RECORD_LENGTH];
int16_t micTempBlock[RECORD_LENGTH];

void handleWebsocketMessage(WebsocketsMessage message);
void handleWebsocketEvent(WebsocketsEvent event, String data);

// ============================================================
// UTILITÁRIOS DE TELA
// ============================================================

void drawScreen(const String& line1, const String& line2 = "", const String& line3 = "", const String& line4 = "") {
  M5Cardputer.Display.clear();
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.println(line1);
  if (line2.length()) M5Cardputer.Display.println(line2);
  if (line3.length()) M5Cardputer.Display.println(line3);
  if (line4.length()) M5Cardputer.Display.println(line4);
}

void drawWifiStatus() {
  drawScreen(
    "Robo Cardputer",
    "Wi-Fi: conectado",
    "IP: " + WiFi.localIP().toString(),
    "RSSI: " + String(WiFi.RSSI()) + " dBm"
  );
}

void drawDisconnectedStatus(const String& reason) {
  drawScreen(
    "Robo Cardputer",
    reason,
    "R: grava / para",
    "S: salva e envia"
  );
}

void drawCaptureStatus() {
  String line2 = "Gravando bloco " + String(micWriteBlockIndex + 1) + "/" + String(RECORD_BLOCKS);
  String line3 = "Blocos: " + String(micBlocksWritten) + " | 17 kHz";
  String line4 = "Ordenacao cronologica";
  drawScreen("Captura ativa", line2, line3, line4);
}

void drawReceivingStatus() {
  drawScreen("Recebendo audio", "Salvando /rx_audio.raw", "Aguardando PLAY_END");
}

void drawMessage(const String& title, const String& message) {
  drawScreen(title, message);
}

// ============================================================
// SD
// ============================================================

bool initSD() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI, 20000000)) {
    drawDisconnectedStatus("SD: falha na inicializacao");
    return false;
  }
  return true;
}

// ============================================================
// DESCOBERTA UDP + HMAC (v2)
// ============================================================

// HMAC-SHA256(ROBOT_SECRET, msg) truncado em DISCO_HMAC_HEX_LEN hex.
String computeHmacHex(const String& msg) {
  byte hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)ROBOT_SECRET, strlen(ROBOT_SECRET));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  String hex = "";
  uint8_t bytesWanted = DISCO_HMAC_HEX_LEN / 2;
  for (uint8_t i = 0; i < bytesWanted; i++) {
    char buf[3];
    sprintf(buf, "%02x", hmacResult[i]);
    hex += buf;
  }
  return hex;
}

// Faz broadcast "RDISCOVER <nonce>" e valida "ROBOT <ws_url> <hmac>".
// Preenche resolvedWsUrl e retorna true em sucesso.
// NUNCA deve ser chamada durante a gravação do microfone.
bool discoverServer() {
  if (micRecording) {
    return false;
  }

  disco.begin(DISCO_LOCAL_PORT);

  for (uint8_t attempt = 0; attempt < DISCO_MAX_TRIES; attempt++) {
    char nonce[9];
    sprintf(nonce, "%08x", (unsigned)esp_random());
    String req = "RDISCOVER " + String(nonce);

    IPAddress ip = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    IPAddress subnetBroadcast(
      ip[0] | (uint8_t)~mask[0],
      ip[1] | (uint8_t)~mask[1],
      ip[2] | (uint8_t)~mask[2],
      ip[3] | (uint8_t)~mask[3]
    );
    IPAddress targets[] = {
      WiFi.gatewayIP(),
      subnetBroadcast,
      IPAddress(255, 255, 255, 255)
    };

    for (uint8_t targetIndex = 0; targetIndex < 3; targetIndex++) {
      IPAddress target = targets[targetIndex];
      if (target == IPAddress(0, 0, 0, 0)) {
        continue;
      }
      disco.beginPacket(target, DISCO_SERVER_PORT);
      disco.write((const uint8_t*)req.c_str(), req.length());
      disco.endPacket();
    }

    uint32_t start = millis();
    while (millis() - start < DISCO_TIMEOUT_MS) {
      int sz = disco.parsePacket();
      if (sz > 0) {
        char buf[160];
        int len = disco.read((uint8_t*)buf, sizeof(buf) - 1);
        if (len > 0) {
          buf[len] = 0;
          String resp = String(buf);

          if (resp.startsWith("ROBOT ")) {
            int sp1 = resp.indexOf(' ');
            int sp2 = resp.indexOf(' ', sp1 + 1);
            if (sp2 > sp1) {
              String url = resp.substring(sp1 + 1, sp2);
              String mac = resp.substring(sp2 + 1);
              mac.trim();

              if (mac == computeHmacHex(String(nonce))) {
                resolvedWsUrl = url;
                disco.stop();
                drawMessage("Descoberta", "Servidor: " + url);
                return true;
              }
            }
          }
        }
      }
      delay(5);
    }
  }

  disco.stop();
  return false;
}

String gatewayWsUrl() {
  IPAddress gateway = WiFi.gatewayIP();
  if (gateway == IPAddress(0, 0, 0, 0)) {
    return String("");
  }
  return String("ws://") + gateway.toString() + ":8765";
}

// ============================================================
// WI-FI / WEBSOCKET
// ============================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  drawDisconnectedStatus("Conectando no Wi-Fi...");

  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
  }

  wifiReady = true;
  drawWifiStatus();
}

bool connectWebSocket() {
  if (!wifiReady) {
    return false;
  }

  if (wsReady) {
    return true;
  }

  client.onMessage(handleWebsocketMessage);
  client.onEvent(handleWebsocketEvent);

  // Prioriza a URL descoberta; se falhar, usa o gateway DHCP do hotspot.
  String gatewayUrl = gatewayWsUrl();
  String targetUrl = (resolvedWsUrl.length() > 0) ? resolvedWsUrl : gatewayUrl;
  if (targetUrl.length() == 0) {
    targetUrl = String(WS_URL);
  }

  if (!client.connect(targetUrl)) {
    wsReady = false;
    // Se a URL descoberta falhou, esquece-a para tentar redescobrir depois.
    resolvedWsUrl = "";
    return false;
  }

  wsReady = true;
  client.send("PING");
  drawWifiStatus();
  return true;
}

void sendPing() {
  if (!wsReady) {
    drawMessage("WebSocket", "Sem conexao para PING");
    return;
  }

  client.send("PING");
  drawMessage("WebSocket", "PING enviado");
}

// ============================================================
// MIC BUFFER CIRCULAR
// ============================================================

void startMicRecording() {
  micRecording = true;
  micWriteBlockIndex = 0;
  micBlocksWritten = 0;
  lastDisplayUpdateMs = 0;
  drawCaptureStatus();
}

void stopMicRecording() {
  micRecording = false;
}

bool captureMicBlock() {
  // Esta chamada depende da biblioteca M5Cardputer/M5Unified instalada.
  // A ideia é preencher exatamente um bloco de 240 amostras por iteração.
  // Se a assinatura da sua versão mudar, ajuste apenas esta linha.
  if (!M5Cardputer.Mic.record(micTempBlock, RECORD_LENGTH, MIC_SAMPLE_RATE)) {
    return false;
  }

  memcpy(micRing[micWriteBlockIndex], micTempBlock, MIC_BLOCK_BYTES);

  micWriteBlockIndex = (micWriteBlockIndex + 1) % RECORD_BLOCKS;
  if (micBlocksWritten < RECORD_BLOCKS) {
    micBlocksWritten++;
  }

  return true;
}

void saveMicSnapshotToSd() {
  SD.remove(MIC_RING_PATH);
  File micFile = SD.open(MIC_RING_PATH, FILE_WRITE);
  if (!micFile) {
    drawMessage("SD", "Falha ao salvar mic");
    return;
  }

  uint16_t blockCount = (micBlocksWritten < RECORD_BLOCKS) ? micBlocksWritten : RECORD_BLOCKS;
  uint16_t startBlock = (micBlocksWritten < RECORD_BLOCKS) ? 0 : micWriteBlockIndex;

  for (uint16_t i = 0; i < blockCount; i++) {
    uint16_t index = (startBlock + i) % RECORD_BLOCKS;
    micFile.write((const uint8_t*)micRing[index], MIC_BLOCK_BYTES);
  }

  micFile.flush();
  micFile.close();
}

void sendMicSnapshotToServer() {
  if (!wsReady) {
    drawMessage("WebSocket", "Sem conexao para enviar mic");
    return;
  }

  stopMicRecording();
  saveMicSnapshotToSd();

  uint16_t blockCount = (micBlocksWritten < RECORD_BLOCKS) ? micBlocksWritten : RECORD_BLOCKS;
  uint16_t startBlock = (micBlocksWritten < RECORD_BLOCKS) ? 0 : micWriteBlockIndex;

  client.send("RECORD_START 17000 1 s16le");

  for (uint16_t i = 0; i < blockCount; i++) {
    uint16_t index = (startBlock + i) % RECORD_BLOCKS;
    client.sendBinary((const char*)micRing[index], MIC_BLOCK_BYTES);
    delay(SEND_DELAY_MS);
  }

  client.send("RECORD_END");
  drawMessage("Mic enviado", "Aguardando transcricao");
}

// ============================================================
// AUDIO RECEBIDO DO SERVIDOR
// ============================================================

void startReceivingAudio() {
  receivingPlayAudio = true;
  rxAudioReadyToPlay = false;
  SD.remove(RX_AUDIO_PATH);
  rxAudioFile = SD.open(RX_AUDIO_PATH, FILE_WRITE);
  if (!rxAudioFile) {
    drawMessage("SD", "Falha ao abrir /rx_audio.raw");
    receivingPlayAudio = false;
    return;
  }
  drawReceivingStatus();
}

void finishReceivingAudioAndPlay() {
  if (rxAudioFile) {
    rxAudioFile.flush();
    rxAudioFile.close();
  }

  receivingPlayAudio = false;
  rxAudioReadyToPlay = true;
}

void stopMicBeforePlayback() {
  micRecording = false;
}

void playRawFromSd(const char* path) {
  stopMicBeforePlayback();

  File file = SD.open(path, FILE_READ);
  if (!file) {
    drawMessage("SD", "Nao abriu o raw de audio");
    return;
  }

  drawMessage("Audio", "Tocando no speaker...");

  uint8_t buffer[1024];
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead == 0) {
      break;
    }

    // Audio em PCM S16LE mono 16 kHz.
    // Ajuste somente se a sua versao da biblioteca tiver outro nome de metodo.
    M5Cardputer.Speaker.playRaw(buffer, bytesRead, 16000, false, 1);
    delay(1);
  }

  file.close();
  drawMessage("Audio", "Fim da reproducao");
}

// ============================================================
// MENSAGENS WEBSOCKET
// ============================================================

void handleTextMessage(const String& text) {
  if (text == "PONG") {
    drawMessage("WebSocket", "PONG recebido");
    return;
  }

  if (text == "PLAY_START") {
    startReceivingAudio();
    return;
  }

  if (text == "PLAY_END") {
    finishReceivingAudioAndPlay();
    if (rxAudioReadyToPlay) {
      playRawFromSd(RX_AUDIO_PATH);
      rxAudioReadyToPlay = false;
    }
    return;
  }

  if (text.startsWith("MSG ")) {
    drawMessage("Servidor", text.substring(4));
    return;
  }

  drawMessage("Mensagem", text);
}

void handleBinaryMessage(const std::string& payload) {
  if (!receivingPlayAudio || !rxAudioFile) {
    return;
  }

  rxAudioFile.write((const uint8_t*)payload.data(), payload.size());
}

void handleWebsocketMessage(WebsocketsMessage message) {
  if (message.isBinary()) {
    handleBinaryMessage(message.rawData());
    return;
  }

  handleTextMessage(message.data());
}

void handleWebsocketEvent(WebsocketsEvent event, String data) {
  (void)data;

  if (event == WebsocketsEvent::ConnectionOpened) {
    wsReady = true;
    drawWifiStatus();
    return;
  }

  if (event == WebsocketsEvent::ConnectionClosed) {
    wsReady = false;
    drawDisconnectedStatus("WebSocket desconectado");
    return;
  }

  if (event == WebsocketsEvent::GotPong) {
    drawMessage("WebSocket", "PONG recebido");
  }
}

// ============================================================
// TECLAS
// ============================================================

void handleKeys() {
  if (!M5Cardputer.Keyboard.isChange()) {
    return;
  }

  if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
    if (micRecording) {
      stopMicRecording();
      drawDisconnectedStatus("Gravacao parada");
    } else {
      startMicRecording();
    }
    delay(180);
    return;
  }

  if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) {
    if (micRecording) {
      sendMicSnapshotToServer();
    } else {
      drawMessage("Mic", "Nao havia gravacao ativa");
    }
    delay(180);
    return;
  }

  if (M5Cardputer.Keyboard.isKeyPressed('p') || M5Cardputer.Keyboard.isKeyPressed('P')) {
    sendPing();
    delay(180);
    return;
  }
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg);
  M5Cardputer.Display.setTextSize(2);

  drawScreen("Robo Cardputer", "Inicializando...");

  if (!initSD()) {
    // Continua mesmo sem SD para depuracao de Wi-Fi/WebSocket.
  }

  connectWiFi();

  // v2: tenta descobrir o servidor antes de conectar; fallback fica no connectWebSocket.
  if (discoverServer()) {
    drawMessage("Descoberta", "OK: " + resolvedWsUrl);
  } else {
    drawMessage("Descoberta", "Falhou, usando WS fixo");
  }

  connectWebSocket();
  drawWifiStatus();
}

void loop() {
  M5Cardputer.update();

  if (!wifiReady || WiFi.status() != WL_CONNECTED) {
    wifiReady = false;
    wsReady = false;
    if (millis() - lastConnectAttemptMs > CONNECT_RETRY_MS) {
      lastConnectAttemptMs = millis();
      connectWiFi();
      // v2: redescoberta apenas fora da gravação (aqui micRecording já é false).
      discoverServer();
      connectWebSocket();
    }
    delay(10);
    return;
  }

  if (!micRecording) {
    if (!wsReady) {
      if (millis() - lastConnectAttemptMs > CONNECT_RETRY_MS) {
        lastConnectAttemptMs = millis();
        // v2: tenta redescobrir se ainda não temos URL resolvida.
        if (resolvedWsUrl.length() == 0) {
          discoverServer();
        }
        connectWebSocket();
      }
    }
    client.poll();
  }

  handleKeys();

  if (micRecording) {
    // Durante a captura, não usamos Wi-Fi nem SD (nem descoberta).
    captureMicBlock();

    if (millis() - lastDisplayUpdateMs >= DISPLAY_UPDATE_MS) {
      lastDisplayUpdateMs = millis();
      drawCaptureStatus();
    }
  }

  if (!micRecording && wsReady) {
    client.poll();
  }

  delay(1);
}
