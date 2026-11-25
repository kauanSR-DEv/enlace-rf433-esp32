"""
coletar_testes.py - Roda a bateria de testes do enlace RF 433 MHz e salva as evidencias.

Feche os Monitores Seriais da Arduino IDE antes. Requer: pip install pyserial

DOIS COMPUTADORES (uma placa em cada notebook):
  1) Notebook B (placa RX), ANTES de comecar:
        python coletar_testes.py --modo rx --rx COM3
     Ele grava tudo o que o RX recebe e confere cada texto/imagem. Ctrl+C encerra.
  2) Notebook A (placa TX):
        python coletar_testes.py --modo tx --tx COM4
     Roda a bateria de testes (pede para posicionar as placas antes de cada grupo).
  3) No fim, copie a pasta gerada no notebook B (evidencias/RX_...) para o notebook A
     (pendrive, WhatsApp, Drive) e junte tudo:
        python coletar_testes.py --modo juntar --pasta-tx evidencias/TX_... --pasta-rx evidencias/RX_...
     Os testes sao casados pelo tamanho da mensagem e pelo horario (o script estima
     sozinho a diferenca entre os relogios dos dois notebooks).

UM COMPUTADOR (as duas placas no mesmo notebook):
        python coletar_testes.py --modo unico --tx COM4 --rx COM3

Opcoes: --rapido (1 repeticao por cenario; padrao 3)
        --grupos distancia,janela,erro,ruido,imagem (padrao: todos)

Grupos de teste:
    distancia  /teste 200 a 0,5 m, 1 m, 1,5 m e 2 m (janela 4)
    janela     /teste 200 a 1 m com janela 1 (Stop-and-Wait), 4 e 8
    erro       /erro 30 e /perda 30 a 1 m
    ruido      /teste 500 a 1 m com ruido real (mao na antena / controle remoto)
    imagem     imagem de teste sem erro e com /erro 30
    alcance    /teste 200 a 3,4 m (janela 4); so roda se pedido em --grupos

Saida final (modo unico ou juntar):
    resultados.csv            uma linha por teste (abre direto no Excel)
    resumo_por_cenario.csv    media das repeticoes de cada cenario
    resumo.txt                as mesmas tabelas em texto
    logs/                     log do TX e do RX de cada teste
    imagens/                  imagens recebidas
"""
import argparse
import csv
import hashlib
import os
import re
import sys
import threading
import time
from datetime import datetime

import serial

# ---------------------------------------------------------------------------
# Plano de testes
# ---------------------------------------------------------------------------
GRUPOS = {
    "distancia": {
        "titulo": "Teste de distancia",
        "cenarios": [
            {"nome": "distancia_0.5m", "distancia": 0.5, "janela": 4, "acao": "/teste 200"},
            {"nome": "distancia_1.0m", "distancia": 1.0, "janela": 4, "acao": "/teste 200"},
            {"nome": "distancia_1.5m", "distancia": 1.5, "janela": 4, "acao": "/teste 200"},
            {"nome": "distancia_2.0m", "distancia": 2.0, "janela": 4, "acao": "/teste 200"},
        ],
    },
    "alcance": {
        "titulo": "Alcance maximo (apos o ajuste de sintonia do receptor)",
        "cenarios": [
            {"nome": "distancia_3.4m", "distancia": 3.4, "janela": 4, "acao": "/teste 200"},
        ],
    },
    "janela": {
        "titulo": "Comparacao de janelas (1 = Stop-and-Wait)",
        "cenarios": [
            {"nome": "janela_1_SAW", "distancia": 1.0, "janela": 1, "acao": "/teste 200"},
            {"nome": "janela_4", "distancia": 1.0, "janela": 4, "acao": "/teste 200"},
            {"nome": "janela_8", "distancia": 1.0, "janela": 8, "acao": "/teste 200"},
        ],
    },
    "erro": {
        "titulo": "Erros e perdas simulados",
        "cenarios": [
            {"nome": "erro_sim_30", "distancia": 1.0, "janela": 4, "erro": 30, "acao": "/teste 200"},
            {"nome": "perda_sim_30", "distancia": 1.0, "janela": 4, "perda": 30, "acao": "/teste 200"},
        ],
    },
    "ruido": {
        "titulo": "Ruido real",
        "aviso": "Durante o teste, encoste a mao na antena do RX, aproxime um controle remoto de 433 MHz "
                 "ou um celular. Mexa durante a transmissao toda.",
        "cenarios": [
            {"nome": "ruido_real", "distancia": 1.0, "janela": 4, "acao": "/teste 500"},
        ],
    },
    "imagem": {
        "titulo": "Transmissao de imagem",
        "cenarios": [
            {"nome": "imagem_limpa", "distancia": 1.0, "janela": 4, "acao": "imagem"},
            {"nome": "imagem_erro_30", "distancia": 1.0, "janela": 4, "erro": 30, "acao": "imagem"},
        ],
    },
}

