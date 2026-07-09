#!/usr/bin/env python3
"""
Responder de descoberta UDP para o robô (Raspberry Pi).
Versão: v1

Escuta requisições "RDISCOVER <nonce>" em UDP e responde, por unicast,
"ROBOT <ws_url> <hmac>" onde hmac = HMAC-SHA256(ROBOT_SECRET, nonce)[:16].

- Não toca no servidor WebSocket de áudio (porta 8765).
- Roda como thread paralela (start_discovery) ou como processo standalone.
- Sem dependências externas: só stdlib.

Uso embutido no seu servidor:
    from discovery_beacon import start_discovery
    start_discovery(ws_port=8765, secret="TROQUE_ESTE_SEGREDO_COMPARTILHADO")

Uso standalone:
    python3 discovery_beacon.py
"""

import hashlib
import hmac
import socket
import struct
import threading

# ============================================================
# CONFIGURAÇÕES (devem casar com o firmware)
# ============================================================

DISCOVERY_PORT = 8766          # porta onde o servidor escuta RDISCOVER
WS_PORT = 8765                 # porta do WebSocket de áudio a anunciar
ROBOT_SECRET = "TROQUE_ESTE_SEGREDO_COMPARTILHADO"  # == ROBOT_SECRET do .ino
HMAC_HEX_LEN = 16              # 8 bytes -> 16 hex (== DISCO_HMAC_HEX_LEN)


# ============================================================
# UTILITÁRIOS
# ============================================================

def compute_hmac_hex(secret: str, nonce: str, hex_len: int = HMAC_HEX_LEN) -> str:
    """HMAC-SHA256(secret, nonce) truncado em hex_len caracteres hex."""
    digest = hmac.new(secret.encode("utf-8"), nonce.encode("utf-8"),
                      hashlib.sha256).hexdigest()
    return digest[:hex_len]


def _local_ip_for(dest_ip: str) -> str:
    """Descobre o IP local usado para alcançar dest_ip (sem enviar nada)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((dest_ip, 1))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


# ============================================================
# LOOP DE DESCOBERTA
# ============================================================

def _discovery_loop(ws_port: int, secret: str, discovery_port: int,
                    stop_event: threading.Event):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Habilita recepção de broadcast direcionado ao 255.255.255.255.
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", discovery_port))
    sock.settimeout(1.0)

    print(f"[discovery] escutando UDP :{discovery_port}, anunciando WS :{ws_port}")

    while not stop_event.is_set():
        try:
            data, addr = sock.recvfrom(256)
        except socket.timeout:
            continue
        except OSError:
            break

        try:
            text = data.decode("utf-8", errors="ignore").strip()
        except Exception:
            continue

        parts = text.split()
        if len(parts) != 2 or parts[0] != "RDISCOVER":
            continue

        nonce = parts[1]
        # Validação simples do nonce (evita responder a lixo).
        if not (1 <= len(nonce) <= 32) or not all(
            c in "0123456789abcdefABCDEF" for c in nonce
        ):
            continue

        client_ip = addr[0]
        ws_url = f"ws://{_local_ip_for(client_ip)}:{ws_port}"
        mac = compute_hmac_hex(secret, nonce)
        reply = f"ROBOT {ws_url} {mac}".encode("utf-8")

        try:
            sock.sendto(reply, addr)
            print(f"[discovery] {client_ip} <- {ws_url} (nonce={nonce})")
        except OSError as e:
            print(f"[discovery] falha ao responder {client_ip}: {e}")

    sock.close()
    print("[discovery] encerrado")


# ============================================================
# API PÚBLICA
# ============================================================

def start_discovery(ws_port: int = WS_PORT, secret: str = ROBOT_SECRET,
                    discovery_port: int = DISCOVERY_PORT) -> threading.Event:
    """
    Inicia o responder de descoberta numa thread daemon.
    Retorna um threading.Event; chame .set() para encerrar.
    """
    stop_event = threading.Event()
    t = threading.Thread(
        target=_discovery_loop,
        args=(ws_port, secret, discovery_port, stop_event),
        daemon=True,
        name="discovery-beacon",
    )
    t.start()
    return stop_event


# ============================================================
# STANDALONE
# ============================================================

if __name__ == "__main__":
    stop = start_discovery()
    try:
        # Mantém o processo vivo enquanto a thread daemon roda.
        threading.Event().wait()
    except KeyboardInterrupt:
        stop.set()
        print("\n[discovery] interrompido pelo usuario")
