import asyncio
import subprocess
import tempfile
import time
import wave
from pathlib import Path

import websockets
from faster_whisper import WhisperModel


# ============================================================
# CONFIGURAÇÕES GERAIS
# ============================================================

PORTA = 8765

MIC_DIR = Path("mic_tests")
MIC_DIR.mkdir(exist_ok=True)

TTS_DIR = Path("tts_out")
TTS_DIR.mkdir(exist_ok=True)

# O Cardputer está mandando PCM S16LE mono 17000 Hz
CARDPUTER_SAMPLE_RATE = 17000
CARDPUTER_CHANNELS = 1
CARDPUTER_SAMPLE_WIDTH = 2  # 2 bytes = 16-bit

# Whisper
WHISPER_MODEL_SIZE = "base"
WHISPER_LANGUAGE = "pt"

# Para Raspberry Pi: CPU + int8
WHISPER_DEVICE = "cpu"
WHISPER_COMPUTE_TYPE = "int8"

# Piper
VOICE_MODEL = Path("voices/pt_BR-faber-medium.onnx")

# Comece desligado. Depois que a transcrição estiver boa, coloque True.
AUTO_TTS_REPLY = False

# Chunks para mandar áudio de volta ao Cardputer
TTS_CHUNK_SIZE = 1024


# ============================================================
# ESTADO GLOBAL
# ============================================================

cliente_atual = None

recebendo_audio = False
buffer_audio = bytearray()
inicio_recebimento = 0.0

sample_rate_atual = CARDPUTER_SAMPLE_RATE
channels_atual = CARDPUTER_CHANNELS
sample_width_atual = CARDPUTER_SAMPLE_WIDTH

print("Carregando faster-whisper...")
whisper_model = WhisperModel(
    WHISPER_MODEL_SIZE,
    device=WHISPER_DEVICE,
    compute_type=WHISPER_COMPUTE_TYPE,
)
print("faster-whisper carregado.")


# ============================================================
# UTILITÁRIOS
# ============================================================

def rodar(cmd: list[str]) -> None:
    print("EXEC:", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


def parse_record_start(texto: str):
    """
    Esperado:
      RECORD_START 17000 1 s16le
    """
    sample_rate = CARDPUTER_SAMPLE_RATE
    channels = CARDPUTER_CHANNELS
    sample_width = CARDPUTER_SAMPLE_WIDTH

    partes = texto.split()

    try:
        if len(partes) >= 2:
            sample_rate = int(partes[1])

        if len(partes) >= 3:
            channels = int(partes[2])

        if len(partes) >= 4:
            formato = partes[3].lower()

            if formato == "s16le":
                sample_width = 2
            else:
                print(f"Formato desconhecido '{formato}', usando s16le.")
                sample_width = 2

    except Exception as erro:
        print("Erro lendo RECORD_START:", erro)
        print("Usando configuração padrão.")

    return sample_rate, channels, sample_width


def salvar_wav(raw: bytes, wav_path: Path, sample_rate: int, channels: int, sample_width: int) -> None:
    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sample_width)
        wf.setframerate(sample_rate)
        wf.writeframes(raw)


def salvar_audio_recebido(raw: bytes, sample_rate: int, channels: int, sample_width: int):
    timestamp = time.strftime("%Y%m%d_%H%M%S")

    raw_path = MIC_DIR / f"cardputer_mic_{timestamp}.raw"
    wav_path = MIC_DIR / f"cardputer_mic_{timestamp}_{sample_rate}hz.wav"
    whisper_path = MIC_DIR / f"cardputer_mic_{timestamp}_whisper_16k.wav"

    raw_path.write_bytes(raw)

    salvar_wav(
        raw=raw,
        wav_path=wav_path,
        sample_rate=sample_rate,
        channels=channels,
        sample_width=sample_width,
    )

    # Converte para 16 kHz mono para facilitar Whisper.
    # Volume 1.5 ajuda um pouco se o mic estiver baixo.
    rodar([
        "ffmpeg",
        "-y",
        "-i",
        str(wav_path),
        "-ac",
        "1",
        "-ar",
        "16000",
        "-filter:a",
        "volume=1.5",
        str(whisper_path),
    ])

    return raw_path, wav_path, whisper_path


def diagnosticar_audio(raw: bytes, sample_rate: int, channels: int, sample_width: int) -> None:
    total_bytes = len(raw)

    if total_bytes == 0:
        print("Áudio vazio.")
        return

    duracao = total_bytes / (sample_rate * channels * sample_width)

    print()
    print("Diagnóstico do áudio recebido:")
    print(f"  Bytes:        {total_bytes}")
    print(f"  Sample rate:  {sample_rate} Hz")
    print(f"  Canais:       {channels}")
    print(f"  Sample width: {sample_width} bytes")
    print(f"  Duração:      {duracao:.2f} s")
    print()


