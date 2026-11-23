# Como reproduzir

## 1. Preparar o ambiente

1. Instale a **Arduino IDE 2** e, no Gerenciador de Placas, o pacote **esp32** da Espressif.
2. Placa: **ESP32 Dev Module**. A biblioteca **LiquidCrystal** já vem com a IDE.
3. Instale o **Python 3** (no instalador do Windows, marque *Add python.exe to PATH*) e o pyserial:

```bash
py -m pip install -r pc/requirements.txt
```

No Windows, use `py` no lugar de `python` se o comando `python` abrir a Microsoft Store.

## 2. Gravar as placas

1. Abra `firmware/tx/tx.ino` e grave na **placa A**.
2. Abra `firmware/rx/rx.ino` e grave na **placa B**.
3. Cada pasta precisa ter o `.ino` **e** o `protocolo_rf.h` (a IDE mostra duas abas). As duas cópias de `protocolo_rf.h` são idênticas; se alterar uma, copie para a outra.

Se a gravação falhar com *"Unable to verify flash chip connection"*, use **Ferramentas → Upload Speed → 115200** e feche qualquer programa que esteja usando a porta.

## 3. Primeiro teste

1. Abra o Monitor Serial de cada placa: **115200 baud**, final de linha **Nova linha**.
2. Aperte **EN** em cada placa. As duas devem mostrar a mesma versão e a mesma taxa:

```
TX pronto (v4.1) | RF 1200 bit/s | CRC-8 (0x07) | Selective Repeat janela=4 | timeout=1117 ms
RX pronto (v4.1) | RF 1200 bit/s | CRC-8 (0x07) | Selective Repeat janela=8 | ACK apos 425 ms de silencio
```

3. Com as placas a ~50 cm, digite `ola` no Monitor do TX. O RX mostra `TEXTO: ola` e o LCD mostra `ola`.

## 4. Comandos

| Comando | Placa | Função |
|---|---|---|
| texto + Enter | TX | Envia o texto |
| `/janela N` | TX | Janela de 1 a 8 (1 = Stop-and-Wait) |
| `/erro P` | TX | Inverte 1 bit em P% dos quadros, depois do CRC |
| `/perda P` | TX | Deixa de transmitir P% dos quadros |
| `/teste N` | TX | Mensagem de teste com N caracteres |
| `/cancela` | TX | Abandona a mensagem atual |
| `/status` | as duas | Estado e contadores |
| `/resumo` | as duas | Totais desde que a placa ligou (`# RESUMO,...`) |
| `/bench` | as duas | Tempo médio do CRC-8 em 10.000 cálculos (`# BENCH,...`) |
| `/reinicia` | RX | Volta a esperar SEQ 0 |

## 5. Enviar uma imagem

Feche os Monitores Seriais. Em dois terminais, na pasta `pc/`:

```bash
# computador B (placa RX) — comece por ele
py receber_imagem.py COM6

# computador A (placa TX)
py enviar_imagem.py COM5 teste_32x32.png
```

O receptor salva `recebida_HHMMSS.png` e mostra o SHA-256. Se for igual ao da imagem original, a imagem chegou perfeita. Imagens de até 4095 bytes.

## 6. Bateria de testes automática

**Um computador** (as duas placas no mesmo notebook):

```bash
py coletar_testes.py --modo unico --tx COM5 --rx COM6
```

**Dois computadores** (uma placa em cada):

```bash
# computador B, antes de começar
py coletar_testes.py --modo rx --rx COM6
# computador A
py coletar_testes.py --modo tx --tx COM5
# no fim: copie a pasta evidencias/RX_... do computador B para o A e junte
py coletar_testes.py --modo juntar --pasta-tx evidencias/TX_... --pasta-rx evidencias/RX_...
```

Opções: `--rapido` (1 repetição em vez de 3) e `--grupos distancia,janela,erro,ruido,imagem`.

O script pede para posicionar as placas antes de cada distância e para preparar a interferência no teste de ruído. Saída em `evidencias/<data_hora>/`:

| Arquivo | Conteúdo |
|---|---|
| `resultados.csv` | Uma linha por teste (abre direto no Excel) |
| `resumo_por_cenario.csv` | Médias de cada cenário |
| `resumo.txt` | As mesmas tabelas em texto |
| `logs/` | Log do TX e do RX de cada teste |
| `imagens/` | Imagens recebidas |

## 7. Problemas comuns

| Sintoma | Causa provável | O que fazer |
|---|---|---|
| Nada chega, nem de perto | Placas com versões ou taxas diferentes | Conferir a linha `pronto (v4.1) \| RF 1200 bit/s` nas duas |
| RX recebe, mas o TX repete o mesmo quadro (ACK não volta) | Receptor da placa A ou transmissor da placa B com mau contato, ou ruído perto deles | Apertar os módulos; afastar placas de computadores e celulares |
| Falhas intermitentes sem mudar nada | Ruído da alimentação (desktop, carregador ruim) | Usar notebook na bateria ou carregador de tomada de boa qualidade |
| `/status` do TX com `ruido_descartado` parado | Receptor não entrega nada ao GPIO34 | Conferir VCC 5 V, divisor e o pino DATA |
| LCD só com quadrados | Contraste | Girar o potenciômetro |
| Monitor Serial vazio | Porta ocupada pelo script | Fechar o script ou o outro Monitor |
| Erro de compilação | `protocolo_rf.h` antigo na pasta do sketch | Copiar o `protocolo_rf.h` do repositório para as duas pastas |
