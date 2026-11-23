/*
 * RX - Receptor (ESP32 B, com LCD)   v4 - Selective Repeat
 * Projeto final de TCD (IFMT): enlace RF 433 MHz confiavel entre duas ESP32.
 *
 * Deteccao de erro : CRC-8 (polinomio 0x07) no campo FCS.
 * Controle de erro : Selective Repeat ARQ. O receptor tem janela de 8 quadros:
 *                    guarda os que chegam fora de ordem e entrega em ordem.
 *                    ACK: SEQ = proximo quadro esperado em ordem,
 *                         DATA[0] = mapa de bits dos quadros guardados a frente.
 *
 * Ligacoes (todos os pinos do lado esquerdo da ESP32):
 *   D25 (GPIO25) -> DATA do modulo transmissor RF (FS1000A), VCC 5 V, GND comum
 *   D34 (GPIO34) <- DATA do modulo receptor RF (MX-RM-5V) por divisor 10k/20k (5 V -> 3,3 V)
 *   LCD 16x2 paralelo, modo 4 bits:
 *     RS -> D13   E -> D14   D4 -> D27   D5 -> D26   D6 -> D33   D7 -> D32
 *     VSS, RW, K -> GND   VDD, A -> 5 V   V0 -> meio do trimpot de 10 kOhm (ou GND)
 *
 * Biblioteca do LCD: "LiquidCrystal" (ja vem com a Arduino IDE). Para testar
 * sem LCD, troque USAR_LCD para 0.
 *
 * Monitor Serial a 115200: /ajuda  /status  /resumo  /bench  /reinicia
 * Imagem: quando chega, o RX imprime IMG_INICIO <n>, linhas em hexadecimal e IMG_FIM.
 *         O script receber_imagem.py le essas linhas e salva o arquivo.
 */
#include <Arduino.h>
#include "protocolo_rf.h"

#define USAR_LCD 1
#if USAR_LCD
#include <LiquidCrystal.h>
//               RS  E   D4  D5  D6  D7
LiquidCrystal lcd(13, 14, 27, 26, 33, 32);
#endif

// ----------------------------- Configuracao -----------------------------
constexpr int PINO_RF_RX = 34;   // GPIO34: so entrada, recebe o DATA do RX pelo divisor
constexpr int PINO_RF_TX = 25;   // GPIO25: vai para o DATA do FS1000A
constexpr size_t MAX_MENSAGEM = 8192;
constexpr char CONTEUDO_TEXTO = 'T';
constexpr char CONTEUDO_IMAGEM = 'I';

// ----------------------------- Estado do receptor Selective Repeat -----------------------------
constexpr uint8_t JANELA_RX = rf::JANELA_MAX;   // 8 quadros (<= 2^(m-1) = 128, m = 8 bits)
uint8_t  esperado = 0;           // SEQ do proximo quadro esperado EM ORDEM (base da janela)
bool     guardado[JANELA_RX];    // guardado[i] = quadro (esperado + i) ja recebido
uint8_t  lenGuardado[JANELA_RX];
uint8_t  dadosGuardados[JANELA_RX][rf::MAX_DADOS];
uint32_t quadrosNaMensagem = 0;  // DATA validos da mensagem atual (entregues ou guardados)
bool     temAck = false;         // ha algo valido para confirmar?
bool     ackPendente = false;    // chegou quadro valido desde o ultimo ACK
int16_t  ultimoEnd = -1;         // SEQ do END ja entregue (para reconfirmar se repetir)
uint32_t ultimaAtividadeMs = 0;  // ultimo instante em que o canal estava ocupado
uint32_t ultimoQuadroValidoMs = 0; // ultimo DATA/END com FCS correto

uint8_t  mensagem[MAX_MENSAGEM];
size_t   tamanhoMensagem = 0;
bool     estouro = false;
uint32_t inicioMensagemMs = 0, numeroMensagem = 0;

