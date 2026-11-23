/*
 * TX - Transmissor (ESP32 A)
 * Projeto final de TCD (IFMT): enlace RF 433 MHz confiavel entre duas ESP32.
 *
 * Deteccao de erro : CRC-8 (polinomio 0x07) no campo FCS.
 * Controle de erro : Go-Back-N ARQ, janela padrao 4 (JANELA 1 = Stop-and-Wait).
 *
 * Ligacoes (iguais nas duas placas; todos os pinos do lado esquerdo da ESP32):
 *   D25 (GPIO25) -> DATA do modulo transmissor RF (FS1000A), VCC 5 V, GND comum
 *   D34 (GPIO34) <- DATA do modulo receptor RF (MX-RM-5V) por divisor 10k/20k (5 V -> 3,3 V)
 *   Antena de ~17,3 cm (1/4 de onda em 433 MHz) nos dois modulos.
 *
 * Uso (Monitor Serial a 115200, final de linha "Nova linha"):
 *   Digite um texto e tecle Enter: ele e enviado ao RX.
 *   /ajuda  /status  /janela N  /erro P  /perda P  /teste N  /cancela
 *   /imagem N  -> recebe N bytes binarios pelo USB e envia como imagem
 *                 (usado pelo script enviar_imagem.py; feche o Monitor Serial antes)
 */
#include <Arduino.h>
#include "protocolo_rf.h"

// ----------------------------- Configuracao -----------------------------
constexpr int PINO_RF_RX = 34;   // GPIO34: so entrada, recebe o DATA do RX pelo divisor
constexpr int PINO_RF_TX = 25;   // GPIO25: vai para o DATA do FS1000A
constexpr uint8_t JANELA_MAXIMA = 8;        // GBN aceita ate 255 com SEQ de 8 bits
constexpr size_t MAX_MENSAGEM = 4096;       // bytes por mensagem
constexpr char CONTEUDO_TEXTO = 'T';        // 1o byte da mensagem: tipo de conteudo
constexpr char CONTEUDO_IMAGEM = 'I';

// ----------------------------- Estado do Go-Back-N -----------------------------
// Os quadros da mensagem sao numerados 0, 1, 2, ... (indice absoluto).
// No ar vai so SEQ = indice % 256. Os indices facilitam a conta da janela.
uint8_t  mensagem[MAX_MENSAGEM];
size_t   tamanhoMensagem = 0;
uint32_t quadrosDados = 0;      // quantidade de quadros DATA
uint32_t totalQuadros = 0;      // quadros DATA + 1 quadro END
uint32_t base = 0;              // quadro mais antigo ainda sem ACK
uint32_t proximo = 0;           // proximo quadro a transmitir
uint32_t maiorEnviado = 0;      // para saber se um envio e retransmissao
bool     ativo = false;
uint32_t ultimoEnvioMs = 0;     // o temporizador conta a partir do ultimo envio
uint8_t  janela = 4;
uint32_t ultimoProgressoMs = 0; // ultimo ACK que avancou a janela
uint32_t bloqueadoAteMs = 0;    // apos uma desistencia, espera o RX descartar

// Estatisticas da mensagem atual
uint32_t inicioMs = 0, nEnvios = 0, nRetransmissoes = 0, nTimeouts = 0;
uint32_t nAcks = 0, nAcksIgnorados = 0, nErrosSimulados = 0, nPerdasSimuladas = 0;

// Simulacao de erros (para a demonstracao)
uint8_t erroPercentual = 0;     // % de quadros com 1 bit invertido apos o calculo do FCS
uint8_t perdaPercentual = 0;    // % de quadros que nao sao transmitidos

// Recepcao da imagem pelo USB (comando /imagem N)
uint8_t  bufImagem[MAX_MENSAGEM];
bool     recebendoImagem = false;
size_t   imagemEsperada = 0, imagemRecebida = 0;
uint32_t imagemUltimoByteMs = 0;
constexpr uint32_t TIMEOUT_IMAGEM_USB_MS = 3000;

rf::Receptor receptor;
char linhaSerial[600];
size_t tamanhoLinha = 0;
bool linhaLonga = false;

