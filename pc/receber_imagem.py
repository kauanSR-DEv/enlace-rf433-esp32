"""
receber_imagem.py - Le a ESP32 B (receptor) e salva cada imagem que chegar.

Uso:
    python receber_imagem.py COM3

- Feche o Monitor Serial da Arduino IDE antes (a porta so pode ter um dono).
- O script mostra o log do RX. Quando chega uma imagem, ele salva o arquivo
  na pasta atual (recebida_HHMMSS.png/.jpg/.bmp/...), mostra o SHA-256 para
  comparar com o do arquivo original e tenta abrir a imagem.
- Ctrl+C encerra.

Requer: pip install pyserial
"""
import hashlib
import os
import sys
import time

import serial


def extensao(dados):
    if dados.startswith(b"\x89PNG"):
        return ".png"
    if dados.startswith(b"\xff\xd8"):
        return ".jpg"
    if dados.startswith(b"BM"):
        return ".bmp"
    if dados.startswith(b"GIF8"):
        return ".gif"
    return ".bin"


def abre_porta(nome):
    porta = serial.Serial()
    porta.port = nome
    porta.baudrate = 115200
    porta.timeout = 0.5
    porta.dtr = False   # evita reiniciar a ESP32 ao abrir a porta
    porta.rts = False
    porta.open()
    return porta


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    porta = abre_porta(sys.argv[1])
    print("Aguardando imagens... (Ctrl+C para sair)")

    esperado, hexa = None, []
    try:
        while True:
            linha = porta.readline().decode(errors="replace").strip()
            if not linha:
                continue
            if linha.startswith("IMG_INICIO"):
                esperado, hexa = int(linha.split()[1]), []
                continue
            if esperado is not None and linha != "IMG_FIM":
                hexa.append(linha)
                continue
            if linha == "IMG_FIM" and esperado is not None:
                dados = bytes.fromhex("".join(hexa))
                if len(dados) != esperado:
                    print(f"ERRO: esperava {esperado} bytes e recebi {len(dados)}")
                else:
                    nome = time.strftime("recebida_%H%M%S") + extensao(dados)
                    with open(nome, "wb") as arq:
                        arq.write(dados)
                    print(f"IMAGEM SALVA: {nome} | {len(dados)} bytes | "
                          f"SHA-256 {hashlib.sha256(dados).hexdigest()}")
                    try:
                        os.startfile(nome)          # Windows: abre a imagem
                    except AttributeError:
                        pass
                esperado, hexa = None, []
                continue
            print("RX>", linha)
    except KeyboardInterrupt:
        pass
    finally:
        porta.close()


if __name__ == "__main__":
    main()
