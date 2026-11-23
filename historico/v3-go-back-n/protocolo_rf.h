/*
 * protocolo_rf.h - Partes do protocolo que sao IGUAIS no TX e no RX.
 *
 * Projeto final de Transmissao e Comunicacao de Dados (IFMT)
 * Enlace RF 433 MHz entre duas ESP32, sem biblioteca de radio.
 *
 * ATENCAO: este arquivo existe em firmware/tx/ e em firmware/rx/.
 * As duas copias precisam ser identicas (a Arduino IDE so enxerga
 * arquivos dentro da pasta do sketch). Altere uma e copie para a outra.
 *
 * Camadas, de baixo para cima:
 *   1. Fisica : UART a 2400 bit/s ligada ao pino DATA dos modulos RF (ASK/OOK).
 *               A UART trabalha INVERTIDA para que o repouso desligue a portadora.
 *   2. Linha  : preambulo 0xF0 + FLAG 0x7E + cada byte do quadro em Manchester
 *               (1 byte do quadro -> 2 bytes na linha). Isso mantem o sinal
 *               equilibrado (mesma quantidade de 0 e 1), o que o receptor ASK exige.
 *   3. Quadro : FLAG | TYPE | SEQ | LEN | DATA (0..16) | FCS   (Figura 2 do PDF)
 *               FCS = CRC-8 (polinomio 0x07) sobre TYPE, SEQ, LEN e DATA.
 */
#ifndef PROTOCOLO_RF_H
#define PROTOCOLO_RF_H
#include <stdint.h>
#include <stddef.h>

namespace rf {

// ---------------------------------------------------------------------------
// Parametros do enlace
// ---------------------------------------------------------------------------
constexpr uint32_t BAUD = 1200;          // bits por segundo na linha (2400 deformava os pulsos)
constexpr uint8_t  FLAG = 0x7E;          // inicio do quadro (Figura 2)
constexpr uint8_t  PREAMBULO_BYTE = 0xF0;
constexpr uint8_t  PREAMBULO_QTD  = 8;   // 8 bytes antes de cada DATA/END para o receptor ajustar o ganho
// O ACK usa preambulo mais longo: o receptor da placa que acabou de transmitir fica
// "ofuscado" pelo proprio transmissor (a poucos cm) e precisa de tempo para voltar
// a ouvir um sinal fraco vindo de longe.
constexpr uint8_t  PREAMBULO_ACK_QTD = 24;
// Tempo extra de silencio antes do ACK, para esse receptor se recuperar.
constexpr uint32_t RECUPERACAO_RX_MS = 100;
constexpr uint8_t  MAX_DADOS = 16;       // campo DATA: 0 a 16 bytes

enum Tipo : uint8_t { DATA = 0x01, ACK = 0x02, END = 0x03 };

inline const char *nomeTipo(uint8_t t) {
  return t == DATA ? "DATA" : t == ACK ? "ACK" : t == END ? "END" : "???";
}

// Tempos derivados do baud. Cada byte UART tem 10 bits (start + 8 + stop).
constexpr float    MS_POR_BYTE = 10000.0f / BAUD;                       // ~4,17 ms
constexpr uint32_t DURACAO_ACK_MS = uint32_t((PREAMBULO_ACK_QTD + 1 + 2 * 4) * MS_POR_BYTE) + 1;
// RX espera esse silencio depois do ultimo quadro antes de responder com ACK.
// Precisa ser maior que o intervalo entre dois quadros seguidos do TX
// (preambulo do proximo quadro + FLAG + folga).
constexpr uint32_t ESPERA_ANTES_ACK_MS = uint32_t((PREAMBULO_QTD + 4) * MS_POR_BYTE) + 25 + RECUPERACAO_RX_MS;
// TX desiste de esperar o ACK depois desse tempo sem resposta.
constexpr uint32_t TIMEOUT_ACK_MS = ESPERA_ANTES_ACK_MS + DURACAO_ACK_MS + 200;
// Se os bytes de um quadro param de chegar por esse tempo, o quadro e abandonado.
constexpr uint32_t TIMEOUT_BYTE_MS = uint32_t(4 * MS_POR_BYTE) + 20;

// Ressincronizacao quando o enlace cai (nao existe quadro de "reset"):
// - o TX desiste da mensagem apos DESISTENCIA_TX_MS sem nenhum ACK que avance a janela;
// - o RX descarta uma mensagem incompleta apos DESCARTE_RX_MS sem quadro valido;
// - depois de desistir, o TX so aceita nova mensagem apos DESCARTE_RX_MS.
// Como DESISTENCIA_TX_MS < DESCARTE_RX_MS, o TX sempre desiste antes de o RX
// descartar, e os dois recomecam juntos em SEQ 0.
constexpr uint32_t DESISTENCIA_TX_MS = 12000;
constexpr uint32_t DESCARTE_RX_MS = 16000;

// ---------------------------------------------------------------------------
// Quadro
// ---------------------------------------------------------------------------
struct Quadro {
  uint8_t tipo = 0;
  uint8_t seq = 0;
  uint8_t len = 0;
  uint8_t dados[MAX_DADOS] = {};
  uint8_t fcs = 0;
};

// CRC-8, polinomio x^8 + x^2 + x + 1 (0x07), valor inicial 0x00,
// sem reflexao, sem XOR final (mesmo CRC-8 do trabalho de telemetria).
inline uint8_t crc8(const uint8_t *p, size_t n) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (uint8_t b = 0; b < 8; ++b)
      crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x07) : uint8_t(crc << 1);
  }
  return crc;
}