TEMPO_MAXIMO_S = 240          # limite para um teste terminar
ESPERA_APOS_FALHA_S = 27      # o TX bloqueia 26 s (DESCARTE_RX_MS) depois de abandonar uma mensagem


# ---------------------------------------------------------------------------
# Comunicacao com as placas
# ---------------------------------------------------------------------------
class Placa:
    """Abre a porta, le linhas numa thread e guarda tudo com o horario."""

    def __init__(self, nome, porta):
        self.nome = nome
        self.ser = serial.Serial()
        self.ser.port = porta
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        self.ser.dtr = False   # nao reinicia a ESP32 ao abrir a porta
        self.ser.rts = False
        self.ser.open()
        self.linhas = []       # (horario, texto)
        self.trava = threading.Lock()
        self.ativa = True
        threading.Thread(target=self._le, daemon=True).start()

    def _le(self):
        buf = b""
        while self.ativa:
            try:
                dado = self.ser.read(256)
            except serial.SerialException:
                break
            if not dado:
                continue
            buf += dado
            while b"\n" in buf:
                linha, buf = buf.split(b"\n", 1)
                texto = linha.decode(errors="replace").rstrip("\r")
                # descarta lixo da USB (bytes invalidos e caracteres de controle) e limita o tamanho
                texto = "".join(ch for ch in texto if ch == "\t" or (ch >= " " and ch != "\ufffd"))[:2000]
                with self.trava:
                    self.linhas.append((time.time(), texto))

    def marca(self):
        with self.trava:
            return len(self.linhas)

    def desde(self, marca):
        with self.trava:
            return [t for _, t in self.linhas[marca:]]

    def envia(self, texto):
        self.ser.write(texto.encode() + b"\n")
        self.ser.flush()

    def envia_bytes(self, dados):
        self.ser.write(dados)
        self.ser.flush()

    def espera(self, marca, padrao, limite_s):
        """Espera uma linha que contenha 'padrao' (texto ou regex compilada)."""
        fim = time.time() + limite_s
        while time.time() < fim:
            for linha in self.desde(marca):
                if (padrao.search(linha) if hasattr(padrao, "search") else padrao in linha):
                    return linha
            time.sleep(0.05)
        return None

    def fecha(self):
        self.ativa = False
        time.sleep(0.3)
        self.ser.close()


# ---------------------------------------------------------------------------
# Leitura dos logs
# ---------------------------------------------------------------------------
RE_FIM_TX = re.compile(r"=== (MENSAGEM ENTREGUE|FALHA: MENSAGEM ABANDONADA)")


def numeros(linha):
    """Extrai pares chave=valor numericos de uma linha de log."""
    return {k: float(v) for k, v in re.findall(r"([A-Za-z_]+)=([\d.]+)", linha)}


def le_fim_tx(linha):
    d = {"resultado": "ENTREGUE" if "ENTREGUE" in linha else "FALHA"}
    m = re.search(r"em (\d+) ms", linha)
    d["tempo_ms"] = int(m.group(1)) if m else ""
    m = re.search(r"([\d.]+) bytes/s", linha)
    d["bytes_por_s"] = float(m.group(1)) if m else ""
    n = numeros(linha)
    d["envios"] = int(n.get("envios", 0))
    d["retransmissoes"] = int(n.get("retransmissoes", 0))
    d["retrans_por_ack"] = int(n["ACK"]) if "ACK" in n else ""          # v4 (Selective Repeat)
    d["retrans_por_timeout"] = int(n["timeout"]) if "timeout" in n else int(n.get("timeouts", 0))
    d["acks_recebidos"] = int(n.get("ACKs", 0))
    d["acks_ignorados"] = int(n.get("ignorados", 0))
    d["erros_simulados"] = int(n.get("erros_sim", 0))
    d["perdas_simuladas"] = int(n.get("perdas_sim", 0))
    return d


CAMPOS_RX = ["aceitos", "guardados_fora_de_ordem", "duplicados", "fora_da_janela",
             "fora_de_ordem", "erros_FCS", "truncados", "ACKs", "ruido_descartado"]


def le_status_rx(linhas):
    for linha in reversed(linhas):
        if "STATUS:" in linha:
            n = numeros(linha)
            return {k: int(n[k]) for k in CAMPOS_RX if k in n}
    return {}


def texto_teste(n):
    return "".join(chr(ord("A") + i % 26) for i in range(n))


# ---------------------------------------------------------------------------
# Execucao de um teste
# ---------------------------------------------------------------------------
def configura(tx, cen):
    for cmd in (f"/janela {cen.get('janela', 4)}", f"/erro {cen.get('erro', 0)}",
                f"/perda {cen.get('perda', 0)}"):
        tx.envia(cmd)
        time.sleep(0.25)


