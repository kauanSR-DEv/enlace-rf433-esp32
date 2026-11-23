# Evolução do firmware

| Versão | O que mudou | Por quê |
|---|---|---|
| v1–v2 | Quadro do enunciado, CRC-8, Go-Back-N, UART a 2400 bit/s invertida, preâmbulo, Manchester, LCD paralelo | Primeiras versões funcionais |
| v3 | **1200 bit/s**, preâmbulo de 24 bytes no ACK, 100 ms de silêncio antes do ACK, envio de imagem pelo USB | A 2400 bit/s e com ACK curto o alcance ficava em 70–80 cm; passou para ~2 m |
| v4 | **Selective Repeat**: janela no RX, ACK com mapa de bits, temporizador por quadro, retransmissão seletiva | Vale 3 pontos e retransmite menos (20–50% mais rápido no simulador com perdas) |
| v4 + log | Log completo no Monitor Serial: bytes do quadro em hex, FCS recebido × calculado, tempo do CRC em µs, RTT, `/resumo`, `/bench` | Evidências pedidas no enunciado |
| **v4.1** | Preâmbulo do ACK de 24 para 48 bytes, silêncio antes do ACK de 100 para 300 ms, desistência de 12 para 20 s | O ACK de volta (B → A) era o ponto fraco do enlace |
