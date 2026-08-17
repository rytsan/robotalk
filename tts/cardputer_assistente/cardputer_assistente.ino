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
#include <Preferences.h>

using namespace websockets;

/* ====================== CONSTANTES CONFIGURAVEIS ===================== */
// --- Wi-Fi / WebSocket ---
// Estes valores sao apenas o padrao de fabrica. O que vale e o que estiver
// salvo na NVS pela tela de configuracao (tecla W). Se nunca foi configurado
// e o SSID ainda for o placeholder, o firmware abre a configuracao sozinho.
#define WIFI_SSID   "SUA_REDE"
#define WIFI_PASS   "SUA_SENHA"
#define WS_URL      "ws://192.168.0.100:8765"   // ultimo recurso (ver descoberta)

// --- Configuracao persistente (NVS) ---
#define CFG_NAMESPACE  "robo"
#define CFG_MAX_SSID   32
#define CFG_MAX_PASS   64
#define CFG_MAX_URL    48

// --- UI de configuracao ---
#define UI_LINE_H      10        // altura de linha com setTextSize(1)
#define UI_LIST_TOP    16
#define UI_LIST_ROWS   9         // linhas visiveis na lista de redes
#define UI_MAX_NETS    24        // redes guardadas do scan

// --- Descoberta UDP do servidor (v2.1) ---
// ROBOT_SECRET deve ser IDENTICO ao do servidor Python.
#define ROBOT_SECRET        "TROQUE_ESTE_SEGREDO_COMPARTILHADO"
// O Cardputer escuta na MESMA porta do servidor: e para la que o beacon em
// broadcast e enviado, e a resposta unicast do RDISCOVER volta pela mesma via.
#define DISCO_SERVER_PORT   8766     // porta que o Raspberry escuta e anuncia
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
#define WIFI_CONNECT_TIMEOUT_MS 15000  // espera maxima por WL_CONNECTED
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

// Telas da configuracao. SETUP_OFF = operacao normal do robo.
enum SetupScreen {
  SETUP_OFF,
  SETUP_MENU,       // menu principal
  SETUP_PICK,       // lista de redes do scan
  SETUP_TEXT,       // entrada de texto (senha, SSID oculto, URL do servidor)
  SETUP_BUSY        // conectando / escaneando
};

// Para onde vai o texto sendo digitado na tela SETUP_TEXT.
enum TextTarget {
  TXT_NONE,
  TXT_PASS,         // senha da rede escolhida
  TXT_SSID,         // SSID de rede oculta
  TXT_SERVER        // URL manual do servidor (vazio = usar descoberta)
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
bool    discoBound = false;          // socket ligado na porta 8766
String  resolvedWsUrl = "";          // v2.1: URL descoberta; vazio => usa WS_URL

// --- configuracao persistente ---
Preferences prefs;
String cfgSsid   = "";               // vazio => cai no #define WIFI_SSID
String cfgPass   = "";
String cfgServer = "";               // vazio => descoberta automatica

// --- estado da UI de configuracao ---
SetupScreen setupScreen = SETUP_OFF;
TextTarget  textTarget  = TXT_NONE;
String      textBuf     = "";        // conteudo sendo digitado
String      textTitle   = "";
String      pendingSsid = "";        // rede escolhida, aguardando senha
int         menuIndex   = 0;
int         netCount    = 0;
int         netIndex    = 0;
int         netScroll   = 0;
String      netSsid[UI_MAX_NETS];
int32_t     netRssi[UI_MAX_NETS];
bool        netOpen[UI_MAX_NETS];    // rede sem senha

// Buffer circular do microfone em RAM
int16_t* micRing = nullptr;

// Buffer DUPLO de playback (RAM estatica, ~7,7 KB no total).
// playRaw() nao copia as amostras, so guarda o ponteiro: por isso o chunk que
// esta tocando nao pode ser o mesmo que estamos preenchendo do SD.
int16_t playBuffer[2][PLAY_CHUNK_SAMPLES];

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
bool pedidoDeConfig();
bool esperarWifi(unsigned long timeoutMs);
void iniciarSD();
void configurarWebSocket();
void conectarWebSocket();
void tentarReconectarWS();
void onMessageCallback(WebsocketsMessage msg);
void tratarTexto(const String& txt);

String computeHmacHex(const String& msg);   // v2.1
String computeSha256Hex(const String& msg, uint8_t hexLen);
bool   descobrirServidor();                  // v2.1
bool   escutarBeacon();                      // escuta passiva do beacon
void   discoEnsure();
void   discoReset();
bool   discoProcessarPacote(const String& nonceEsperado);
int    jsonInt(const String& s, const char* chave, int padrao);
String jsonStr(const String& s, const char* chave);
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

void configLoad();
void configSaveWifi(const String& ssid, const String& pass);
void configSaveServer(const String& url);
bool temConfigWifi();

void setupEnter();
void setupExit(bool reconectar);
void setupLoop();
void setupDraw();
void setupDrawMenu();
void setupDrawPick();
void setupDrawText();
void setupBusy(const String& linha1, const String& linha2);
void setupScanRedes();
void setupAbrirTexto(TextTarget alvo, const String& titulo, const String& inicial);
void setupPedirSenha(const String& ssid);
void setupConfirmarTexto();
void setupConectarRede(const String& ssid, const String& pass);
void setupTeclado();

void drawFaceBase();
void drawBattery(bool force);
void drawMoodEye(int cx, int eyeIndex, uint16_t col, bool allowBlink);
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
  configLoad();
  configurarWebSocket();

