import asyncio
import math
import struct
import wave
import websockets

PORTA = 8765
SAMPLE_RATE = 16000
CHUNK_SIZE = 1024

cliente_atual = None


def gerar_senoide(freq_hz: float, duracao_s: float, amplitude: float) -> bytes:
    total_amostras = int(SAMPLE_RATE * duracao_s)
    pcm = bytearray()

    fade = int(0.02 * SAMPLE_RATE)

    for n in range(total_amostras):
        t = n / SAMPLE_RATE

        envelope = 1.0
        if n < fade:
            envelope = n / fade
        elif n > total_amostras - fade:
            envelope = max(0.0, (total_amostras - n) / fade)

        valor = math.sin(2.0 * math.pi * freq_hz * t)
        sample = int(valor * amplitude * envelope * 32767)

        pcm.extend(struct.pack("<h", sample))

    return bytes(pcm)


def gerar_silencio(duracao_s: float) -> bytes:
    total_amostras = int(SAMPLE_RATE * duracao_s)
    return b"\x00\x00" * total_amostras


def gerar_escala(amplitude: float = 0.6) -> bytes:
    audio = bytearray()

    frequencias = [220, 330, 440, 660, 880, 1200, 1760]

    for freq in frequencias:
        audio.extend(gerar_senoide(freq, 0.45, amplitude))
        audio.extend(gerar_silencio(0.08))

    return bytes(audio)


def gerar_sweep(
    freq_ini: float = 200,
    freq_fim: float = 3000,
    duracao_s: float = 3.0,
    amplitude: float = 0.45,
) -> bytes:
    total_amostras = int(SAMPLE_RATE * duracao_s)
    pcm = bytearray()
    fase = 0.0

    for n in range(total_amostras):
        progresso = n / max(1, total_amostras - 1)
        freq = freq_ini + (freq_fim - freq_ini) * progresso

        fase += 2.0 * math.pi * freq / SAMPLE_RATE
        valor = math.sin(fase)

        sample = int(valor * amplitude * 32767)
        pcm.extend(struct.pack("<h", sample))

    return bytes(pcm)


def salvar_wav_debug(pcm: bytes, nome: str) -> None:
    with wave.open(nome, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm)


async def enviar_audio(pcm: bytes, nome_debug: str = "debug.wav") -> None:
    global cliente_atual

    if cliente_atual is None:
        print("Nenhum Cardputer conectado.")
        return

    salvar_wav_debug(pcm, nome_debug)

    print(f"Debug salvo: {nome_debug}")
    print(f"Enviando {len(pcm)} bytes em chunks de {CHUNK_SIZE}")

    await cliente_atual.send("PLAY_START")

    enviado = 0

    while enviado < len(pcm):
        chunk = pcm[enviado:enviado + CHUNK_SIZE]
        await cliente_atual.send(chunk)

        enviado += len(chunk)

        print(f"chunk: {len(chunk)} bytes | total: {enviado}")

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
    print("  tone 440 1.0 0.7")
    print("  scale")
    print("  sweep")
    print("  silence")
    print()

    while True:
        cmd = await asyncio.to_thread(input, "audio-lab> ")
        cmd = cmd.strip()

        if not cmd:
            continue

        partes = cmd.split()

        try:
            if partes[0] == "ping":
                if cliente_atual:
                    await cliente_atual.send("PING")
                    print("PING enviado.")
                else:
                    print("Nenhum Cardputer conectado.")
                continue

            if partes[0] == "vol":
                if len(partes) < 2:
                    print("Uso: vol 230")
                    continue

                volume = int(partes[1])
                volume = max(0, min(255, volume))

                if cliente_atual:
                    await cliente_atual.send(f"VOL {volume}")
                    print(f"Volume enviado: {volume}")
                else:
                    print("Nenhum Cardputer conectado.")
                continue

            if partes[0] == "tone":
                freq = float(partes[1]) if len(partes) > 1 else 440.0
                duracao = float(partes[2]) if len(partes) > 2 else 1.0
                amplitude = float(partes[3]) if len(partes) > 3 else 0.6

                amplitude = max(0.0, min(1.0, amplitude))

                pcm = gerar_senoide(freq, duracao, amplitude)
                await enviar_audio(pcm, f"tone_{int(freq)}hz.wav")
                continue

            if partes[0] == "scale":
                pcm = gerar_escala(0.6)
                await enviar_audio(pcm, "scale_debug.wav")
                continue

            if partes[0] == "sweep":
                pcm = gerar_sweep(200, 3000, 3.0, 0.45)
                await enviar_audio(pcm, "sweep_debug.wav")
                continue

            if partes[0] == "silence":
                pcm = gerar_silencio(1.0)
                await enviar_audio(pcm, "silence_debug.wav")
                continue

            print("Comando desconhecido.")

        except Exception as erro:
            print("Erro:", erro)


async def main():
    print(f"Servidor WebSocket em ws://0.0.0.0:{PORTA}")

    async with websockets.serve(handler, "0.0.0.0", PORTA, max_size=None):
        await console_loop()


if __name__ == "__main__":
    asyncio.run(main())