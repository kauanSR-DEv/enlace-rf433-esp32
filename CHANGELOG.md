# Evolução do firmware

| Versão | O que mudou | Por quê |
|---|---|---|
| v1–v2 | Quadro do enunciado, CRC-8, Go-Back-N, UART a 2400 bit/s invertida, preâmbulo, Manchester, LCD paralelo | Primeiras versões funcionais |
| v3 | **1200 bit/s**, preâmbulo de 24 bytes no ACK, 100 ms de silêncio antes do ACK, envio de imagem pelo USB | A 2400 bit/s e com ACK curto o alcance ficava em 70–80 cm; passou para ~2 m |
| v4 | **Selective Repeat**: janela no RX, ACK com mapa de bits, temporizador por quadro, retransmissão seletiva | Vale 3 pontos e retransmite menos (20–50% mais rápido no simulador com perdas) |
| v4 + log | Log completo no Monitor Serial: bytes do quadro em hex, FCS recebido × calculado, tempo do CRC em µs, RTT, `/resumo`, `/bench` | Evidências pedidas no enunciado |
| v4.1 | Preâmbulo do ACK de 24 para 48 bytes, silêncio antes do ACK de 100 para 300 ms, desistência de 12 para 20 s | O ACK de volta (B → A) era o ponto fraco do enlace |
| **v4.2** (só TX) | Espera de 300 ms após cada ACK antes da próxima rajada; preâmbulo de 24 bytes (em vez de 8) no primeiro quadro de cada rajada (placa mais de 150 ms sem transmitir) | O primeiro quadro da rajada era confirmado em 72% das vezes, contra 93% dos demais. A 3,4 m, 0 retransmissões nas entregas |

## Scripts do PC

| Data | Mudança |
|---|---|
| 25/11/2026 | Grupo `alcance` (3,4 m, janela 4) no `coletar_testes.py`, fora do padrão; `teste_3_4m.py` roda só esse grupo |
| 23/11/2026 | `coletar_testes.py --modo juntar`: o primeiro teste passa a usar o `STATUS` do `sessao.txt` do RX como base dos contadores (antes herdava os contadores acumulados desde que a placa ligou) |

## Bancada

| Data | Mudança |
|---|---|
| 25/11/2026 | Antena de mola de um receptor trocada por fio reto de 17,3 cm; sintonia desse receptor reajustada (alcance de <50 cm para ~3,4 m); ele passou para a placa B |
