from pathlib import Path
import subprocess
import sys
from faster_whisper import WhisperModel

MIC_DIR = Path("mic_tests")

# Para Raspberry Pi 5 8GB:
# tiny  = mais rápido, menos preciso
# base  = bom primeiro teste
# small = melhor, mas mais lento
MODEL_SIZE = "base"

LANGUAGE = "pt"

def run(cmd):
    print("EXEC:", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)

def latest_wav():
    wavs = sorted(MIC_DIR.glob("*.wav"), key=lambda p: p.stat().st_mtime, reverse=True)

    if not wavs:
        raise FileNotFoundError("Nenhum .wav encontrado em mic_tests/")

    return wavs[0]

def convert_for_whisper(input_wav: Path) -> Path:
    """
    Converte para WAV mono 16kHz.
    Whisper aceita vários formatos, mas isso remove uma variável do teste.
    """
    out = MIC_DIR / "whisper_input_16k.wav"

    run([
        "ffmpeg",
        "-y",
        "-i",
        str(input_wav),
        "-ac",
        "1",
        "-ar",
        "16000",
        "-filter:a",
        "volume=1.5",
        str(out),
    ])

    return out

def transcribe(path: Path):
    print()
    print("Carregando modelo:", MODEL_SIZE)
    print("Arquivo:", path)
    print()

    model = WhisperModel(
        MODEL_SIZE,
        device="cpu",
        compute_type="int8",
    )

    segments, info = model.transcribe(
        str(path),
        language=LANGUAGE,
        beam_size=5,
        vad_filter=False,
        condition_on_previous_text=False,
        temperature=0.0,
    )

    print("Idioma detectado:", info.language)
    print("Probabilidade:", info.language_probability)
    print()

    texto_final = []

    print("Segmentos:")
    print("-" * 40)

    for segment in segments:
        texto = segment.text.strip()
        texto_final.append(texto)

        print(f"[{segment.start:.2f}s -> {segment.end:.2f}s] {texto}")

    print("-" * 40)
    print()
    print("Texto final:")
    print(" ".join(texto_final).strip())
    print()

def main():
    if len(sys.argv) > 1:
        wav = Path(sys.argv[1])
    else:
        wav = latest_wav()

    if not wav.exists():
        raise FileNotFoundError(wav)

    prepared = convert_for_whisper(wav)
    transcribe(prepared)

if __name__ == "__main__":
    main()