  nextBlinkMs = millis() + 3000;

  // Escape de boot: segurando W na ligacao, entra direto na configuracao.
  // Vale mesmo com credencial salva e rede funcionando.
  M5Cardputer.update();
  bool forcarConfig = false;
  if (M5Cardputer.Keyboard.isPressed()) {
    Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
    for (auto c : st.word) if (tolower(c) == 'w') forcarConfig = true;
  }

  // Sem credencial salva e com o SSID ainda no placeholder de fabrica, tentar
  // conectar so gastaria 15 s ate falhar. Abre a configuracao direto.
  if (forcarConfig || !temConfigWifi()) {
    setupEnter();
    return;
  }

  conectarWifi();

  // conectarWifi() pode ter aberto a configuracao a pedido do usuario
  if (setupScreen != SETUP_OFF) return;

  // Sem rede nao ha o que descobrir. Em vez de cair num ciclo de retry que
  // nunca vai dar certo, abre a configuracao: e o que o usuario precisa aqui.
  if (WiFi.status() != WL_CONNECTED) {
    setupEnter();
    return;
  }

  // v2.1: tenta descobrir o servidor antes de conectar; fallback fica no conectarWebSocket
  if (descobrirServidor()) {
    snprintf(msgLine, sizeof(msgLine), "Servidor: %s", resolvedWsUrl.c_str());
  } else {
    strcpy(msgLine, "Descoberta falhou, WS fixo");
  }
  drawMsgLine();

  conectarWebSocket();
}

