import asyncio
import audioop
import time
import wave
from pathlib import Path

import websockets

PORTA = 8765

SAMPLE_RATE_PADRAO = 17000
CHANNELS_PADRAO = 1
SAMPLE_WIDTH_PADRAO = 2

gravando = False
buffer_audio = bytearray()
inicio_gravacao = 0.0

sample_rate_atual = SAMPLE_RATE_PADRAO
channels_atual = CHANNELS_PADRAO
sample_width_atual = SAMPLE_WIDTH_PADRAO

saida_dir = Path("mic_tests")
saida_dir.mkdir(exist_ok=True)


def parse_record_start(texto: str):
    sample_rate = SAMPLE_RATE_PADRAO
    channels = CHANNELS_PADRAO
    sample_width = SAMPLE_WIDTH_PADRAO

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
        print("Usando configuracao padrao.")

    return sample_rate, channels, sample_width


def diagnosticar_audio(raw: bytes, sample_width: int):
    if not raw:
        print("Audio vazio.")
        return

    try:
        pico = audioop.max(raw, sample_width)
        rms = audioop.rms(raw, sample_width)
    except Exception as erro:
        print("Erro no diagnostico audioop:", erro)
        return

    print()
    print("Diagnostico de amplitude:")
    print(f"  Pico: {pico}")
    print(f"  RMS:  {rms}")

    if pico == 0:
        print("  AVISO: audio totalmente zerado.")
    elif pico < 100:
        print("  AVISO: audio extremamente baixo.")
    elif pico < 1000:
        print("  AVISO: audio baixo.")
    elif pico > 30000:
        print("  AVISO: audio provavelmente estourando/clipping.")
    else:
        print("  Amplitude parece utilizavel.")

    print()


def salvar_wav(raw: bytes, wav_path: Path, sample_rate: int, channels: int, sample_width: int):
    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sample_width)
        wf.setframerate(sample_rate)
        wf.writeframes(raw)


def salvar_arquivos(raw: bytes, sample_rate: int, channels: int, sample_width: int):
    timestamp = time.strftime("%Y%m%d_%H%M%S")

    raw_path = saida_dir / f"cardputer_mic_{timestamp}.raw"
    wav_path = saida_dir / f"cardputer_mic_{timestamp}_{sample_rate}hz.wav"

    raw_path.write_bytes(raw)
    salvar_wav(raw, wav_path, sample_rate, channels, sample_width)

    return raw_path, wav_path


def mostrar_diagnostico_tamanho(total_bytes: int, sample_rate: int, channels: int, sample_width: int):
    segundos = total_bytes / (sample_rate * channels * sample_width)
    esperado_5s = sample_rate * channels * sample_width * 5

    print()
    print("Diagnostico de tamanho:")
    print(f"  Bytes recebidos: {total_bytes}")
    print(f"  Sample rate:     {sample_rate} Hz")
    print(f"  Canais:          {channels}")
    print(f"  Sample width:    {sample_width} bytes")
    print(f"  Duracao aprox:   {segundos:.2f} s")
    print(f"  Esperado 5s:     {esperado_5s} bytes")

    if total_bytes < esperado_5s * 0.7:
        print("  AVISO: veio bem menos audio que o esperado.")
    elif total_bytes > esperado_5s * 1.3:
        print("  AVISO: veio bem mais audio que o esperado.")
    else:
        print("  Tamanho parece coerente.")

    print()


async def handler(websocket):
    global gravando
    global buffer_audio
    global inicio_gravacao
    global sample_rate_atual
    global channels_atual
    global sample_width_atual

    print("Cardputer conectado.")

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                if gravando:
                    buffer_audio.extend(message)
                    print(f"chunk recebido: {len(message)} bytes | total: {len(buffer_audio)}")
                else:
                    print(f"binario ignorado: {len(message)} bytes sem RECORD_START")

                continue

            texto = str(message).strip()
            print("Texto:", texto)

            if texto == "PING":
                await websocket.send("PONG")
                continue

            if texto.startswith("RECORD_START"):
                sample_rate_atual, channels_atual, sample_width_atual = parse_record_start(texto)

                gravando = True
                buffer_audio = bytearray()
                inicio_gravacao = time.time()

                print("RECORD_START recebido")
                print(f"Config: {sample_rate_atual} Hz, {channels_atual} canal, {sample_width_atual} bytes")

                await websocket.send("RECORDING")
                continue

            if texto == "RECORD_END":
                gravando = False

                duracao_envio = time.time() - inicio_gravacao
                total = len(buffer_audio)

                print()
                print("RECORD_END recebido.")
                print(f"Total: {total} bytes.")
                print(f"Tempo de envio: {duracao_envio:.2f}s")

                if total == 0:
                    print("ERRO: audio vazio")
                    await websocket.send("RECORD_EMPTY")
                    continue

                raw_bytes = bytes(buffer_audio)

                diagnosticar_audio(raw_bytes, sample_width_atual)

                raw_path, wav_path = salvar_arquivos(
                    raw_bytes,
                    sample_rate_atual,
                    channels_atual,
                    sample_width_atual,
                )

                print(f"RAW salvo: {raw_path}")
                print(f"WAV salvo: {wav_path}")

                mostrar_diagnostico_tamanho(
                    total,
                    sample_rate_atual,
                    channels_atual,
                    sample_width_atual,
                )

                print("Para ouvir:")
                print(f"  aplay {wav_path}")
                print()
                print("Ou o mais recente:")
                print("  aplay \"$(ls -t mic_tests/*.wav | head -n 1)\"")
                print()

                await websocket.send("RECORD_SAVED")
                continue

            await websocket.send("ECO " + texto)

    except websockets.exceptions.ConnectionClosed:
        print("Cardputer desconectou.")


async def main():
    print(f"Servidor MIC em ws://0.0.0.0:{PORTA}")
    print("Esperando Cardputer...")
    print()

    async with websockets.serve(handler, "0.0.0.0", PORTA, max_size=None):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())