// FCS calculado sobre TYPE, SEQ, LEN e DATA (exclui FLAG e o proprio FCS).
inline uint8_t calculaFcs(const Quadro &q) {
  uint8_t buf[3 + MAX_DADOS];
  buf[0] = q.tipo; buf[1] = q.seq; buf[2] = q.len;
  for (uint8_t i = 0; i < q.len && i < MAX_DADOS; ++i) buf[3 + i] = q.dados[i];
  return crc8(buf, 3 + (q.len <= MAX_DADOS ? q.len : MAX_DADOS));
}

// ---------------------------------------------------------------------------
// Codificacao Manchester por nibble
// Cada bit do nibble vira um par: 1 -> "10", 0 -> "01" (ordem de envio na UART).
// Todo byte valido tem exatamente quatro bits 1 e quatro bits 0.
// So 16 dos 256 valores possiveis sao validos: ruido quase nunca passa.
// ---------------------------------------------------------------------------
inline uint8_t codificaNibble(uint8_t n) {
  uint8_t saida = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    const uint8_t par = ((n >> i) & 1) ? 0b01 : 0b10;  // UART envia o bit menos significativo primeiro
    saida |= uint8_t(par << (2 * i));
  }
  return saida;
}

// Devolve 0..15, ou -1 se o byte nao for Manchester valido.
inline int8_t decodificaNibble(uint8_t b) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    const uint8_t par = (b >> (2 * i)) & 0b11;
    if (par == 0b01) n |= uint8_t(1 << i);
    else if (par != 0b10) return -1;
  }
  return int8_t(n);
}

// Tamanho maximo de um quadro na linha (preambulo + FLAG + 2 bytes por byte do quadro).
constexpr size_t MAX_PREAMBULO = PREAMBULO_ACK_QTD > PREAMBULO_QTD ? PREAMBULO_ACK_QTD : PREAMBULO_QTD;
constexpr size_t MAX_LINHA = MAX_PREAMBULO + 1 + 2 * (3 + MAX_DADOS + 1);

// Monta os bytes que vao para a UART. Retorna quantos bytes foram escritos.
inline size_t montaLinha(const Quadro &q, uint8_t *saida) {
  size_t n = 0;
  const uint8_t preambulo = (q.tipo == ACK) ? PREAMBULO_ACK_QTD : PREAMBULO_QTD;
  for (uint8_t i = 0; i < preambulo; ++i) saida[n++] = PREAMBULO_BYTE;
  saida[n++] = FLAG;
  uint8_t campos[3 + MAX_DADOS + 1];
  uint8_t total = 0;
  campos[total++] = q.tipo;
  campos[total++] = q.seq;
  campos[total++] = q.len;
  for (uint8_t i = 0; i < q.len; ++i) campos[total++] = q.dados[i];
  campos[total++] = q.fcs;
  for (uint8_t i = 0; i < total; ++i) {
    saida[n++] = codificaNibble(campos[i] >> 4);    // nibble alto primeiro
    saida[n++] = codificaNibble(campos[i] & 0x0F);
  }
  return n;
}