/* ============================= LOOP ================================= */
void loop() {
  M5Cardputer.update();          // atualiza teclado + power

  // A configuracao toma a tela inteira: nada de rede, mic ou rosto enquanto
  // ela esta aberta.
  if (setupScreen != SETUP_OFF) {
    setupLoop();
    delay(5);
    return;
  }

  lerTeclado();

  // lerTeclado() pode ter aberto a configuracao (tecla W). Sem este return, a
  // mesma iteracao seguia para a rede e para animateRobotFace()/drawBattery(),
  // que desenhavam o rosto por cima do menu recem-desenhado.
  if (setupScreen != SETUP_OFF) {
    delay(5);
    return;
  }

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
      // O beacon do servidor pode chegar a qualquer momento. E barato e nao
      // bloqueia, entao vale checar a cada volta: se o Raspberry subir depois
      // do Cardputer, o proximo beacon resolve sem esperar o ciclo de retry.
      escutarBeacon();
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

/* =================== CONFIGURACAO PERSISTENTE (NVS) ================= *
 * Fica na NVS e nao no SD: o SD e opcional e pode estar ausente, mas a
 * credencial de rede precisa sobreviver a qualquer boot.
 */
void configLoad() {
  prefs.begin(CFG_NAMESPACE, true);              // true = somente leitura
  cfgSsid   = prefs.getString("ssid", "");
  cfgPass   = prefs.getString("pass", "");
  cfgServer = prefs.getString("server", "");
  prefs.end();
}

void configSaveWifi(const String& ssid, const String& pass) {
  prefs.begin(CFG_NAMESPACE, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  cfgSsid = ssid;
  cfgPass = pass;
}

void configSaveServer(const String& url) {
  prefs.begin(CFG_NAMESPACE, false);
  prefs.putString("server", url);
  prefs.end();
  cfgServer = url;
}

// Ha rede utilizavel? O placeholder compilado nao conta como configuracao.
bool temConfigWifi() {
  if (cfgSsid.length() > 0) return true;
  return String(WIFI_SSID) != "SUA_REDE";
}

/* ========================= WI-FI ==================================== */

// Le o teclado no meio de uma espera longa e diz se W foi pedido.
// Sem isto, um toque em W durante os 15 s de conexao era perdido: o teclado
// so e amostrado quando M5Cardputer.update() roda, e ele nao rodava aqui.
bool pedidoDeConfig() {
  M5Cardputer.update();
  if (!M5Cardputer.Keyboard.isChange())  return false;
  if (!M5Cardputer.Keyboard.isPressed()) return false;

  Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
  for (auto c : st.word) {
    if (tolower(c) == 'w') return true;
  }
  return false;
}

// Espera a conexao. Devolve false se o usuario pediu a configuracao no meio.
bool esperarWifi(unsigned long timeoutMs) {
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    if (pedidoDeConfig()) return false;
    delay(30);
  }
  return true;
}

void conectarWifi() {
  String ssid = cfgSsid.length() ? cfgSsid : String(WIFI_SSID);
  String pass = cfgPass.length() ? cfgPass : String(WIFI_PASS);

  snprintf(msgLine, sizeof(msgLine), "Conectando... (W = config)");
  drawMsgLine();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  if (!esperarWifi(WIFI_CONNECT_TIMEOUT_MS)) {
    setupEnter();                    // usuario pediu a config durante a espera
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(msgLine, sizeof(msgLine), "WiFi OK %s", WiFi.localIP().toString().c_str());
  } else {
    strcpy(msgLine, "WiFi falhou");
  }
  drawMsgLine();
}

/* ==================== UI DE CONFIGURACAO (tecla W) ================== *
 * Teclas: ;  sobe   .  desce   Enter confirma   Ctrl volta
 * Del com o campo vazio tambem volta, como rota de fuga garantida caso a
 * tecla Ctrl sozinha nao gere evento na sua versao da lib.
 */

// Indices nomeados: o switch de acao e o desenho dependem da ordem, e com
// numero cru qualquer item novo quebraria os dois em silencio.
enum MenuItem {
  MENU_ESCOLHER = 0,
  MENU_CONECTAR,
  MENU_OCULTA,
  MENU_SERVIDOR,
  MENU_ESQUECER,
  MENU_SAIR,
  MENU_TOTAL
};

static const char* MENU_ITENS[MENU_TOTAL] = {
  "Escolher rede Wi-Fi",
  "Conectar agora",
  "Rede oculta (digitar SSID)",
  "Servidor",
  "Esquecer rede salva",
  "Sair",
};

void setupCabecalho(const String& titulo) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextSize(1);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.setCursor(4, 3);
  d.print(titulo);
  d.drawFastHLine(0, 13, SCR_W, TFT_CYAN);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
}

void setupRodape(const String& dica) {
  auto& d = M5Cardputer.Display;
  d.drawFastHLine(0, SCR_H - 12, SCR_W, TFT_DARKGREY);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setCursor(4, SCR_H - 9);
  d.print(dica);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
}

void setupBusy(const String& linha1, const String& linha2) {
  setupCabecalho("Aguarde");
  auto& d = M5Cardputer.Display;
  d.setCursor(4, 40);
  d.print(linha1);
  d.setCursor(4, 40 + UI_LINE_H);
  d.print(linha2);
}

