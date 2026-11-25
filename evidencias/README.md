# Evidências

## `2026-11-21_1258/` — bateria oficial

36 testes, 3 repetições por cenário, firmware v4.1, gerados por `pc/coletar_testes.py --modo unico`. Análise em [docs/resultados.md](../docs/resultados.md).

| Arquivo | Conteúdo |
|---|---|
| `resultados.csv` | Uma linha por teste (separador `;`, decimal `,`, abre no Excel) |
| `resumo_por_cenario.csv` | Médias por cenário |
| `resumo.txt` | As mesmas tabelas em texto |
| `sessao.txt` | Data, portas, versão e status inicial das placas |
| `logs/NN_cenario_repK_TX.txt` | Monitor Serial do TX durante o teste |
| `logs/NN_cenario_repK_RX.txt` | Monitor Serial do RX durante o teste |
| `imagens/` | As 6 imagens reconstruídas no computador receptor |

## `2026-11-23_dois_computadores/` — validação com dois notebooks

Uma placa em cada notebook: bateria de 21 testes de distância e janela (`bateria/`, gerada por `coletar_testes.py --modo tx/rx/juntar`), imagens de 490 e 2866 bytes a 1,8 m com SHA-256 idêntico, LCD, `/resumo` e `/bench`. Detalhes em [`2026-11-23_dois_computadores/README.md`](2026-11-23_dois_computadores/README.md).

## `2026-11-25_alcance_3.4m/` — 3,4 m com o TX v4.2

Depois do reajuste de sintonia do receptor da placa B: 2 de 3 mensagens entregues a 3,4 m, em 12,1 s e sem nenhuma retransmissão; a falha foi de ACKs perdidos na volta. Detalhes em [`2026-11-25_alcance_3.4m/README.md`](2026-11-25_alcance_3.4m/README.md).

## Observações sobre os arquivos brutos de 21/11


- Alguns logs do TX começam com caracteres ilegíveis (`����`). São bytes inválidos que a porta USB entregou ao abrir; o conteúdo do teste vem logo depois. O script foi corrigido para descartá-los nas próximas coletas.
- Em `sessao.txt`, a linha de status inicial do RX veio corrompida da mesma forma e foi substituída por uma nota. Nenhum log de teste foi editado.
- Cada placa tem seu próprio relógio (`[ ms]` desde que ligou); os tempos do TX e do RX não são comparáveis entre si.
