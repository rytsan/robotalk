/* =====================================================================
 *  CARDPUTER ASSISTENTE CONVERSACIONAL  -  FIRMWARE COMPLETA
 *  Versao: v2.1
 *  Placa : M5Stack Cardputer (ESP32-S3FN8, 8 MB flash, SEM PSRAM)
 *
 *  MUDANCAS v2.1 (descoberta do servidor; audio/rosto intocados):
 *   - Descoberta do servidor por UDP broadcast antes do WebSocket.
 *   - Validacao por HMAC-SHA256 sobre um nonce (mbedtls, ja no core).
 *   - Fallback para WS_URL fixo se a descoberta falhar.
 *   - Descoberta NUNCA roda durante a gravacao do microfone.
 *
 *  MUDANCAS v2.0 (correcao do erro de memoria):
 *   - Cardputer normal NAO tem PSRAM livre para buffers grandes.
 *   - REMOVIDO o rxBuffer de 480 KB.
 *   - Audio recebido do servidor agora e tocado lendo o SD em chunks.
 *   - Apenas o micRing (buffer circular do mic) fica em RAM.
 *   - RECORD_BLOCKS reduzido para 256 (~3,6 s) para caber com folga.
 *
 *  IMPORTANTE:
 *   - Compilar com M5Stack board package 3.2.2 (o microfone QUEBRA no 3.3.7)
 *   - Bibliotecas: M5Cardputer / M5Unified / M5GFX + ArduinoWebsockets
 *
 *  Audio:
 *   - Mic  -> servidor : PCM S16LE mono 17000 Hz
 *   - Servidor -> spk  : PCM S16LE mono 16000 Hz
 *
 *  Trade-offs adotados:
 *   estabilidade > estetica | audio > animacao | simplicidade > arquitetura
 * ===================================================================== */

#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoWebsockets.h>
#include <mbedtls/md.h>

using namespace websockets;

/* ====================== CONSTANTES CONFIGURAVEIS ===================== */
// --- Wi-Fi / WebSocket ---
#define WIFI_SSID   "SUA_REDE"
#define WIFI_PASS   "SUA_SENHA"
#define WS_URL      "ws://192.168.0.100:8765"   // fallback fixo (IP/porta do Raspberry)

// --- Descoberta UDP do servidor (v2.1) ---
// ROBOT_SECRET deve ser IDENTICO ao do servidor Python.
#define ROBOT_SECRET        "TROQUE_ESTE_SEGREDO_COMPARTILHADO"
#define DISCO_SERVER_PORT   8766     // porta que o Raspberry escuta
#define DISCO_LOCAL_PORT    8767     // porta local para receber a resposta
#define DISCO_TIMEOUT_MS    350      // espera por resposta a cada tentativa
#define DISCO_MAX_TRIES     4        // tentativas antes do fallback
#define DISCO_HMAC_HEX_LEN  16       // 8 bytes -> 16 hex

// --- Audio ---
#define MIC_SAMPLE_RATE   17000     // mic -> servidor
#define RX_SAMPLE_RATE    16000     // servidor -> speaker
#define RECORD_LENGTH     240       // amostras por bloco circular
#define RECORD_BLOCKS     256       // ~3,6 s (256*240/17000) -> ~123 KB de RAM
#define MIC_RING_SAMPLES  ((size_t)RECORD_LENGTH * RECORD_BLOCKS)
#define MIC_RING_BYTES    (MIC_RING_SAMPLES * sizeof(int16_t))

// --- Playback: le do SD em pedacos pequenos (sem buffer gigante em RAM) ---
// O chunk e um multiplo EXATO da janela de visema. Se nao for, sobra um resto
// de amostras sem visema e a boca acumula adiantamento ao longo da fala.
#define VIS_WINDOW         320                            // 20 ms @ 16 kHz
#define VIS_PER_CHUNK      6                              // visemas por chunk
#define PLAY_CHUNK_SAMPLES (VIS_WINDOW * VIS_PER_CHUNK)   // 1920 = 120 ms
#define PLAY_CHUNK_BYTES   (PLAY_CHUNK_SAMPLES * sizeof(int16_t))
#define VIS_FIFO_LEN       16                             // 2 chunks + folga
#define VIS_FRAME_MS       20                             // ritmo de saida da FIFO
#define VIS_ZCR_PCT        15                             // % de cruzamentos = sibilante

// --- SD (pinos do Cardputer) ---
#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14
#define SD_CS    12

// --- Tempos ---
#define WS_RETRY_MS       5000      // intervalo de reconexao WebSocket
#define BAT_UPDATE_MS     15000     // atualizacao da bateria
#define SPEAKER_VOLUME    200       // 0-255

/* ====================== GEOMETRIA DA INTERFACE ====================== */
// Tela 240x135. Rosto = "cabeca" de robo (borda ciano, miolo preto = BMO-like)
#define SCR_W   240
#define SCR_H   135

#define HEAD_X  16
#define HEAD_Y  16
#define HEAD_W  208
#define HEAD_H  92

// olhos
#define EYE_CY      52
#define L_EYE_CX    80
#define R_EYE_CX    160
#define EYE_BOX     44               // regiao quadrada de cada olho