void setupDrawMenu() {
  setupCabecalho("Configuracao");
  auto& d = M5Cardputer.Display;

  for (int i = 0; i < MENU_TOTAL; i++) {
    int y = UI_LIST_TOP + i * UI_LINE_H;
    bool sel = (i == menuIndex);

    d.setTextColor(sel ? TFT_BLACK : TFT_WHITE, sel ? TFT_CYAN : TFT_BLACK);
    d.setCursor(4, y);
    d.print(sel ? ">" : " ");
    d.print(MENU_ITENS[i]);

    // o item "Servidor" mostra o valor atual na propria linha
    if (i == MENU_SERVIDOR) {
      d.print(": ");
      d.print(cfgServer.length() ? "manual" : "auto");
    }
    d.print("   ");
  }

  // Status da rede. Sem isto, a tela que existe justamente para resolver rede
  // nao diz em que pe a rede esta.
  int y = UI_LIST_TOP + (MENU_TOTAL + 1) * UI_LINE_H;

  String salva = cfgSsid.length() ? cfgSsid : String("(padrao do firmware)");
  if (salva.length() > 31) salva = salva.substring(0, 30) + "~";

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(4, y);
  d.print("Salva: ");
  d.print(salva);

  bool online = (WiFi.status() == WL_CONNECTED);
  d.setTextColor(online ? TFT_GREEN : TFT_RED, TFT_BLACK);
  d.setCursor(4, y + UI_LINE_H);

  if (online) {
    String atual = WiFi.SSID();
    if (atual.length() > 20) atual = atual.substring(0, 19) + "~";
    d.print("Online: ");
    d.print(WiFi.localIP().toString());
    if (atual.length()) { d.print(" ("); d.print(atual); d.print(")"); }
  } else {
    d.print("Offline");
  }

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  setupRodape("; sobe  . desce  Enter ok  Ctrl volta");
}

void setupDrawPick() {
  setupCabecalho("Redes encontradas");
  auto& d = M5Cardputer.Display;

  if (netCount == 0) {
    d.setCursor(4, UI_LIST_TOP);
    d.print("Nenhuma rede encontrada.");
    setupRodape("Ctrl volta");
    return;
  }

  // mantem o item selecionado dentro da janela visivel
  if (netIndex < netScroll)                 netScroll = netIndex;
  if (netIndex >= netScroll + UI_LIST_ROWS) netScroll = netIndex - UI_LIST_ROWS + 1;

  for (int row = 0; row < UI_LIST_ROWS; row++) {
    int i = netScroll + row;
    if (i >= netCount) break;

    int y = UI_LIST_TOP + row * UI_LINE_H;
    bool sel = (i == netIndex);

    d.setTextColor(sel ? TFT_BLACK : TFT_WHITE, sel ? TFT_CYAN : TFT_BLACK);
    d.setCursor(4, y);
    d.print(sel ? ">" : " ");

    // RSSI -> 4 barras; -50 dBm ou melhor = cheio, -90 = vazio
    int barras = (netRssi[i] + 90) / 10;
    if (barras > 4) barras = 4;
    if (barras < 0) barras = 0;

    String nome = netSsid[i];
    if (nome.length() > 24) nome = nome.substring(0, 23) + "~";

    d.print(nome);
    d.setCursor(SCR_W - 46, y);
    for (int b = 0; b < 4; b++) d.print(b < barras ? "|" : ".");
    d.print(netOpen[i] ? " " : "*");
  }

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  setupRodape("* = com senha   Enter escolhe");
}

void setupDrawText() {
  setupCabecalho(textTitle);
  auto& d = M5Cardputer.Display;

  // Senha em texto claro de proposito: digitar as cegas num teclado deste
  // tamanho gera mais erro do que o mascaramento evita.
  String vis = textBuf + "_";
  const int PER_LINE = 39;
  int linha = 0;
  for (int i = 0; i < (int)vis.length() && linha < 5; i += PER_LINE, linha++) {
    d.setCursor(4, UI_LIST_TOP + linha * UI_LINE_H);
    d.print(vis.substring(i, i + PER_LINE));
  }

  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setCursor(4, UI_LIST_TOP + 6 * UI_LINE_H);
  d.print(textBuf.length());
  d.print(" caracteres");
  d.setTextColor(TFT_WHITE, TFT_BLACK);

  setupRodape("Enter confirma  Del apaga  Ctrl volta");
}

void setupDraw() {
  switch (setupScreen) {
    case SETUP_MENU: setupDrawMenu(); break;
    case SETUP_PICK: setupDrawPick(); break;
    case SETUP_TEXT: setupDrawText(); break;
    default: break;
  }
}