def roda_teste(tx, rx, cen, imagem, pasta_img, nome_arquivo):
    marca_tx, marca_rx = tx.marca(), (rx.marca() if rx else 0)
    configura(tx, cen)
    res = {"sha_original": "", "sha_recebido": "", "integridade_rx": "NAO_RECEBIDO" if rx else "",
           "inicio_epoch": round(time.time(), 3)}

    if cen["acao"] == "imagem":
        tx.envia(f"/imagem {len(imagem)}")
        if not tx.espera(marca_tx, "PRONTO_IMAGEM", 5):
            res["resultado"] = "ERRO_TX"
            res["fim_epoch"] = round(time.time(), 3)
            res["status_rx"] = {}
            return res, tx.desde(marca_tx), (rx.desde(marca_rx) if rx else [])
        tx.envia_bytes(imagem)
        res["tamanho_bytes"] = len(imagem)
        res["sha_original"] = hashlib.sha256(imagem).hexdigest()
    else:
        tx.envia(cen["acao"])
        n = int(cen["acao"].split()[1]) if cen["acao"].startswith("/teste") else len(cen["acao"])
        res["tamanho_bytes"] = n

    linha_fim = tx.espera(marca_tx, RE_FIM_TX, TEMPO_MAXIMO_S)
    res["fim_epoch"] = round(time.time(), 3)
    if linha_fim:
        res.update(le_fim_tx(linha_fim))
    else:
        res["resultado"] = "TIMEOUT_SCRIPT"
        tx.envia("/cancela")
    time.sleep(2.5)                       # ultimas linhas do RX (ACK do END, estatisticas)
    if rx is None:
        res["status_rx"] = {}
        return res, tx.desde(marca_tx), []
    marca_status = rx.marca()
    rx.envia("/status")
    rx.espera(marca_status, "STATUS:", 3)
    time.sleep(0.3)

    linhas_tx, linhas_rx = tx.desde(marca_tx), rx.desde(marca_rx)

    # Integridade: o que o RX entregou e igual ao que foi enviado?
    if cen["acao"] == "imagem":
        if "IMG_INICIO" in "\n".join(linhas_rx):
            i = max(k for k, l in enumerate(linhas_rx) if l.startswith("IMG_INICIO"))
            hexa = []
            for l in linhas_rx[i + 1:]:
                if l.strip() == "IMG_FIM":
                    break
                hexa.append(l.strip())
            try:
                recebido = bytes.fromhex("".join(hexa))
                res["sha_recebido"] = hashlib.sha256(recebido).hexdigest()
                res["integridade_rx"] = "OK" if res["sha_recebido"] == res["sha_original"] else "DIFERENTE"
                with open(os.path.join(pasta_img, nome_arquivo + ".png"), "wb") as f:
                    f.write(recebido)
            except ValueError:
                res["integridade_rx"] = "ERRO_HEX"
    else:
        esperado = texto_teste(res["tamanho_bytes"])
        textos = [l[len("TEXTO: "):] for l in linhas_rx if l.startswith("TEXTO: ")]
        if textos:
            res["integridade_rx"] = "OK" if textos[-1] == esperado else "DIFERENTE"
            if len(textos) > 1:
                res["integridade_rx"] += "_DUPLICADO"

    res["status_rx"] = le_status_rx(linhas_rx)
    return res, linhas_tx, linhas_rx


# ---------------------------------------------------------------------------
# Saida
# ---------------------------------------------------------------------------
COLUNAS = ["id", "grupo", "cenario", "repeticao", "distancia_m", "janela", "erro_sim_pct", "perda_sim_pct",
           "tamanho_bytes", "resultado", "integridade_rx", "tempo_ms", "bytes_por_s", "envios",
           "retransmissoes", "retrans_por_ack", "retrans_por_timeout", "acks_recebidos", "acks_ignorados",
           "erros_simulados", "perdas_simuladas", "rx_aceitos", "rx_guardados_fora_de_ordem",
           "rx_duplicados", "rx_erros_FCS", "rx_truncados", "rx_ACKs_enviados", "sha_original",
           "sha_recebido", "observacao", "log_tx", "log_rx"]


def br(v):
    """Numero no formato do Excel em portugues (virgula decimal)."""
    if isinstance(v, float):
        return f"{v:.2f}".replace(".", ",")
    return v


def grava_csv(caminho, linhas, colunas):
    with open(caminho, "w", newline="", encoding="utf-8-sig") as f:   # BOM: Excel abre os acentos certo
        w = csv.writer(f, delimiter=";")
        w.writerow(colunas)
        for l in linhas:
            w.writerow([br(l.get(c, "")) for c in colunas])


