from pathlib import Path
import wave
import struct

PASTA = Path("mic_tests")
SAMPLE_RATE = 17000
SAMPLE_WIDTH = 2
CHANNELS = 1

raw_files = sorted(PASTA.glob("*.raw"), key=lambda p: p.stat().st_mtime, reverse=True)

if not raw_files:
    raise SystemExit("Nenhum .raw encontrado em mic_tests/")

raw_path = raw_files[0]
raw = raw_path.read_bytes()

print(f"Usando RAW: {raw_path}")
print(f"Tamanho: {len(raw)} bytes")

# Garante tamanho par
if len(raw) % 2 != 0:
    raw = raw[:-1]

samples = list(struct.unpack("<" + "h" * (len(raw) // 2), raw))

print(f"Samples totais: {len(samples)}")

pares = samples[0::2]
impares = samples[1::2]

def salvar_wav(nome, samples_out, sample_rate=SAMPLE_RATE):
    out_path = raw_path.with_name(nome)
    data = struct.pack("<" + "h" * len(samples_out), *samples_out)

    with wave.open(str(out_path), "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(sample_rate)
        wf.writeframes(data)

    print(f"Gerado: {out_path}")

salvar_wav(raw_path.stem + "_mono_original_17000hz.wav", samples, 17000)
salvar_wav(raw_path.stem + "_samples_pares_17000hz.wav", pares, 17000)
salvar_wav(raw_path.stem + "_samples_impares_17000hz.wav", impares, 17000)

# Também gera pares/ímpares em 8500 Hz, caso o buffer original seja estéreo intercalado por frames.
salvar_wav(raw_path.stem + "_samples_pares_8500hz.wav", pares, 8500)
salvar_wav(raw_path.stem + "_samples_impares_8500hz.wav", impares, 8500)