void setupScanRedes() {
  setupBusy("Procurando redes...", "Isso leva alguns segundos");

  int n = WiFi.scanNetworks();
  netCount = 0;

  for (int i = 0; i < n && netCount < UI_MAX_NETS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;          // rede oculta: usar menu proprio

    netSsid[netCount] = ssid;
    netRssi[netCount] = WiFi.RSSI(i);
    netOpen[netCount] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    netCount++;
  }

  WiFi.scanDelete();
  netIndex = 0;
  netScroll = 0;
  setupScreen = SETUP_PICK;
  setupDraw();
}

void setupAbrirTexto(TextTarget alvo, const String& titulo, const String& inicial) {
  textTarget = alvo;
  textTitle  = titulo;
  textBuf    = inicial;
  setupScreen = SETUP_TEXT;
  setupDraw();
}

// Abre o campo de senha para uma rede. Se for a rede que ja esta salva, comeca
// com a senha conhecida em vez de obrigar a redigitar tudo de novo.
void setupPedirSenha(const String& ssid) {
  bool conhecida = (ssid == cfgSsid && cfgPass.length() > 0);
  String titulo = conhecida ? ("Senha de " + ssid + " (salva)")
                            : ("Senha de " + ssid);
  setupAbrirTexto(TXT_PASS, titulo, conhecida ? cfgPass : String(""));
}

void setupConectarRede(const String& ssid, const String& pass) {
  setupBusy("Conectando em:", ssid);

  WiFi.disconnect();
  delay(120);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    // So grava depois de funcionar: senha errada nao apaga a config boa.
    configSaveWifi(ssid, pass);
    setupBusy("Conectado!", WiFi.localIP().toString());
    delay(1200);
    setupExit(false);                          // ja esta conectado
    return;
  }

  setupBusy("Falhou. Senha errada?", "Nada foi salvo.");
  delay(2000);
  setupScreen = SETUP_MENU;
  setupDraw();
}

void setupConfirmarTexto() {
  switch (textTarget) {
    case TXT_PASS:
      setupConectarRede(pendingSsid, textBuf);
      break;

    case TXT_SSID:
      pendingSsid = textBuf;
      if (pendingSsid.length() == 0) {
        setupScreen = SETUP_MENU;
        setupDraw();
        return;
      }
      setupPedirSenha(pendingSsid);
      break;

    case TXT_SERVER:
      textBuf.trim();
      configSaveServer(textBuf);
      resolvedWsUrl = "";                      // forca redescoberta
      setupScreen = SETUP_MENU;
      setupDraw();
      break;

    default:
      setupScreen = SETUP_MENU;
      setupDraw();
      break;
  }
}

void setupEnter() {
  if (gravando) pararGravacaoCircular();
  setupScreen = SETUP_MENU;
  menuIndex = 0;
  setupDraw();
}

void setupExit(bool reconectar) {
  setupScreen = SETUP_OFF;
  textTarget = TXT_NONE;

  drawFaceBase();
  setRobotState(STATE_IDLE);
  drawBattery(true);

  if (reconectar && WiFi.status() != WL_CONNECTED) {
    conectarWifi();
  }

  if (WiFi.status() == WL_CONNECTED) {
    resolvedWsUrl = "";
    descobrirServidor();
    conectarWebSocket();
  }

  drawMsgLine();
}