// boca
#define MOUTH_CX    120
#define MOUTH_CY    87
#define MOUTH_BOX_X 78
#define MOUTH_BOX_Y 72
#define MOUTH_BOX_W 84
#define MOUTH_BOX_H 30

// rodape de status
#define STATUS_Y    110
#define STATUS_H    25

// bateria
#define BAT_X   186
#define BAT_Y   3
#define BAT_W   50
#define BAT_H   11

/* ============================ ESTADOS =============================== */
enum RobotState {
  STATE_IDLE,
  STATE_LISTENING,
  STATE_SENDING,
  STATE_THINKING,
  STATE_SPEAKING,
  STATE_ERROR
};

enum RobotMood {
  MOOD_NEUTRAL,
  MOOD_HAPPY,
  MOOD_SAD,
  MOOD_CONFUSED,
  MOOD_EXCITED,
  MOOD_CONCERNED
};

// Visemas: formato da boca derivado do audio que esta tocando.
// Duas dimensoes baratas de extrair: energia (RMS) -> abertura,
// zero-crossing rate -> forma (larga e fina vs. redonda e alta).
enum Viseme {
  VIS_CLOSED,   // silencio / pausa entre palavras
  VIS_MMM,      // energia baixa, grave: m / b / p
  VIS_SS,       // energia baixa, agudo: s / f / ch
  VIS_EE,       // energia media, agudo: e / i
  VIS_OH,       // energia media, grave: o / u
  VIS_AH        // energia alta, grave: a aberto
};

/* ========================= VARIAVEIS GLOBAIS ======================== */
WebsocketsClient client;
WiFiUDP disco;                       // v2.1: socket UDP de descoberta
String  resolvedWsUrl = "";          // v2.1: URL descoberta; vazio => usa WS_URL

// Buffer circular do microfone em RAM
int16_t* micRing = nullptr;

// Buffer pequeno para playback vindo do SD (fica em RAM estatica)
int16_t playBuffer[PLAY_CHUNK_SAMPLES];

// estado do robo / animacao
volatile RobotState currentState = STATE_IDLE;
RobotMood currentMood = MOOD_NEUTRAL;
RobotState lastDrawnState = STATE_ERROR;     // forca primeiro desenho
unsigned long lastAnimMs   = 0;
int   animFrame   = 0;               // contador generico de animacao
int   gazeDir     = 1;               // 0=esq 1=centro 2=dir (THINKING)
int   thinkDots   = 0;               // 0..3 pontinhos
// --- animacao de fala por visema ---
Viseme   currentViseme = VIS_CLOSED;
uint8_t  mouthH = 4;                 // altura suavizada da boca (px)
uint8_t  mouthW = 30;                // largura suavizada da boca (px)
uint32_t peakEnv = 3000;             // pico movel: normaliza o RMS ao nivel do Piper
Viseme   visFifo[VIS_FIFO_LEN];
uint8_t  visHead = 0;
uint8_t  visTail = 0;
unsigned long lastVisMs = 0;
bool  blinking    = false;           // IDLE: piscar
unsigned long blinkUntilMs = 0;
unsigned long nextBlinkMs  = 0;

// gravacao circular
bool   gravando        = false;
size_t writeBlockIndex = 0;
size_t blocksWritten   = 0;

// recepcao de audio
bool  recebendoAudio  = false;
bool  pendingPlayback = false;
File  rxFile;

// rede / bateria
unsigned long wsLastTry  = 0;
unsigned long batLastMs  = 0;
int  batLevel = -1;

// mensagem do servidor (rodape)
char msgLine[40] = "";

bool sdOk = false;

/* ====================== PROTOTIPOS DE FUNCAO ======================== */
void conectarWifi();
void iniciarSD();
void configurarWebSocket();
void conectarWebSocket();
void tentarReconectarWS();
void onMessageCallback(WebsocketsMessage msg);
void tratarTexto(const String& txt);

String computeHmacHex(const String& msg);   // v2.1
bool   descobrirServidor();                  // v2.1
String gatewayWsUrl();                       // fallback automatico pelo DHCP

void enableMic();
void enableSpeaker();

void iniciarGravacaoCircular();
void pararGravacaoCircular();
void capturarBlocoCircular();
void enviarSnapshotParaServidor();
void salvarSnapshotNoSD();

void tocarArquivoRaw(const char* path);

void lerTeclado();
void tratarTecla(char k);

void drawFaceBase();
void drawBattery(bool force);
void drawEyes();
void drawMouth();
void drawStatusText();
void drawMsgLine();
void setRobotState(RobotState s);
void setRobotMood(RobotMood mood);
void animateRobotFace();

Viseme visemeFromWindow(const int16_t* samples, int sampleCount);
void   visemeShape(Viseme v, uint8_t* h, uint8_t* w);
void   pushVisemesFromChunk(const int16_t* buf, int sampleCount);
void   visPush(Viseme v);
bool   visPop(Viseme* out);
void   tickMouth();
void   resetMouthAnim();

uint16_t stateColor(RobotState s);
const char* stateLabel(RobotState s);

