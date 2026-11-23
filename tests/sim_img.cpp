#define main main_sim
#include "sim.cpp"
#undef main
#include <fstream>
#include <sstream>
int main(int argc, char **argv) {
  srand(argc > 1 ? atoi(argv[1]) : 1);
  txns::Serial2.peer = &rxns::Serial2; rxns::Serial2.peer = &txns::Serial2;
  txns::setup(); rxns::setup();
  std::ifstream f("../pc/teste_32x32.png", std::ios::binary);
  std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});
  Canal canal; canal.perdaQuadro = 0.15; canal.erroByte = 0.003;
  txns::Serial2.canal = rxns::Serial2.canal = &canal;
  poe(txns::Serial, "/erro 20\n"); roda(50);
  txns::Serial.out.clear(); rxns::Serial.out.clear();
  poe(txns::Serial, "/imagem " + std::to_string(img.size()) + "\n");
  for (int i = 0; i < 1000 && txns::Serial.out.find("PRONTO_IMAGEM") == std::string::npos; i++) roda(1);
  for (auto b : img) txns::Serial.in.push_back(b);
  roda(600000, "x"); roda(3000);
  const std::string &o = rxns::Serial.out;
  size_t a = o.find("IMG_INICIO"), z = o.find("IMG_FIM");
  std::string hex; std::istringstream ss(o.substr(o.find('\n', a) + 1, z - o.find('\n', a) - 1)); std::string l;
  while (std::getline(ss, l)) hex += l;
  std::vector<uint8_t> rec; for (size_t i = 0; i + 1 < hex.size(); i += 2) rec.push_back(std::stoi(hex.substr(i, 2), nullptr, 16));
  size_t e = txns::Serial.out.find("=== MENSAGEM ENTREGUE");
  printf("imagem %zu bytes | recebida %zu bytes | identica=%s | %s\n", img.size(), rec.size(), rec == img ? "SIM" : "NAO",
         e != std::string::npos ? txns::Serial.out.substr(e, txns::Serial.out.find('\n', e) - e).c_str() : "sem entrega");
  return rec == img ? 0 : 1;
}