void setupTeclado() {
  if (!M5Cardputer.Keyboard.isChange())  return;
  if (!M5Cardputer.Keyboard.isPressed()) return;

  Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();

  // ---------- entrada de texto ----------
  if (setupScreen == SETUP_TEXT) {
    if (st.del) {
      if (textBuf.length() == 0) {             // apagar no vazio = voltar
        setupScreen = SETUP_MENU;
        setupDraw();
        return;
      }
      textBuf.remove(textBuf.length() - 1);
      setupDraw();
      return;
    }

    if (st.enter) { setupConfirmarTexto(); return; }

    if (st.ctrl) {
      setupScreen = SETUP_MENU;
      setupDraw();
      return;
    }

    bool mudou = false;
    for (auto c : st.word) {
      if (textBuf.length() < CFG_MAX_PASS) { textBuf += c; mudou = true; }
    }
    if (st.space && textBuf.length() < CFG_MAX_PASS) { textBuf += ' '; mudou = true; }
    if (mudou) setupDraw();
    return;
  }

  // ---------- navegacao em listas ----------
  bool sobe = false, desce = false, voltar = st.ctrl;
  for (auto c : st.word) {
    if (c == ';') sobe  = true;
    if (c == '.') desce = true;
    if (c == '`') voltar = true;               // tecla ESC do Cardputer
  }

  if (setupScreen == SETUP_MENU) {
    if (voltar) { setupExit(true); return; }
    if (sobe)  { menuIndex = (menuIndex + MENU_TOTAL - 1) % MENU_TOTAL; setupDraw(); return; }
    if (desce) { menuIndex = (menuIndex + 1) % MENU_TOTAL;              setupDraw(); return; }

    if (st.enter) {
      switch (menuIndex) {
        case MENU_ESCOLHER:
          setupScanRedes();
          break;

        case MENU_CONECTAR: {
          // Reconecta com o que ja esta salvo, sem re-escolher a rede nem
          // redigitar a senha. Cobre o caso do roteador estar fora no boot.
          if (!temConfigWifi()) {
            setupBusy("Nenhuma rede salva.", "Escolha uma no menu.");
            delay(1400);
            setupDraw();
            break;
          }
          String ssid = cfgSsid.length() ? cfgSsid : String(WIFI_SSID);
          String pass = cfgPass.length() ? cfgPass : String(WIFI_PASS);
          setupConectarRede(ssid, pass);
          break;
        }

        case MENU_OCULTA:
          setupAbrirTexto(TXT_SSID, "SSID da rede oculta", "");
          break;

        case MENU_SERVIDOR:
          setupAbrirTexto(TXT_SERVER, "Servidor (vazio = auto)", cfgServer);
          break;

        case MENU_ESQUECER:
          configSaveWifi("", "");
          setupBusy("Rede esquecida.", "Escolha outra no menu.");
          delay(1200);
          setupDraw();
          break;

        case MENU_SAIR:
          setupExit(true);
          break;
      }
    }
    return;
  }

  if (setupScreen == SETUP_PICK) {
    if (voltar) { setupScreen = SETUP_MENU; setupDraw(); return; }
    if (netCount == 0) return;

    if (sobe)  { netIndex = (netIndex + netCount - 1) % netCount; setupDraw(); return; }
    if (desce) { netIndex = (netIndex + 1) % netCount;            setupDraw(); return; }

    if (st.enter) {
      pendingSsid = netSsid[netIndex];
      if (netOpen[netIndex]) setupConectarRede(pendingSsid, "");
      else                   setupPedirSenha(pendingSsid);
    }
    return;
  }
}

void setupLoop() {
  setupTeclado();
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

// SHA-256 puro (sem HMAC), truncado. Usado para validar o token_hash que o
// servidor publica no beacon: sha256(ROBOT_SECRET)[:16].
String computeSha256Hex(const String& msg, uint8_t hexLen) {
  byte out[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)msg.c_str(), msg.length());
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);

  String hex = "";
  for (uint8_t i = 0; i < hexLen / 2; i++) {
    char b[3];
    sprintf(b, "%02x", out[i]);
    hex += b;
  }
  return hex;
}

// Extratores minimos de JSON. O beacon e um objeto plano e conhecido; puxar
// uma lib de JSON para isso custaria mais RAM do que o firmware tem sobrando.
int jsonInt(const String& s, const char* chave, int padrao) {
  int i = s.indexOf(chave);
  if (i < 0) return padrao;
  i = s.indexOf(':', i);
  if (i < 0) return padrao;
  return s.substring(i + 1).toInt();
}

String jsonStr(const String& s, const char* chave) {
  int i = s.indexOf(chave);
  if (i < 0) return String("");
  i = s.indexOf(':', i);
  if (i < 0) return String("");
  int a = s.indexOf('"', i);
  if (a < 0) return String("");
  int b = s.indexOf('"', a + 1);
  if (b < 0) return String("");
  return s.substring(a + 1, b);
}

