#!/usr/bin/env python3
"""Servidor persistente do robô experimental.

Fluxo:
- recebe PING/PONG, RECORD_START, bytes de áudio e RECORD_END;
- salva RAW/WAV em `server/mic_tests/`;
- converte para 16 kHz mono para Whisper;
- carrega `faster-whisper` uma única vez no início;
- gera resposta simples;
- opcionalmente gera fala com Piper e envia para o Cardputer.
"""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import hmac
import ipaddress
import json
import os
import socket
import struct
import subprocess
import time
import wave
from pathlib import Path
from typing import Any
from urllib import error, request

try:
    import fcntl                               # só existe em POSIX
except ImportError:                            # pragma: no cover
    fcntl = None                               # type: ignore[assignment]

# ioctls de rede do Linux, usados para descobrir o broadcast de cada interface
SIOCGIFADDR = 0x8915
SIOCGIFNETMASK = 0x891B

import sentiment
import websockets
from faster_whisper import WhisperModel
from memory_store import (
    MemoryStore,
    answer_from_memory,
    build_memory_context,
    extract_user_facts,
)

BASE_DIR = Path(__file__).resolve().parent
MIC_TESTS_DIR = BASE_DIR / "mic_tests"
TTS_OUT_DIR = BASE_DIR / "tts_out"
VOICES_DIR = BASE_DIR / "voices"
MEMORY_DIR = BASE_DIR / "memory"
MEMORY_DB = MEMORY_DIR / "robo_memory.sqlite3"
VOICE_MODEL = VOICES_DIR / "pt_BR-faber-medium.onnx"
VOICE_MODEL_JSON = VOICES_DIR / "pt_BR-faber-medium.onnx.json"

HOST = "0.0.0.0"
PORT = 8765
DISCOVERY_ENABLED = os.environ.get("ROBO_DISCOVERY_ENABLED", "1").strip() != "0"
DISCOVERY_BEACON_ENABLED = os.environ.get("ROBO_DISCOVERY_BEACON_ENABLED", "0").strip() == "1"
DISCOVERY_PORT = int(os.environ.get("ROBO_DISCOVERY_PORT", "8766"))
DISCOVERY_INTERVAL_S = float(os.environ.get("ROBO_DISCOVERY_INTERVAL_S", "1.0"))
BEACON_TARGETS_REFRESH_S = float(os.environ.get("ROBO_BEACON_REFRESH_S", "10.0"))
DISCOVERY_SERVER_ID = os.environ.get("ROBO_DISCOVERY_SERVER_ID", "robo-main").strip() or "robo-main"
DISCOVERY_TOKEN = os.environ.get("ROBO_DISCOVERY_TOKEN", "").strip()
DISCOVERY_ADVERTISE_HOST = os.environ.get("ROBO_DISCOVERY_ADVERTISE_HOST", "").strip()
DISCOVERY_HOTSPOT_CIDR = os.environ.get("ROBO_DISCOVERY_HOTSPOT_CIDR", "10.42.0.0/24").strip()
DISCOVERY_HOTSPOT_HOST = os.environ.get("ROBO_DISCOVERY_HOTSPOT_HOST", "10.42.0.1").strip()
PROTOCOL_VERSION = 1

CARDPUTER_SAMPLE_RATE = 17000
CARDPUTER_CHANNELS = 1
CARDPUTER_SAMPLE_WIDTH = 2
WHISPER_SAMPLE_RATE = 16000
WHISPER_LANGUAGE = "pt"
WHISPER_DEVICE = "cpu"
WHISPER_COMPUTE_TYPE = "int8"
WHISPER_MODEL_SIZE = "base"
TTS_CHUNK_SIZE = 1024
TTS_CHUNK_DELAY_S = 0.003
AUTO_TTS_REPLY = True
OLLAMA_BASE_URL = os.environ.get("ROBO_OLLAMA_BASE_URL", "http://127.0.0.1:11434/api/chat").strip()
OLLAMA_MODEL = os.environ.get("ROBO_OLLAMA_MODEL", "qwen2.5:1.5b-instruct").strip()
OLLAMA_TIMEOUT_S = float(os.environ.get("ROBO_OLLAMA_TIMEOUT_S", "30"))
OLLAMA_HISTORY_LIMIT = 8
DEFAULT_SPEAKER_ID = os.environ.get("ROBO_DEFAULT_SPEAKER_ID", "ricardo").strip() or "ricardo"

