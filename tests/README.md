# Simulador

Roda o `tx.ino` e o `rx.ino` **sem alteração** no computador, ligados por um canal simulado com perdas, inversão de bits e atraso. Serve para testar a lógica do protocolo antes da bancada.

```bash
cd tests
cp ../firmware/tx/tx.ino ../firmware/rx/rx.ino ../firmware/tx/protocolo_rf.h .
g++ -std=c++17 -O2 -I. -o sim sim.cpp
./sim 1          # cenários: canal limpo, janelas 1/4/8, perdas, erro de bit, /erro, /perda, mensagem de 4090 bytes
./sim 1 v        # o mesmo, com o log completo das duas placas
g++ -std=c++17 -O2 -I. -o sim_img sim_img.cpp
./sim_img        # envia a imagem de teste e confere se chegou idêntica
```

Cada cenário confere se o texto chegou inteiro, sem corrupção nem duplicação, e se o TX declarou a mensagem entregue. `Arduino.h` e `LiquidCrystal.h` são imitações mínimas das bibliotecas da placa.

## Go-Back-N × Selective Repeat no simulador

Média de 5 execuções, mensagem de 300 caracteres (19 quadros), janela 4 exceto onde indicado. O simulador não reproduz os tempos reais do rádio; compare as colunas entre si.

| Cenário | Go-Back-N (v3) | Selective Repeat (v4) | Ganho |
|---|---|---|---|
| Canal limpo | 4,5 s | 4,6 s | — |
| Erro de bit no canal | 35,7 s | 23,0 s | 36% |
| `/erro 30` | 39,3 s | 19,6 s | 50% |
| `/perda 30` | 30,4 s | 16,1 s | 47% |
| Perda de 40% | 40,9 s (desistiu em 4 de 5) | 32,9 s (entregou 5 de 5) | 20% |
| Perda 20% + erro, janela 8 | 38,1 s (desistiu em 3 de 5) | 20,5 s (entregou 5 de 5) | 46% |

Limitação: o simulador não modela colisões, o controle de ganho do receptor nem o comportamento analógico do rádio. A bancada é o teste definitivo.