// Abre o socket UDP uma unica vez e o mantem aberto.
// Escuta na PORTA DO SERVIDOR (8766), nao numa porta local propria: e para la
// que o servidor manda o beacon periodico em broadcast. A resposta unicast do
// RDISCOVER volta para a mesma porta, entao um socket so atende os dois casos.
void discoEnsure() {
  if (discoBound) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (disco.begin(DISCO_SERVER_PORT)) discoBound = true;
}

void discoReset() {
  if (!discoBound) return;
  disco.stop();
  discoBound = false;
}

// Processa um pacote UDP pendente. Aceita duas formas:
//   "ROBOT <ws_url> <hmac>"  -> resposta ao nosso RDISCOVER (valida o nonce)
//   {"type":"ROBO_BEACON"...} -> anuncio espontaneo do servidor
// nonceEsperado vazio = modo passivo, so beacon.
// Retorna true quando resolvedWsUrl foi preenchido.
bool discoProcessarPacote(const String& nonceEsperado) {
  int sz = disco.parsePacket();
  if (sz <= 0) return false;

  char buf[256];
  int len = disco.read((uint8_t*)buf, sizeof(buf) - 1);
  if (len <= 0) return false;
  buf[len] = 0;

  String resp = String(buf);
  IPAddress origem = disco.remoteIP();

  // eco do nosso proprio broadcast: chega porque escutamos a mesma porta
  if (resp.startsWith("RDISCOVER")) return false;

  if (resp.startsWith("ROBOT ") && nonceEsperado.length() > 0) {
    int sp1 = resp.indexOf(' ');
    int sp2 = resp.indexOf(' ', sp1 + 1);
    if (sp2 > sp1) {
      String url = resp.substring(sp1 + 1, sp2);
      String mac = resp.substring(sp2 + 1);
      mac.trim();

      if (mac == computeHmacHex(nonceEsperado)) {
        resolvedWsUrl = url;
        return true;
      }
    }
    return false;
  }

  if (resp.indexOf("ROBO_BEACON") >= 0) {
    String hashRecebido = jsonStr(resp, "token_hash");
    String hashEsperado = computeSha256Hex(String(ROBOT_SECRET), DISCO_HMAC_HEX_LEN);

    // Beacon sem token_hash so e aceito se nos tambem nao temos segredo.
    if (hashRecebido.length() > 0 && hashRecebido != hashEsperado) return false;

    int porta = jsonInt(resp, "ws_port", 8765);
    if (porta <= 0 || porta > 65535) return false;
    if (origem == IPAddress(0, 0, 0, 0)) return false;

    resolvedWsUrl = String("ws://") + origem.toString() + ":" + String(porta);
    return true;
  }

  return false;
}

// Faz broadcast "RDISCOVER <nonce>" e valida "ROBOT <ws_url> <hmac>".
// Preenche resolvedWsUrl e retorna true em sucesso.
// NUNCA deve ser chamada durante a gravacao do microfone.
bool descobrirServidor() {
  if (gravando) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  // Override manual vence tudo: em rede com isolamento de cliente o broadcast
  // nao circula e a descoberta nunca responderia.
  if (cfgServer.length() > 0) {
    resolvedWsUrl = cfgServer;
    return true;
  }

  discoEnsure();
  if (!discoBound) return false;

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
      if (discoProcessarPacote(String(nonce))) return true;
      delay(5);
    }
  }

  return false;
}

// Poll nao bloqueante do beacon do servidor. Chamado no loop enquanto nao ha
// WebSocket: se o servidor subir depois do Cardputer, o proximo beacon resolve
// sem precisar de um ciclo completo de RDISCOVER.
bool escutarBeacon() {
  if (gravando) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (cfgServer.length() > 0) return false;

  discoEnsure();
  if (!discoBound) return false;

  return discoProcessarPacote("");
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

  // Prioridade: URL manual da configuracao > descoberta UDP > gateway DHCP >
  // WS_URL compilado. O WS_URL virou ultimo recurso: com o Wi-Fi configuravel
  // pelo teclado, um IP fixo no codigo quase nunca corresponde a rede em uso.
  String target = cfgServer;
  if (target.length() == 0) target = resolvedWsUrl;
  if (target.length() == 0) target = gatewayWsUrl();
  if (target.length() == 0) target = String(WS_URL);

  if (!client.connect(target)) {
    // Esquece so o resultado da descoberta; a URL manual e escolha do usuario
    // e nao deve ser descartada por uma falha de conexao.
    resolvedWsUrl = "";
  }
}