CONVERSATION_SYSTEM_PROMPT = (
    "Você é o cérebro de um robô experimental em português do Brasil. "
    "Responda de forma curta, natural e útil. "
    "Se a entrada for ambígua, faça uma resposta simples e objetiva. "
    "Se o usuário pedir ações do robô, explique em uma frase o que será feito."
)

whisper_model: WhisperModel | None = None
memory_store: MemoryStore | None = None
active_websocket: Any = None
send_lock: asyncio.Lock | None = None
conversation_history: list[dict[str, str]] = []

recording = False
record_buffer = bytearray()
record_sample_rate = CARDPUTER_SAMPLE_RATE
record_channels = CARDPUTER_CHANNELS
record_sample_width = CARDPUTER_SAMPLE_WIDTH
record_started_at = 0.0


def ensure_directories() -> None:
    MIC_TESTS_DIR.mkdir(parents=True, exist_ok=True)
    TTS_OUT_DIR.mkdir(parents=True, exist_ok=True)
    VOICES_DIR.mkdir(parents=True, exist_ok=True)
    MEMORY_DIR.mkdir(parents=True, exist_ok=True)


def run_command(command: list[str]) -> None:
    print("EXEC:", " ".join(command))
    subprocess.run(command, check=True)


def token_hash() -> str:
    if not DISCOVERY_TOKEN:
        return ""
    return hashlib.sha256(DISCOVERY_TOKEN.encode("utf-8")).hexdigest()[:16]


def hello_proof(nonce: str) -> str:
    if not DISCOVERY_TOKEN:
        return ""
    material = f"{nonce}:{DISCOVERY_TOKEN}".encode("utf-8")
    return hashlib.sha256(material).hexdigest()[:16]


def discovery_hmac(nonce: str) -> str:
    if not DISCOVERY_TOKEN:
        return ""
    return hmac.new(
        DISCOVERY_TOKEN.encode("utf-8"),
        nonce.encode("utf-8"),
        hashlib.sha256,
    ).hexdigest()[:16]


def local_ip_for(dest_ip: str) -> str:
    sock: socket.socket | None = None
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect((dest_ip, 1))
        return str(sock.getsockname()[0])
    except OSError:
        return "127.0.0.1"
    finally:
        if sock is not None:
            sock.close()


def client_in_hotspot(client_ip: str) -> bool:
    if not DISCOVERY_HOTSPOT_CIDR or not DISCOVERY_HOTSPOT_HOST:
        return False

    try:
        client_addr = ipaddress.ip_address(client_ip)
        hotspot_net = ipaddress.ip_network(DISCOVERY_HOTSPOT_CIDR, strict=False)
    except ValueError:
        return False

    return client_addr in hotspot_net


def advertised_host_for(client_ip: str) -> str:
    if DISCOVERY_ADVERTISE_HOST:
        return DISCOVERY_ADVERTISE_HOST
    if client_in_hotspot(client_ip):
        return DISCOVERY_HOTSPOT_HOST
    return local_ip_for(client_ip)


def interface_broadcasts() -> list[str]:
    """Endereço de broadcast dirigido de cada interface IPv4 ativa.

    Usa ioctl direto em vez de subprocess ou dependência externa. É específico
    de Linux, que é onde o servidor roda; em outro sistema devolve lista vazia
    e o chamador cai no fallback.
    """
    if fcntl is None:
        return []

    targets: list[str] = []

    try:
        names = [name for _index, name in socket.if_nameindex()]
    except OSError:
        return targets

    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for name in names:
            if name == "lo":
                continue

            request = struct.pack("256s", name.encode("utf-8")[:15])
            try:
                addr = socket.inet_ntoa(fcntl.ioctl(probe.fileno(), SIOCGIFADDR, request)[20:24])
                mask = socket.inet_ntoa(fcntl.ioctl(probe.fileno(), SIOCGIFNETMASK, request)[20:24])
            except OSError:
                continue                       # interface sem IPv4 no momento

            try:
                network = ipaddress.ip_network(f"{addr}/{mask}", strict=False)
            except ValueError:
                continue

            if network.prefixlen >= 31:        # /31 e /32 não têm broadcast
                continue

            broadcast = str(network.broadcast_address)
            if broadcast not in targets:
                targets.append(broadcast)
    finally:
        probe.close()

    return targets


