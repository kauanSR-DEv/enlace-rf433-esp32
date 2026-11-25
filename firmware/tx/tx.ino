/*
 * TX - Transmissor (ESP32 A)   v4.2 - Selective Repeat
 *
 * v4.2 (so no TX): espera ESPERA_APOS_ACK_MS depois de cada ACK antes da proxima
 * rajada e usa preambulo de PREAMBULO_RAJADA_QTD bytes no primeiro quadro da rajada.
 * O RX continua com o rx.ino da v4.1.
 * Projeto final de TCD (IFMT): enlace RF 433 MHz confiavel entre duas ESP32.
 *
 * Deteccao de erro : CRC-8 (polinomio 0x07) no campo FCS.
 * Controle de erro : Selective Repeat ARQ, janela padrao 4 (maximo 8).
 *                    /janela 1 = Stop-and-Wait.
 *
 * Ligacoes (iguais nas duas placas; todos os pinos do lado esquerdo da ESP32):
 *   D25 (GPIO25) -> DATA do modulo transmissor RF (FS1000A), VCC 5 V, GND comum
 *   D34 (GPIO34) <- DATA do modulo receptor RF (MX-RM-5V) por divisor 10k/20k (5 V -> 3,3 V)
 *   Antena de ~17,3 cm (1/4 de onda em 433 MHz) nos modulos.
 *
 * Uso (Monitor Serial a 115200, final de linha "Nova linha"):
 *   Digite um texto e tecle Enter: ele e enviado ao RX.
 *   /ajuda  /status  /resumo  /bench  /janela N  /erro P  /perda P  /teste N  /cancela
 *   /imagem N  -> recebe N bytes binarios pelo USB e envia como imagem
 *                 (usado pelo script enviar_imagem.py; feche o Monitor Serial antes)
 *
 * Como funciona o Selective Repeat aqui:
 *   - O TX envia uma rajada de ate JANELA quadros novos e para para ouvir
 *     (os dois radios usam a mesma frequencia: canal half-duplex).
 *   - O RX guarda quadros fora de ordem e responde com UM ACK por rajada:
 *     SEQ = proximo quadro esperado em ordem, DATA[0] = mapa de bits dos
 *     quadros ja guardados a frente.
 *   - Ao receber o ACK, o TX marca cada quadro confirmado. Quadro enviado antes
 *     do ACK e que nao aparece nele foi perdido: e retransmitido na hora
 *     (o ACK seletivo funciona como NAK). Se nenhum ACK chegar, cada quadro
 *     pendente tem seu temporizador e e retransmitido quando ele estoura.
 *   - So o quadro perdido e retransmitido (no Go-Back-N iam todos os seguintes).
 */
#include <Arduino.h>
#include "protocolo_rf.h"

// ----------------------------- Configuracao -----------------------------
constexpr int PINO_RF_RX = 34;   // GPIO34: so entrada, recebe o DATA do RX pelo divisor
constexpr int PINO_RF_TX = 25;   // GPIO25: vai para o DATA do FS1000A
constexpr uint8_t JANELA_MAXIMA = rf::JANELA_MAX;   // 8 (tamanho do mapa de bits do ACK)
constexpr size_t MAX_MENSAGEM = 4096;               // bytes por mensagem
constexpr uint32_t MAX_QUADROS = (MAX_MENSAGEM + rf::MAX_DADOS - 1) / rf::MAX_DADOS + 1;
constexpr char CONTEUDO_TEXTO = 'T';                // 1o byte da mensagem: tipo de conteudo
constexpr char CONTEUDO_IMAGEM = 'I';
// Depois de terminar uma mensagem, espera um pouco antes de comecar outra,
// para um ACK atrasado da mensagem anterior nao ser confundido com um da nova.
constexpr uint32_t GUARDA_ENTRE_MENSAGENS_MS = rf::TIMEOUT_ACK_MS + 300;

