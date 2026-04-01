/*
 * ============================================================
 *  AmiiboBox - M5StickC Plus2
 *  Emula amiibo via:
 *    - IrDA (UART1 → G19) para 3DS Old/2DS/3DS XL
 *    - NFC NTAG215 (PN532 via I2C) para Switch / Wii U
 *
 *  WebUI via WiFi AP para upload de arquivos .bin
 *
 *  Bibliotecas necessárias (instalar no Arduino IDE):
 *    - M5StickCPlus2  (M5Stack)
 *    - Adafruit PN532 (Adafruit)
 *    - ESPAsyncWebServer + AsyncTCP (me-no-dev)
 *    - ArduinoJson    (Benoit Blanchon)
 *    - SPIFFS         (built-in ESP32)
 *
 *  Pinagem PN532:
 *    SDA → G0
 *    SCL → G26
 *    VCC → 3.3V
 *    GND → GND
 *
 *  IR: usa G19 embutido do M5StickC Plus2 via UART1 IrDA
 * ============================================================
 */

#include <M5StickCPlus2.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_PN532.h>

// ──────────────────────────────────────────────
//  CONFIGURAÇÕES
// ──────────────────────────────────────────────
#define AP_SSID     "AmiiboBox"
#define AP_PASSWORD "amiibo123"

#define IR_TX_PIN   19      // LED IR embutido do Plus2
#define IR_RX_PIN   -1      // Sem receptor por padrão
#define IR_BAUD     115200  // IrDA-SIR baud rate do 3DS

#define PN532_SDA   32   // Porta A do M5StickC Plus2
#define PN532_SCL   33   // Porta A do M5StickC Plus2

#define MAX_AMIIBO  20      // Máximo de amiibos em memória
#define BIN_SIZE    540     // Tamanho do dump NTAG215

// ──────────────────────────────────────────────
//  PROTOCOLO 3DS - Constantes
//  Fonte: https://www.3dbrew.org/wiki/NFC_adapter
// ──────────────────────────────────────────────
// Bytes de handshake e comandos
static const uint8_t CMD_HELLO[]     = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t CMD_ACK[]       = {0xAA, 0x01};
static const uint8_t CMD_NFC_DATA    = 0x24;
static const uint8_t CMD_WRITE_ACK   = 0x26;
static const uint8_t PROTO_VERSION   = 0x01;

// XOR key para obfuscação de pacotes (layer 2 do protocolo)
static const uint8_t XOR_KEY[]       = {0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A};

// ──────────────────────────────────────────────
//  OBJETOS GLOBAIS
// ──────────────────────────────────────────────
AsyncWebServer server(80);
TwoWire i2cBus = TwoWire(0);
Adafruit_PN532 nfc(PN532_SDA, PN532_SCL, &i2cBus);

// Lista de amiibos na memória
struct AmiiboEntry {
  String name;
  String filename;
};
AmiiboEntry amiiboList[MAX_AMIIBO];
int amiiboCount = 0;
int selectedIndex = -1;

// Estado da máquina
enum State { STATE_MENU, STATE_IR_WAITING, STATE_IR_SENDING, STATE_NFC_WAITING, STATE_NFC_ACTIVE };
State currentState = STATE_MENU;

bool pn532_ok = false;
uint8_t currentBin[BIN_SIZE];
bool binLoaded = false;
String statusMsg = "Pronto";