// Estatisticas
uint32_t nAceitos = 0, nGuardadosForaDeOrdem = 0, nDuplicados = 0, nForaDaJanela = 0, nErrosFcs = 0, nTruncados = 0, nAcksEnviados = 0;

rf::Receptor receptor;
char linhaSerial[64];
size_t tamanhoLinha = 0;

// ----------------------------- Utilitarios -----------------------------
void registra(const char *formato, ...) {
  char buf[256];
  va_list args;
  va_start(args, formato);
  vsnprintf(buf, sizeof(buf), formato, args);
  va_end(args);
  Serial.printf("[%8lu ms] %s\n", (unsigned long)millis(), buf);
}

void transmiteLinha(const uint8_t *bytes, size_t n) {
  Serial2.write(bytes, n);
  Serial2.flush();
  delay(uint32_t(2 * rf::MS_POR_BYTE) + 1);   // descarta o eco do proprio ACK
  while (Serial2.available()) Serial2.read();
  receptor.reinicia();
}

// ----------------------------- LCD -----------------------------
// Mostra o texto em paginas de 32 caracteres (2 linhas x 16), trocando a cada 2,5 s.
char textoLcd[MAX_MENSAGEM];
size_t tamanhoTextoLcd = 0, paginaLcd = 0;
uint32_t trocaPaginaMs = 0;

void desenhaPaginaLcd() {
#if USAR_LCD
  lcd.clear();
  for (uint8_t linha = 0; linha < 2; ++linha) {
    lcd.setCursor(0, linha);
    for (uint8_t col = 0; col < 16; ++col) {
      const size_t i = paginaLcd * 32 + linha * 16 + col;
      if (i < tamanhoTextoLcd) lcd.print(textoLcd[i]);
    }
  }
#endif
}

void mostraNoLcd(const uint8_t *texto, size_t n) {
  // O LCD so tem ASCII basico: troca o resto por '?'
  tamanhoTextoLcd = 0;
  for (size_t i = 0; i < n && i < MAX_MENSAGEM; ++i)
    textoLcd[tamanhoTextoLcd++] = (texto[i] >= 32 && texto[i] < 127) ? char(texto[i]) : '?';
  paginaLcd = 0;
  trocaPaginaMs = millis();
  desenhaPaginaLcd();
}

void atualizaLcd() {
  if (tamanhoTextoLcd <= 32 || millis() - trocaPaginaMs < 2500) return;
  trocaPaginaMs = millis();
  paginaLcd = (paginaLcd + 1) % ((tamanhoTextoLcd + 31) / 32);
  desenhaPaginaLcd();
}

// ----------------------------- Entrega da mensagem -----------------------------
void entregaMensagem() {
  ++numeroMensagem;
  const uint32_t duracao = millis() - inicioMensagemMs;
  if (estouro) registra("AVISO: mensagem maior que %u bytes, final descartado", (unsigned)MAX_MENSAGEM);

  if (tamanhoMensagem > 0 && mensagem[0] == CONTEUDO_TEXTO) {
    registra("=== MENSAGEM #%lu COMPLETA: %u caracteres em %lu ms ===", (unsigned long)numeroMensagem,
             unsigned(tamanhoMensagem - 1), (unsigned long)duracao);
    Serial.print("TEXTO: ");
    Serial.write(mensagem + 1, tamanhoMensagem - 1);
    Serial.println();
    mostraNoLcd(mensagem + 1, tamanhoMensagem - 1);
  } else if (tamanhoMensagem > 0 && mensagem[0] == CONTEUDO_IMAGEM) {
    const size_t n = tamanhoMensagem - 1;
    registra("=== MENSAGEM #%lu COMPLETA: IMAGEM de %u bytes em %lu ms ===", (unsigned long)numeroMensagem,
             unsigned(n), (unsigned long)duracao);
    // Envia a imagem ao PC em hexadecimal (texto puro, 32 bytes por linha).
    Serial.printf("IMG_INICIO %u\n", unsigned(n));
    for (size_t i = 0; i < n; ++i) {
      Serial.printf("%02X", mensagem[1 + i]);
      if (i % 32 == 31 || i == n - 1) Serial.print('\n');
    }
    Serial.println("IMG_FIM");
    char resumo[40];
    snprintf(resumo, sizeof(resumo), "IMAGEM RECEBIDA %u bytes", unsigned(n));
    mostraNoLcd(reinterpret_cast<const uint8_t *>(resumo), strlen(resumo));
  } else {
    registra("=== MENSAGEM #%lu COMPLETA: %u bytes de conteudo '%c' (ainda nao tratado) ===",
             (unsigned long)numeroMensagem, unsigned(tamanhoMensagem), tamanhoMensagem ? char(mensagem[0]) : '-');
  }
  registra("estatisticas: aceitos=%lu guardados_fora_de_ordem=%lu duplicados=%lu erros_FCS=%lu truncados=%lu ACKs=%lu",
           (unsigned long)nAceitos, (unsigned long)nGuardadosForaDeOrdem, (unsigned long)nDuplicados,
           (unsigned long)nErrosFcs, (unsigned long)nTruncados, (unsigned long)nAcksEnviados);
}