// ----------------------------- Estado do Selective Repeat -----------------------------
// Os quadros sao numerados 0, 1, 2, ... (indice absoluto). No ar vai SEQ = indice % 256.
uint8_t  mensagem[MAX_MENSAGEM];
size_t   tamanhoMensagem = 0;
uint32_t quadrosDados = 0;      // quantidade de quadros DATA
uint32_t totalQuadros = 0;      // quadros DATA + 1 quadro END
uint32_t base = 0;              // quadro mais antigo ainda sem confirmacao
uint32_t proximo = 0;           // proximo quadro NOVO a transmitir
bool     ativo = false;
uint8_t  janela = 4;
uint32_t ultimoProgressoMs = 0; // ultima confirmacao nova
// v4.2: tempos do inicio de rajada
constexpr uint32_t ESPERA_APOS_ACK_MS = 300;   // receptor da placa B se recupera do proprio ACK
constexpr uint32_t SILENCIO_RAJADA_MS = 150;   // sem transmitir ha mais que isso = inicio de rajada
uint32_t liberadoAposMs = 0;                   // nao transmite antes deste instante
uint32_t fimUltimaTxMs = 0;                    // quando o ultimo quadro terminou de sair
uint32_t bloqueadoAteMs = 0;    // apos uma desistencia, espera o RX descartar
uint32_t fimUltimaMensagemMs = 0;

bool     confirmado[MAX_QUADROS];   // quadro ja confirmado pelo RX
bool     jaEnviado[MAX_QUADROS];    // para contar retransmissoes
uint32_t enviadoMs[MAX_QUADROS];    // fim da ultima transmissao do quadro
// Temporizador de cada quadro: 0 = ainda na rajada (sera armado quando o TX
// parar de transmitir); REENVIAR_JA = retransmitir agora; outro = instante do timeout.
uint32_t prazo[MAX_QUADROS];
constexpr uint32_t REENVIAR_JA = 1;

// Totais desde que a placa ligou (/resumo)
uint32_t totMensagens = 0, totEntregues = 0, totAbandonadas = 0, totEnvios = 0, totRetransmissoes = 0;
uint32_t totTimeouts = 0, totReenviosPorAck = 0, totAcks = 0, totErrosSim = 0, totPerdasSim = 0;
uint64_t totBytesEntregues = 0, totTempoEntregueMs = 0;

// Estatisticas da mensagem atual
uint32_t inicioMs = 0, nEnvios = 0, nRetransmissoes = 0, nTimeouts = 0, nReenviosPorAck = 0;
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
  const uint32_t t0 = micros();
  q.fcs = rf::calculaFcs(q);
  const uint32_t crcUs = micros() - t0;

  const bool retransmissao = jaEnviado[indice];
  jaEnviado[indice] = true;
  ++nEnvios; ++totEnvios;
  if (retransmissao) { ++nRetransmissoes; ++totRetransmissoes; }

  char hex[3 * (4 + rf::MAX_DADOS) + 1], txt[rf::MAX_DADOS + 1];
  rf::quadroHex(q, hex, sizeof(hex));
  rf::dadosTexto(q, txt, sizeof(txt));
  const bool inicioRajada = (millis() - fimUltimaTxMs) > SILENCIO_RAJADA_MS;
  const size_t naLinha = rf::bytesNaLinha(q, inicioRajada);
  registra("TX %s%s seq=%u len=%u FCS=0x%02X crc_us=%lu janela=[%lu..%lu] linha=%uB/%lums",
      rf::nomeTipo(q.tipo), retransmissao ? " (retransmissao seletiva)" : "", q.seq, q.len, q.fcs,
      (unsigned long)crcUs, (unsigned long)base, (unsigned long)(base + janela - 1),
      unsigned(naLinha), (unsigned long)(naLinha * rf::MS_POR_BYTE));
  registra("   quadro=[%s]%s%s%s", hex, q.len ? " dados=\"" : "", txt, q.len ? "\"" : "");

  prazo[indice] = 0;   // o temporizador sera armado quando a rajada terminar

  // --- Simulacao de erros (feita DEPOIS do calculo do FCS) ---
  if (perdaPercentual && uint8_t(random(100)) < perdaPercentual) {
    ++nPerdasSimuladas; ++totPerdasSim;
    registra("   [SIM] quadro seq=%u NAO transmitido (perda simulada)", q.seq);
    enviadoMs[indice] = millis();
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
    ++nErrosSimulados; ++totErrosSim;
    rf::quadroHex(q, hex, sizeof(hex));
    registra("   [SIM] bit invertido no campo %s (mascara 0x%02X) -> enviado=[%s]", nome, mascara, hex);
  }

  uint8_t linha[rf::MAX_LINHA];
  const size_t n = rf::montaLinha(q, linha, inicioRajada);
  transmiteLinha(linha, n);
  fimUltimaTxMs = millis();
  enviadoMs[indice] = millis();
}

