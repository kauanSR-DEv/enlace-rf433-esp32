# Firmware

| Pasta | Placa | Arquivos |
|---|---|---|
| `tx/` | A — transmissor | `tx.ino` + `protocolo_rf.h` |
| `rx/` | B — receptor com LCD | `rx.ino` + `protocolo_rf.h` |

As duas cópias de `protocolo_rf.h` são **idênticas** (a Arduino IDE só enxerga arquivos dentro da pasta do sketch). Ele contém tudo o que precisa ser igual nos dois lados: taxa, tempos, formato do quadro, CRC-8, Manchester e o receptor de linha.

Arduino IDE 2 · pacote esp32 (Espressif) · placa *ESP32 Dev Module* · biblioteca LiquidCrystal (inclusa na IDE). Para testar o RX sem LCD, use `#define USAR_LCD 0` em `rx.ino`.

Para conferir se as cópias estão iguais:

```bash
diff firmware/tx/protocolo_rf.h firmware/rx/protocolo_rf.h && echo iguais
```

Versão atual: **v4.1** (veja o [CHANGELOG](../CHANGELOG.md)).