// ----------------------------- Regras do receptor Selective Repeat -----------------------------
void limpaJanela() {
  for (uint8_t i = 0; i < JANELA_RX; ++i) guardado[i] = false;
}

// Entrega em ordem tudo o que ja esta guardado a partir de 'esperado'.
void entregaEmOrdem() {
  while (guardado[0]) {
    const size_t cabe = MAX_MENSAGEM - tamanhoMensagem;
    const size_t n = lenGuardado[0] < cabe ? lenGuardado[0] : cabe;
    memcpy(mensagem + tamanhoMensagem, dadosGuardados[0], n);
    tamanhoMensagem += n;
    if (n < lenGuardado[0]) estouro = true;
    registra("   entregue em ordem: seq=%u (%u bytes)", esperado, lenGuardado[0]);
    // desliza a janela uma posicao
    for (uint8_t i = 0; i + 1 < JANELA_RX; ++i) {
      guardado[i] = guardado[i + 1];
      lenGuardado[i] = lenGuardado[i + 1];
      memcpy(dadosGuardados[i], dadosGuardados[i + 1], rf::MAX_DADOS);
    }
    guardado[JANELA_RX - 1] = false;
    ++esperado;   // uint8_t: 255 -> 0 automaticamente
  }
}

void trataQuadro(const rf::Quadro &q) {
  if (q.tipo == rf::DATA || q.tipo == rf::END) {
    ultimoQuadroValidoMs = millis();
    // Recalcula o FCS so para medir o tempo da verificacao (o Receptor ja conferiu).
    const uint32_t t0 = micros();
    const uint8_t calc = rf::calculaFcs(q);
    const uint32_t verifUs = micros() - t0;
    char hex[3 * (4 + rf::MAX_DADOS) + 1], txt[rf::MAX_DADOS + 1];
    rf::quadroHex(q, hex, sizeof(hex));
    rf::dadosTexto(q, txt, sizeof(txt));
    registra("RX %s seq=%u len=%u FCS recebido=0x%02X calculado=0x%02X verif_us=%lu quadro=[%s]%s%s%s",
             rf::nomeTipo(q.tipo), q.seq, q.len, q.fcs, calc, (unsigned long)verifUs, hex,
             q.len ? " dados=\"" : "", txt, q.len ? "\"" : "");
  }

  if (q.tipo == rf::DATA && q.len > 0) {
    ackPendente = true;   // todo DATA valido gera um ACK depois
    const uint8_t deslocamento = uint8_t(q.seq - esperado);
    if (deslocamento < JANELA_RX) {
      if (quadrosNaMensagem == 0) {   // primeiro quadro de uma mensagem nova
        inicioMensagemMs = millis(); tamanhoMensagem = 0; estouro = false; ultimoEnd = -1;
      }
      if (guardado[deslocamento]) {
        ++nDuplicados;
        registra("RX DATA seq=%u duplicado (ja guardado) -> descartado", q.seq);
        return;
      }
      guardado[deslocamento] = true;
      lenGuardado[deslocamento] = q.len;
      memcpy(dadosGuardados[deslocamento], q.dados, q.len);
      ++quadrosNaMensagem; ++nAceitos;
      temAck = true;
      if (deslocamento == 0) {
        registra("RX DATA seq=%u len=%u FCS ok -> ACEITO (em ordem)", q.seq, q.len);
      } else {
        ++nGuardadosForaDeOrdem;
        registra("RX DATA seq=%u len=%u FCS ok -> GUARDADO fora de ordem (esperado=%u)", q.seq, q.len, esperado);
      }
      entregaEmOrdem();
    } else if (deslocamento >= 128) {
      // Atras da janela: ja foi entregue. O ACK dele se perdeu; o novo ACK confirma de novo.
      ++nDuplicados;
      registra("RX DATA seq=%u ja entregue antes (esperado=%u) -> descartado, ACK sera repetido", q.seq, esperado);
    } else {
      ++nForaDaJanela;
      registra("RX DATA seq=%u fora da janela (esperado=%u) -> DESCARTADO", q.seq, esperado);
    }
    return;
  }

  if (q.tipo == rf::END) {
    ackPendente = true;
    bool janelaVazia = true;
    for (uint8_t i = 0; i < JANELA_RX; ++i) if (guardado[i]) janelaVazia = false;
    if (q.seq == esperado && quadrosNaMensagem > 0 && janelaVazia) {
      ++nAceitos;
      registra("RX END seq=%u FCS ok -> fim da mensagem", q.seq);
      entregaMensagem();
      ultimoEnd = q.seq; temAck = true;
      esperado = 0; quadrosNaMensagem = 0; limpaJanela();   // proxima mensagem comeca em SEQ 0
    } else if (quadrosNaMensagem == 0 && q.seq == ultimoEnd) {
      temAck = true;   // o TX nao recebeu nosso ACK do END e repetiu: so confirma de novo
      registra("RX END seq=%u repetido (mensagem ja entregue) -> confirma de novo", q.seq);
    } else {
      ++nForaDaJanela;
      ackPendente = false;
      registra("RX END seq=%u inesperado (esperado=%u) -> DESCARTADO", q.seq, esperado);
    }
    return;
  }
  // ACK (eco do proprio transmissor) ou DATA vazio: nao responde.
}