// ──────────────────────────────────────────────
//  DISPLAY - funções auxiliares
// ──────────────────────────────────────────────
void drawScreen() {
  auto& disp = M5.Display;
  disp.fillScreen(BLACK);
  disp.setTextSize(1);

  // Header
  disp.fillRect(0, 0, disp.width(), 14, DARKGREY);
  disp.setTextColor(WHITE);
  disp.setCursor(2, 3);
  disp.print("AmiiboBox");
  disp.setCursor(disp.width() - 28, 3);
  disp.print(WiFi.softAPIP().toString());

  disp.setTextColor(CYAN);
  disp.setCursor(2, 18);

  switch (currentState) {
    case STATE_MENU:
      if (selectedIndex >= 0) {
        disp.setTextColor(YELLOW);
        disp.print("Selecionado:");
        disp.setCursor(2, 28);
        disp.setTextColor(WHITE);
        // Trunca nome se longo
        String nm = amiiboList[selectedIndex].name;
        if (nm.length() > 18) nm = nm.substring(0, 17) + ".";
        disp.print(nm);
      } else {
        disp.print("Nenhum amiibo");
        disp.setCursor(2, 28);
        disp.setTextColor(DARKGREY);
        disp.print("Use WebUI p/ enviar");
      }
      disp.setCursor(2, 46);
      disp.setTextColor(GREEN);
      disp.print("[A] IR->3DS");
      disp.setCursor(2, 58);
      disp.setTextColor(BLUE);
      disp.print("[B] NFC->Switch");
      break;

    case STATE_IR_WAITING:
      disp.setTextColor(ORANGE);
      disp.print("MODE: IR / 3DS");
      disp.setCursor(2, 28);
      disp.setTextColor(WHITE);
      disp.print("Aponte para o 3DS");
      disp.setCursor(2, 40);
      disp.print("Porta IR (baixo)");
      disp.setCursor(2, 58);
      disp.setTextColor(YELLOW);
      disp.print("Aguardando 3DS...");
      disp.setCursor(2, 72);
      disp.setTextColor(DARKGREY);
      disp.print("[A] Cancelar");
      break;

    case STATE_IR_SENDING:
      disp.setTextColor(ORANGE);
      disp.print("MODE: IR / 3DS");
      disp.setCursor(2, 28);
      disp.setTextColor(GREEN);
      disp.print("Enviando amiibo...");
      disp.setCursor(2, 44);
      disp.setTextColor(WHITE);
      disp.print(statusMsg);
      break;

    case STATE_NFC_WAITING:
      disp.setTextColor(BLUE);
      disp.print("MODE: NFC / Switch");
      disp.setCursor(2, 28);
      disp.setTextColor(WHITE);
      disp.print("Aproxime o Switch");
      disp.setCursor(2, 40);
      disp.print("do PN532");
      disp.setCursor(2, 58);
      disp.setTextColor(YELLOW);
      disp.print("NFC ativo");
      if (!pn532_ok) {
        disp.setCursor(2, 70);
        disp.setTextColor(RED);
        disp.print("PN532 nao encontrado!");
      }
      disp.setCursor(2, 82);
      disp.setTextColor(DARKGREY);
      disp.print("[B] Cancelar");
      break;

    case STATE_NFC_ACTIVE:
      disp.setTextColor(BLUE);
      disp.print("NFC: Emulando...");
      disp.setCursor(2, 28);
      disp.setTextColor(GREEN);
      disp.print(statusMsg);
      break;
  }

  // Footer - WiFi info
  disp.fillRect(0, disp.height() - 12, disp.width(), 12, DARKGREY);
  disp.setTextColor(WHITE);
  disp.setCursor(2, disp.height() - 10);
  disp.print("WiFi: ");
  disp.print(AP_SSID);
}

// ──────────────────────────────────────────────
//  SPIFFS - gerenciamento de arquivos
// ──────────────────────────────────────────────
void scanAmiiboFiles() {
  amiiboCount = 0;
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file && amiiboCount < MAX_AMIIBO) {
    String fname = String(file.name());
    if (fname.endsWith(".bin")) {
      amiiboList[amiiboCount].filename = fname;
      // Remove path e extensão para o nome de exibição
      String nm = fname;
      if (nm.startsWith("/")) nm = nm.substring(1);
      if (nm.endsWith(".bin")) nm = nm.substring(0, nm.length() - 4);
      amiiboList[amiiboCount].name = nm;
      amiiboCount++;
    }
    file = root.openNextFile();
  }
}

bool loadAmiiboBin(int index) {
  if (index < 0 || index >= amiiboCount) return false;
  File f = SPIFFS.open(amiiboList[index].filename, "r");
  if (!f) return false;
  if (f.size() < BIN_SIZE) { f.close(); return false; }
  f.read(currentBin, BIN_SIZE);
  f.close();
  binLoaded = true;
  return true;
}

// ──────────────────────────────────────────────
//  PROTOCOLO IrDA 3DS
//  Baseado em: https://www.3dbrew.org/wiki/NFC_adapter
//  e: https://github.com/HubSteven/3ds_ir
// ──────────────────────────────────────────────