/* ============================ SETUP ================================= */
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);                // true = habilita teclado
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);

  Serial.begin(115200);

  // --- alocacao do buffer circular do microfone em RAM comum ---
  Serial.print("Heap livre antes do micRing: ");
  Serial.println(heap_caps_get_free_size(MALLOC_CAP_8BIT));

  micRing = (int16_t*)malloc(MIC_RING_BYTES);

  if (!micRing) {
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5Cardputer.Display.setCursor(8, 50);
    M5Cardputer.Display.println("ERRO RAM micRing");
    M5Cardputer.Display.setCursor(8, 65);
    M5Cardputer.Display.print("Bytes: ");
    M5Cardputer.Display.println((int)MIC_RING_BYTES);

    Serial.print("Falha alocando micRing bytes: ");
    Serial.println((int)MIC_RING_BYTES);
    Serial.print("Heap livre: ");
    Serial.println(heap_caps_get_free_size(MALLOC_CAP_8BIT));

    while (true) delay(1000);     // se falhar, reduza RECORD_BLOCKS
  }

  memset(micRing, 0, MIC_RING_BYTES);

  Serial.print("micRing alocado bytes: ");
  Serial.println((int)MIC_RING_BYTES);
  Serial.print("Heap livre depois do micRing: ");
  Serial.println(heap_caps_get_free_size(MALLOC_CAP_8BIT));

  // --- tela base ---
  drawFaceBase();
  setRobotState(STATE_IDLE);
  drawBattery(true);

  // --- speaker ---
  M5Cardputer.Speaker.setVolume(SPEAKER_VOLUME);

  // --- perifericos ---
  iniciarSD();
  conectarWifi();
  configurarWebSocket();

  // v2.1: tenta descobrir o servidor antes de conectar; fallback fica no conectarWebSocket
  if (descobrirServidor()) {
    snprintf(msgLine, sizeof(msgLine), "Servidor: %s", resolvedWsUrl.c_str());
  } else {
    strcpy(msgLine, "Descoberta falhou, WS fixo");
  }
  drawMsgLine();

  conectarWebSocket();

  nextBlinkMs = millis() + 3000;
}

/* ============================= LOOP ================================= */
void loop() {
  M5Cardputer.update();          // atualiza teclado + power
  lerTeclado();

  if (gravando) {
    /* ---- TRECHO CRITICO: so captura microfone ----
       sem WebSocket, sem SD, sem Serial por bloco, animacao minima */
    capturarBlocoCircular();
    animateRobotFace();          // throttle interno limita a captura
  } else {
    /* ---- fora da captura: rede normal ---- */
    if (client.available()) {
      client.poll();
    } else {
      tentarReconectarWS();
    }

    if (pendingPlayback) {
      pendingPlayback = false;
      tocarArquivoRaw("/rx_audio.raw");
    }

    animateRobotFace();
  }

  drawBattery(false);
  yield();
}

/* ========================= WI-FI ==================================== */
void conectarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(msgLine, sizeof(msgLine), "WiFi OK %s", WiFi.localIP().toString().c_str());
  } else {
    strcpy(msgLine, "WiFi falhou");
  }
  drawMsgLine();
}

/* ================== DESCOBERTA UDP + HMAC (v2.1) ==================== */

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
// NUNCA deve ser chamada durante a gravacao do microfone.
bool descobrirServidor() {
  if (gravando) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

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

    unsigned long start = millis();
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

/* =========================== SD ===================================== */
void iniciarSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, SPI, 25000000);
  strcpy(msgLine, sdOk ? "SD OK" : "SD falhou");
  drawMsgLine();
}

// grava o buffer circular em ordem cronologica (chamado FORA da captura)
void salvarSnapshotNoSD() {
  if (!sdOk) return;
  File f = SD.open("/mic_ring.raw", FILE_WRITE);
  if (!f) return;

  size_t total = (blocksWritten < RECORD_BLOCKS) ? blocksWritten : RECORD_BLOCKS;
  size_t start = (blocksWritten < RECORD_BLOCKS) ? 0 : writeBlockIndex;
  for (size_t i = 0; i < total; i++) {
    size_t b = (start + i) % RECORD_BLOCKS;
    f.write((const uint8_t*)&micRing[b * RECORD_LENGTH], RECORD_LENGTH * sizeof(int16_t));
  }
  f.close();
}

/* ======================== WEBSOCKET ================================= */
void configurarWebSocket() {
  client.onMessage(onMessageCallback);
  client.onEvent([](WebsocketsEvent ev, String data) {
    if (ev == WebsocketsEvent::ConnectionOpened) {
      strcpy(msgLine, "WS conectado");
      drawMsgLine();
    } else if (ev == WebsocketsEvent::ConnectionClosed) {
      strcpy(msgLine, "WS desconectado");
      drawMsgLine();
    }
  });
}

void conectarWebSocket() {
  wsLastTry = millis();
  if (WiFi.status() != WL_CONNECTED) return;

  // v2.1: prioriza a URL descoberta; se falhar, usa o gateway DHCP do hotspot.
  String gatewayUrl = gatewayWsUrl();
  String target = (resolvedWsUrl.length() > 0) ? resolvedWsUrl : gatewayUrl;
  if (target.length() == 0) {
    target = String(WS_URL);
  }
  if (!client.connect(target)) {
    // se a URL descoberta falhou, esquece-a para redescobrir depois
    resolvedWsUrl = "";
  }
}