// O ACK sai so quando o canal fica em silencio: o TX envia uma rajada e depois
// para para ouvir. Assim os dois radios (mesma frequencia) nao transmitem juntos.
void enviaAckSeNecessario() {
  if (!ackPendente || receptor.recebendo()) return;
  if (millis() - ultimaAtividadeMs < rf::ESPERA_ANTES_ACK_MS) return;
  ackPendente = false;
  if (!temAck) return;

  rf::Quadro ack;
  ack.tipo = rf::ACK;
  ack.len = rf::LEN_ACK;
  if (quadrosNaMensagem == 0 && ultimoEnd >= 0) {
    ack.seq = uint8_t(ultimoEnd + 1);   // confirma o END: proximo esperado = END + 1
    ack.dados[0] = 0;
  } else {
    ack.seq = esperado;                 // proximo esperado em ordem
    uint8_t mapa = 0;
    for (uint8_t i = 0; i < JANELA_RX; ++i) if (guardado[i]) mapa |= uint8_t(1 << i);
    ack.dados[0] = mapa;
  }
  ack.fcs = rf::calculaFcs(ack);
  uint8_t linha[rf::MAX_LINHA];
  const size_t n = rf::montaLinha(ack, linha);
  char hex[3 * (4 + rf::MAX_DADOS) + 1];
  rf::quadroHex(ack, hex, sizeof(hex));
  registra("TX ACK prox=%u mapa=0x%02X FCS=0x%02X quadro=[%s] linha=%uB/%lums", ack.seq, ack.dados[0], ack.fcs, hex,
           unsigned(n), (unsigned long)(n * rf::MS_POR_BYTE));
  transmiteLinha(linha, n);
  ++nAcksEnviados;
  ultimaAtividadeMs = millis();
}