// ---------------------------------------------------------------------------
// Receptor de linha: recebe um byte por vez da UART e devolve quadros.
// ---------------------------------------------------------------------------
class Receptor {
 public:
  enum Resultado : uint8_t {
    NADA,             // nenhum quadro terminou neste byte
    QUADRO_OK,        // FCS confere: quadro em 'quadro'
    ERRO_FCS,         // quadro completo, mas FCS nao confere -> descartar
    QUADRO_TRUNCADO   // cabecalho lido, mas o resto chegou invalido/incompleto
  };

  Quadro quadro;            // ultimo quadro lido (valido ou nao)
  uint8_t fcsCalculado = 0; // FCS que o receptor calculou para 'quadro'
  uint32_t descartesRuido = 0;  // inicios falsos causados por ruido (nao sao quadros)

  // true enquanto um quadro esta sendo lido (FLAG ja encontrada)
  bool recebendo() const { return !buscando_; }

  void reinicia() { buscando_ = true; }

  Resultado recebe(uint8_t b, uint32_t agoraMs) {
    Resultado r = verificaTimeout(agoraMs);

    if (buscando_) {
      if (b == FLAG) comecaQuadro(agoraMs);
      return r;
    }
    ultimoByteMs_ = agoraMs;

    const int8_t nib = decodificaNibble(b);
    if (nib < 0) {                       // byte fora do codigo Manchester
      Resultado erro = abandona();
      if (b == FLAG) comecaQuadro(agoraMs);  // pode ser o inicio de outro quadro
      return erro;
    }
    if (nibbleAlto_ < 0) { nibbleAlto_ = nib; return r; }

    bruto_[n_++] = uint8_t((nibbleAlto_ << 4) | nib);
    nibbleAlto_ = -1;

    if (n_ == 3 && bruto_[2] > MAX_DADOS) return abandona();  // LEN impossivel
    if (n_ < 3 || n_ < 3u + bruto_[2] + 1u) return r;         // ainda faltam bytes

    // Quadro completo: separa os campos e confere o FCS.
    buscando_ = true;
    quadro.tipo = bruto_[0];
    quadro.seq = bruto_[1];
    quadro.len = bruto_[2];
    for (uint8_t i = 0; i < quadro.len; ++i) quadro.dados[i] = bruto_[3 + i];
    quadro.fcs = bruto_[3 + quadro.len];
    fcsCalculado = calculaFcs(quadro);
    if (fcsCalculado != quadro.fcs) return ERRO_FCS;
    if (quadro.tipo != DATA && quadro.tipo != ACK && quadro.tipo != END) return ERRO_FCS;
    return QUADRO_OK;
  }

  // Chamar periodicamente: abandona um quadro cujos bytes pararam de chegar.
  Resultado verificaTimeout(uint32_t agoraMs) {
    if (!buscando_ && agoraMs - ultimoByteMs_ > TIMEOUT_BYTE_MS) return abandona();
    return NADA;
  }

 private:
  bool buscando_ = true;
  uint8_t bruto_[3 + MAX_DADOS + 1] = {};
  uint8_t n_ = 0;
  int8_t nibbleAlto_ = -1;
  uint32_t ultimoByteMs_ = 0;

  void comecaQuadro(uint32_t agoraMs) {
    buscando_ = false; n_ = 0; nibbleAlto_ = -1; ultimoByteMs_ = agoraMs;
  }
  // Sem cabecalho completo, foi so ruido parecido com FLAG: conta e segue.
  Resultado abandona() {
    buscando_ = true;
    if (n_ >= 3) { quadro.tipo = bruto_[0]; quadro.seq = bruto_[1]; quadro.len = bruto_[2]; return QUADRO_TRUNCADO; }
    ++descartesRuido;
    return NADA;
  }
};

}  // namespace rf

#endif  // PROTOCOLO_RF_H