def resumo_por_cenario(resultados):
    ordem, grupos = [], {}
    for r in resultados:
        if r["cenario"] not in grupos:
            ordem.append(r["cenario"])
            grupos[r["cenario"]] = []
        grupos[r["cenario"]].append(r)
    saida = []
    for c in ordem:
        rs = grupos[c]
        ok = [r for r in rs if r.get("resultado") == "ENTREGUE" and r.get("integridade_rx", "").startswith("OK")]

        def media(campo):
            vals = [r[campo] for r in ok if isinstance(r.get(campo), (int, float))]
            return round(sum(vals) / len(vals), 2) if vals else ""
        saida.append({
            "cenario": c, "distancia_m": rs[0]["distancia_m"], "janela": rs[0]["janela"],
            "erro_sim_pct": rs[0]["erro_sim_pct"], "perda_sim_pct": rs[0]["perda_sim_pct"],
            "repeticoes": len(rs), "sucessos": len(ok),
            "tempo_medio_s": round(media("tempo_ms") / 1000, 2) if media("tempo_ms") != "" else "",
            "bytes_por_s_medio": media("bytes_por_s"), "retransmissoes_media": media("retransmissoes"),
            "rx_erros_FCS_media": media("rx_erros_FCS"),
        })
    return saida


COLUNAS_RESUMO = ["cenario", "distancia_m", "janela", "erro_sim_pct", "perda_sim_pct", "repeticoes", "sucessos",
                  "tempo_medio_s", "bytes_por_s_medio", "retransmissoes_media", "rx_erros_FCS_media"]


def tabela_texto(linhas, colunas):
    larg = {c: max(len(c), *(len(str(br(l.get(c, "")))) for l in linhas)) for c in colunas}
    out = ["  ".join(c.ljust(larg[c]) for c in colunas)]
    out.append("  ".join("-" * larg[c] for c in colunas))
    for l in linhas:
        out.append("  ".join(str(br(l.get(c, ""))).ljust(larg[c]) for c in colunas))
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Modo bateria (unico computador ou so o TX)
# ---------------------------------------------------------------------------
COLUNAS_TX = COLUNAS + ["inicio_epoch", "fim_epoch"]


def identifica(placa, rotulo):
    m = placa.marca()
    placa.envia("/status")
    st = placa.espera(m, "STATUS:", 3)
    if st:
        st = st[st.rfind("STATUS:"):]   # ignora o que veio grudado antes da linha
    if not st:
        print(f"ATENCAO: a placa {rotulo} nao respondeu ao /status. Confira a porta e o firmware.")
    return st or "(sem resposta)"


def grava_saida_final(pasta, resultados, versao):
    resumo = resumo_por_cenario(resultados)
    grava_csv(os.path.join(pasta, "resultados.csv"), resultados, COLUNAS)
    grava_csv(os.path.join(pasta, "resumo_por_cenario.csv"), resumo, COLUNAS_RESUMO)
    colunas_curtas = ["id", "cenario", "repeticao", "resultado", "integridade_rx", "tempo_ms",
                      "bytes_por_s", "retransmissoes", "rx_erros_FCS", "observacao"]
    with open(os.path.join(pasta, "resumo.txt"), "w", encoding="utf-8") as f:
        f.write(f"Bateria de testes - {datetime.now():%d/%m/%Y %H:%M} - {versao}\n\n")
        f.write("RESUMO POR CENARIO (media dos testes entregues e integros)\n")
        f.write(tabela_texto(resumo, COLUNAS_RESUMO) + "\n\nTODOS OS TESTES\n")
        f.write(tabela_texto(resultados, colunas_curtas) + "\n")
    print("\n" + tabela_texto(resumo, COLUNAS_RESUMO))