void processaRadio() {
  while (Serial2.available()) {
    const uint8_t b = uint8_t(Serial2.read());
    const uint32_t agora = millis();
    const rf::Receptor::Resultado r = receptor.recebe(b, agora);
    if (receptor.recebendo() || r != rf::Receptor::NADA) ultimaAtividadeMs = agora;

    switch (r) {
      case rf::Receptor::QUADRO_OK:
        trataQuadro(receptor.quadro);
        break;
      case rf::Receptor::ERRO_FCS:
        ++nErrosFcs;
      {
        char hex[3 * (4 + rf::MAX_DADOS) + 1];
        rf::quadroHex(receptor.quadro, hex, sizeof(hex));
        registra("RX quadro %s seq=%u: FCS recebido=0x%02X calculado=0x%02X quadro=[%s] -> ERRO DETECTADO, descartado",
                 rf::nomeTipo(receptor.quadro.tipo), receptor.quadro.seq, receptor.quadro.fcs, receptor.fcsCalculado, hex);
      }
        break;
      case rf::Receptor::QUADRO_TRUNCADO:
        ++nTruncados;
        registra("RX quadro truncado/corrompido na linha (seq lido=%u) -> descartado", receptor.quadro.seq);
        break;
      default: break;
    }
  }
  if (receptor.verificaTimeout(millis()) == rf::Receptor::QUADRO_TRUNCADO) {
    ++nTruncados;
    registra("RX quadro interrompido (timeout entre bytes) -> descartado");
  }
}

// Se o TX desistiu no meio de uma mensagem, ele recomeca do SEQ 0 na proxima.
// Por isso, depois de DESCARTE_RX_MS sem quadro valido, o RX joga fora o que
// tinha da mensagem incompleta e volta a esperar SEQ 0.
void descartaMensagemParada() {
  if (quadrosNaMensagem == 0 && ultimoEnd < 0 && !temAck) return;
  if (millis() - ultimoQuadroValidoMs < rf::DESCARTE_RX_MS) return;
  if (quadrosNaMensagem > 0)
    registra("%lu s sem quadros: mensagem incompleta (%u bytes) DESCARTADA, esperando SEQ 0",
             (unsigned long)(rf::DESCARTE_RX_MS / 1000), unsigned(tamanhoMensagem));
  esperado = 0; quadrosNaMensagem = 0; tamanhoMensagem = 0; limpaJanela();
  temAck = false; ackPendente = false; ultimoEnd = -1;
}

// ----------------------------- Comandos pelo USB -----------------------------
void reiniciaEstado() {
  esperado = 0; quadrosNaMensagem = 0; limpaJanela(); temAck = false; ackPendente = false; ultimoEnd = -1;
  tamanhoMensagem = 0; estouro = false;
  nAceitos = nGuardadosForaDeOrdem = nDuplicados = nForaDaJanela = nErrosFcs = nTruncados = nAcksEnviados = 0;
  receptor.reinicia();
}

void bench() {
  rf::Quadro q; q.tipo = rf::DATA; q.seq = 7; q.len = rf::MAX_DADOS;
  for (uint8_t i = 0; i < rf::MAX_DADOS; ++i) q.dados[i] = uint8_t('A' + i);
  volatile uint8_t acumulado = 0;
  const uint32_t repeticoes = 10000, t0 = micros();
  for (uint32_t i = 0; i < repeticoes; ++i) { q.seq = uint8_t(i); acumulado ^= rf::calculaFcs(q); }
  const uint32_t total = micros() - t0;
  registra("# BENCH,RX,CRC-8,bytes_por_calculo=19,repeticoes=%lu,tempo_total_us=%lu,us_por_verificacao=%.3f",
           (unsigned long)repeticoes, (unsigned long)total, double(total) / repeticoes);
  (void)acumulado;
}