// ----------------------------- Utilitarios -----------------------------
void registra(const char *formato, ...) {
  char buf[256];
  va_list args;
  va_start(args, formato);
  vsnprintf(buf, sizeof(buf), formato, args);
  va_end(args);
  Serial.printf("[%8lu ms] %s\n", (unsigned long)millis(), buf);
}

// Envia os bytes pelo radio e descarta o eco: o receptor RF da propria placa
// tambem escuta o que o transmissor da placa envia.
void transmiteLinha(const uint8_t *bytes, size_t n) {
  Serial2.write(bytes, n);
  Serial2.flush();                                   // espera o ultimo bit sair
  delay(uint32_t(2 * rf::MS_POR_BYTE) + 1);          // bytes de eco ainda chegando
  while (Serial2.available()) Serial2.read();
  receptor.reinicia();
}

// ----------------------------- Envio de quadros -----------------------------
void enviaQuadro(uint32_t indice) {
  rf::Quadro q;
  q.seq = uint8_t(indice & 0xFF);
  if (indice < quadrosDados) {
    const size_t inicio = size_t(indice) * rf::MAX_DADOS;
    const size_t resto = tamanhoMensagem - inicio;
    q.tipo = rf::DATA;
    q.len = uint8_t(resto < rf::MAX_DADOS ? resto : rf::MAX_DADOS);
    memcpy(q.dados, mensagem + inicio, q.len);
  } else {
    q.tipo = rf::END;
    q.len = 0;
  }
  q.fcs = rf::calculaFcs(q);

  const bool retransmissao = indice < maiorEnviado;
  if (!retransmissao) maiorEnviado = indice + 1;
  ++nEnvios;
  if (retransmissao) ++nRetransmissoes;

  registra("TX %s%s seq=%u len=%u FCS=0x%02X  janela=[%lu..%lu]",
      rf::nomeTipo(q.tipo), retransmissao ? " (retransmissao)" : "", q.seq, q.len, q.fcs,
      (unsigned long)base, (unsigned long)(base + janela - 1));

  // --- Simulacao de erros (feita DEPOIS do calculo do FCS) ---
  if (perdaPercentual && uint8_t(random(100)) < perdaPercentual) {
    ++nPerdasSimuladas;
    registra("   [SIM] quadro seq=%u NAO transmitido (perda simulada)", q.seq);
    ultimoEnvioMs = millis();
    return;
  }
  if (erroPercentual && uint8_t(random(100)) < erroPercentual) {
    // Escolhe um byte entre TYPE, SEQ, DATA e FCS (LEN fica intacto para
    // manter o tamanho) e inverte um bit. O RX deve detectar pelo CRC.
    const uint8_t campos = 3 + q.len;              // TYPE, SEQ, dados..., FCS
    const uint8_t alvo = uint8_t(random(campos));
    const uint8_t mascara = uint8_t(1 << random(8));
    const char *nome;
    if (alvo == 0)      { q.tipo ^= mascara; nome = "TYPE"; }
    else if (alvo == 1) { q.seq  ^= mascara; nome = "SEQ"; }
    else if (alvo == campos - 1) { q.fcs ^= mascara; nome = "FCS"; }
    else { q.dados[alvo - 2] ^= mascara; nome = "DATA"; }
    ++nErrosSimulados;
    registra("   [SIM] bit invertido no campo %s (mascara 0x%02X)", nome, mascara);
  }

  uint8_t linha[rf::MAX_LINHA];
  const size_t n = rf::montaLinha(q, linha);
  transmiteLinha(linha, n);
  ultimoEnvioMs = millis();   // (re)inicia o temporizador
}