void tentarReconectarWS() {
  // nao reconectar agressivamente; nunca durante gravacao
  if (gravando) return;
  if (millis() - wsLastTry < WS_RETRY_MS) return;
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); }
  // v2.1: tenta redescobrir se ainda nao temos URL resolvida
  if (resolvedWsUrl.length() == 0) descobrirServidor();
  conectarWebSocket();
}

void onMessageCallback(WebsocketsMessage msg) {
  if (msg.isText()) {
    tratarTexto(msg.data());
  } else if (msg.isBinary()) {
    // binario so e valido entre PLAY_START e PLAY_END
    if (recebendoAudio && rxFile) {
      const std::string& d = msg.rawData();
      rxFile.write((const uint8_t*)d.data(), d.size());
    }
  }
}

void tratarTexto(const String& txt) {
  if (txt == "PONG") {
    strcpy(msgLine, "PONG");
    drawMsgLine();
  }
  else if (txt == "RECORDING") {
    setRobotState(STATE_LISTENING);
  }
  else if (txt == "RECORD_SAVED") {
    strcpy(msgLine, "Servidor: gravacao recebida");
    drawMsgLine();
  }
  else if (txt.startsWith("MSG ")) {
    strncpy(msgLine, txt.c_str() + 4, sizeof(msgLine) - 1);
    msgLine[sizeof(msgLine) - 1] = 0;
    drawMsgLine();
  }
  else if (txt.startsWith("STATE ")) {
    String s = txt.substring(6);
    if      (s == "IDLE")      setRobotState(STATE_IDLE);
    else if (s == "LISTENING") setRobotState(STATE_LISTENING);
    else if (s == "SENDING")   setRobotState(STATE_SENDING);
    else if (s == "THINKING")  setRobotState(STATE_THINKING);
    else if (s == "SPEAKING")  setRobotState(STATE_SPEAKING);
    else if (s == "ERROR")     setRobotState(STATE_ERROR);
  }
  else if (txt.startsWith("EMO ")) {
    String e = txt.substring(4);
    if      (e == "HAPPY")     setRobotMood(MOOD_HAPPY);
    else if (e == "SAD")       setRobotMood(MOOD_SAD);
    else if (e == "CONFUSED")  setRobotMood(MOOD_CONFUSED);
    else if (e == "EXCITED")   setRobotMood(MOOD_EXCITED);
    else if (e == "CONCERNED") setRobotMood(MOOD_CONCERNED);
    else                       setRobotMood(MOOD_NEUTRAL);
  }
  else if (txt == "PLAY_START") {
    // half-duplex: desliga mic antes de receber/tocar audio
    if (gravando) pararGravacaoCircular();
    if (M5Cardputer.Mic.isEnabled()) M5Cardputer.Mic.end();
    if (sdOk) {
      SD.remove("/rx_audio.raw");
      rxFile = SD.open("/rx_audio.raw", FILE_WRITE);
    }
    recebendoAudio = true;
    // Ainda estamos so RECEBENDO bytes; falar comeca em tocarArquivoRaw().
    // Marcar SPEAKING aqui fazia o robo gesticular durante o download.
    setRobotState(STATE_THINKING);
  }
  else if (txt == "PLAY_END") {
    recebendoAudio = false;
    if (rxFile) rxFile.close();
    pendingPlayback = true;        // playback acontece no loop(), nao no callback
  }
}

/* ===================== MOTOR DE VISEMAS (lip sync) ==================== *
 * Uma passada por janela de 20 ms extrai os dois sinais usados:
 *   - energia RMS       -> quanto a boca abre
 *   - zero-crossing rate-> forma (sibilante = larga e fina; grave = redonda)
 * O RMS e normalizado por um pico movel, entao o resultado nao depende do
 * nivel absoluto do Piper (que muda se o servidor aplicar filtro de volume).
 */
Viseme visemeFromWindow(const int16_t* samples, int sampleCount) {
  if (!samples || sampleCount <= 0) return VIS_CLOSED;

  uint64_t energy = 0;
  uint16_t crossings = 0;

  for (int i = 0; i < sampleCount; i++) {
    int32_t s = samples[i];
    energy += (uint64_t)(s * s);
    if (i > 0 && ((samples[i] < 0) != (samples[i - 1] < 0))) crossings++;
  }

  uint32_t rms = (uint32_t)sqrtf((float)(energy / (uint32_t)sampleCount));

  // pico movel: sobe na hora, decai devagar (~1/128 por janela)
  if (rms > peakEnv) peakEnv = rms;
  else               peakEnv -= (peakEnv >> 7);
  if (peakEnv < 800) peakEnv = 800;            // piso: evita amplificar ruido

  uint32_t lvl = (rms * 100) / peakEnv;                       // 0..100
  bool sibilante = ((uint32_t)crossings * 100 / (uint32_t)sampleCount) > VIS_ZCR_PCT;

  if (lvl < 8)  return VIS_CLOSED;
  if (lvl < 20) return sibilante ? VIS_SS : VIS_MMM;
  if (lvl < 55) return sibilante ? VIS_EE : VIS_OH;
  return sibilante ? VIS_EE : VIS_AH;
}