def hotspot_broadcast() -> str | None:
    """Broadcast da rede do hotspot, derivado da CIDR já configurada.

    Serve de rede de segurança: se a enumeração de interfaces falhar mas o
    hotspot estiver de pé, o beacon ainda chega nos clientes dele.
    """
    if not DISCOVERY_HOTSPOT_CIDR:
        return None

    try:
        network = ipaddress.ip_network(DISCOVERY_HOTSPOT_CIDR, strict=False)
    except ValueError:
        return None

    if network.prefixlen >= 31:
        return None

    return str(network.broadcast_address)


def beacon_targets() -> list[str]:
    """Para onde mandar o beacon.

    Broadcast dirigido por interface, e não 255.255.255.255: o endereço global
    depende da rota padrão, que num hotspot isolado simplesmente não existe.
    O envio falhava a cada intervalo e enchia o log de erro.

    Efeito colateral desejado: mandando para 10.42.0.255, o kernel escolhe
    10.42.0.1 como origem, que é o endereço que o Cardputer precisa ver.
    """
    targets = interface_broadcasts()

    hotspot = hotspot_broadcast()
    if hotspot and hotspot not in targets:
        targets.append(hotspot)

    if not targets:
        targets.append("255.255.255.255")      # último recurso
    return targets


def send_beacon(sock: socket.socket, targets: list[str], failed: set[str]) -> None:
    """Envia o beacon para cada alvo, sem repetir erro no log.

    Um alvo que falha é anunciado uma vez e depois silenciado até voltar.
    """
    payload = discovery_payload()

    for target in targets:
        try:
            sock.sendto(payload, (target, DISCOVERY_PORT))
        except OSError as exc:
            if target not in failed:
                failed.add(target)
                print(f"Beacon UDP falhou para {target}: {exc}")
        else:
            if target in failed:
                failed.discard(target)
                print(f"Beacon UDP voltou a funcionar para {target}.")


def discovery_payload() -> bytes:
    payload = {
        "type": "ROBO_BEACON",
        "server_id": DISCOVERY_SERVER_ID,
        "proto": PROTOCOL_VERSION,
        "ws_port": PORT,
        "token_hash": token_hash(),
    }
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


def discovery_response(request_text: str, client_ip: str) -> bytes | None:
    parts = request_text.split()
    if len(parts) != 2 or parts[0] != "RDISCOVER":
        return None

    nonce = parts[1].strip()
    is_hex_nonce = all(ch in "0123456789abcdefABCDEF" for ch in nonce)
    if not (1 <= len(nonce) <= 32) or not is_hex_nonce:
        return None

    mac = discovery_hmac(nonce)
    if not mac:
        return None

    ws_url = f"ws://{advertised_host_for(client_ip)}:{PORT}"
    return f"ROBOT {ws_url} {mac}".encode("utf-8")


async def discovery_beacon_loop() -> None:
    if not DISCOVERY_ENABLED:
        print("Discovery UDP desativado.")
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("", DISCOVERY_PORT))
    except OSError as exc:
        print(f"Discovery UDP não iniciou na porta {DISCOVERY_PORT}: {exc}")
        return

    sock.setblocking(False)
    print(f"Discovery UDP escutando RDISCOVER na porta {DISCOVERY_PORT}.")
    if DISCOVERY_ADVERTISE_HOST:
        print(f"Discovery UDP anunciando host fixo: {DISCOVERY_ADVERTISE_HOST}.")
    elif DISCOVERY_HOTSPOT_CIDR and DISCOVERY_HOTSPOT_HOST:
        print(f"Discovery UDP anunciando {DISCOVERY_HOTSPOT_HOST} para clientes em {DISCOVERY_HOTSPOT_CIDR}.")
    if DISCOVERY_BEACON_ENABLED:
        print(f"Discovery UDP beacon ativo a cada {DISCOVERY_INTERVAL_S:.1f}s.")
    else:
        print("Discovery UDP beacon desativado; RDISCOVER continua ativo.")
    if not DISCOVERY_TOKEN:
        print("Aviso: ROBO_DISCOVERY_TOKEN vazio; firmware v2 com HMAC usará fallback WS_URL.")

    loop = asyncio.get_running_loop()
    last_broadcast_at = 0.0
    last_targets_at = 0.0
    targets: list[str] = []
    failed: set[str] = set()

    try:
        while True:
            now = loop.time()

            # A lista é recalculada de tempos em tempos porque o hotspot pode
            # subir depois do servidor, criando uma interface nova.
            if DISCOVERY_BEACON_ENABLED and now - last_targets_at >= BEACON_TARGETS_REFRESH_S:
                last_targets_at = now
                novos = beacon_targets()
                if novos != targets:
                    targets = novos
                    failed.intersection_update(targets)
                    print(f"Beacon UDP transmitindo para: {', '.join(targets)}")

            if DISCOVERY_BEACON_ENABLED and now - last_broadcast_at >= DISCOVERY_INTERVAL_S:
                last_broadcast_at = now
                send_beacon(sock, targets, failed)

            try:
                data, addr = await asyncio.wait_for(loop.sock_recvfrom(sock, 512), timeout=0.25)
            except asyncio.TimeoutError:
                continue

            text = data.decode("utf-8", errors="ignore").strip()
            response = discovery_response(text, str(addr[0]))
            if response is None:
                continue

            try:
                sock.sendto(response, addr)
                print(f"Discovery UDP respondeu {addr[0]}: {response.decode('utf-8')}")
            except OSError as exc:
                print(f"Falha respondendo discovery UDP para {addr[0]}: {exc}")
    finally:
        sock.close()


