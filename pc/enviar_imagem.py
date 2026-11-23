"""
enviar_imagem.py - Envia uma imagem do notebook A para a ESP32 A (transmissor).

Uso:
    python enviar_imagem.py COM4 imagem.png

- Feche o Monitor Serial da Arduino IDE antes (a porta so pode ter um dono).
- A imagem precisa ter no maximo 4095 bytes (ex.: 32x32 PNG ou JPEG pequeno).
- O script mostra o log do TX e termina quando aparece MENSAGEM ENTREGUE ou FALHA.

Requer: pip install pyserial
"""
import hashlib
import sys
import time

import serial

MAX_BYTES = 4095


def abre_porta(nome):
    porta = serial.Serial()
    porta.port = nome
    porta.baudrate = 115200
    porta.timeout = 0.5
    porta.dtr = False   # evita reiniciar a ESP32 ao abrir a porta
    porta.rts = False
    porta.open()
    time.sleep(2)
    porta.reset_input_buffer()
    return porta


def espera_linha(porta, texto, limite_s):
    fim = time.time() + limite_s
    while time.time() < fim:
        linha = porta.readline().decode(errors="replace").strip()
        if linha:
            print("TX>", linha)
        if texto in linha:
            return linha
    return None


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    nome_porta, caminho = sys.argv[1], sys.argv[2]

    with open(caminho, "rb") as arq:
        dados = arq.read()
    if not 1 <= len(dados) <= MAX_BYTES:
        sys.exit(f"ERRO: a imagem tem {len(dados)} bytes; o maximo e {MAX_BYTES}.")

    print(f"Imagem: {caminho} | {len(dados)} bytes | SHA-256 {hashlib.sha256(dados).hexdigest()}")
    porta = abre_porta(nome_porta)

    porta.write(f"/imagem {len(dados)}\n".encode())
    if not espera_linha(porta, "PRONTO_IMAGEM", 5):
        sys.exit("ERRO: o TX nao respondeu ao /imagem. Confira a porta e se o tx.ino novo esta gravado.")

    porta.write(dados)
    porta.flush()
    print("Bytes enviados ao TX. Transmitindo pelo radio...")

    inicio = time.time()
    while True:
        linha = porta.readline().decode(errors="replace").strip()
        if linha:
            print("TX>", linha)
        if "MENSAGEM ENTREGUE" in linha:
            print(f"OK: imagem entregue em {time.time() - inicio:.1f} s")
            break
        if "FALHA" in linha or "ERRO" in linha:
            print("A transmissao falhou. Veja o log acima.")
            break
    porta.close()


if __name__ == "__main__":
    main()
