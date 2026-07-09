import asyncio
import subprocess
import tempfile
from pathlib import Path

import websockets

PORTA = 8765
CHUNK_SIZE = 1024

VOICE_MODEL = Path("voices/pt_BR-faber-medium.onnx")
RAW_OUT = Path("piper_out_16k.raw")

cliente_atual = None


def rodar(cmd: list[str]) -> None:
    print("EXEC:", " ".join(cmd))
    subprocess.run(cmd, check=True)


def gerar_piper_raw(texto: str, volume: float = 2.0) -> Path:
    if not VOICE_MODEL.exists():
        raise FileNotFoundError(f"Modelo nao encontrado: {VOICE_MODEL}")

    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        texto_path = Path(f.name)
        f.write(texto)

    wav_path = Path("piper_out.wav")

    rodar([
        "python",
        "-m",
        "piper",
        "--model",
        str(VOICE_MODEL),
        "--input-file",
        str(texto_path),
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
        str(RAW_OUT),
    ])

    try:
        texto_path.unlink()
    except Exception:
        pass

    return RAW_OUT


async def enviar_raw(path: Path) -> None:
    global cliente_atual

    if cliente_atual is None:
        print("Nenhum Cardputer conectado.")
        return

    if not path.exists():
        print(f"Arquivo nao existe: {path}")
        return

    data = path.read_bytes()

    print(f"Enviando {path} | {len(data)} bytes em chunks de {CHUNK_SIZE}")

    await cliente_atual.send("PLAY_START")

    enviado = 0

    while enviado < len(data):
        chunk = data[enviado:enviado + CHUNK_SIZE]
        await cliente_atual.send(chunk)

        enviado += len(chunk)
        print(f"chunk {len(chunk)} bytes | total {enviado}")

        await asyncio.sleep(0.01)

    await cliente_atual.send("PLAY_END")

    print("PLAY_END enviado. O Cardputer deve tocar agora.")


async def handler(websocket):
    global cliente_atual

    cliente_atual = websocket
    print("Cardputer conectado.")

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                print(f"Binario recebido do Cardputer: {len(message)} bytes")
                continue

            print("Cardputer:", message)

            if message == "PING":
                await websocket.send("PONG")

    except websockets.exceptions.ConnectionClosed:
        print("Cardputer desconectou.")

    finally:
        if cliente_atual is websocket:
            cliente_atual = None


async def console_loop():
    print()
    print("Comandos:")
    print("  ping")
    print("  vol 230")
    print("  say texto que o Cardputer vai falar")
    print("  sayv 2.5 texto com volume maior")
    print("  file fala_16k.raw")
    print()

    while True:
        cmd = await asyncio.to_thread(input, "piper-lab> ")
        cmd = cmd.strip()

        if not cmd:
            continue

        try:
            if cmd == "ping":
                if cliente_atual:
                    await cliente_atual.send("PING")
                    print("PING enviado.")
                else:
                    print("Nenhum Cardputer conectado.")
                continue

            if cmd.startswith("vol "):
                volume = int(cmd.split()[1])
                volume = max(0, min(255, volume))

                if cliente_atual:
                    await cliente_atual.send(f"VOL {volume}")
                    print(f"Volume enviado: {volume}")
                else:
                    print("Nenhum Cardputer conectado.")
                continue

            if cmd.startswith("file "):
                path = Path(cmd[5:].strip())
                await enviar_raw(path)
                continue

            if cmd.startswith("sayv "):
                partes = cmd.split(maxsplit=2)

                if len(partes) < 3:
                    print("Uso: sayv 2.0 texto")
                    continue

                vol_audio = float(partes[1])
                texto = partes[2]

                raw = gerar_piper_raw(texto, volume=vol_audio)
                await enviar_raw(raw)
                continue

            if cmd.startswith("say "):
                texto = cmd[4:].strip()

                if not texto:
                    print("Uso: say texto")
                    continue

                raw = gerar_piper_raw(texto, volume=2.0)
                await enviar_raw(raw)
                continue

            print("Comando desconhecido.")

        except Exception as e:
            print("Erro:", e)


async def main():
    print(f"Servidor WebSocket em ws://0.0.0.0:{PORTA}")

    async with websockets.serve(handler, "0.0.0.0", PORTA, max_size=None):
        await console_loop()


if __name__ == "__main__":
    asyncio.run(main())