void iniciaMensagem(const uint8_t *conteudo, size_t tamanho, char tipoConteudo = CONTEUDO_TEXTO) {
  if (ativo) { registra("ERRO: ainda enviando a mensagem anterior (use /cancela)"); return; }
  if (int32_t(bloqueadoAteMs - millis()) > 0) {
    registra("ERRO: aguarde %lu s (o RX ainda pode ter parte da mensagem anterior)",
             (unsigned long)((bloqueadoAteMs - millis()) / 1000 + 1));
    return;
  }
  if (tamanho == 0 || tamanho + 1 > MAX_MENSAGEM) { registra("ERRO: mensagem vazia ou grande demais"); return; }

  // Guarda entre mensagens: deixa passar qualquer ACK atrasado da mensagem anterior.
  if (fimUltimaMensagemMs && millis() - fimUltimaMensagemMs < GUARDA_ENTRE_MENSAGENS_MS)
    delay(GUARDA_ENTRE_MENSAGENS_MS - (millis() - fimUltimaMensagemMs));
  while (Serial2.available()) Serial2.read();
  receptor.reinicia();

  mensagem[0] = tipoConteudo;
  memcpy(mensagem + 1, conteudo, tamanho);
  tamanhoMensagem = tamanho + 1;
  quadrosDados = (tamanhoMensagem + rf::MAX_DADOS - 1) / rf::MAX_DADOS;
  totalQuadros = quadrosDados + 1;
  base = proximo = 0;
  for (uint32_t i = 0; i < totalQuadros; ++i) { confirmado[i] = jaEnviado[i] = false; prazo[i] = 0; enviadoMs[i] = 0; }
  ultimoProgressoMs = millis();
  nEnvios = nRetransmissoes = nTimeouts = nReenviosPorAck = nAcks = nAcksIgnorados = 0;
  nErrosSimulados = nPerdasSimuladas = 0;
  inicioMs = millis();
  ativo = true;
  registra("=== NOVA MENSAGEM (%s): %u bytes -> %lu quadros DATA + 1 END | Selective Repeat janela=%u timeout=%lu ms ===",
      tipoConteudo == CONTEUDO_IMAGEM ? "imagem" : "texto", (unsigned)tamanhoMensagem,
      (unsigned long)quadrosDados, janela, (unsigned long)rf::TIMEOUT_ACK_MS);
}