// Configura UART1 no modo IrDA-SIR (sem modulação de portadora)
// O ESP32 tem suporte nativo a IrDA via registrador de hardware
void irda_begin() {
  // Inicia Serial2 em G19 (TX) sem pino RX
  Serial2.begin(IR_BAUD, SERIAL_8N1, IR_RX_PIN, IR_TX_PIN);

  // Habilita modo IrDA no hardware UART1 do ESP32
  // REG 0x3FF6E020 = UART1 CONF0
  // Bit 16 = IRDA_EN, Bit 10 = IRDA_TX_EN
  #include "soc/uart_struct.h"
  UART1.conf0.irda_en    = 1;
  UART1.conf0.irda_tx_en = 1;
  UART1.conf0.irda_rx_en = 0; // sem receptor por padrão
}

// Calcula checksum XOR dos bytes
uint8_t calc_checksum(uint8_t* data, int len) {
  uint8_t csum = 0;
  for (int i = 0; i < len; i++) csum ^= data[i];
  return csum;
}

// Aplica XOR de obfuscação (layer 2 do protocolo 3DS)
void xor_obfuscate(uint8_t* data, int len, uint8_t conn_id) {
  for (int i = 0; i < len; i++) {
    data[i] ^= XOR_KEY[i % 8] ^ conn_id;
  }
}

// Monta e envia um pacote IrDA para o 3DS
// Estrutura: [0xAA][len_low][len_high][payload...][checksum]
void irda_send_packet(uint8_t* payload, int payload_len, uint8_t conn_id) {
  uint8_t pkt[payload_len + 4];
  pkt[0] = 0xAA;  // magic
  pkt[1] = payload_len & 0xFF;
  pkt[2] = (payload_len >> 8) & 0xFF;

  memcpy(&pkt[3], payload, payload_len);
  xor_obfuscate(&pkt[3], payload_len, conn_id);

  pkt[3 + payload_len] = calc_checksum(&pkt[3], payload_len);

  Serial2.write(pkt, sizeof(pkt));
  Serial2.flush();
}

// Aguarda resposta do 3DS (timeout em ms)
// Retorna true se recebeu ACK válido
bool irda_wait_ack(int timeout_ms) {
  unsigned long start = millis();
  uint8_t buf[64];
  int idx = 0;
  while (millis() - start < timeout_ms) {
    if (Serial2.available()) {
      uint8_t b = Serial2.read();
      buf[idx++] = b;
      // ACK mínimo: [0xAA, 0x01, ...]
      if (idx >= 2 && buf[0] == 0xAA && buf[1] == 0x01) return true;
      if (idx >= 64) break;
    }
    delay(1);
  }
  return false;
}

// Lê dados recebidos do 3DS (para handshake inicial)
bool irda_read_packet(uint8_t* out, int* out_len, int timeout_ms) {
  unsigned long start = millis();
  *out_len = 0;
  bool got_magic = false;
  int expected_len = 0;
  int read_idx = 0;

  while (millis() - start < timeout_ms) {
    if (Serial2.available()) {
      uint8_t b = Serial2.read();
      if (!got_magic) {
        if (b == 0xAA) got_magic = true;
      } else if (expected_len == 0) {
        expected_len = b;
      } else {
        out[read_idx++] = b;
        if (read_idx >= expected_len) {
          *out_len = read_idx;
          return true;
        }
      }
    }
    delay(1);
  }
  return false;
}

// Envia o dump completo do amiibo para o 3DS via IrDA
// Segue o protocolo documentado no 3dbrew.org
bool irda_send_amiibo(uint8_t* bin, uint8_t conn_id) {
  // Pacote de dados NFC: CMD + UID (7 bytes) + data dump (532 bytes)
  // Total payload = 1 + 7 + 532 = 540 bytes
  uint8_t payload[BIN_SIZE + 1];
  payload[0] = CMD_NFC_DATA;

  // UID está nos bytes 0-6 do dump NTAG215
  memcpy(&payload[1], bin, BIN_SIZE);

  irda_send_packet(payload, sizeof(payload), conn_id);
  statusMsg = "Dados enviados!";
  drawScreen();

  // Aguarda confirmação do 3DS
  if (irda_wait_ack(3000)) {
    statusMsg = "3DS aceitou!";
    return true;
  }
  statusMsg = "Sem resposta do 3DS";
  return false;
}

