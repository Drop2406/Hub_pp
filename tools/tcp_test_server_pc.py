import getpass
import json
import ssl
import time
import urllib.request
import uuid
import wave
import io

GROQ_URL = "https://api.groq.com/openai/v1/audio/transcriptions"
MODEL = "whisper-large-v3-turbo"

SAMPLE_RATE = 16000
DURATION_SECONDS = 5
SAMPLE_COUNT = SAMPLE_RATE * DURATION_SECONDS

print("======================================")
print("PC -> Groq STT upload benchmark")
print("======================================")
print()
print("This sends a 5-second silent WAV")
print("matching the ESP32 payload size.")
print()

api_key = getpass.getpass("Groq API key: ").strip()

if not api_key:
    raise SystemExit("API key is empty.")

# Build a 5-second, 16 kHz, mono, PCM16 WAV in memory.
wav_buffer = io.BytesIO()

with wave.open(wav_buffer, "wb") as wav:
    wav.setnchannels(1)
    wav.setsampwidth(2)
    wav.setframerate(SAMPLE_RATE)
    wav.writeframes(b"\x00\x00" * SAMPLE_COUNT)

wav_bytes = wav_buffer.getvalue()

print(f"WAV size: {len(wav_bytes)} bytes")

boundary = "----PythonGroqBoundary" + uuid.uuid4().hex

prefix = (
    f"--{boundary}\r\n"
    'Content-Disposition: form-data; name="model"\r\n'
    "\r\n"
    f"{MODEL}\r\n"
    f"--{boundary}\r\n"
    'Content-Disposition: form-data; name="response_format"\r\n'
    "\r\n"
    "json\r\n"
    f"--{boundary}\r\n"
    'Content-Disposition: form-data; name="file"; filename="benchmark.wav"\r\n'
    "Content-Type: audio/wav\r\n"
    "\r\n"
).encode("utf-8")

suffix = (
    f"\r\n--{boundary}--\r\n"
).encode("utf-8")

body = prefix + wav_bytes + suffix

print(f"HTTP body size: {len(body)} bytes")
print()
print("Sending to Groq...")

request = urllib.request.Request(
    GROQ_URL,
    data=body,
    method="POST",
    headers={
        "Authorization": f"Bearer {api_key}",
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Content-Length": str(len(body)),
        "User-Agent": "ESP32-Groq-Benchmark/1.0",
    },
)

ssl_context = ssl.create_default_context()

start = time.perf_counter()

try:
    with urllib.request.urlopen(
        request,
        timeout=60,
        context=ssl_context,
    ) as response:
        response_body = response.read()
        status = response.status

except Exception as exc:
    elapsed = time.perf_counter() - start
    print()
    print("FAILED")
    print(f"Time : {elapsed:.3f} s")
    print(f"Error: {exc}")
    raise SystemExit(1)

elapsed = time.perf_counter() - start

kib = len(body) / 1024.0
kib_per_sec = kib / elapsed
mbps = (len(body) * 8.0) / elapsed / 1_000_000.0

print()
print("------------- RESULT -------------")
print(f"HTTP status : {status}")
print(f"Body size   : {len(body)} bytes")
print(f"Total time  : {elapsed:.3f} s")
print(f"Avg speed   : {kib_per_sec:.2f} KiB/s")
print(f"Avg speed   : {mbps:.3f} Mbps")

try:
    parsed = json.loads(response_body.decode("utf-8"))
    print(f"Response    : {parsed}")
except Exception:
    print(f"Response    : {response_body[:500]!r}")

print("----------------------------------")