void iniciaMensagem(const uint8_t *conteudo, size_t tamanho, char tipoConteudo = CONTEUDO_TEXTO) {
  if (ativo) { registra("ERRO: ainda enviando a mensagem anterior (use /cancela)"); return; }
  if (int32_t(bloqueadoAteMs - millis()) > 0) {
    registra("ERRO: aguarde %lu s (o RX ainda pode ter parte da mensagem anterior)",
             (unsigned long)((bloqueadoAteMs - millis()) / 1000 + 1));
    return;
  }
  if (tamanho == 0 || tamanho + 1 > MAX_MENSAGEM) { registra("ERRO: mensagem vazia ou grande demais"); return; }
  mensagem[0] = tipoConteudo;
  memcpy(mensagem + 1, conteudo, tamanho);
  tamanhoMensagem = tamanho + 1;
  quadrosDados = (tamanhoMensagem + rf::MAX_DADOS - 1) / rf::MAX_DADOS;
  totalQuadros = quadrosDados + 1;
  base = proximo = maiorEnviado = 0;
  ultimoProgressoMs = millis();
  nEnvios = nRetransmissoes = nTimeouts = nAcks = nAcksIgnorados = 0;
  nErrosSimulados = nPerdasSimuladas = 0;
  inicioMs = millis();
  ativo = true;
  registra("=== NOVA MENSAGEM (%s): %u bytes -> %lu quadros DATA + 1 END | janela=%u timeout=%lu ms ===",
      tipoConteudo == CONTEUDO_IMAGEM ? "imagem" : "texto", (unsigned)tamanhoMensagem, (unsigned long)quadrosDados, janela, (unsigned long)rf::TIMEOUT_ACK_MS);
}

void finalizaMensagem(bool sucesso) {
  ativo = false;
  if (!sucesso) bloqueadoAteMs = millis() + rf::DESCARTE_RX_MS;
  const uint32_t duracao = millis() - inicioMs;
  registra("=== %s em %lu ms | envios=%lu retransmissoes=%lu timeouts=%lu ACKs=%lu (ignorados=%lu) "
      "erros_sim=%lu perdas_sim=%lu | %.1f bytes/s ===",
      sucesso ? "MENSAGEM ENTREGUE" : "FALHA: MENSAGEM ABANDONADA", (unsigned long)duracao,
      (unsigned long)nEnvios, (unsigned long)nRetransmissoes, (unsigned long)nTimeouts,
      (unsigned long)nAcks, (unsigned long)nAcksIgnorados, (unsigned long)nErrosSimulados,
      (unsigned long)nPerdasSimuladas, duracao ? 1000.0 * tamanhoMensagem / duracao : 0.0);
}

// ----------------------------- Recepcao de ACKs -----------------------------
// ACK cumulativo: "ACK n" confirma o quadro de SEQ n e todos os anteriores.
void trataAck(uint8_t seq) {
  ++nAcks;
  if (!ativo) { registra("RX ACK seq=%u (nenhuma mensagem ativa, ignorado)", seq); return; }
  // Procura ate o maior quadro ja enviado: depois de um timeout 'proximo' volta
  // para a base, mas um ACK atrasado ainda pode confirmar quadros ja enviados.
  for (uint32_t i = base; i < maiorEnviado; ++i) {
    if (uint8_t(i & 0xFF) == seq) {
      base = i + 1;                 // desliza a janela
      if (proximo < base) proximo = base;
      ultimoProgressoMs = millis();
      ultimoEnvioMs = millis();     // reinicia o temporizador para os que faltam
      registra("RX ACK seq=%u -> confirmados ate o quadro %lu, base=%lu", seq,
          (unsigned long)i, (unsigned long)base);
      return;
    }
  }
  ++nAcksIgnorados;
  registra("RX ACK seq=%u nao confirma nada novo (base=%lu) -> ignorado", seq, (unsigned long)base);
}

void processaRadio() {
  while (Serial2.available()) {
    const uint8_t b = uint8_t(Serial2.read());
    switch (receptor.recebe(b, millis())) {
      case rf::Receptor::QUADRO_OK:
        if (receptor.quadro.tipo == rf::ACK) trataAck(receptor.quadro.seq);
        else registra("RX %s seq=%u inesperado no TX, ignorado", rf::nomeTipo(receptor.quadro.tipo), receptor.quadro.seq);
        break;
      case rf::Receptor::ERRO_FCS:
        registra("RX quadro com FCS invalido (recebido 0x%02X, calculado 0x%02X) -> descartado",
            receptor.quadro.fcs, receptor.fcsCalculado);
        break;
      case rf::Receptor::QUADRO_TRUNCADO:
        registra("RX quadro truncado/corrompido na linha -> descartado");
        break;
      default: break;
    }
  }
  receptor.verificaTimeout(millis());
}