// Fluxo completo de comunicação IrDA com o 3DS
void run_irda_session() {
  currentState = STATE_IR_WAITING;
  drawScreen();

  statusMsg = "Aguardando handshake...";
  uint8_t conn_id = random(0x01, 0xFE); // ID de conexão aleatório
  uint8_t rx_buf[128];
  int rx_len = 0;

  // Passo 1: Enviar pacote HELLO e aguardar resposta do 3DS
  uint8_t hello_payload[8];
  memcpy(hello_payload, CMD_HELLO, 8);
  hello_payload[0] = PROTO_VERSION;
  hello_payload[1] = conn_id;

  bool connected = false;
  for (int attempt = 0; attempt < 30 && !connected; attempt++) {
    irda_send_packet(hello_payload, 8, conn_id);
    if (irda_read_packet(rx_buf, &rx_len, 500)) {
      connected = true;
    }
    delay(100);
    // Verifica botão A para cancelar
    M5.update();
    if (M5.BtnA.wasPressed()) {
      currentState = STATE_MENU;
      drawScreen();
      return;
    }
  }

  if (!connected) {
    statusMsg = "3DS nao respondeu";
    currentState = STATE_MENU;
    drawScreen();
    delay(2000);
    drawScreen();
    return;
  }

  // Passo 2: Enviar ACK de conexão
  uint8_t ack_payload[2];
  memcpy(ack_payload, CMD_ACK, 2);
  irda_send_packet(ack_payload, 2, conn_id);

  // Passo 3: Aguardar pedido de NFC do 3DS
  currentState = STATE_IR_SENDING;
  statusMsg = "Conectado! Aguardando pedido...";
  drawScreen();

  bool got_request = false;
  unsigned long wait_start = millis();
  while (millis() - wait_start < 15000 && !got_request) {
    if (irda_read_packet(rx_buf, &rx_len, 1000)) {
      // Qualquer pacote do 3DS neste ponto é um pedido de NFC
      if (rx_len > 0) got_request = true;
    }
    M5.update();
    if (M5.BtnA.wasPressed()) break;
  }

  if (!got_request) {
    statusMsg = "Timeout aguardando 3DS";
    currentState = STATE_MENU;
    drawScreen();
    delay(2000);
    drawScreen();
    return;
  }

  // Passo 4: Enviar dados do amiibo
  statusMsg = "Enviando amiibo...";
  drawScreen();

  bool ok = irda_send_amiibo(currentBin, conn_id);

  currentState = STATE_MENU;
  drawScreen();
  delay(3000);
  drawScreen();
}

// ──────────────────────────────────────────────
//  NFC - PN532 em modo Card Emulation (NTAG215)
// ──────────────────────────────────────────────

// Inicializa o PN532
bool pn532_init() {
  i2cBus.begin(PN532_SDA, PN532_SCL, 400000);
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) return false;
  nfc.SAMConfig();
  return true;
}