def modo_bateria(args, porta_tx, porta_rx):
    reps = args.repeticoes or (1 if args.rapido else 3)
    grupos = [g.strip() for g in args.grupos.split(",") if g.strip()]
    for g in grupos:
        if g not in GRUPOS:
            sys.exit(f"Grupo desconhecido: {g}. Use: {', '.join(GRUPOS)}")

    imagem = b""
    if "imagem" in grupos:
        if not os.path.exists(args.imagem):
            sys.exit(f"Imagem nao encontrada: {args.imagem}")
        with open(args.imagem, "rb") as f:
            imagem = f.read()
        if len(imagem) > 4095:
            sys.exit(f"A imagem tem {len(imagem)} bytes; o maximo e 4095.")

    so_tx = porta_rx is None
    pasta = os.path.join(args.saida, ("TX_" if so_tx else "") + datetime.now().strftime("%Y-%m-%d_%H%M"))
    pasta_logs, pasta_img = os.path.join(pasta, "logs"), os.path.join(pasta, "imagens")
    os.makedirs(pasta_logs, exist_ok=True)
    os.makedirs(pasta_img, exist_ok=True)

    print("Abrindo as portas (feche os Monitores Seriais da Arduino IDE)...")
    tx = Placa("TX", porta_tx)
    rx = None if so_tx else Placa("RX", porta_rx)
    time.sleep(2)
    st_tx = identifica(tx, "TX")
    st_rx = identifica(rx, "RX") if rx else "(no outro notebook)"
    versao = "v4 Selective Repeat" if "Selective Repeat" in st_tx else "v3 Go-Back-N (ou anterior)"
    print(f"TX: {st_tx}\nRX: {st_rx}\nVersao detectada: {versao}\n")
    if so_tx:
        input(">> O script do RX (--modo rx) ja esta rodando no outro notebook? Tecle Enter para comecar...")
    with open(os.path.join(pasta, "sessao.txt"), "w", encoding="utf-8") as f:
        f.write(f"Data: {datetime.now():%d/%m/%Y %H:%M}\nModo: {'tx (dois computadores)' if so_tx else 'unico'}\n"
                f"Porta TX: {porta_tx}\nPorta RX: {porta_rx or '-'}\nVersao: {versao}\n"
                f"STATUS TX: {st_tx}\nSTATUS RX: {st_rx}\nRepeticoes: {reps}\nGrupos: {', '.join(grupos)}\n"
                f"Imagem: {args.imagem if imagem else '-'}\n")

    resultados = []
    status_anterior = le_status_rx([st_rx])
    contador = 0
    try:
        for g in grupos:
            grupo = GRUPOS[g]
            print("=" * 70 + f"\n{grupo['titulo'].upper()}\n" + "=" * 70)
            distancia_atual = None
            for cen in grupo["cenarios"]:
                if cen["distancia"] != distancia_atual:
                    input(f"\n>> Posicione as placas a {str(cen['distancia']).replace('.', ',')} m "
                          f"(antenas em pe) e tecle Enter...")
                    distancia_atual = cen["distancia"]
                if grupo.get("aviso"):
                    input(f">> {grupo['aviso']}\n   Tecle Enter quando estiver pronto...")
                for k in range(1, reps + 1):
                    contador += 1
                    nome = f"{contador:02d}_{cen['nome']}_rep{k}"
                    print(f"\n[{nome}] rodando...", flush=True)
                    res, ltx, lrx = roda_teste(tx, rx, cen, imagem, pasta_img, nome)

                    with open(os.path.join(pasta_logs, nome + "_TX.txt"), "w", encoding="utf-8") as f:
                        f.write("\n".join(ltx) + "\n")
                    if rx:
                        with open(os.path.join(pasta_logs, nome + "_RX.txt"), "w", encoding="utf-8") as f:
                            f.write("\n".join(lrx) + "\n")

                    # Contadores do RX sao acumulados desde que a placa ligou: guarda a diferenca
                    st = res.pop("status_rx")
                    delta = {c: st.get(c, 0) - status_anterior.get(c, 0) for c in st}
                    if any(v < 0 for v in delta.values()):     # RX reiniciou no meio
                        delta = st
                    status_anterior = st or status_anterior

                    linha = {
                        "id": contador, "grupo": g, "cenario": cen["nome"], "repeticao": k,
                        "distancia_m": float(cen["distancia"]), "janela": cen.get("janela", 4),
                        "erro_sim_pct": cen.get("erro", 0), "perda_sim_pct": cen.get("perda", 0),
                        "rx_aceitos": delta.get("aceitos", ""),
                        "rx_guardados_fora_de_ordem": delta.get("guardados_fora_de_ordem", delta.get("fora_de_ordem", "")),
                        "rx_duplicados": delta.get("duplicados", ""), "rx_erros_FCS": delta.get("erros_FCS", ""),
                        "rx_truncados": delta.get("truncados", ""), "rx_ACKs_enviados": delta.get("ACKs", ""),
                        "log_tx": nome + "_TX.txt", "log_rx": (nome + "_RX.txt") if rx else "",
                    }
                    linha.update(res)
                    tempo = linha.get("tempo_ms")
                    print(f"   resultado={linha.get('resultado')}"
                          + (f"  integridade={linha.get('integridade_rx')}" if rx else "")
                          + f"  tempo={tempo / 1000 if isinstance(tempo, int) else '-'} s"
                          + f"  retransmissoes={linha.get('retransmissoes', '-')}"
                          + (f"  erros_FCS_no_RX={linha.get('rx_erros_FCS', '-')}" if rx else ""))
                    if linha.get("sha_original") and rx:
                        print(f"   SHA-256 original={linha['sha_original'][:16]}...  "
                              f"recebido={(linha.get('sha_recebido') or '-')[:16]}...")
                    linha["observacao"] = input("   Observacao (Enter pula): ").strip()
                    resultados.append(linha)

                    # salva a cada teste: se algo travar, nada se perde
                    grava_csv(os.path.join(pasta, "resultados_tx.csv" if so_tx else "resultados.csv"),
                              resultados, COLUNAS_TX if so_tx else COLUNAS)

                    if linha.get("resultado") != "ENTREGUE":
                        print(f"   (aguardando {ESPERA_APOS_FALHA_S} s: o TX bloqueia apos uma falha)")
                        time.sleep(ESPERA_APOS_FALHA_S)
    except KeyboardInterrupt:
        print("\nInterrompido. Salvando o que foi coletado...")
    finally:
        tx.envia("/erro 0")
        tx.envia("/perda 0")
        tx.envia("/janela 4")
        time.sleep(0.5)
        tx.fecha()
        if rx:
            rx.fecha()

    if not resultados:
        return
    if so_tx:
        grava_csv(os.path.join(pasta, "resultados_tx.csv"), resultados, COLUNAS_TX)
        print(f"\nLado TX salvo em: {os.path.abspath(pasta)}")
        print("Agora encerre o script do RX (Ctrl+C), traga a pasta dele para este notebook e rode:")
        print(f'  python coletar_testes.py --modo juntar --pasta-tx "{pasta}" --pasta-rx "<pasta RX_...>"')
    else:
        grava_saida_final(pasta, resultados, versao)
        print(f"\nTudo salvo em: {os.path.abspath(pasta)}")