void finalizaMensagem(bool sucesso) {
  ativo = false;
  fimUltimaMensagemMs = millis();
  if (!sucesso) bloqueadoAteMs = millis() + rf::DESCARTE_RX_MS;
  const uint32_t duracao = millis() - inicioMs;
  ++totMensagens;
  if (sucesso) { ++totEntregues; totBytesEntregues += tamanhoMensagem; totTempoEntregueMs += duracao; }
  else ++totAbandonadas;
  registra("=== %s em %lu ms | envios=%lu retransmissoes=%lu (por ACK=%lu, por timeout=%lu) ACKs=%lu (ignorados=%lu) "
      "erros_sim=%lu perdas_sim=%lu | %.1f bytes/s ===",
      sucesso ? "MENSAGEM ENTREGUE" : "FALHA: MENSAGEM ABANDONADA", (unsigned long)duracao,
      (unsigned long)nEnvios, (unsigned long)nRetransmissoes, (unsigned long)nReenviosPorAck,
      (unsigned long)nTimeouts, (unsigned long)nAcks, (unsigned long)nAcksIgnorados,
      (unsigned long)nErrosSimulados, (unsigned long)nPerdasSimuladas,
      duracao ? 1000.0 * tamanhoMensagem / duracao : 0.0);
}

// ----------------------------- Recepcao de ACKs -----------------------------
// ACK seletivo: SEQ = proximo quadro esperado em ordem (todos os anteriores chegaram);
// bit i do mapa = quadro (SEQ + i) ja guardado pelo RX.
void trataAck(uint8_t seq, uint8_t mapa) {
  ++nAcks; ++totAcks;
  if (!ativo) { registra("RX ACK prox=%u (nenhuma mensagem ativa, ignorado)", seq); return; }

  // Acha o indice absoluto que corresponde a SEQ, dentro do que ja foi enviado.
  int32_t k = -1;
  for (uint32_t i = base; i <= proximo && i <= base + JANELA_MAXIMA; ++i)
    if (uint8_t(i & 0xFF) == seq) { k = int32_t(i); break; }
  if (k < 0) {
    ++nAcksIgnorados;
    registra("RX ACK prox=%u fora da janela (base=%lu) -> ignorado", seq, (unsigned long)base);
    return;
  }

  bool progresso = false;
  for (uint32_t i = base; i < uint32_t(k); ++i)
    if (!confirmado[i]) { confirmado[i] = true; progresso = true; }
  for (uint8_t b = 0; b < 8; ++b) {
    const uint32_t i = uint32_t(k) + b;
    if (((mapa >> b) & 1) && i < proximo && !confirmado[i]) { confirmado[i] = true; progresso = true; }
  }
  while (base < totalQuadros && confirmado[base]) ++base;

  char lista[48] = "";
  size_t pos = 0;
  for (uint8_t b = 0; b < 8; ++b)
    if ((mapa >> b) & 1) pos += snprintf(lista + pos, sizeof(lista) - pos, "%u ", uint8_t(seq + b));
  if (pos) lista[pos - 1] = 0;   // tira o espaco final
  // RTT: do fim do envio do ultimo quadro confirmado em ordem ate a chegada deste ACK.
  const uint32_t rtt = (k > 0 && enviadoMs[k - 1]) ? millis() - enviadoMs[k - 1] : 0;
  registra("RX ACK prox=%u mapa=0x%02X guardados_no_RX=[%s] -> base=%lu rtt_ms=%lu", seq, mapa, lista,
           (unsigned long)base, (unsigned long)rtt);

  if (progresso) ultimoProgressoMs = millis();

  // Quadro enviado ANTES deste ACK e que ainda nao foi confirmado se perdeu:
  // o RX so responde depois de o canal ficar em silencio, entao ele ja teria visto.
  // Retransmite so ele, ja (o ACK seletivo faz o papel de NAK).
  const uint32_t agora = millis();
  for (uint32_t i = base; i < proximo; ++i) {
    if (confirmado[i] || prazo[i] == REENVIAR_JA || !jaEnviado[i]) continue;
    if (enviadoMs[i] + rf::ESPERA_ANTES_ACK_MS + rf::DURACAO_ACK_MS <= agora + 10) {
      prazo[i] = REENVIAR_JA;
      ++nReenviosPorAck; ++totReenviosPorAck;
      registra("   quadro seq=%u nao confirmado neste ACK -> retransmissao seletiva", uint8_t(i & 0xFF));
    }
  }
}