void executaComando(const char *linha) {
  if (!strcmp(linha, "/ajuda")) {
    Serial.println(F("  /status    estado e contadores"));
    Serial.println(F("  /resumo    totais desde que a placa ligou"));
    Serial.println(F("  /bench     mede o tempo da verificacao do CRC-8 (10000 calculos)"));
    Serial.println(F("  /reinicia  volta a esperar SEQ 0 (use se o TX for reiniciado no meio de uma mensagem)"));
  } else if (!strcmp(linha, "/status")) {
    registra("STATUS: esperado=%u quadros_msg=%lu bytes=%u | aceitos=%lu guardados_fora_de_ordem=%lu duplicados=%lu "
             "fora_da_janela=%lu erros_FCS=%lu truncados=%lu ACKs=%lu ruido_descartado=%lu",
             esperado, (unsigned long)quadrosNaMensagem, unsigned(tamanhoMensagem), (unsigned long)nAceitos,
             (unsigned long)nGuardadosForaDeOrdem, (unsigned long)nDuplicados, (unsigned long)nForaDaJanela,
             (unsigned long)nErrosFcs, (unsigned long)nTruncados, (unsigned long)nAcksEnviados,
             (unsigned long)receptor.descartesRuido);
  } else if (!strcmp(linha, "/resumo")) {
    registra("# RESUMO,RX,mensagens_entregues=%lu,quadros_aceitos=%lu,guardados_fora_de_ordem=%lu,duplicados=%lu,"
             "fora_da_janela=%lu,erros_FCS_detectados=%lu,truncados_na_linha=%lu,ACKs_enviados=%lu,ruido_descartado=%lu",
             (unsigned long)numeroMensagem, (unsigned long)nAceitos, (unsigned long)nGuardadosForaDeOrdem,
             (unsigned long)nDuplicados, (unsigned long)nForaDaJanela, (unsigned long)nErrosFcs,
             (unsigned long)nTruncados, (unsigned long)nAcksEnviados, (unsigned long)receptor.descartesRuido);
  } else if (!strcmp(linha, "/bench")) {
    bench();
  } else if (!strcmp(linha, "/reinicia")) {
    reiniciaEstado();
    registra("estado do receptor reiniciado: esperando SEQ 0");
  } else registra("comando desconhecido; use /ajuda");
}

void processaUsb() {
  while (Serial.available()) {
    const char c = char(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (tamanhoLinha + 1 < sizeof(linhaSerial)) linhaSerial[tamanhoLinha++] = c;
      continue;
    }
    linhaSerial[tamanhoLinha] = 0;
    if (tamanhoLinha) executaComando(linhaSerial);
    tamanhoLinha = 0;
  }
}

// ----------------------------- Arduino -----------------------------
void setup() {
  Serial.begin(115200);
  Serial2.setRxBufferSize(1024);
  Serial2.begin(rf::BAUD, SERIAL_8N1, PINO_RF_RX, PINO_RF_TX, true, 20000UL, 1);
  Serial2.setRxTimeout(1);
#if USAR_LCD
  lcd.begin(16, 2);
  lcd.setCursor(0, 0); lcd.print("RX RF 433 MHz");
  lcd.setCursor(0, 1); lcd.print("Aguardando...");
#endif
  delay(200);
  registra("RX pronto (v4.1) | RF %lu bit/s | CRC-8 (0x07) | Selective Repeat janela=8 | ACK apos %lu ms de silencio",
           (unsigned long)rf::BAUD, (unsigned long)rf::ESPERA_ANTES_ACK_MS);
}

void loop() {
  processaUsb();
  processaRadio();
  enviaAckSeNecessario();
  descartaMensagemParada();
  atualizaLcd();
}