# ---------------------------------------------------------------------------
# Modo rx (notebook B): grava tudo e confere cada mensagem recebida
# ---------------------------------------------------------------------------
COLUNAS_MSG = ["n", "epoch", "horario", "tipo", "tamanho_bytes", "integridade", "sha_recebido", "arquivo",
               "aceitos", "guardados_fora_de_ordem", "fora_de_ordem", "duplicados", "erros_FCS", "truncados", "ACKs"]


def modo_rx(args):
    pasta = os.path.join(args.saida, "RX_" + datetime.now().strftime("%Y-%m-%d_%H%M"))
    pasta_img = os.path.join(pasta, "imagens")
    os.makedirs(pasta_img, exist_ok=True)
    print("Abrindo a porta do RX (feche o Monitor Serial da Arduino IDE)...")
    rx = Placa("RX", args.rx)
    time.sleep(2)
    st = identifica(rx, "RX")
    print(f"RX: {st}")
    with open(os.path.join(pasta, "sessao.txt"), "w", encoding="utf-8") as f:
        f.write(f"Data: {datetime.now():%d/%m/%Y %H:%M}\nModo: rx\nPorta RX: {args.rx}\nSTATUS RX: {st}\n")
    print(f"\nGravando em {os.path.abspath(pasta)}\nPode comecar os testes no notebook do TX. Ctrl+C encerra.\n")

    log = open(os.path.join(pasta, "rx_log_completo.txt"), "w", encoding="utf-8")
    mensagens = []
    pendente = None          # mensagem esperando a linha de estatisticas
    img_hex, img_tam = None, 0
    lido = 0
    try:
        while True:
            with rx.trava:
                novas = rx.linhas[lido:]
            lido += len(novas)
            for epoch, linha in novas:
                log.write(f"{epoch:.3f}\t{linha}\n")
                log.flush()
                if linha.startswith("TEXTO: "):
                    texto = linha[len("TEXTO: "):]
                    padrao = texto_teste(len(texto))
                    pendente = {"epoch": round(epoch, 3), "tipo": "texto", "tamanho_bytes": len(texto),
                                "integridade": "OK" if texto == padrao else "TEXTO_LIVRE", "sha_recebido": "", "arquivo": ""}
                elif linha.startswith("IMG_INICIO"):
                    img_hex, img_tam = [], int(linha.split()[1])
                elif img_hex is not None and linha.strip() == "IMG_FIM":
                    try:
                        dados = bytes.fromhex("".join(img_hex))
                        sha = hashlib.sha256(dados).hexdigest()
                        nome = f"rx_{datetime.fromtimestamp(epoch):%H%M%S}.png"
                        with open(os.path.join(pasta_img, nome), "wb") as f:
                            f.write(dados)
                        integ = "OK" if len(dados) == img_tam else "TAMANHO_ERRADO"
                    except ValueError:
                        dados, sha, nome, integ = b"", "", "", "ERRO_HEX"
                    pendente = {"epoch": round(epoch, 3), "tipo": "imagem", "tamanho_bytes": len(dados),
                                "integridade": integ, "sha_recebido": sha, "arquivo": nome}
                    img_hex = None
                elif img_hex is not None:
                    img_hex.append(linha.strip())
                elif "estatisticas:" in linha and pendente:
                    n = numeros(linha)
                    for c in ["aceitos", "guardados_fora_de_ordem", "fora_de_ordem", "duplicados", "erros_FCS", "truncados", "ACKs"]:
                        pendente[c] = int(n[c]) if c in n else ""
                    pendente["n"] = len(mensagens) + 1
                    pendente["horario"] = datetime.fromtimestamp(pendente["epoch"]).strftime("%H:%M:%S")
                    mensagens.append(pendente)
                    grava_csv(os.path.join(pasta, "rx_mensagens.csv"), mensagens, COLUNAS_MSG)
                    print(f"[{pendente['horario']}] mensagem #{pendente['n']}: {pendente['tipo']} "
                          f"{pendente['tamanho_bytes']} bytes -> {pendente['integridade']}"
                          + (f"  SHA-256 {pendente['sha_recebido'][:16]}..." if pendente["sha_recebido"] else ""))
                    pendente = None
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        log.close()
        rx.fecha()
    print(f"\nRX encerrado: {len(mensagens)} mensagens. Pasta: {os.path.abspath(pasta)}")
    print("Copie essa pasta para o notebook do TX e rode o --modo juntar.")