void visemeShape(Viseme v, uint8_t* h, uint8_t* w) {
  switch (v) {
    case VIS_CLOSED: *h = 4;  *w = 30; break;
    case VIS_MMM:    *h = 6;  *w = 26; break;
    case VIS_SS:     *h = 7;  *w = 38; break;
    case VIS_EE:     *h = 12; *w = 40; break;
    case VIS_OH:     *h = 18; *w = 22; break;
    case VIS_AH:     *h = 24; *w = 32; break;
    default:         *h = 4;  *w = 30; break;
  }
}

void visPush(Viseme v) {
  uint8_t next = (uint8_t)((visHead + 1) % VIS_FIFO_LEN);
  if (next == visTail) return;                 // cheia: descarta o mais novo
  visFifo[visHead] = v;
  visHead = next;
}

bool visPop(Viseme* out) {
  if (visTail == visHead) return false;
  *out = visFifo[visTail];
  visTail = (uint8_t)((visTail + 1) % VIS_FIFO_LEN);
  return true;
}

// Calcula todos os visemas de um chunk de uma vez, no momento da leitura do SD.
void pushVisemesFromChunk(const int16_t* buf, int sampleCount) {
  int windows = sampleCount / VIS_WINDOW;
  for (int w = 0; w < windows; w++) {
    visPush(visemeFromWindow(&buf[w * VIS_WINDOW], VIS_WINDOW));
  }
}

// Consome a FIFO no ritmo do audio e redesenha so quando a forma muda.
void tickMouth() {
  unsigned long now = millis();
  if (now - lastVisMs < VIS_FRAME_MS) return;
  lastVisMs = now;

  Viseme v;
  if (visPop(&v)) currentViseme = v;
  else            currentViseme = VIS_CLOSED;  // sem dado: fecha a boca

  uint8_t th, tw;
  visemeShape(currentViseme, &th, &tw);

  uint8_t prevH = mouthH;
  uint8_t prevW = mouthW;

  // attack imediato, release suave (~3 frames): sem isso a boca treme
  mouthH = (th > mouthH) ? th : (uint8_t)(mouthH - ((mouthH - th) >> 1));
  mouthW = (tw > mouthW) ? tw : (uint8_t)(mouthW - ((mouthW - tw) >> 1));

  if (mouthH != prevH || mouthW != prevW) drawMouth();
}

void resetMouthAnim() {
  visHead = 0;
  visTail = 0;
  currentViseme = VIS_CLOSED;
  mouthH = 4;
  mouthW = 30;
  peakEnv = 3000;
  lastVisMs = millis();
}

/* ===================== MIC / SPEAKER (half-duplex) ================== */
void enableMic() {
  if (M5Cardputer.Speaker.isEnabled()) M5Cardputer.Speaker.end();
  if (!M5Cardputer.Mic.isEnabled())    M5Cardputer.Mic.begin();
}

void enableSpeaker() {
  if (M5Cardputer.Mic.isEnabled())      M5Cardputer.Mic.end();
  if (!M5Cardputer.Speaker.isEnabled()) M5Cardputer.Speaker.begin();
}

/* ====================== GRAVACAO CIRCULAR =========================== */
void iniciarGravacaoCircular() {
  enableMic();
  writeBlockIndex = 0;
  blocksWritten   = 0;
  gravando        = true;
  setRobotState(STATE_LISTENING);
}

void pararGravacaoCircular() {
  gravando = false;
  // aguarda o ultimo bloco em DMA terminar
  while (M5Cardputer.Mic.isRecording()) { delay(1); }
}

// chamada a cada loop() enquanto gravando == true
void capturarBlocoCircular() {
  int16_t* dst = &micRing[writeBlockIndex * RECORD_LENGTH];
  // record() enfileira o bloco; retorna false se fila cheia (tenta no proximo loop)
  if (M5Cardputer.Mic.record(dst, RECORD_LENGTH, MIC_SAMPLE_RATE)) {
    writeBlockIndex = (writeBlockIndex + 1) % RECORD_BLOCKS;
    if (blocksWritten < RECORD_BLOCKS) blocksWritten++;
  }
}

// envia o snapshot do buffer circular em ordem cronologica
void enviarSnapshotParaServidor() {
  if (gravando) pararGravacaoCircular();

  if (blocksWritten == 0) {
    strcpy(msgLine, "Nada gravado");
    drawMsgLine();
    return;
  }
  if (!client.available()) {
    strcpy(msgLine, "WS offline");
    drawMsgLine();
    setRobotState(STATE_ERROR);
    return;
  }

  salvarSnapshotNoSD();                       // opcional: /mic_ring.raw
  setRobotState(STATE_SENDING);

  size_t total = (blocksWritten < RECORD_BLOCKS) ? blocksWritten : RECORD_BLOCKS;
  size_t start = (blocksWritten < RECORD_BLOCKS) ? 0 : writeBlockIndex;

  // cabecalho
  client.send("RECORD_START 17000 1 s16le");

  // blocos binarios pequenos: 240 amostras = 480 bytes por frame
  for (size_t i = 0; i < total; i++) {
    size_t b = (start + i) % RECORD_BLOCKS;
    client.sendBinary((const char*)&micRing[b * RECORD_LENGTH],
                      RECORD_LENGTH * sizeof(int16_t));
    client.poll();
    delay(2);                                  // deixa o TX drenar
  }

  client.send("RECORD_END");
  setRobotState(STATE_THINKING);               // servidor ira processar
  strcpy(msgLine, "Enviado");
  drawMsgLine();
}