def parse_record_start(text: str) -> tuple[int, int, int]:
    parts = text.split()
    sample_rate = CARDPUTER_SAMPLE_RATE
    channels = CARDPUTER_CHANNELS
    sample_width = CARDPUTER_SAMPLE_WIDTH

    try:
        if len(parts) >= 2:
            sample_rate = int(parts[1])
        if len(parts) >= 3:
            channels = int(parts[2])
        if len(parts) >= 4:
            format_name = parts[3].strip().lower()
            if format_name != "s16le":
                print(f"Formato '{format_name}' informado; usando S16LE como padrão.")
    except ValueError as error:
        print(f"Erro lendo RECORD_START: {error}")
        print("Usando valores padrão do Cardputer.")

    return sample_rate, channels, sample_width


def parse_key_value_message(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for part in text.split()[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        values[key.strip().lower()] = value.strip()
    return values


def build_hello_response(text: str) -> str:
    values = parse_key_value_message(text)
    nonce = values.get("nonce", "")
    parts = [
        "HELLO_ROBO",
        f"id={DISCOVERY_SERVER_ID}",
        f"proto={PROTOCOL_VERSION}",
        "ok=1",
    ]
    if nonce:
        parts.append(f"nonce={nonce}")
    proof = hello_proof(nonce)
    if proof:
        parts.append(f"proof={proof}")
    return " ".join(parts)


def write_wav(raw_data: bytes, wav_path: Path, sample_rate: int, channels: int, sample_width: int) -> None:
    with wave.open(str(wav_path), "wb") as wav_file:
        wav_file.setnchannels(channels)
        wav_file.setsampwidth(sample_width)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(raw_data)


def convert_to_whisper_wav(input_wav: Path, whisper_wav: Path) -> None:
    run_command([
        "ffmpeg",
        "-y",
        "-i",
        str(input_wav),
        "-ac",
        "1",
        "-ar",
        str(WHISPER_SAMPLE_RATE),
        str(whisper_wav),
    ])


def save_recording(raw_data: bytes, sample_rate: int, channels: int, sample_width: int) -> tuple[Path, Path, Path]:
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    raw_path = MIC_TESTS_DIR / f"cardputer_mic_{timestamp}.raw"
    wav_path = MIC_TESTS_DIR / f"cardputer_mic_{timestamp}_{sample_rate}hz.wav"
    whisper_wav_path = MIC_TESTS_DIR / f"cardputer_mic_{timestamp}_whisper_16k.wav"

    raw_path.write_bytes(raw_data)
    write_wav(raw_data, wav_path, sample_rate, channels, sample_width)
    convert_to_whisper_wav(wav_path, whisper_wav_path)

    return raw_path, wav_path, whisper_wav_path


def transcribe_audio(wav_path: Path) -> str:
    if whisper_model is None:
        raise RuntimeError("Whisper não foi carregado.")

    print()
    print(f"Transcrevendo: {wav_path}")
    started_at = time.time()

    segments, info = whisper_model.transcribe(
        str(wav_path),
        language=WHISPER_LANGUAGE,
        beam_size=5,
        vad_filter=False,
        condition_on_previous_text=False,
        temperature=0.0,
    )

    print(f"Idioma detectado: {info.language} ({info.language_probability:.2f})")
    print("Segmentos:")
    print("-" * 48)

    pieces: list[str] = []
    for segment in segments:
        text = segment.text.strip()
        if text:
            pieces.append(text)
        print(f"[{segment.start:.2f}s -> {segment.end:.2f}s] {text}")

    print("-" * 48)

    final_text = " ".join(pieces).strip()
    elapsed = time.time() - started_at

    print(f"Transcrição final: {final_text or '[vazia]'}")
    print(f"Tempo STT: {elapsed:.2f}s")
    print()

    return final_text


def basic_reply(text: str) -> str:
    cleaned = text.strip()
    if not cleaned:
        return "Não consegui entender."

    lowered = cleaned.lower()
    if "teste" in lowered:
        return "Teste recebido com sucesso."
    if "oi" in lowered or "olá" in lowered:
        return "Oi, eu ouvi você."
    return f"Eu ouvi: {cleaned}"


def classify_mood(user_text: str) -> str:
    """Canal de empatia: o rosto reflete a emoção de QUEM FALOU.

    O estado do próprio robô (pensando, falando, erro) é outro canal e vive
    no firmware, na cor do rosto. Recebe só a fala do usuário de propósito:
    misturar a resposta do robô fazia o "erro" dito pelo usuário deixar o
    rosto preocupado mesmo com uma resposta tranquila.
    """
    result = sentiment.analyze(user_text)
    print(
        f"Sentimento: {result.mood} "
        f"(val={result.valence:+.2f} aro={result.arousal:.2f} "
        f"conf={result.confidence:.2f} hits={result.hits})"
    )
    return result.mood


def remember_turn(role: str, content: str) -> None:
    conversation_history.append({"role": role, "content": content})
    if len(conversation_history) > OLLAMA_HISTORY_LIMIT * 2:
        del conversation_history[:-OLLAMA_HISTORY_LIMIT * 2]


def build_llm_messages(user_text: str, memory_context: str = "") -> list[dict[str, str]]:
    messages = [{"role": "system", "content": CONVERSATION_SYSTEM_PROMPT}]
    if memory_context:
        messages.append({"role": "system", "content": memory_context})
    messages.extend(conversation_history[-OLLAMA_HISTORY_LIMIT * 2 :])
    messages.append({"role": "user", "content": user_text})
    return messages


def call_ollama_llm(user_text: str, memory_context: str = "") -> tuple[str, str]:
    if not OLLAMA_BASE_URL:
        return basic_reply(user_text), "fallback"

    payload: dict[str, Any] = {
        "model": OLLAMA_MODEL,
        "messages": build_llm_messages(user_text, memory_context),
        "stream": False,
        "options": {
            "temperature": 0.4,
            "num_predict": 128,
        },
    }

    body = json.dumps(payload).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
    }

    req = request.Request(OLLAMA_BASE_URL, data=body, headers=headers, method="POST")

    try:
        with request.urlopen(req, timeout=OLLAMA_TIMEOUT_S) as response:
            raw = response.read().decode("utf-8")
    except error.URLError as exc:
        print(f"Ollama indisponível, usando fallback local: {exc}")
        return basic_reply(user_text), "fallback"

    try:
        data = json.loads(raw)
        message = data.get("message", {})
        content = str(message.get("content", "")).strip()
        if content:
            return content, "ollama"
    except Exception as exc:
        print(f"Falha ao interpretar resposta do Ollama: {exc}")

    return basic_reply(user_text), "fallback"


def generate_reply(transcription: str) -> str:
    if memory_store is None:
        reply, _source = call_ollama_llm(transcription)
        remember_turn("user", transcription)
        remember_turn("assistant", reply)
        return reply

    memory_store.add_turn("user", transcription, speaker_id=DEFAULT_SPEAKER_ID)
    for fact in extract_user_facts(transcription, speaker_id=DEFAULT_SPEAKER_ID):
        memory_store.upsert_fact(fact)
        print(f"Memória atualizada: {fact.subject} {fact.predicate} = {fact.object_value}")

    fast_reply = answer_from_memory(transcription, memory_store, speaker_id=DEFAULT_SPEAKER_ID)
    if fast_reply:
        reply = fast_reply
        print("Resposta gerada pela memória local, sem Ollama.")
    else:
        cached = memory_store.semantic_cached_response(transcription, speaker_id=DEFAULT_SPEAKER_ID)
        if cached:
            reply, score = cached
            print(f"Resposta gerada pelo cache semântico, sem Ollama. Score={score:.2f}")
        else:
            memory_context = build_memory_context(memory_store, transcription)
            reply, source = call_ollama_llm(transcription, memory_context)
            if source == "ollama":
                memory_store.add_response_cache(
                    transcription,
                    reply,
                    speaker_id=DEFAULT_SPEAKER_ID,
                    confidence=0.72,
                )

    memory_store.add_turn("assistant", reply, speaker_id="assistant")
    remember_turn("user", transcription)
    remember_turn("assistant", reply)
    return reply


def make_tts_raw(reply_text: str) -> Path:
    if not VOICE_MODEL.exists() or not VOICE_MODEL_JSON.exists():
        raise FileNotFoundError("Arquivos de voz do Piper não encontrados em server/voices/.")

    timestamp = time.strftime("%Y%m%d_%H%M%S")
    text_path = TTS_OUT_DIR / f"reply_{timestamp}.txt"
    wav_path = TTS_OUT_DIR / f"reply_{timestamp}.wav"
    raw_path = TTS_OUT_DIR / f"reply_{timestamp}_16k.raw"

    text_path.write_text(reply_text, encoding="utf-8")

    run_command([
        "python3",
        "-m",
        "piper",
        "--model",
        str(VOICE_MODEL),
        "--input-file",
        str(text_path),
        "--output-file",
        str(wav_path),
    ])

    run_command([
        "ffmpeg",
        "-y",
        "-i",
        str(wav_path),
        "-ac",
        "1",
        "-ar",
        "16000",
        "-f",
        "s16le",
        str(raw_path),
    ])

    return raw_path


async def send_text(websocket: Any, text: str) -> None:
    assert send_lock is not None
    async with send_lock:
        await websocket.send(text)


async def stream_raw_audio(websocket: Any, raw_path: Path) -> None:
    if not raw_path.exists():
        print(f"Arquivo de áudio não encontrado: {raw_path}")
        return

    data = raw_path.read_bytes()
    print(f"Enviando áudio ao Cardputer: {raw_path} ({len(data)} bytes)")

    await send_text(websocket, "PLAY_START")

    offset = 0
    while offset < len(data):
        chunk = data[offset:offset + TTS_CHUNK_SIZE]
        assert send_lock is not None
        async with send_lock:
            await websocket.send(chunk)
        offset += len(chunk)
        await asyncio.sleep(TTS_CHUNK_DELAY_S)

    await send_text(websocket, "PLAY_END")
    print("PLAY_END enviado.")


async def handle_saved_recording(websocket: Any, raw_data: bytes, sample_rate: int, channels: int, sample_width: int) -> None:
    print()
    print("RECORD_END recebido.")
    print(f"Bytes recebidos: {len(raw_data)}")
    print(f"Formato: {sample_rate} Hz, {channels} canal(is), {sample_width * 8}-bit")

    if not raw_data:
        await send_text(websocket, "MSG Audio vazio")
        return

    raw_path, wav_path, whisper_wav_path = await asyncio.to_thread(
        save_recording,
        raw_data,
        sample_rate,
        channels,
        sample_width,
    )

    print(f"Salvo RAW: {raw_path}")
    print(f"Salvo WAV: {wav_path}")
    print(f"Salvo WAV Whisper: {whisper_wav_path}")

    transcription = await asyncio.to_thread(transcribe_audio, whisper_wav_path)
    await send_text(websocket, f"MSG Transcricao: {transcription}")

    reply = generate_reply(transcription)
    await send_text(websocket, f"EMO {classify_mood(transcription)}")
    await send_text(websocket, f"MSG Resposta: {reply}")

    if AUTO_TTS_REPLY:
        try:
            tts_raw = await asyncio.to_thread(make_tts_raw, reply)
            await stream_raw_audio(websocket, tts_raw)
        except Exception as error:
            print(f"Erro gerando TTS: {error}")
            await send_text(websocket, f"MSG Erro no TTS: {error}")


async def handle_client(websocket: Any) -> None:
    global active_websocket
    global recording
    global record_buffer
    global record_sample_rate
    global record_channels
    global record_sample_width
    global record_started_at

    active_websocket = websocket
    print("Cardputer conectado.")

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                if recording:
                    record_buffer.extend(message)
                else:
                    print(f"Binário ignorado: {len(message)} bytes sem RECORD_START")
                continue

            text = str(message).strip()
            print(f"Texto recebido: {text}")

            if text.startswith("HELLO_CARDPUTER"):
                await send_text(websocket, build_hello_response(text))
                continue

            if text == "PING":
                await send_text(websocket, "PONG")
                continue

            if text.startswith("RECORD_START"):
                record_sample_rate, record_channels, record_sample_width = parse_record_start(text)
                recording = True
                record_buffer = bytearray()
                record_started_at = time.time()
                print(
                    f"RECORD_START: {record_sample_rate} Hz, {record_channels} canal(is), {record_sample_width * 8}-bit"
                )
                await send_text(websocket, "RECORDING")
                continue

            if text == "RECORD_END":
                recording = False
                elapsed = time.time() - record_started_at if record_started_at else 0.0
                print(f"Tempo de captura: {elapsed:.2f}s")
                raw_snapshot = bytes(record_buffer)
                record_buffer = bytearray()
                await handle_saved_recording(
                    websocket,
                    raw_snapshot,
                    record_sample_rate,
                    record_channels,
                    record_sample_width,
                )
                continue

            if text.startswith("SAY "):
                say_text = text[4:].strip()
                if not say_text:
                    await send_text(websocket, "MSG SAY vazio")
                    continue

                await send_text(websocket, f"MSG Falando: {say_text}")
                try:
                    tts_raw = await asyncio.to_thread(make_tts_raw, say_text)
                    await stream_raw_audio(websocket, tts_raw)
                except Exception as error:
                    print(f"Erro no SAY/TTS: {error}")
                    await send_text(websocket, f"MSG Erro no SAY/TTS: {error}")
                continue

            await send_text(websocket, f"MSG {text}")

    except websockets.exceptions.ConnectionClosed:
        print("Cardputer desconectou.")
    finally:
        if active_websocket is websocket:
            active_websocket = None
        recording = False
        record_buffer = bytearray()


async def console_loop() -> None:
    global AUTO_TTS_REPLY

    print()
    print("Console do servidor:")
    print("  msg texto")
    print("  say texto")
    print("  auto_tts on")
    print("  auto_tts off")
    print("  llm on/off via ROBO_OLLAMA_BASE_URL")
    print("  ping")
    print()

    while True:
        try:
            command = await asyncio.to_thread(input, "robo> ")
        except EOFError:
            print("Console encerrado (EOF).")
            return

        command = command.strip()
        if not command:
            continue

        if command == "auto_tts on":
            AUTO_TTS_REPLY = True
            print("AUTO_TTS_REPLY = True")
            continue

        if command == "auto_tts off":
            AUTO_TTS_REPLY = False
            print("AUTO_TTS_REPLY = False")
            continue

        websocket = active_websocket
        if websocket is None:
            print("Nenhum Cardputer conectado.")
            continue

        try:
            if command.startswith("msg "):
                await send_text(websocket, f"MSG {command[4:].strip()}")
                continue

            if command.startswith("say "):
                say_text = command[4:].strip()
                if not say_text:
                    print("Uso: say texto")
                    continue

                await send_text(websocket, f"MSG Falando: {say_text}")
                tts_raw = await asyncio.to_thread(make_tts_raw, say_text)
                await stream_raw_audio(websocket, tts_raw)
                continue

            if command == "ping":
                await send_text(websocket, "PING")
                continue

            print("Comando desconhecido.")
        except Exception as error:
            print(f"Erro no console: {error}")


async def main() -> None:
    global whisper_model
    global memory_store
    global send_lock

    ensure_directories()
    sentiment.init_from_env()
    memory_store = MemoryStore(MEMORY_DB)
    print(f"Memória persistente em: {MEMORY_DB}")

    print("Carregando faster-whisper uma única vez...")
    whisper_model = WhisperModel(
        WHISPER_MODEL_SIZE,
        device=WHISPER_DEVICE,
        compute_type=WHISPER_COMPUTE_TYPE,
    )
    print("faster-whisper carregado.")
    print(f"Servidor em ws://{HOST}:{PORT}")

    send_lock = asyncio.Lock()

    async with websockets.serve(handle_client, HOST, PORT, max_size=None):
        console_task = asyncio.create_task(console_loop())
        discovery_task = asyncio.create_task(discovery_beacon_loop())
        try:
            await asyncio.Future()
        finally:
            console_task.cancel()
            discovery_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await console_task
            with contextlib.suppress(asyncio.CancelledError):
                await discovery_task


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
