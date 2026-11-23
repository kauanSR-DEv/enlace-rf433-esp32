# Validação com dois computadores — 23/11/2026

Cada placa ligada a um notebook diferente (A: transmissor, B: receptor), firmware v4.1.

## Bateria de distância e janela — `bateria/`

21 testes (3 repetições), `coletar_testes.py --modo tx` no notebook A (COM8), `--modo rx` no notebook B (COM15), unidos com `--modo juntar`.

| Arquivo | Conteúdo |
|---|---|
| `resultados.csv`, `resumo_por_cenario.csv`, `resumo.txt` | Resultado unido (TX + RX) de cada teste |
| `logs/NN_cenario_repK_TX.txt` / `_RX.txt` | Monitor Serial de cada placa durante o teste |
| `bruto_tx/`, `bruto_rx/` | Saída original de cada notebook antes da junção |

Resultado: 19 de 21 entregues e íntegras; as 2 falhas foram a 2 m, sem nenhum quadro registrado no RX. Os contadores do RX acumulam desde que a placa ligou; o primeiro teste usa como base o `STATUS` gravado em `bruto_rx/sessao.txt`. Em `bruto_tx/sessao.txt`, a linha `STATUS TX` veio corrompida pela porta USB (1,6 MB de bytes inválidos) e foi substituída por uma nota.

## Imagens a 1,8 m

Transferidas com `enviar_imagem.py` / `receber_imagem.py`, janela 4, antenas a **1,8 m**. Registradas por fotos e prints das telas; as linhas abaixo foram transcritas deles.

## Imagens transmitidas

| | PNG 32×32 (490 bytes) | PNG 1280×1280 (2866 bytes) |
|---|---|---|
| Arquivo | [`pc/teste_32x32.png`](../../pc/teste_32x32.png) | [`imagens/gengar_1280.png`](imagens/gengar_1280.png) |
| Quadros | 31 DATA + END | 180 DATA + END |
| Tempo no receptor | 64,5 s | 263,1 s |
| Vazão útil | 7,6 bytes/s | 10,9 bytes/s (87 bit/s) |
| SHA-256 (enviado = recebido) | `80e0254066353f1ba5c001529b17b0036d4843329d59aee5de46a81e93f48204` | `c56983efd93b41a31a81a5bf7d4937d7901ffaf2bd991284d71baf0568d476d3` |

Fim de cada transferência:

```
TX> === MENSAGEM ENTREGUE em 65799 ms | envios=63 retransmissoes=31 (por ACK=10, por timeout=21) ACKs=18 (ignorados=0) erros_sim=0 perdas_sim=0 | 7.5 bytes/s ===
RX> === MENSAGEM #25 COMPLETA: IMAGEM de 490 bytes em 64453 ms ===
IMAGEM SALVA: recebida_154843.png | 490 bytes | SHA-256 80e0254066353f1ba5c001529b17b0036d4843329d59aee5de46a81e93f48204
RX> estatisticas: aceitos=82 guardados_fora_de_ordem=21 duplicados=5 erros_FCS=0 truncados=26 ACKs=73

TX> Imagem: gengar_1280.png | 2866 bytes | SHA-256 c56983efd93b41a31a81a5bf7d4937d7901ffaf2bd991284d71baf0568d476d3
TX> === NOVA MENSAGEM (imagem): 2867 bytes -> 180 quadros DATA + 1 END | Selective Repeat janela=4 timeout=1117 ms ===
RX> === MENSAGEM #26 COMPLETA: IMAGEM de 2866 bytes em 263145 ms ===
IMAGEM SALVA: recebida_160433.png | 2866 bytes | SHA-256 c56983efd93b41a31a81a5bf7d4937d7901ffaf2bd991284d71baf0568d476d3
RX> estatisticas: aceitos=263 guardados_fora_de_ordem=156 duplicados=5 erros_FCS=1 truncados=32 ACKs=167
```

As estatísticas do RX acumulam desde que a placa ligou. Diferença entre as duas linhas = imagem de 2866 bytes: 181 quadros aceitos, 135 guardados fora de ordem, 6 truncados, **1 quadro rejeitado pelo CRC por erro real do canal** (sem `/erro`), 94 ACKs.

## Resumo da sessão e benchmark do CRC (TX)

```
# RESUMO,TX,mensagens=63,entregues=50,abandonadas=13,quadros_enviados=896,retransmissoes=566,retrans_por_ack=71,retrans_por_timeout=495,ACKs=216,erros_sim=0,perdas_sim=0,bytes_entregues=3731,vazao_media_Bps=4.8
# BENCH,TX,CRC-8,bytes_por_calculo=19,repeticoes=10000,tempo_total_us=77860,us_por_calculo=7.786
```

O `/resumo` cobre os ~78 min da sessão, incluindo as tentativas de alcance acima de 1,7 m, onde ficaram as 13 mensagens abandonadas. 495 das 566 retransmissões foram por *timeout*: na distância limite, o que se perde é o ACK de volta. Alcance observado nessa montagem: estável até ~1,5 m, intermitente a partir de ~1,7 m.

## Fotos

| Arquivo | Conteúdo |
|---|---|
| `fotos/01`, `02` | Texto digitado no Monitor Serial do TX e exibido no LCD da placa B |
| `fotos/03`, `04` | Disposição: placa A e notebook A em primeiro plano, placa B e notebook B ao fundo. Rostos de quem não é do grupo foram borrados (o grupo aparece apenas como o integrante de camisa azul, junto à placa B) |
| `fotos/05`–`09` | Imagem de 490 bytes: TX entregue, log do RX, imagem aberta, SHA-256, LCD |
| `fotos/10`–`15` | Imagem de 2866 bytes: TX com SHA-256, retransmissões seletivas, imagem aberta no receptor, SHA-256, LCD |
| `fotos/16`, `17` | Saída de `/resumo` e `/bench` |