/* ========================= PLAYBACK ================================= */
// Toca /rx_audio.raw lendo o SD em pedacos pequenos (sem buffer gigante)
void tocarArquivoRaw(const char* path) {
  if (!sdOk) {
    setRobotState(STATE_ERROR);
    strcpy(msgLine, "SD offline");
    drawMsgLine();
    return;
  }

  File f = SD.open(path, FILE_READ);

  if (!f) {
    setRobotState(STATE_ERROR);
    strcpy(msgLine, "Erro abrindo audio");
    drawMsgLine();
    return;
  }

  enableSpeaker();
  setRobotState(STATE_SPEAKING);
  resetMouthAnim();

  while (f.available()) {
    int bytesLidos = f.read((uint8_t*)playBuffer, PLAY_CHUNK_BYTES);

    if (bytesLidos <= 0) {
      break;
    }

    if (bytesLidos % 2 != 0) {
      bytesLidos--;
    }

    int samples = bytesLidos / 2;

    if (samples > 0) {
      // Visemas do chunk inteiro sao calculados aqui, de uma vez;
      // tickMouth() os consome a 20 ms enquanto o audio toca.
      pushVisemesFromChunk(playBuffer, samples);

      M5Cardputer.Speaker.playRaw(
        playBuffer,
        samples,
        RX_SAMPLE_RATE,
        false,
        1,
        0
      );

      while (M5Cardputer.Speaker.isPlaying()) {
        M5Cardputer.update();
        tickMouth();
        delay(1);
      }
    }
  }

  f.close();

  M5Cardputer.Speaker.end();
  resetMouthAnim();
  drawMouth();

  setRobotState(STATE_IDLE);
  strcpy(msgLine, "Playback fim");
  drawMsgLine();
}

/* ========================== TECLADO ================================= */
void lerTeclado() {
  if (!M5Cardputer.Keyboard.isChange())  return;
  if (!M5Cardputer.Keyboard.isPressed()) return;

  Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();

  for (auto c : st.word) {
    char k = tolower(c);
    if (k == 'r' || k == 's' || k == 'p') tratarTecla(k);
  }
  if (st.space) tratarTecla(' ');
}

void tratarTecla(char k) {
  switch (k) {
    case 'r':
    case ' ':                              // R ou SPACE: toggle gravacao
      if (!gravando) iniciarGravacaoCircular();
      else           pararGravacaoCircular();
      break;

    case 's':                              // envia snapshot
      enviarSnapshotParaServidor();
      break;

    case 'p':                              // ping / debug
      if (client.available()) {
        client.send("PING");
        strcpy(msgLine, "PING enviado");
      } else {
        strcpy(msgLine, "WS offline");
      }
      drawMsgLine();
      break;
  }
}

/* =========================== UI ===================================== */
uint16_t stateColor(RobotState s) {
  if (s != STATE_ERROR) {
    switch (currentMood) {
      case MOOD_HAPPY:     return TFT_GREEN;
      case MOOD_SAD:       return TFT_BLUE;
      case MOOD_CONFUSED:  return TFT_MAGENTA;
      case MOOD_EXCITED:   return TFT_ORANGE;
      case MOOD_CONCERNED: return TFT_YELLOW;
      case MOOD_NEUTRAL:   break;
    }
  }

  switch (s) {
    case STATE_IDLE:      return TFT_GREEN;
    case STATE_LISTENING: return TFT_CYAN;
    case STATE_SENDING:   return TFT_CYAN;
    case STATE_THINKING:  return TFT_YELLOW;
    case STATE_SPEAKING:  return TFT_GREEN;
    case STATE_ERROR:     return TFT_RED;
  }
  return TFT_WHITE;
}

const char* stateLabel(RobotState s) {
  switch (s) {
    case STATE_IDLE:      return "Pronto";
    case STATE_LISTENING: return "Ouvindo";
    case STATE_SENDING:   return "Enviando";
    case STATE_THINKING:  return "Pensando";
    case STATE_SPEAKING:  return "Falando";
    case STATE_ERROR:     return "ERRO";
  }
  return "";
}

// desenha o fundo + a "cabeca" do robo (chamado 1x; nao e redesenhado)
void drawFaceBase() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);

  // antena
  d.drawLine(HEAD_X + HEAD_W / 2, HEAD_Y, HEAD_X + HEAD_W / 2, HEAD_Y - 7, TFT_CYAN);
  d.fillCircle(HEAD_X + HEAD_W / 2, HEAD_Y - 9, 3, TFT_CYAN);

  // cabeca (borda 2px ciano, miolo preto)
  d.drawRoundRect(HEAD_X,     HEAD_Y,     HEAD_W,     HEAD_H,     14, TFT_CYAN);
  d.drawRoundRect(HEAD_X + 1, HEAD_Y + 1, HEAD_W - 2, HEAD_H - 2, 13, TFT_CYAN);
}