def transcrever_audio(wav_path: Path) -> str:
    print()
    print("Transcrevendo:", wav_path)

    inicio = time.time()

    segments, info = whisper_model.transcribe(
        str(wav_path),
        language=WHISPER_LANGUAGE,
        beam_size=5,
        vad_filter=False,
        condition_on_previous_text=False,
        temperature=0.0,
    )

    textos = []

    print("Segmentos:")
    print("-" * 40)

    for segment in segments:
        texto = segment.text.strip()
        textos.append(texto)
        print(f"[{segment.start:.2f}s -> {segment.end:.2f}s] {texto}")

    print("-" * 40)

    final = " ".join(textos).strip()

    tempo = time.time() - inicio

    print()
    print("Transcrição final:")
    print(final)
    print(f"Tempo STT: {tempo:.2f}s")
    print()

    return final


def gerar_resposta_basica(transcricao: str) -> str:
    """
    Por enquanto não tem LLM.
    Só uma resposta simples para fechar o ciclo.
    """
    texto = transcricao.strip()

    if not texto:
        return "Não consegui entender o áudio."

    texto_lower = texto.lower()

    if "olá" in texto_lower or "oi" in texto_lower:
        return "Oi, eu ouvi você."

    if "teste" in texto_lower:
        return "Teste recebido com sucesso."

    return f"Eu ouvi: {texto}"


def gerar_piper_raw(texto: str, volume: float = 2.0) -> Path:
    if not VOICE_MODEL.exists():
        raise FileNotFoundError(f"Modelo Piper não encontrado: {VOICE_MODEL}")

    timestamp = time.strftime("%Y%m%d_%H%M%S")

    txt_path = TTS_DIR / f"tts_{timestamp}.txt"
    wav_path = TTS_DIR / f"tts_{timestamp}.wav"
    raw_path = TTS_DIR / f"tts_{timestamp}_16k.raw"

    txt_path.write_text(texto, encoding="utf-8")

    rodar([
        "python",
        "-m",
        "piper",
        "--model",
        str(VOICE_MODEL),
        "--input-file",
        str(txt_path),
        "--output-file",
        str(wav_path),
    ])

    rodar([
        "ffmpeg",
        "-y",
        "-i",
        str(wav_path),
        "-ac",
        "1",
        "-ar",
        "16000",
        "-filter:a",
        f"volume={volume}",
        "-f",
        "s16le",
        str(raw_path),
    ])

    return raw_path


async def enviar_texto_cardputer(websocket, texto: str) -> None:
    """
    O firmware atual do Cardputer mostra textos recebidos.
    """
    await websocket.send("MSG " + texto)


async def enviar_audio_raw_para_cardputer(websocket, raw_path: Path) -> None:
    if not raw_path.exists():
        print("Arquivo TTS raw não existe:", raw_path)
        return

    data = raw_path.read_bytes()

    print(f"Enviando TTS para Cardputer: {raw_path} | {len(data)} bytes")

    await websocket.send("PLAY_START")

    enviado = 0

    while enviado < len(data):
        chunk = data[enviado:enviado + TTS_CHUNK_SIZE]
        await websocket.send(chunk)

        enviado += len(chunk)

        print(f"TTS chunk {len(chunk)} bytes | total {enviado}")

        await asyncio.sleep(0.01)

    await websocket.send("PLAY_END")

    print("TTS PLAY_END enviado.")


async def processar_audio_recebido(websocket, raw: bytes) -> None:
    global sample_rate_atual
    global channels_atual
    global sample_width_atual

    diagnosticar_audio(
        raw=raw,
        sample_rate=sample_rate_atual,
        channels=channels_atual,
        sample_width=sample_width_atual,
    )

    raw_path, wav_path, whisper_path = salvar_audio_recebido(
        raw=raw,
        sample_rate=sample_rate_atual,
        channels=channels_atual,
        sample_width=sample_width_atual,
    )

    print("Arquivos salvos:")
    print(" RAW:    ", raw_path)
    print(" WAV:    ", wav_path)
    print(" Whisper:", whisper_path)
    print()

    transcricao = transcrever_audio(whisper_path)

    if not transcricao:
        transcricao = "[vazio]"

    await enviar_texto_cardputer(websocket, "Transcricao: " + transcricao)

    resposta = gerar_resposta_basica(transcricao)

    print("Resposta básica:", resposta)

    await enviar_texto_cardputer(websocket, "Resposta: " + resposta)

    if AUTO_TTS_REPLY:
        try:
            raw_tts = gerar_piper_raw(resposta, volume=2.0)
            await enviar_audio_raw_para_cardputer(websocket, raw_tts)
        except Exception as erro:
            print("Erro gerando/enviando TTS:", erro)
            await enviar_texto_cardputer(websocket, "Erro no TTS")


