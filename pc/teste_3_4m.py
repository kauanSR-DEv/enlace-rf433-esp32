"""
Teste de alcance a 3,4 m: /teste 200, janela 4, 3 repeticoes.

Usa o coletar_testes.py (precisa estar na mesma pasta) com o grupo "alcance".

Uma placa em cada notebook:
    notebook B (placa B, primeiro):  py teste_3_4m.py rx COM5
    notebook A (placa A, depois):    py teste_3_4m.py tx COM8
    no fim, Ctrl+C no notebook B e mande as pastas TX_... e RX_... geradas em evidencias/

As duas placas no mesmo notebook:
    py teste_3_4m.py unico COM8 COM5      (primeiro a porta da placa A, depois a da B)

Feche o Monitor Serial antes de rodar.
"""
import sys

import coletar_testes

USO = __doc__


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("tx", "rx", "unico"):
        sys.exit(USO)
    modo = args[0]
    if modo == "tx" and len(args) == 2:
        sys.argv = ["coletar_testes.py", "--modo", "tx", "--tx", args[1], "--grupos", "alcance"]
    elif modo == "rx" and len(args) == 2:
        sys.argv = ["coletar_testes.py", "--modo", "rx", "--rx", args[1]]
    elif modo == "unico" and len(args) == 3:
        sys.argv = ["coletar_testes.py", "--modo", "unico", "--tx", args[1], "--rx", args[2],
                    "--grupos", "alcance"]
    else:
        sys.exit(USO)
    coletar_testes.main()


if __name__ == "__main__":
    main()