// bateria no canto superior direito (atualizada esporadicamente)
void drawBattery(bool force) {
  if (!force && millis() - batLastMs < BAT_UPDATE_MS) return;
  batLastMs = millis();

  int lvl = M5Cardputer.Power.getBatteryLevel();
  if (!force && lvl == batLevel) return;
  batLevel = lvl;

  auto& d = M5Cardputer.Display;
  d.fillRect(BAT_X - 2, BAT_Y - 1, (SCR_W - BAT_X) + 2, BAT_H + 3, TFT_BLACK);

  // icone
  d.drawRect(BAT_X, BAT_Y, 22, BAT_H, TFT_WHITE);
  d.fillRect(BAT_X + 22, BAT_Y + 3, 2, BAT_H - 6, TFT_WHITE);

  int pct = (lvl < 0) ? 0 : lvl;
  uint16_t fc = (pct > 50) ? TFT_GREEN : (pct > 20 ? TFT_YELLOW : TFT_RED);
  int fillW = (20 * pct) / 100;
  if (fillW > 0) d.fillRect(BAT_X + 1, BAT_Y + 1, fillW, BAT_H - 2, fc);

  // texto
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(BAT_X + 26, BAT_Y + 2);
  if (lvl < 0) d.print("--");
  else { d.print(lvl); d.print("%"); }
}

// desenha os dois olhos conforme o estado / animacao (dirty-rect)
void drawEyes() {
  auto& d = M5Cardputer.Display;
  uint16_t col = stateColor(currentState);
  int half = EYE_BOX / 2;

  int cxs[2] = { L_EYE_CX, R_EYE_CX };
  for (int i = 0; i < 2; i++) {
    int cx = cxs[i];
    // limpa a regiao do olho (interior preto da cabeca)
    d.fillRect(cx - half, EYE_CY - half, EYE_BOX, EYE_BOX, TFT_BLACK);

    switch (currentState) {
      case STATE_IDLE:
        if (currentMood == MOOD_HAPPY)
          d.fillRoundRect(cx - 15, EYE_CY - 11, 30, 22, 10, col);
        else if (currentMood == MOOD_SAD)
          d.fillRoundRect(cx - 13, EYE_CY + 1, 26, 9, 4, col);
        else if (currentMood == MOOD_CONFUSED) {
          int off = (i == 0) ? -5 : 5;
          d.fillRoundRect(cx - 10 + off, EYE_CY - 10, 20, 22, 6, col);
        }
        else if (currentMood == MOOD_EXCITED)
          d.fillRoundRect(cx - 17, EYE_CY - 18, 34, 36, 9, col);
        else if (blinking)
          d.fillRoundRect(cx - 13, EYE_CY - 3, 26, 6, 3, col);
        else
          d.fillRoundRect(cx - 13, EYE_CY - 15, 26, 30, 8, col);
        break;

      case STATE_LISTENING: {
        // pulso leve: alterna tamanho
        int e = (animFrame % 2) ? 16 : 13;
        d.fillRoundRect(cx - e, EYE_CY - e - 2, e * 2, (e + 2) * 2, 8, col);
        break;
      }

      case STATE_SENDING:                       // olhos semi-fechados
        d.fillRoundRect(cx - 13, EYE_CY - 5, 26, 10, 4, col);
        break;

      case STATE_THINKING: {                    // olhos olhando p/ os lados
        int off = (gazeDir == 0) ? -10 : (gazeDir == 2 ? 10 : 0);
        d.fillRoundRect(cx - 10 + off, EYE_CY - 11, 20, 22, 6, col);
        break;
      }

      case STATE_SPEAKING:
        d.fillRoundRect(cx - 13, EYE_CY - 15, 26, 30, 8, col);
        break;

      case STATE_ERROR:                         // olhos em X
        d.drawLine(cx - 11, EYE_CY - 11, cx + 11, EYE_CY + 11, col);
        d.drawLine(cx - 11, EYE_CY - 10, cx + 11, EYE_CY + 12, col);
        d.drawLine(cx + 11, EYE_CY - 11, cx - 11, EYE_CY + 11, col);
        d.drawLine(cx + 11, EYE_CY - 10, cx - 11, EYE_CY + 12, col);
        break;
    }
  }
}