# ---------------------------------------------------------------------------
# Modo juntar: casa os testes do TX com as mensagens do RX
# ---------------------------------------------------------------------------
def le_csv(caminho):
    with open(caminho, encoding="utf-8-sig") as f:
        linhas = list(csv.DictReader(f, delimiter=";"))
    for l in linhas:
        for k, v in l.items():
            if v and re.fullmatch(r"-?\d+", v):
                l[k] = int(v)
            elif v and re.fullmatch(r"-?\d+,\d+", v):
                l[k] = float(v.replace(",", "."))
    return linhas


def modo_juntar(args):
    testes = le_csv(os.path.join(args.pasta_tx, "resultados_tx.csv"))
    msgs = le_csv(os.path.join(args.pasta_rx, "rx_mensagens.csv"))
    log_rx = []
    with open(os.path.join(args.pasta_rx, "rx_log_completo.txt"), encoding="utf-8") as f:
        for l in f:
            t, _, texto = l.rstrip("\n").partition("\t")
            log_rx.append((float(t), texto))

    # Diferenca entre os relogios: o RX entrega a mensagem um pouco antes de o TX
    # receber o ultimo ACK. Pega a diferenca (fim no TX - entrega no RX) mais comum
    # entre todos os pares de mesmo tamanho.
    difs = [t["fim_epoch"] - m["epoch"] for t in testes for m in msgs
            if t.get("tamanho_bytes") == m.get("tamanho_bytes") and isinstance(t.get("fim_epoch"), (int, float))]
    if not difs:
        sys.exit("Nenhuma mensagem do RX tem o mesmo tamanho dos testes do TX. Confira as pastas.")
    caixas = {}
    for d in difs:
        caixas.setdefault(round(d), []).append(d)
    melhor = max(caixas, key=lambda c: len(caixas.get(c - 1, [])) + len(caixas[c]) + len(caixas.get(c + 1, [])))
    perto = caixas.get(melhor - 1, []) + caixas[melhor] + caixas.get(melhor + 1, [])
    deslocamento = sorted(perto)[len(perto) // 2]
    print(f"Diferenca estimada entre os relogios (incluindo o ultimo ACK): {deslocamento:.2f} s")

    pasta = os.path.join(args.saida, "FINAL_" + datetime.now().strftime("%Y-%m-%d_%H%M"))
    pasta_logs, pasta_img = os.path.join(pasta, "logs"), os.path.join(pasta, "imagens")
    os.makedirs(pasta_logs, exist_ok=True)
    os.makedirs(pasta_img, exist_ok=True)

    # Contadores do RX acumulam desde que a placa ligou: o primeiro teste usa como
    # base o STATUS gravado no sessao.txt do RX quando o --modo rx comecou.
    anterior = {}
    try:
        with open(os.path.join(args.pasta_rx, "sessao.txt"), encoding="utf-8", errors="replace") as f:
            anterior = le_status_rx(f.readlines())
    except OSError:
        pass
    usadas, resultados = set(), []
    for t in testes:
        ini_rx, fim_rx = t["inicio_epoch"] - deslocamento - 2, t["fim_epoch"] - deslocamento + 3
        cand = [m for m in msgs if m["n"] not in usadas and m.get("tamanho_bytes") == t.get("tamanho_bytes")
                and ini_rx <= m["epoch"] <= fim_rx]
        m = min(cand, key=lambda m: abs(t["fim_epoch"] - deslocamento - m["epoch"])) if cand else None
        linha = dict(t)
        if m:
            usadas.add(m["n"])
            if t.get("sha_original"):
                linha["sha_recebido"] = m["sha_recebido"]
                linha["integridade_rx"] = "OK" if m["sha_recebido"] == t["sha_original"] else "DIFERENTE"
                if m.get("arquivo"):
                    origem = os.path.join(args.pasta_rx, "imagens", m["arquivo"])
                    if os.path.exists(origem):
                        with open(origem, "rb") as a, open(os.path.join(pasta_img, f"{t['id']:02d}_{t['cenario']}_rep{t['repeticao']}.png"), "wb") as b:
                            b.write(a.read())
            else:
                linha["integridade_rx"] = m["integridade"]
            atual = {c: m[c] for c in ["aceitos", "guardados_fora_de_ordem", "fora_de_ordem", "duplicados", "erros_FCS", "truncados", "ACKs"]
                     if isinstance(m.get(c), int)}
            delta = {c: v - anterior.get(c, 0) for c, v in atual.items()}
            if any(v < 0 for v in delta.values()):
                delta = atual
            anterior = atual
            linha["rx_aceitos"] = delta.get("aceitos", "")
            linha["rx_guardados_fora_de_ordem"] = delta.get("guardados_fora_de_ordem", delta.get("fora_de_ordem", ""))
            linha["rx_duplicados"] = delta.get("duplicados", "")
            linha["rx_erros_FCS"] = delta.get("erros_FCS", "")
            linha["rx_truncados"] = delta.get("truncados", "")
            linha["rx_ACKs_enviados"] = delta.get("ACKs", "")
        else:
            linha["integridade_rx"] = "NAO_RECEBIDO"
        # log do RX daquele teste
        nome = f"{t['id']:02d}_{t['cenario']}_rep{t['repeticao']}"
        trecho = [txt for ep, txt in log_rx if ini_rx <= ep <= fim_rx]
        with open(os.path.join(pasta_logs, nome + "_RX.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(trecho) + "\n")
        origem_tx = os.path.join(args.pasta_tx, "logs", nome + "_TX.txt")
        if os.path.exists(origem_tx):
            with open(origem_tx, encoding="utf-8") as a, open(os.path.join(pasta_logs, nome + "_TX.txt"), "w", encoding="utf-8") as b:
                b.write(a.read())
        linha["log_rx"] = nome + "_RX.txt"
        resultados.append(linha)
        print(f"  {nome}: {linha.get('resultado')} / RX {linha['integridade_rx']}")

    sobras = [m for m in msgs if m["n"] not in usadas]
    if sobras:
        print(f"  {len(sobras)} mensagem(ns) do RX nao casaram com nenhum teste (ex.: textos digitados a mao).")
    with open(os.path.join(args.pasta_tx, "sessao.txt"), encoding="utf-8") as f:
        versao = next((l.split(":", 1)[1].strip() for l in f if l.startswith("Versao:")), "")
    grava_saida_final(pasta, resultados, versao)
    print(f"\nTudo salvo em: {os.path.abspath(pasta)}")


# ---------------------------------------------------------------------------
# Principal
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Bateria de testes do enlace RF 433 MHz")
    ap.add_argument("--modo", choices=["unico", "tx", "rx", "juntar"],
                    help="unico (as duas placas aqui), tx, rx ou juntar. Padrao: deduzido pelas portas.")
    ap.add_argument("--tx", help="porta da placa A (transmissor), ex.: COM4")
    ap.add_argument("--rx", help="porta da placa B (receptor), ex.: COM3")
    ap.add_argument("--pasta-tx", help="(juntar) pasta TX_... gerada no notebook A")
    ap.add_argument("--pasta-rx", help="(juntar) pasta RX_... gerada no notebook B")
    ap.add_argument("--rapido", action="store_true", help="1 repeticao por cenario (padrao: 3)")
    ap.add_argument("--repeticoes", type=int, default=None)
    ap.add_argument("--grupos", default=",".join(g for g in GRUPOS if g != "alcance"), help="ex.: distancia,erro,imagem")
    ap.add_argument("--imagem", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "teste_32x32.png"))
    ap.add_argument("--saida", default="evidencias")
    args = ap.parse_args()

    modo = args.modo or ("unico" if args.tx and args.rx else "tx" if args.tx else "rx" if args.rx else None)
    if modo == "unico" and args.tx and args.rx:
        modo_bateria(args, args.tx, args.rx)
    elif modo == "tx" and args.tx:
        modo_bateria(args, args.tx, None)
    elif modo == "rx" and args.rx:
        modo_rx(args)
    elif modo == "juntar" and args.pasta_tx and args.pasta_rx:
        modo_juntar(args)
    else:
        ap.print_help()
        sys.exit("\nFaltam argumentos. Exemplos:\n"
                 "  notebook B:  python coletar_testes.py --modo rx --rx COM3\n"
                 "  notebook A:  python coletar_testes.py --modo tx --tx COM4\n"
                 "  depois:      python coletar_testes.py --modo juntar --pasta-tx evidencias/TX_... --pasta-rx evidencias/RX_...")


if __name__ == "__main__":
    main()