// Emula NTAG215 com os dados do amiibo carregado
// O PN532 vai responder a leituras NFC como se fosse um tag real
void run_nfc_emulation() {
  currentState = STATE_NFC_WAITING;
  drawScreen();

  if (!pn532_ok) {
    statusMsg = "PN532 nao conectado!";
    drawScreen();
    delay(3000);
    currentState = STATE_MENU;
    drawScreen();
    return;
  }

  statusMsg = "Aproxime o Switch...";
  drawScreen();

  // Configura PN532 para emular card ISO14443A (NTAG215 é Ultralight)
  // UID do amiibo (7 bytes) vem dos bytes 0-6 do .bin
  uint8_t uid[7];
  memcpy(uid, currentBin, 7);

  // ATQA e SAK para NTAG215
  uint8_t atqa[2] = {0x44, 0x00};
  uint8_t sak = 0x00;

  // Inicia emulação (modo target)
  // O PN532 emula um tag e responde a comandos do leitor
  uint8_t responseBuffer[64];
  uint8_t responseLength = 0;

  currentState = STATE_NFC_ACTIVE;
  statusMsg = "NFC: emulando NTAG215";
  drawScreen();

  unsigned long session_start = millis();
  int page_offset = 0;
  bool reading_active = true;

  while (reading_active && millis() - session_start < 30000) {
    // Tenta iniciar emulação de target
    // O PN532 aguarda um initiator (Switch/Wii U) se conectar
    uint8_t cmd[64];
    uint8_t cmdLen = 0;

    // Configura target para emulação passiva 106kbps (ISO14443A)
    uint8_t targetData[37];
    targetData[0]  = 0x00; // SENS_RES (ATQA byte 0)
    targetData[1]  = 0x44; // SENS_RES (ATQA byte 1)
    targetData[2]  = 0x00; // SAK
    targetData[3]  = 7;    // NFCID length
    memcpy(&targetData[4], uid, 7);

    bool got_data = nfc.tgInitAsTarget(
      0x00,        // mode: passive
      targetData,
      NULL, 0,     // GeneralBytes
      responseBuffer, &responseLength,
      2000         // timeout 2s
    );

    if (!got_data) {
      M5.update();
      if (M5.BtnB.wasPressed()) break;
      continue;
    }

    currentState = STATE_NFC_ACTIVE;
    statusMsg = "Leitura em andamento!";
    drawScreen();

    // Responde a comandos READ do NTAG215
    // Cada READ pede 4 páginas (16 bytes) a partir de uma página inicial
    bool session_active = true;
    while (session_active) {
      if (responseLength >= 2) {
        uint8_t cmd_code = responseBuffer[0];

        if (cmd_code == 0x30) {
          // READ command - responseBuffer[1] = página inicial
          uint8_t start_page = responseBuffer[1];
          uint8_t read_data[16];

          for (int i = 0; i < 16; i++) {
            int byte_idx = (start_page * 4 + i) % BIN_SIZE;
            read_data[i] = currentBin[byte_idx];
          }

          nfc.tgSetData(read_data, 16);

        } else if (cmd_code == 0xA2) {
          // WRITE command - responde com ACK
          uint8_t ack = 0x0A;
          nfc.tgSetData(&ack, 1);

          // Salva dado escrito (para write-back)
          if (responseLength >= 6) {
            uint8_t write_page = responseBuffer[1];
            int byte_start = write_page * 4;
            if (byte_start + 4 <= BIN_SIZE) {
              memcpy(&currentBin[byte_start], &responseBuffer[2], 4);
            }
          }

        } else if (cmd_code == 0x60) {
          // GET_VERSION - retorna info do NTAG215
          uint8_t version[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
          nfc.tgSetData(version, 8);

        } else if (cmd_code == 0x3A) {
          // FAST_READ
          uint8_t start_pg = responseBuffer[1];
          uint8_t end_pg   = responseBuffer[2];
          int num_pages    = end_pg - start_pg + 1;
          int num_bytes    = num_pages * 4;
          uint8_t fast_data[num_bytes];
          for (int i = 0; i < num_bytes; i++) {
            int idx = (start_pg * 4 + i) % BIN_SIZE;
            fast_data[i] = currentBin[idx];
          }
          nfc.tgSetData(fast_data, num_bytes);

        } else {
          // Comando desconhecido - NACK
          uint8_t nack = 0x00;
          nfc.tgSetData(&nack, 1);
        }
      }

      // Aguarda próximo comando
      responseLength = 0;
      bool got_cmd = nfc.tgGetData(responseBuffer, &responseLength, 2000);
      if (!got_cmd) session_active = false;
    }

    statusMsg = "Leitura concluida!";
    drawScreen();
    delay(1500);

    M5.update();
    if (M5.BtnB.wasPressed()) break;
  }

  currentState = STATE_MENU;
  drawScreen();
}

// ──────────────────────────────────────────────
//  WEB SERVER - Interface de upload
// ──────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AmiiboBox</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, sans-serif; background: #0d0d1a; color: #eee; min-height: 100vh; }
  .header { background: linear-gradient(135deg,#1a1a3e,#2d1b6e); padding: 20px; text-align: center; }
  .header h1 { font-size: 1.8em; color: #a78bfa; }
  .header p  { color: #888; font-size: 0.9em; margin-top: 4px; }
  .container { max-width: 480px; margin: 0 auto; padding: 16px; }
  .card { background: #1a1a2e; border: 1px solid #2d2d5e; border-radius: 12px; padding: 16px; margin-bottom: 16px; }
  .card h2 { font-size: 1em; color: #a78bfa; margin-bottom: 12px; display: flex; align-items: center; gap: 8px; }
  .upload-area { border: 2px dashed #3d3d7e; border-radius: 8px; padding: 24px; text-align: center; cursor: pointer; transition: border-color 0.2s; }
  .upload-area:hover { border-color: #a78bfa; }
  .upload-area input { display: none; }
  .upload-area .icon { font-size: 2em; margin-bottom: 8px; }
  .upload-area p { color: #888; font-size: 0.85em; }
  .btn { display: inline-block; padding: 10px 20px; border-radius: 8px; border: none; cursor: pointer; font-size: 0.9em; font-weight: 600; transition: opacity 0.2s; width: 100%; margin-top: 8px; }
  .btn:hover { opacity: 0.85; }
  .btn-primary { background: #6d28d9; color: white; }
  .btn-danger  { background: #7f1d1d; color: #fca5a5; }
  .btn-success { background: #14532d; color: #86efac; }
  .amiibo-list { list-style: none; }
  .amiibo-list li { display: flex; justify-content: space-between; align-items: center; padding: 10px 12px; border-radius: 8px; margin-bottom: 6px; background: #0d0d1a; border: 1px solid #2d2d5e; cursor: pointer; transition: border-color 0.2s; }
  .amiibo-list li:hover { border-color: #6d28d9; }
  .amiibo-list li.selected { border-color: #a78bfa; background: #1e1b4b; }
  .amiibo-list li .name { font-size: 0.9em; flex: 1; }
  .amiibo-list li .del { color: #f87171; background: none; border: none; cursor: pointer; font-size: 1.1em; padding: 4px 8px; }
  .status { padding: 10px 14px; border-radius: 8px; font-size: 0.85em; margin-top: 8px; }
  .status.ok  { background: #14532d; color: #86efac; }
  .status.err { background: #7f1d1d; color: #fca5a5; }
  .status.info{ background: #1e3a5f; color: #93c5fd; }
  .instructions { font-size: 0.82em; color: #666; line-height: 1.6; }
  .instructions li { margin-bottom: 4px; }
  .badge { font-size: 0.75em; background: #2d2d5e; color: #a78bfa; padding: 2px 8px; border-radius: 20px; }
  progress { width: 100%; height: 8px; border-radius: 4px; margin-top: 8px; appearance: none; }
  progress::-webkit-progress-bar { background: #0d0d1a; border-radius: 4px; }
  progress::-webkit-progress-value { background: #6d28d9; border-radius: 4px; }
  #noamiibo { color: #444; text-align: center; padding: 16px; font-size: 0.85em; }
</style>
</head>
<body>
<div class="header">
  <h1>🎮 AmiiboBox</h1>
  <p>M5StickC Plus2 · IR para 3DS · NFC para Switch</p>
</div>
<div class="container">

  <!-- Upload -->
  <div class="card">
    <h2>📤 Enviar Amiibo <span class="badge">.bin</span></h2>
    <div class="upload-area" onclick="document.getElementById('fileInput').click()">
      <div class="icon">📁</div>
      <p>Clique para selecionar um arquivo .bin</p>
      <p style="margin-top:4px;">Dump NTAG215 · 540 bytes</p>
      <input type="file" id="fileInput" accept=".bin" onchange="uploadFile(this)">
    </div>
    <progress id="uploadProgress" value="0" max="100" style="display:none;"></progress>
    <div id="uploadStatus"></div>
  </div>

  <!-- Lista -->
  <div class="card">
    <h2>🗂 Amiibos Salvos</h2>
    <ul class="amiibo-list" id="amiiboList">
      <li id="noamiibo">Nenhum amiibo salvo</li>
    </ul>
    <div id="selectStatus"></div>
  </div>

  <!-- Como usar -->
  <div class="card">
    <h2>📖 Como usar</h2>
    <ul class="instructions">
      <li>1. Conecte-se ao WiFi <strong>AmiiboBox</strong> (senha: <strong>amiibo123</strong>)</li>
      <li>2. Envie um arquivo <strong>.bin</strong> de dump de amiibo (540 bytes)</li>
      <li>3. Selecione o amiibo na lista acima</li>
      <li>4. No M5Stick, pressione <strong>[A]</strong> para modo IR (3DS Old)</li>
      <li>5. Ou pressione <strong>[B]</strong> para modo NFC (Switch / Wii U)</li>
    </ul>
  </div>

  <!-- Status do dispositivo -->
  <div class="card">
    <h2>📡 Status do Dispositivo</h2>
    <div id="deviceStatus" class="status info">Carregando...</div>
    <button class="btn btn-success" onclick="loadStatus()" style="margin-top:8px;">🔄 Atualizar</button>
  </div>

</div>

<script>
async function uploadFile(input) {
  const file = input.files[0];
  if (!file) return;
  if (!file.name.endsWith('.bin')) {
    showStatus('uploadStatus', 'Erro: apenas arquivos .bin são aceitos', 'err');
    return;
  }

  const prog = document.getElementById('uploadProgress');
  prog.style.display = 'block';
  prog.value = 10;

  const formData = new FormData();
  formData.append('file', file);

  try {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/upload');
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) prog.value = (e.loaded / e.total) * 100;
    };
    xhr.onload = () => {
      prog.style.display = 'none';
      if (xhr.status === 200) {
        showStatus('uploadStatus', '✅ ' + file.name + ' enviado com sucesso!', 'ok');
        loadAmiiboList();
      } else {
        showStatus('uploadStatus', '❌ Erro no upload: ' + xhr.responseText, 'err');
      }
    };
    xhr.onerror = () => {
      prog.style.display = 'none';
      showStatus('uploadStatus', '❌ Falha na conexão', 'err');
    };
    xhr.send(formData);
  } catch(e) {
    showStatus('uploadStatus', '❌ Erro: ' + e.message, 'err');
  }
}

async function loadAmiiboList() {
  try {
    const r = await fetch('/list');
    const data = await r.json();
    const list = document.getElementById('amiiboList');
    list.innerHTML = '';
    if (!data.amiibos || data.amiibos.length === 0) {
      list.innerHTML = '<li id="noamiibo">Nenhum amiibo salvo</li>';
      return;
    }
    data.amiibos.forEach((a, i) => {
      const li = document.createElement('li');
      if (data.selected === i) li.classList.add('selected');
      li.innerHTML = `<span class="name">🎴 ${a}</span>
        <button class="del" onclick="deleteAmiibo('${a}',event)" title="Deletar">🗑</button>`;
      li.onclick = (e) => { if (!e.target.classList.contains('del')) selectAmiibo(i, a); };
      list.appendChild(li);
    });
  } catch(e) {
    document.getElementById('amiiboList').innerHTML = '<li id="noamiibo">Erro ao carregar lista</li>';
  }
}

async function selectAmiibo(index, name) {
  try {
    const r = await fetch('/select?index=' + index);
    const data = await r.json();
    if (data.ok) {
      showStatus('selectStatus', '✅ Selecionado: ' + name, 'ok');
      loadAmiiboList();
    } else {
      showStatus('selectStatus', '❌ Erro ao selecionar', 'err');
    }
  } catch(e) {
    showStatus('selectStatus', '❌ ' + e.message, 'err');
  }
}

async function deleteAmiibo(name, event) {
  event.stopPropagation();
  if (!confirm('Deletar ' + name + '?')) return;
  try {
    const r = await fetch('/delete?name=' + encodeURIComponent(name));
    const data = await r.json();
    if (data.ok) {
      showStatus('selectStatus', '🗑 ' + name + ' deletado', 'info');
      loadAmiiboList();
    }
  } catch(e) {}
}

async function loadStatus() {
  try {
    const r = await fetch('/status');
    const data = await r.json();
    const div = document.getElementById('deviceStatus');
    div.className = 'status ' + (data.bin_loaded ? 'ok' : 'info');
    div.innerHTML =
      `📱 Amiibo selecionado: <strong>${data.selected_name || 'Nenhum'}</strong><br>
       💾 Amiibos salvos: <strong>${data.count}</strong><br>
       📡 PN532: <strong>${data.pn532 ? '✅ Conectado' : '❌ Não detectado'}</strong><br>
       🔋 Estado: <strong>${data.state}</strong>`;
  } catch(e) {
    document.getElementById('deviceStatus').textContent = 'Erro ao obter status';
  }
}

function showStatus(id, msg, type) {
  const el = document.getElementById(id);
  el.className = 'status ' + type;
  el.textContent = msg;
  el.style.display = 'block';
  setTimeout(() => { el.style.display = 'none'; }, 5000);
}

// Inicializa
loadAmiiboList();
loadStatus();
setInterval(loadStatus, 5000);
</script>
</body>
</html>
)rawliteral";

// Nomes dos estados para o status JSON
const char* stateNames[] = {"Menu", "IR Aguardando", "IR Enviando", "NFC Aguardando", "NFC Ativo"};

void setupWebServer() {
  // Página principal
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", INDEX_HTML);
  });

  // Upload de arquivo .bin
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      req->send(200, "text/plain", "OK");
      scanAmiiboFiles();
    },
    [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      if (!filename.endsWith(".bin")) {
        req->send(400, "text/plain", "Apenas .bin");
        return;
      }
      String path = "/" + filename;
      if (index == 0) {
        File f = SPIFFS.open(path, "w");
        if (!f) { req->send(500, "text/plain", "Erro ao criar arquivo"); return; }
        f.close();
      }
      File f = SPIFFS.open(path, "a");
      if (f) { f.write(data, len); f.close(); }
      if (final) { scanAmiiboFiles(); }
    }
  );

  // Lista de amiibos
  server.on("/list", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"amiibos\":[";
    for (int i = 0; i < amiiboCount; i++) {
      if (i > 0) json += ",";
      json += "\"" + amiiboList[i].name + "\"";
    }
    json += "],\"selected\":" + String(selectedIndex) + "}";
    req->send(200, "application/json", json);
  });

  // Selecionar amiibo
  server.on("/select", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("index")) { req->send(400); return; }
    int idx = req->getParam("index")->value().toInt();
    if (idx >= 0 && idx < amiiboCount && loadAmiiboBin(idx)) {
      selectedIndex = idx;
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(400, "application/json", "{\"ok\":false}");
    }
    drawScreen();
  });

  // Deletar amiibo
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) { req->send(400); return; }
    String name = req->getParam("name")->value();
    String path = "/" + name + ".bin";
    bool ok = SPIFFS.remove(path);
    if (ok) {
      if (selectedIndex >= 0 && amiiboList[selectedIndex].name == name) {
        selectedIndex = -1; binLoaded = false;
      }
      scanAmiiboFiles();
    }
    req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    drawScreen();
  });

  // Status do dispositivo
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    String selected_name = (selectedIndex >= 0) ? amiiboList[selectedIndex].name : "";
    String json = "{";
    json += "\"selected_name\":\"" + selected_name + "\",";
    json += "\"count\":" + String(amiiboCount) + ",";
    json += "\"bin_loaded\":" + String(binLoaded ? "true" : "false") + ",";
    json += "\"pn532\":" + String(pn532_ok ? "true" : "false") + ",";
    json += "\"state\":\"" + String(stateNames[currentState]) + "\"";
    json += "}";
    req->send(200, "application/json", json);
  });

  server.begin();
}

// ──────────────────────────────────────────────
//  SETUP
// ──────────────────────────────────────────────
void setup() {
  auto cfg = M5.config();
  StickCP2.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(10, 50);
  M5.Display.print("AmiiboBox init...");

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    M5.Display.setCursor(10, 65);
    M5.Display.setTextColor(RED);
    M5.Display.print("SPIFFS falhou!");
    delay(3000);
  }

  // WiFi AP
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  setupWebServer();

  // IrDA
  irda_begin();

  // PN532
  pn532_ok = pn532_init();

  // Carrega lista
  scanAmiiboFiles();

  currentState = STATE_MENU;
  drawScreen();
}

// ──────────────────────────────────────────────
//  LOOP
// ──────────────────────────────────────────────
void loop() {
  M5.update();

  if (currentState == STATE_MENU) {
    // Botão A = modo IR para 3DS
    if (M5.BtnA.wasPressed()) {
      if (!binLoaded) {
        statusMsg = "Selecione um amiibo!";
        drawScreen();
        delay(2000);
        drawScreen();
      } else {
        run_irda_session();
      }
    }
    // Botão B = modo NFC para Switch
    if (M5.BtnB.wasPressed()) {
      if (!binLoaded) {
        statusMsg = "Selecione um amiibo!";
        drawScreen();
        delay(2000);
        drawScreen();
      } else {
        run_nfc_emulation();
      }
    }
  }

  delay(20);
}