// desenha a boca conforme o estado / animacao (dirty-rect)
void drawMouth() {
  auto& d = M5Cardputer.Display;
  uint16_t col = stateColor(currentState);

  d.fillRect(MOUTH_BOX_X, MOUTH_BOX_Y, MOUTH_BOX_W, MOUTH_BOX_H, TFT_BLACK);

  switch (currentState) {
    case STATE_IDLE:                            // boca neutra
      if (currentMood == MOOD_HAPPY || currentMood == MOOD_EXCITED) {
        d.drawArc(MOUTH_CX, MOUTH_CY - 4, 24, 14, 20, 160, col);
        d.drawArc(MOUTH_CX, MOUTH_CY - 3, 24, 14, 20, 160, col);
      } else if (currentMood == MOOD_SAD || currentMood == MOOD_CONCERNED) {
        d.drawArc(MOUTH_CX, MOUTH_CY + 9, 22, 12, 200, 340, col);
        d.drawArc(MOUTH_CX, MOUTH_CY + 10, 22, 12, 200, 340, col);
      } else if (currentMood == MOOD_CONFUSED) {
        d.fillRoundRect(MOUTH_CX - 9, MOUTH_CY - 2, 18, 5, 2, col);
      } else {
        d.fillRoundRect(MOUTH_CX - 18, MOUTH_CY - 3, 36, 6, 3, col);
      }
      break;

    case STATE_LISTENING:                       // "o" pequeno
      d.fillCircle(MOUTH_CX, MOUTH_CY, 7, col);
      break;

    case STATE_SENDING:
      d.fillRoundRect(MOUTH_CX - 11, MOUTH_CY - 2, 22, 4, 2, col);
      break;

    case STATE_THINKING:                        // linha pequena
      d.fillRoundRect(MOUTH_CX - 10, MOUTH_CY - 2, 20, 5, 2, col);
      break;

    case STATE_SPEAKING: {                      // forma vinda do visema atual
      int h = (mouthH < 3) ? 3 : mouthH;
      int w = (mouthW < 8) ? 8 : mouthW;
      int r = (h < 6) ? (h / 2) : 5;            // raio > h/2 renderiza torto
      d.fillRoundRect(MOUTH_CX - w / 2, MOUTH_CY - h / 2, w, h, r, col);
      break;
    }

    case STATE_ERROR:                           // boca triste
      d.drawLine(MOUTH_CX - 16, MOUTH_CY + 6, MOUTH_CX, MOUTH_CY - 4, col);
      d.drawLine(MOUTH_CX,      MOUTH_CY - 4, MOUTH_CX + 16, MOUTH_CY + 6, col);
      d.drawLine(MOUTH_CX - 16, MOUTH_CY + 7, MOUTH_CX, MOUTH_CY - 3, col);
      d.drawLine(MOUTH_CX,      MOUTH_CY - 3, MOUTH_CX + 16, MOUTH_CY + 7, col);
      break;
  }
}

// texto de estado no rodape (com pontinhos animados em THINKING)
void drawStatusText() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, STATUS_Y, SCR_W, 11, TFT_BLACK);
  d.setTextSize(1);
  d.setTextColor(stateColor(currentState), TFT_BLACK);
  d.setCursor(6, STATUS_Y + 1);
  d.print(stateLabel(currentState));

  if (currentState == STATE_THINKING) {
    for (int i = 0; i < thinkDots; i++) d.print(".");
  } else if (currentState != STATE_ERROR) {
    d.print("...");
  }
}

// linha de mensagem do servidor (rodape, abaixo do status)
void drawMsgLine() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, STATUS_Y + 12, SCR_W, 12, TFT_BLACK);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(6, STATUS_Y + 13);
  char buf[40];
  strncpy(buf, msgLine, 38);
  buf[38] = 0;
  d.print(buf);
}

// troca o estado e redesenha apenas as regioes afetadas
void setRobotState(RobotState s) {
  currentState = s;
  // reset de animacao
  animFrame = 0;
  gazeDir   = 1;
  thinkDots = 0;
  blinking  = false;
  lastAnimMs = millis();

  drawEyes();
  drawMouth();
  drawStatusText();
  lastDrawnState = s;
}

void setRobotMood(RobotMood mood) {
  currentMood = mood;
  drawEyes();
  drawMouth();
  drawStatusText();
}

// loop de animacao leve baseado em millis() (sem delay)
void animateRobotFace() {
  unsigned long now = millis();
  // durante a captura a animacao e bem mais lenta para nao estragar o audio
  unsigned long interval = gravando ? 400 : 90;
  if (now - lastAnimMs < interval) return;
  lastAnimMs = now;

  switch (currentState) {
    case STATE_IDLE:
      // piscar esporadico
      if (!blinking && now >= nextBlinkMs) {
        blinking = true;
        blinkUntilMs = now + 120;
        drawEyes();
      } else if (blinking && now >= blinkUntilMs) {
        blinking = false;
        drawEyes();
        nextBlinkMs = now + 2500 + (esp_random() % 3000);
      }
      break;

    case STATE_LISTENING:
      animFrame++;
      drawEyes();                 // pulso leve
      break;

    case STATE_SENDING:
      // estado curto e estatico
      break;

    case STATE_THINKING:
      animFrame++;
      gazeDir   = animFrame % 3;          // 0 esq, 1 centro, 2 dir
      thinkDots = (animFrame % 4);        // 0..3 pontinhos
      drawEyes();
      drawStatusText();
      break;

    case STATE_SPEAKING:
      // a boca e dirigida por tickMouth() dentro de tocarArquivoRaw().
      // Nada a fazer aqui: um toggle cego brigaria com o visema.
      break;

    case STATE_ERROR:
      // estatico
      break;
  }
}