void processaRadio() {
  while (Serial2.available()) {
    const uint8_t b = uint8_t(Serial2.read());
    switch (receptor.recebe(b, millis())) {
      case rf::Receptor::QUADRO_OK:
        if (receptor.quadro.tipo == rf::ACK) {
          liberadoAposMs = millis() + ESPERA_APOS_ACK_MS;   // v4.2
          trataAck(receptor.quadro.seq, receptor.quadro.len ? receptor.quadro.dados[0] : 0);
        } else registra("RX %s seq=%u inesperado no TX, ignorado", rf::nomeTipo(receptor.quadro.tipo), receptor.quadro.seq);
        break;
      case rf::Receptor::ERRO_FCS:
      {
        char hex[3 * (4 + rf::MAX_DADOS) + 1];
        rf::quadroHex(receptor.quadro, hex, sizeof(hex));
        registra("RX quadro com FCS invalido (recebido 0x%02X, calculado 0x%02X) quadro=[%s] -> descartado",
            receptor.quadro.fcs, receptor.fcsCalculado, hex);
      }
        break;
      case rf::Receptor::QUADRO_TRUNCADO:
        registra("RX quadro truncado/corrompido na linha -> descartado");
        break;
      default: break;
    }
  }
  receptor.verificaTimeout(millis());
}

// ----------------------------- Maquina do Selective Repeat -----------------------------
void executaSelectiveRepeat() {
  if (!ativo) return;
  if (base == totalQuadros) { finalizaMensagem(true); return; }
  const uint32_t agora = millis();
  if (int32_t(liberadoAposMs - agora) > 0) return;   // v4.2: espera apos o ACK

  // 1) Retransmissao seletiva: quadro marcado pelo ACK ou com temporizador estourado.
  for (uint32_t i = base; i < proximo; ++i) {
    if (confirmado[i] || prazo[i] == 0) continue;
    if (prazo[i] == REENVIAR_JA || int32_t(agora - prazo[i]) >= 0) {
      if (prazo[i] != REENVIAR_JA) {
        ++nTimeouts; ++totTimeouts;
        registra("TIMEOUT do quadro seq=%u -> retransmissao seletiva (so ele)", uint8_t(i & 0xFF));
      }
      enviaQuadro(i);
      return;
    }
  }

  // 2) Quadro novo, se couber na janela. O END so sai quando todos os DATA foram confirmados.
  const bool cabeNaJanela = proximo < base + janela;
  const bool ehEnd = proximo == quadrosDados;
  if (proximo < totalQuadros && cabeNaJanela && (!ehEnd || base == quadrosDados)) {
    enviaQuadro(proximo);
    ++proximo;
    return;
  }

  // 3) Nada para transmitir: a rajada terminou. Arma o temporizador de cada quadro
  //    pendente que acabou de ser enviado e espera o ACK.
  for (uint32_t i = base; i < proximo; ++i)
    if (!confirmado[i] && prazo[i] == 0) prazo[i] = agora + rf::TIMEOUT_ACK_MS;

  if (agora - ultimoProgressoMs > rf::DESISTENCIA_TX_MS) {
    registra("%lu s sem nenhuma confirmacao nova: enlace caiu", (unsigned long)(rf::DESISTENCIA_TX_MS / 1000));
    finalizaMensagem(false);
  }
}

// ----------------------------- Comandos pelo USB -----------------------------
void ajuda() {
  Serial.println(F("Digite um texto e tecle Enter para enviar. Comandos:"));
  Serial.println(F("  /status      estado atual e configuracao"));
  Serial.println(F("  /resumo      totais desde que a placa ligou"));
  Serial.println(F("  /bench       mede o tempo do CRC-8 (10000 calculos)"));
  Serial.println(F("  /janela N    tamanho da janela (1..8). 1 = Stop-and-Wait"));
  Serial.println(F("  /erro P      inverte 1 bit em P% dos quadros (0 desliga)"));
  Serial.println(F("  /perda P     deixa de transmitir P% dos quadros (0 desliga)"));
  Serial.println(F("  /teste N     envia uma mensagem de teste com N caracteres"));
  Serial.println(F("  /cancela     abandona a mensagem atual"));
  Serial.println(F("  /imagem N    recebe N bytes pelo USB e envia como imagem (use o enviar_imagem.py)"));
}