// ----------------------------- Maquina do Go-Back-N -----------------------------
void executaGoBackN() {
  if (!ativo) return;

  if (base == totalQuadros) { finalizaMensagem(true); return; }

  // Pode transmitir se: ha quadro novo, ele cabe na janela e, no caso do END,
  // todos os DATA ja foram confirmados (o END so sai sozinho; ver relatorio).
  const bool cabeNaJanela = proximo < base + janela;
  const bool ehEnd = proximo == quadrosDados;
  if (proximo < totalQuadros && cabeNaJanela && (!ehEnd || base == quadrosDados)) {
    enviaQuadro(proximo);
    ++proximo;
    return;
  }

  // Esperando ACK. Se o tempo estourar, volta N: retransmite a partir da base.
  if (base < proximo && millis() - ultimoEnvioMs >= rf::TIMEOUT_ACK_MS) {
    ++nTimeouts;
    if (millis() - ultimoProgressoMs > rf::DESISTENCIA_TX_MS) {
      registra("%lu s sem nenhum ACK novo: enlace caiu", (unsigned long)(rf::DESISTENCIA_TX_MS / 1000));
      finalizaMensagem(false);
      return;
    }
    registra("TIMEOUT: sem ACK do quadro %lu (seq=%u) -> Go-Back-N: retransmite %lu..%lu",
        (unsigned long)base, uint8_t(base & 0xFF), (unsigned long)base, (unsigned long)(proximo - 1));
    proximo = base;
  }
}

// ----------------------------- Comandos pelo USB -----------------------------
void ajuda() {
  Serial.println(F("Digite um texto e tecle Enter para enviar. Comandos:"));
  Serial.println(F("  /status      estado atual e configuracao"));
  Serial.println(F("  /janela N    tamanho da janela (1..8). 1 = Stop-and-Wait"));
  Serial.println(F("  /erro P      inverte 1 bit em P% dos quadros (0 desliga)"));
  Serial.println(F("  /perda P     deixa de transmitir P% dos quadros (0 desliga)"));
  Serial.println(F("  /teste N     envia uma mensagem de teste com N caracteres"));
  Serial.println(F("  /cancela     abandona a mensagem atual"));
  Serial.println(F("  /imagem N    recebe N bytes pelo USB e envia como imagem (use o enviar_imagem.py)"));
}

void status() {
  registra("STATUS: ativo=%u base=%lu proximo=%lu total=%lu | janela=%u erro=%u%% perda=%u%% | "
      "baud=%lu timeout=%lu ms | ruido_descartado=%lu",
      ativo, (unsigned long)base, (unsigned long)proximo, (unsigned long)totalQuadros, janela,
      erroPercentual, perdaPercentual, (unsigned long)rf::BAUD, (unsigned long)rf::TIMEOUT_ACK_MS,
      (unsigned long)receptor.descartesRuido);
}

void executaComando(char *linha) {
  char *arg = strchr(linha, ' ');
  long valor = -1;
  if (arg) { *arg++ = 0; valor = strtol(arg, nullptr, 10); }

  if (!strcmp(linha, "/ajuda")) ajuda();
  else if (!strcmp(linha, "/status")) status();
  else if (!strcmp(linha, "/janela")) {
    if (ativo) registra("ERRO: espere a mensagem terminar para mudar a janela");
    else if (valor < 1 || valor > JANELA_MAXIMA) registra("ERRO: use /janela 1..%u", JANELA_MAXIMA);
    else { janela = uint8_t(valor); registra("janela = %u%s", janela, janela == 1 ? " (Stop-and-Wait)" : ""); }
  } else if (!strcmp(linha, "/erro")) {
    if (valor < 0 || valor > 100) registra("ERRO: use /erro 0..100");
    else { erroPercentual = uint8_t(valor); registra("erro simulado = %u%%", erroPercentual); }
  } else if (!strcmp(linha, "/perda")) {
    if (valor < 0 || valor > 100) registra("ERRO: use /perda 0..100");
    else { perdaPercentual = uint8_t(valor); registra("perda simulada = %u%%", perdaPercentual); }
  } else if (!strcmp(linha, "/teste")) {
    if (valor < 1 || valor > long(MAX_MENSAGEM - 1)) { registra("ERRO: use /teste 1..%u", unsigned(MAX_MENSAGEM - 1)); return; }
    static uint8_t teste[MAX_MENSAGEM];
    for (long i = 0; i < valor; ++i) teste[i] = uint8_t('A' + (i % 26));
    iniciaMensagem(teste, size_t(valor));
  } else if (!strcmp(linha, "/imagem")) {
    if (ativo) { registra("ERRO: ainda enviando a mensagem anterior"); return; }
    if (valor < 1 || valor > long(MAX_MENSAGEM - 1)) {
      registra("ERRO: imagem precisa ter de 1 a %u bytes", unsigned(MAX_MENSAGEM - 1)); return;
    }
    recebendoImagem = true;
    imagemEsperada = size_t(valor);
    imagemRecebida = 0;
    imagemUltimoByteMs = millis();
    Serial.printf("PRONTO_IMAGEM %u\n", unsigned(imagemEsperada));   // o script espera esta linha
  } else if (!strcmp(linha, "/cancela")) {
    if (ativo) finalizaMensagem(false);   // tambem bloqueia por DESCARTE_RX_MS
  } else registra("comando desconhecido; use /ajuda");
}

