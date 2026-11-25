# Alcance de 3,4 m — 25/11/2026

Ensaio feito depois do reajuste de sintonia do receptor da placa B ([docs/resultados.md](../../docs/resultados.md#diagnóstico-do-receptor-e-ajuste-de-sintonia-2411-e-2511)). Uma placa em cada notebook, antenas a **3,4 m**, mensagem `/teste 200` (201 bytes = 13 DATA + END), janela 4.

- TX: placa A, **firmware v4.2**, COM5, `py teste_3_4m.py tx COM5`
- RX: placa B, firmware v4.1 (o RX não mudou na v4.2), COM11, `py teste_3_4m.py rx COM11`
- Junção: `coletar_testes.py --modo juntar`

| Rep. | Resultado | Tempo | Envios | Retransmissões | ACKs recebidos no TX | Quadros aceitos no RX |
|---|---|---|---|---|---|---|
| 1 | ✅ entregue, íntegra | 12,10 s | 14 | 0 | 5 | 14 |
| 2 | ✅ entregue, íntegra | 12,10 s | 14 | 0 | 5 | 14 |
| 3 | ❌ abandonada após 20 s | 20,17 s | 28 | 24 (timeout) | 0 | 4 + 24 duplicados |

Na repetição 3 os dados chegaram: o RX aceitou os quadros 0–3, recebeu sem erro as 24 retransmissões e enviou 7 ACKs (`logs/03_distancia_3.4m_rep3_RX.txt`). Nenhum ACK chegou à placa A. A falha foi no retorno B → A, cujo receptor (placa A) não foi reajustado.

| Arquivo | Conteúdo |
|---|---|
| `resultados.csv`, `resumo_por_cenario.csv`, `resumo.txt` | Resultado unido (TX + RX) |
| `logs/` | Monitor Serial de cada placa em cada repetição |
| `bruto_tx/`, `bruto_rx/` | Saída original de cada notebook antes da junção |

Observações:

- No `resultados.csv`, a repetição 3 aparece com `integridade_rx=NAO_RECEBIDO` e contadores do RX vazios porque a mensagem não foi concluída no RX; os quadros recebidos estão no log.
- Os relógios dos notebooks diferiam em cerca de 1,2 s; a junção usa a ordem dos testes, não o horário.
- O `sessao.txt` do RX mostra contadores acumulados dos testes de ajuste feitos antes, com a mesma placa ligada.
