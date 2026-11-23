#include "Arduino.h"
#include "LiquidCrystal.h"
#include "protocolo_rf.h"
#include <vector>
#include <cassert>
uint64_t g_now = 0;
struct Canal { double perdaQuadro = 0, erroByte = 0; uint64_t enviados = 0, perdidos = 0, corrompidos = 0; };
size_t HS::write(const uint8_t *b, size_t n) {
  pend += n;
  if (!peer) { for (size_t i = 0; i < n; i++) out += char(b[i]); return n; }
  canal->enviados++;
  if ((double)rand() / RAND_MAX < canal->perdaQuadro) { canal->perdidos++; return n; }
  bool c = false;
  for (size_t i = 0; i < n; i++) {
    uint8_t x = b[i];
    if ((double)rand() / RAND_MAX < canal->erroByte) { x ^= uint8_t(1 << (rand() % 8)); c = true; }
    peer->in.push_back(x);
  }
  if (c) canal->corrompidos++;
  // ruido do receptor ocioso: alguns bytes aleatorios depois do quadro
  int lixo = rand() % 3; for (int i = 0; i < lixo; i++) peer->in.push_back(uint8_t(rand()));
  return n;
}
namespace txns { HS Serial, Serial2;
#include "tx.ino"
}
namespace rxns { HS Serial, Serial2;
#include "rx.ino"
}
static void poe(HS &s, const std::string &t) { for (char c : t) s.in.push_back(uint8_t(c)); }
static void roda(uint64_t ms, const char *ate = nullptr) {
  uint64_t fim = g_now + ms;
  while (g_now < fim) {
    txns::loop(); rxns::loop(); g_now += 1;
    if (ate && (txns::Serial.out.find("=== MENSAGEM ENTREGUE") != std::string::npos ||
                txns::Serial.out.find("=== FALHA") != std::string::npos)) return;
  }
}
static int falhas = 0;
static void cenario(const char *nome, double perda, double erroByte, int janela, const std::string &texto,
                    int simErro = 0, int simPerda = 0, bool verbose = false, int testeN = 0) {
  Canal canal; canal.perdaQuadro = perda; canal.erroByte = erroByte;
  txns::Serial2.canal = rxns::Serial2.canal = &canal;
  txns::Serial.out.clear(); rxns::Serial.out.clear();
  char cmd[64];
  snprintf(cmd, sizeof cmd, "/janela %d\n/erro %d\n/perda %d\n", janela, simErro, simPerda); poe(txns::Serial, cmd);
  roda(50);
  txns::Serial.out.clear();
  if (testeN) { snprintf(cmd, sizeof cmd, "/teste %d\n", testeN); poe(txns::Serial, cmd); }
  else poe(txns::Serial, texto + "\n");
  uint64_t t0 = g_now;
  roda(600000, "=== ");   // ate ENTREGUE ou FALHA
  bool entregueTx = txns::Serial.out.find("MENSAGEM ENTREGUE") != std::string::npos;
  roda(3000);
  std::string esperado = "TEXTO: " + texto + "\n";
  size_t p = rxns::Serial.out.find("TEXTO: ");
  bool ok = rxns::Serial.out.find(esperado) != std::string::npos;
  bool dup = ok && rxns::Serial.out.find("TEXTO: ", p + 1) != std::string::npos;
  bool corrompido = p != std::string::npos && !ok;
  // linha de estatistica do TX
  size_t e = txns::Serial.out.find("=== MENSAGEM ENTREGUE"); if (e == std::string::npos) e = txns::Serial.out.find("=== FALHA");
  std::string est = e != std::string::npos ? txns::Serial.out.substr(e, txns::Serial.out.find('\n', e) - e) : "(sem fim)";
  bool passou = ok && !dup && !corrompido && entregueTx;
  if (!passou) falhas++;
  printf("%-34s %s  %.1fs  quadros_canal=%llu perdidos=%llu corromp=%llu\n   %s\n", nome, passou ? "OK  " : "FALHOU",
         (g_now - t0) / 1000.0, (unsigned long long)canal.enviados, (unsigned long long)canal.perdidos,
         (unsigned long long)canal.corrompidos, est.c_str());
  if (verbose || !passou) { printf("---- TX ----\n%s---- RX ----\n%s", txns::Serial.out.c_str(), rxns::Serial.out.c_str()); }
  roda(20000);   // deixa os dois lados se acalmarem (desistencia/descarte)
}
int main(int argc, char **argv) {
  srand(argc > 1 ? atoi(argv[1]) : 1);
  bool v = argc > 2;
  txns::Serial2.peer = &rxns::Serial2; rxns::Serial2.peer = &txns::Serial2;
  txns::setup(); rxns::setup();
  std::string longo; for (int i = 0; i < 300; i++) longo += char('a' + i % 26);
  std::string enorme; for (int i = 0; i < 4090; i++) enorme += char('A' + i % 26);
  std::string t300; for (int i = 0; i < 300; i++) t300 += char('A' + i % 26);
  cenario("canal limpo, janela 4", 0, 0, 4, "ola mundo", 0, 0, v);
  cenario("canal limpo, janela 1 (SAW)", 0, 0, 1, longo);
  cenario("canal limpo, janela 8", 0, 0, 8, longo);
  cenario("perda 20% (canal), janela 4", 0.20, 0, 4, longo);
  cenario("perda 40% (canal), janela 4", 0.40, 0, 4, longo);
  cenario("erro de bit no canal, janela 4", 0, 0.01, 4, longo);
  cenario("/erro 30 (sim. no TX), janela 4", 0, 0, 4, longo, 30, 0);
  cenario("/perda 30 (sim. no TX), janela 4", 0, 0, 4, longo, 0, 30);
  cenario("perda 20% + erro, janela 8", 0.2, 0.005, 8, longo);
  cenario("mensagem 4090 bytes (>256 quadros)", 0.1, 0, 4, enorme, 0, 0, false, 4090);
  cenario("depois de tudo: canal limpo de novo", 0, 0, 4, "fim");
  printf("\nFALHAS: %d\n", falhas);
  return falhas != 0;
}