void status() {
  registra("STATUS: ativo=%u base=%lu proximo=%lu total=%lu | Selective Repeat janela=%u erro=%u%% perda=%u%% | "
      "baud=%lu timeout=%lu ms | ruido_descartado=%lu",
      ativo, (unsigned long)base, (unsigned long)proximo, (unsigned long)totalQuadros, janela,
      erroPercentual, perdaPercentual, (unsigned long)rf::BAUD, (unsigned long)rf::TIMEOUT_ACK_MS,
      (unsigned long)receptor.descartesRuido);
}

void resumo() {
  registra("# RESUMO,TX,mensagens=%lu,entregues=%lu,abandonadas=%lu,quadros_enviados=%lu,retransmissoes=%lu,"
      "retrans_por_ack=%lu,retrans_por_timeout=%lu,ACKs=%lu,erros_sim=%lu,perdas_sim=%lu,bytes_entregues=%llu,"
      "vazao_media_Bps=%.1f",
      (unsigned long)totMensagens, (unsigned long)totEntregues, (unsigned long)totAbandonadas, (unsigned long)totEnvios,
      (unsigned long)totRetransmissoes, (unsigned long)totReenviosPorAck, (unsigned long)totTimeouts,
      (unsigned long)totAcks, (unsigned long)totErrosSim, (unsigned long)totPerdasSim,
      (unsigned long long)totBytesEntregues,
      totTempoEntregueMs ? 1000.0 * double(totBytesEntregues) / double(totTempoEntregueMs) : 0.0);
}

// Tempo medio do CRC-8 sobre um quadro DATA cheio (TYPE+SEQ+LEN+16 bytes = 19 bytes).
void bench() {
  if (ativo) { registra("ERRO: /bench so fora de uma transmissao"); return; }
  rf::Quadro q; q.tipo = rf::DATA; q.seq = 7; q.len = rf::MAX_DADOS;
  for (uint8_t i = 0; i < rf::MAX_DADOS; ++i) q.dados[i] = uint8_t('A' + i);
  volatile uint8_t acumulado = 0;
  const uint32_t repeticoes = 10000, t0 = micros();
  for (uint32_t i = 0; i < repeticoes; ++i) { q.seq = uint8_t(i); acumulado ^= rf::calculaFcs(q); }
  const uint32_t total = micros() - t0;
  registra("# BENCH,TX,CRC-8,bytes_por_calculo=19,repeticoes=%lu,tempo_total_us=%lu,us_por_calculo=%.3f",
      (unsigned long)repeticoes, (unsigned long)total, double(total) / repeticoes);
  (void)acumulado;
}

void executaComando(char *linha) {
  char *arg = strchr(linha, ' ');
  long valor = -1;
  if (arg) { *arg++ = 0; valor = strtol(arg, nullptr, 10); }

  if (!strcmp(linha, "/ajuda")) ajuda();
  else if (!strcmp(linha, "/status")) status();
  else if (!strcmp(linha, "/resumo")) resumo();
  else if (!strcmp(linha, "/bench")) bench();
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
  registra("TX pronto (v4.2) | RF %lu bit/s | CRC-8 (0x07) | Selective Repeat janela=%u | timeout=%lu ms",
      (unsigned long)rf::BAUD, janela, (unsigned long)rf::TIMEOUT_ACK_MS);
  ajuda();
}

void loop() {
  processaUsb();
  processaRadio();
  executaSelectiveRepeat();
}