void tentarReconectarWS() {
  // nao reconectar agressivamente; nunca durante gravacao
  if (gravando) return;
  if (millis() - wsLastTry < WS_RETRY_MS) return;

  if (WiFi.status() != WL_CONNECTED) {
    discoReset();              // o socket UDP morre junto com a interface
    WiFi.reconnect();
    wsLastTry = millis();
    // Sem rede, descoberta e WebSocket sao puro desperdicio: gastavam segundos
    // bloqueados a cada ciclo, e era justamente nesse tempo que o toque em W
    // se perdia, deixando a configuracao inalcancavel quando mais precisava.
    return;
  }

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

  uint8_t bufIdx = 0;

  while (f.available()) {
    // Espera ter VAGA na fila do canal antes de tocar no buffer.
    // isPlaying(canal): 0 = parado, 1 = tocando, 2 = tocando + 1 enfileirado.
    // Em 2 os dois buffers estao em uso; so abaixo disso o mais antigo e nosso.
    while (M5Cardputer.Speaker.isPlaying(0) == 2) {
      M5Cardputer.update();
      tickMouth();
      delay(1);
    }

    int16_t* buf = playBuffer[bufIdx];
    int bytesLidos = f.read((uint8_t*)buf, PLAY_CHUNK_BYTES);

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
      pushVisemesFromChunk(buf, samples);

      M5Cardputer.Speaker.playRaw(buf, samples, RX_SAMPLE_RATE, false, 1, 0);
      bufIdx ^= 1;
    }
  }

  // drena o que ficou enfileirado, mantendo a boca viva ate o fim
  while (M5Cardputer.Speaker.isPlaying()) {
    M5Cardputer.update();
    tickMouth();
    delay(1);
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
    if (k == 'r' || k == 's' || k == 'p' || k == 'w') tratarTecla(k);
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

    case 'w':                              // abre a configuracao de rede
      setupEnter();
      break;
  }
}

/* =========================== UI ===================================== */
// CANAIS SEPARADOS:
//   forma dos olhos/boca -> emocao do USUARIO  (currentMood, via EMO)
//   cor do rosto         -> estado do ROBO     (currentState)
// Antes o humor sequestrava a cor e o estado era so fallback, entao nao dava
// para ler "o robo esta pensando" e "o usuario esta triste" ao mesmo tempo.
uint16_t stateColor(RobotState s) {
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

// Forma do olho ditada pelo humor. Usada em IDLE e em SPEAKING.
// eyeIndex: 0 = esquerdo, 1 = direito (MOOD_CONFUSED desalinha os dois).
void drawMoodEye(int cx, int eyeIndex, uint16_t col, bool allowBlink) {
  auto& d = M5Cardputer.Display;

  switch (currentMood) {
    case MOOD_HAPPY:
      d.fillRoundRect(cx - 15, EYE_CY - 11, 30, 22, 10, col);
      return;
    case MOOD_SAD:
      d.fillRoundRect(cx - 13, EYE_CY + 1, 26, 9, 4, col);
      return;
    case MOOD_CONFUSED: {
      int off = (eyeIndex == 0) ? -5 : 5;
      d.fillRoundRect(cx - 10 + off, EYE_CY - 10, 20, 22, 6, col);
      return;
    }
    case MOOD_EXCITED:
      d.fillRoundRect(cx - 17, EYE_CY - 18, 34, 36, 9, col);
      return;
    case MOOD_CONCERNED:
      // sobrancelha caida: olho menor, deslocado para baixo
      d.fillRoundRect(cx - 12, EYE_CY - 6, 24, 20, 6, col);
      return;
    case MOOD_NEUTRAL:
      break;
  }

  if (allowBlink && blinking) d.fillRoundRect(cx - 13, EYE_CY - 3, 26, 6, 3, col);
  else                        d.fillRoundRect(cx - 13, EYE_CY - 15, 26, 30, 8, col);
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
        drawMoodEye(cx, i, col, true);
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
        // mesma forma de humor do IDLE, mas sem piscar: e enquanto o robo
        // fala que a emocao da resposta precisa estar visivel
        drawMoodEye(cx, i, col, false);
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