// Modo binario: depois de /imagem N, os proximos N bytes do USB sao a imagem.
void recebeImagemUsb() {
  while (Serial.available() && imagemRecebida < imagemEsperada) {
    bufImagem[imagemRecebida++] = uint8_t(Serial.read());
    imagemUltimoByteMs = millis();
  }
  if (imagemRecebida == imagemEsperada) {
    recebendoImagem = false;
    registra("imagem recebida pelo USB: %u bytes", unsigned(imagemRecebida));
    iniciaMensagem(bufImagem, imagemRecebida, CONTEUDO_IMAGEM);
  } else if (millis() - imagemUltimoByteMs > TIMEOUT_IMAGEM_USB_MS) {
    recebendoImagem = false;
    registra("ERRO: imagem incompleta pelo USB (%u de %u bytes)", unsigned(imagemRecebida), unsigned(imagemEsperada));
  }
}

void processaUsb() {
  if (recebendoImagem) { recebeImagemUsb(); return; }
  while (Serial.available()) {
    if (recebendoImagem) { recebeImagemUsb(); return; }
    const char c = char(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (tamanhoLinha + 1 < sizeof(linhaSerial)) linhaSerial[tamanhoLinha++] = c;
      else linhaLonga = true;
      continue;
    }
    linhaSerial[tamanhoLinha] = 0;
    if (linhaLonga) {
      registra("ERRO: linha com mais de %u caracteres; para textos longos use /teste N",
               unsigned(sizeof(linhaSerial) - 1));
    } else if (tamanhoLinha) {
      if (linhaSerial[0] == '/') executaComando(linhaSerial);
      else iniciaMensagem(reinterpret_cast<const uint8_t *>(linhaSerial), tamanhoLinha);
    }
    tamanhoLinha = 0;
    linhaLonga = false;
  }
}

// ----------------------------- Arduino -----------------------------
void setup() {
  Serial.setRxBufferSize(MAX_MENSAGEM);   // a imagem chega de uma vez pelo USB
  Serial.begin(115200);
  Serial2.setRxBufferSize(1024);
  // invert=true: em repouso o pino fica em 0 V -> transmissor RF desligado.
  // Sem isso a portadora ficaria ligada o tempo todo e bloquearia os ACKs.
  // rxfifo_full_thrhd=1: entrega cada byte recebido imediatamente.
  Serial2.begin(rf::BAUD, SERIAL_8N1, PINO_RF_RX, PINO_RF_TX, true, 20000UL, 1);
  Serial2.setRxTimeout(1);
  // random() usa o gerador de numeros aleatorios por hardware da ESP32 (sem randomSeed).
  delay(200);
  registra("TX pronto | RF %lu bit/s | CRC-8 (0x07) | Go-Back-N janela=%u | timeout=%lu ms",
      (unsigned long)rf::BAUD, janela, (unsigned long)rf::TIMEOUT_ACK_MS);
  ajuda();
}

void loop() {
  processaUsb();
  processaRadio();
  executaGoBackN();
}