# ============================================================
# WEBSOCKET
# ============================================================

async def handler(websocket):
    global cliente_atual
    global recebendo_audio
    global buffer_audio
    global inicio_recebimento
    global sample_rate_atual
    global channels_atual
    global sample_width_atual

    cliente_atual = websocket
    print("Cardputer conectado.")

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                if recebendo_audio:
                    buffer_audio.extend(message)
                    print(f"chunk mic: {len(message)} bytes | total: {len(buffer_audio)}")
                else:
                    print(f"binário ignorado: {len(message)} bytes sem RECORD_START")
                continue

            texto = str(message).strip()
            print("Texto:", texto)

            if texto == "PING":
                await websocket.send("PONG")
                continue

            if texto.startswith("RECORD_START"):
                sample_rate_atual, channels_atual, sample_width_atual = parse_record_start(texto)

                recebendo_audio = True
                buffer_audio = bytearray()
                inicio_recebimento = time.time()

                print("RECORD_START recebido")
                print(f"Config: {sample_rate_atual} Hz, {channels_atual} canal, {sample_width_atual} bytes")

                await websocket.send("RECORDING")
                continue

            if texto == "RECORD_END":
                recebendo_audio = False

                tempo_envio = time.time() - inicio_recebimento
                total = len(buffer_audio)

                print()
                print("RECORD_END recebido.")
                print(f"Total recebido: {total} bytes")
                print(f"Tempo de envio: {tempo_envio:.2f}s")
                print()

                if total == 0:
                    await websocket.send("RECORD_EMPTY")
                    await enviar_texto_cardputer(websocket, "Audio vazio")
                    continue

                await websocket.send("RECORD_SAVED")

                raw_copia = bytes(buffer_audio)

                # Processa depois de responder RECORD_SAVED.
                await processar_audio_recebido(websocket, raw_copia)
                continue

            # Comando manual para falar pelo servidor:
            # SAY texto aqui
            if texto.startswith("SAY "):
                fala = texto[4:].strip()

                if not fala:
                    await enviar_texto_cardputer(websocket, "SAY vazio")
                    continue

                await enviar_texto_cardputer(websocket, "Falando: " + fala)

                try:
                    raw_tts = gerar_piper_raw(fala, volume=2.0)
                    await enviar_audio_raw_para_cardputer(websocket, raw_tts)
                except Exception as erro:
                    print("Erro no SAY/TTS:", erro)
                    await enviar_texto_cardputer(websocket, "Erro no SAY/TTS")

                continue

            await websocket.send("ECO " + texto)

    except websockets.exceptions.ConnectionClosed:
        print("Cardputer desconectou.")

    finally:
        if cliente_atual is websocket:
            cliente_atual = None


async def console_loop():
    """
    Console opcional do servidor.
    Dá para mandar mensagem manual ao Cardputer pelo terminal.
    """
    print()
    print("Console do servidor:")
    print("  msg texto")
    print("  say texto")
    print("  auto_tts on")
    print("  auto_tts off")
    print()

    global AUTO_TTS_REPLY

    while True:
        cmd = await asyncio.to_thread(input, "robo> ")
        cmd = cmd.strip()

        if not cmd:
            continue

        try:
            if cmd.startswith("msg "):
                if cliente_atual is None:
                    print("Nenhum Cardputer conectado.")
                    continue

                texto = cmd[4:].strip()
                await enviar_texto_cardputer(cliente_atual, texto)
                continue

            if cmd.startswith("say "):
                if cliente_atual is None:
                    print("Nenhum Cardputer conectado.")
                    continue

                texto = cmd[4:].strip()

                await enviar_texto_cardputer(cliente_atual, "Falando: " + texto)

                raw_tts = gerar_piper_raw(texto, volume=2.0)
                await enviar_audio_raw_para_cardputer(cliente_atual, raw_tts)
                continue

            if cmd == "auto_tts on":
                AUTO_TTS_REPLY = True
                print("AUTO_TTS_REPLY = True")
                continue

            if cmd == "auto_tts off":
                AUTO_TTS_REPLY = False
                print("AUTO_TTS_REPLY = False")
                continue

            print("Comando desconhecido.")

        except Exception as erro:
            print("Erro no console:", erro)


async def main():
    print(f"Servidor único do robô em ws://0.0.0.0:{PORTA}")
    print("Esperando Cardputer...")
    print()

    async with websockets.serve(handler, "0.0.0.0", PORTA, max_size=None):
        await console_loop()


if __name__ == "__main__":
    asyncio.run(main())