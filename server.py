import asyncio
import base64
import concurrent.futures
import datetime
import io
import json
import os
import re
import string
import tempfile
import time
import traceback
import urllib.parse
import urllib.request
import wave
import xml.etree.ElementTree as ET
from html import unescape

import numpy as np
import websockets
from sarvamai import SarvamAI


# OPTIONAL SCIPY

try:
    from scipy.signal import resample_poly
    from math import gcd

    HAVE_SCIPY = True
except ImportError:
    HAVE_SCIPY = False


# WEB SEARCH LIBRARY

try:
    from ddgs import DDGS
except ImportError:
    try:
        from duckduckgo_search import DDGS
    except ImportError:
        DDGS = None



# SERVER
HOST = "0.0.0.0"
PORT = 8765


# INPUT AUDIO FROM ESP32
INPUT_SAMPLE_RATE = 16000
INPUT_CHANNELS = 1
INPUT_SAMPLE_WIDTH = 2

MIN_UTTERANCE_SECONDS = 0.5


# WAKE WORD

WAKE_WORDS = {
    "victor", "viktor", "vikter", "victer", "wiktor", "victor's",
    "विक्टर", "विक्टोर", "विक्तर", "विक्टर।",
}

WAKE_MAX_POSITION = 4

# Seconds after Victor finishes speaking during which you can answer
# WITHOUT saying "Victor". Set to 0.0 to require "Victor" every time.
FOLLOWUP_SECONDS = 8.0

WAKE_ACK = "Yes, I'm listening."

END_PHRASES = {
    "stop", "thanks", "thank you", "bye", "goodbye", "that's all",
    "that is all", "okay thanks", "ok thanks", "okay thank you",
    "never mind", "nevermind",
}


# TTS / AUDIO OUT (must match the ESP32 sketch)

TTS_SAMPLE_RATE = 22050
TTS_CHANNELS = 1
TTS_SAMPLE_WIDTH = 2

BYTES_PER_SECOND = TTS_SAMPLE_RATE * TTS_CHANNELS * TTS_SAMPLE_WIDTH

TTS_PEAK_TARGET = 0.90

# Gentle streaming (avoids flooding the ESP32)
CHUNK_SIZE = 1024
PREBUFFER_BYTES = 24 * 1024       # matches TTS_PREBUFFER_BYTES on the ESP32
PREBUFFER_SPEED = 3.0             # prebuffer is sent at 3x real time
LEAD_SECONDS = 0.4                # stay this far ahead of playback

MAX_HISTORY_MESSAGES = 20


# SEARCH / SCRAPING SETTINGS

SEARCH_MAX_RESULTS = 5
SEARCH_SNIPPET_CHARS = 400
SEARCH_REGION = "wt-wt"           # "in-en" prefers India results
SEARCH_RETRIES = 3

SCRAPE_PAGES = 2                  # read the text of the top N pages
SCRAPE_CHARS = 1200               # per page
SCRAPE_TIMEOUT = 5
DIGEST_MAX_CHARS = 4000

SCRAPE_SKIP_DOMAINS = (
    "youtube.com", "youtu.be", "facebook.com", "instagram.com",
    "twitter.com", "x.com", "tiktok.com", "linkedin.com",
    "pinterest.com", "reddit.com",
)

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0 Safari/537.36"
)

# Used for weather and the system prompt. Example: "Bhubaneswar, Odisha"
USER_LOCATION = ""

# Questions that always trigger a search (no need to ask the LLM first)
FORCE_SEARCH_RE = re.compile(
    r"\b(weather|temperature|forecast|news|headlines|score|scores|"
    r"price|prices|stock|stocks|latest|trending|who won|"
    r"release date|exchange rate|petrol price|gold rate)\b",
    re.IGNORECASE,
)

# ...except plain date/time questions (the system prompt has the date)
NO_SEARCH_RE = re.compile(
    r"\b(what('s| is)? (the )?(time|day|date)|date today|time now|"
    r"what day is it)\b",
    re.IGNORECASE,
)

WEATHER_RE = re.compile(
    r"\b(weather|temperature|forecast|humidity)\b",
    re.IGNORECASE,
)

NEWS_RE = re.compile(
    r"\b(news|headlines|latest|today|yesterday|score|scores)\b",
    re.IGNORECASE,
)

SEARCH_RE = re.compile(r"SEARCH:\s*(.+)", re.IGNORECASE)


# MUSIC SETTINGS

MUSIC_VOLUME = 0.8
DEFAULT_MUSIC_QUERY = "popular songs"
MAX_SONG_SECONDS = 600

# Do NOT stop music just because the mic detects sound
BARGE_IN_STOPS_MUSIC = False

MUSIC_STOP_PHRASES = {
    "stop", "stop victor", "victor stop",
    "stop the music", "stop music",
    "stop the song", "stop song",
    "stop playing",
    "pause", "pause music", "pause the music",
    "turn off the music", "turn the music off",
    "stop victor playing", "victor stop playing",
}

# Spoken after a stop command. Set to "" to stay silent.
STOP_ACK = "Okay."

STOP_TRIGGER_WORDS = {"stop", "pause", "quit", "enough"}

STOP_FILLER_WORDS = STOP_TRIGGER_WORDS | {
    "music", "song", "the", "please", "victor", "playing", "it",
    "this", "now", "that", "off",
}


def is_stop_command(normalized):
    """'stop', 'stop stop', 'please stop the music now' ..."""

    tokens = normalized.split()

    return (
        bool(tokens)
        and any(t in STOP_TRIGGER_WORDS for t in tokens)
        and all(t in STOP_FILLER_WORDS for t in tokens)
    )


PLAY_RE = re.compile(
    r"^(?:(?:can|could|will) you |please )*"
    r"(?:play|put on|start playing)\s+(.+)$",
    re.IGNORECASE,
)

PUNCT = string.punctuation + "।॥…“”‘’"


# SARVAM

SARVAM_API_KEY = os.environ.get("SARVAM_API_KEY")

if not SARVAM_API_KEY:
    raise RuntimeError("SARVAM_API_KEY environment variable is not set.")

sarvam = SarvamAI(api_subscription_key=SARVAM_API_KEY)


# CONVERSATION


conversation_history = []


def build_system_prompt():

    today = datetime.datetime.now().strftime("%A, %d %B %Y")

    loc = f" The user is in {USER_LOCATION}." if USER_LOCATION else ""

    return {
        "role": "system",
        "content": (
            "You are Victor, a helpful personal voice assistant. "
            "Your answers will be spoken aloud. "
            "Keep responses natural, concise and conversational, "
            "at most three short sentences unless asked for detail. "
            "Do not use markdown, lists or URLs. "
            f"Today's date is {today}.{loc} "
            "You can search the web. "
            "If the question needs current or real-time information "
            "(news, weather, sports scores, prices, recent events, "
            "anything after your training data) or a fact you are not "
            "sure about, reply with ONLY this line and nothing else: "
            "SEARCH: <short search query>. "
            "Otherwise answer directly. "
            "When you are given search results, answer from them in "
            "one to three short spoken sentences."
        ),
    }


def trim_history():

    global conversation_history

    if len(conversation_history) > MAX_HISTORY_MESSAGES:
        conversation_history = conversation_history[-MAX_HISTORY_MESSAGES:]

    while (
        conversation_history
        and conversation_history[0]["role"] != "user"
    ):
        conversation_history.pop(0)


# STT

def speech_to_text(wav_path):

    print()
    print("================================")
    print("SARVAM SAARAS V4")
    print("================================")

    try:
        with open(wav_path, "rb") as audio_file:
            response = sarvam.speech_to_text.transcribe(
                file=audio_file,
                model="saaras:v4",
                mode="codemix",
            )

        transcript = response.transcript.strip()
        print("TRANSCRIPT:", transcript)
        return transcript

    except Exception as e:
        print("STT ERROR:", e)
        traceback.print_exc()
        return ""


# HTTP / SCRAPING HELPERS

def http_get(url, timeout=SCRAPE_TIMEOUT, max_bytes=400_000):
    """Download a page. Returns (text, content_type)."""

    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept-Language": "en-US,en;q=0.9",
            "Accept": "text/html,application/xhtml+xml,text/plain,*/*",
        },
    )

    with urllib.request.urlopen(req, timeout=timeout) as resp:
        ctype = resp.headers.get("Content-Type", "") or ""
        charset = resp.headers.get_content_charset() or "utf-8"
        data = resp.read(max_bytes)

    return data.decode(charset, errors="replace"), ctype


def html_to_text(html):
    """Readable text from HTML (prefers real paragraphs)."""

    html = re.sub(
        r"(?is)<(script|style|noscript|svg|nav|footer|header|form|aside)"
        r"[^>]*>.*?</\1>",
        " ",
        html,
    )
    html = re.sub(r"(?s)<!--.*?-->", " ", html)

    paragraphs = []

    for p in re.findall(r"(?is)<p[^>]*>(.*?)</p>", html):
        t = unescape(re.sub(r"(?s)<[^>]+>", " ", p))
        t = re.sub(r"\s+", " ", t).strip()

        if len(t) >= 60:
            paragraphs.append(t)

    if paragraphs:
        return " ".join(paragraphs)

    text = unescape(re.sub(r"(?s)<[^>]+>", " ", html))

    return re.sub(r"\s+", " ", text).strip()


def fetch_page_text(url):
    """Scrape the main text of a page. Returns '' on any failure."""

    try:
        host = urllib.parse.urlparse(url).netloc.lower()

        if any(d in host for d in SCRAPE_SKIP_DOMAINS):
            return ""

        body, ctype = http_get(url)

        if "html" not in ctype.lower() and "text" not in ctype.lower():
            return ""

        return html_to_text(body)[:SCRAPE_CHARS]

    except Exception as e:
        print(f"  (scrape failed: {url[:60]}... {e})")
        return ""


def get_weather(query):
    """Live weather from wttr.in (plain text). Returns '' on failure."""

    q = query.strip(" ?.!")

    m = re.search(r"\b(?:in|at|for|of)\s+([A-Za-z][A-Za-z .,'-]{1,40})$", q)

    place = m.group(1) if m else ""

    place = re.sub(
        r"\b(today|tomorrow|now|right now|tonight|currently|please)\b",
        "",
        place,
        flags=re.IGNORECASE,
    ).strip(" ,.")

    if not place and USER_LOCATION:
        place = USER_LOCATION.split(",")[0].strip()

    fmt = "%l: %C, %t, feels like %f, humidity %h, wind %w, rain %p"

    url = (
        "https://wttr.in/"
        + urllib.parse.quote(place)
        + "?m&format="
        + urllib.parse.quote(fmt, safe="")
    )

    try:
        text, _ = http_get(url, timeout=8, max_bytes=5000)
        text = text.strip()

        if (
            not text
            or "unknown location" in text.lower()
            or "<html" in text.lower()
        ):
            return ""

        return text

    except Exception as e:
        print("  (weather lookup failed:", e, ")")
        return ""


def google_news_rss(query):
    """Fallback news source when DuckDuckGo returns nothing."""

    url = (
        "https://news.google.com/rss/search?q="
        + urllib.parse.quote_plus(query)
        + "&hl=en-IN&gl=IN&ceid=IN:en"
    )

    try:
        xml_text, _ = http_get(url, timeout=8, max_bytes=600_000)
        root = ET.fromstring(xml_text)

        out = []

        for item in root.findall(".//item")[:SEARCH_MAX_RESULTS]:
            out.append(
                {
                    "title": item.findtext("title") or "",
                    "body": item.findtext("pubDate") or "",
                }
            )

        return out

    except Exception as e:
        print("  (Google News fallback failed:", e, ")")
        return []


def ddgs_search(query, news):
    """DuckDuckGo with retries."""

    for attempt in range(SEARCH_RETRIES):

        try:
            results = []

            with DDGS(timeout=10) as ddgs:

                if news:
                    results = list(
                        ddgs.news(
                            query,
                            region=SEARCH_REGION,
                            max_results=SEARCH_MAX_RESULTS,
                        )
                    )

                if not results:
                    results = list(
                        ddgs.text(
                            query,
                            region=SEARCH_REGION,
                            max_results=SEARCH_MAX_RESULTS,
                        )
                    )

            if results:
                return results

            print(f"  (search attempt {attempt + 1}: no results)")

        except Exception as e:
            print(f"  (search attempt {attempt + 1} failed: {e})")

        time.sleep(1.0 * (attempt + 1))

    return []


# WEB SEARCH (search + scrape)

def web_search(query):

    print()
    print("================================")
    print("WEB SEARCH:", query)
    print("================================")

    parts = []

    # ---- live weather ----
    got_weather = False

    if WEATHER_RE.search(query):
        weather = get_weather(query)

        if weather:
            parts.append(f"- Live weather: {weather}")
            got_weather = True

    # ---- search engine ----
    results = []

    wants_news = bool(NEWS_RE.search(query))

    if DDGS is not None:
        results = ddgs_search(query, wants_news)
    else:
        print("Search library missing. Run: pip install ddgs")

    if not results and wants_news:
        results = google_news_rss(query)

    for r in results:

        title = (r.get("title") or "").strip()
        body = (r.get("body") or "").strip()
        date = (r.get("date") or "").strip()

        line = f"- {title}"

        if date:
            line += f" ({date[:16]})"

        if body:
            line += f": {body[:SEARCH_SNIPPET_CHARS]}"

        parts.append(line)

    # ---- scrape the top pages for more detail ----
    if results and not got_weather:

        urls = [
            r.get("href")
            for r in results
            if r.get("href")
        ][:SCRAPE_PAGES]

        if urls:
            with concurrent.futures.ThreadPoolExecutor(
                max_workers=len(urls)
            ) as pool:
                pages = list(pool.map(fetch_page_text, urls))

            for url, text in zip(urls, pages):

                if text:
                    host = urllib.parse.urlparse(url).netloc
                    parts.append(f"- Page text from {host}: {text}")

    digest = "\n".join(parts)[:DIGEST_MAX_CHARS]

    if digest:
        print(digest)
    else:
        print("NO SEARCH RESULTS.")

    return digest


# AI

def chat(messages):

    response = sarvam.chat.completions(
        model="sarvam-105b-conversations",
        messages=messages,
        temperature=0.5,
        max_tokens=500,
        reasoning_effort=None,
    )

    return response.choices[0].message.content.strip()


def clean_for_speech(text):

    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"[*_`#>]+", "", text)

    return re.sub(r"\s+", " ", text).strip()


def forced_search_query(text):
    """Return a search query if the question clearly needs the web."""

    if NO_SEARCH_RE.search(text):
        return ""

    if FORCE_SEARCH_RE.search(text):
        return text.strip(PUNCT + " ")

    return ""


def ask_ai(text):

    print()
    print("================================")
    print("SARVAM 105B CONVERSATIONS")
    print("================================")

    try:
        conversation_history.append({"role": "user", "content": text})
        trim_history()

        messages = [build_system_prompt()] + conversation_history

        query = forced_search_query(text)
        first = ""

        if query:
            print("Search needed (keyword):", query)
            first = f"SEARCH: {query}"
        else:
            first = chat(messages)
            print("AI (first pass):", first)

            m = SEARCH_RE.search(first)

            if m:
                query = (
                    m.group(1)
                    .strip()
                    .splitlines()[0]
                    .strip(" \"'")
                )

        if query:

            results = web_search(query)

            if results:

                followup = messages + [
                    {"role": "assistant", "content": first},
                    {
                        "role": "user",
                        "content": (
                            f"Search results for '{query}':\n{results}\n\n"
                            "Using these results, answer my original "
                            "question in one to three short spoken "
                            "sentences. Do not mention the search or "
                            "read out any URLs."
                        ),
                    },
                ]

                answer = chat(followup)

                # Model repeated the SEARCH line instead of answering
                if SEARCH_RE.search(answer):
                    answer = (
                        "Sorry, I couldn't find a clear answer to that."
                    )

            else:
                answer = (
                    "Sorry, I couldn't look that up right now. "
                    "Please try again in a bit."
                )

        else:
            answer = first

        answer = clean_for_speech(answer)

        conversation_history.append(
            {"role": "assistant", "content": answer}
        )

        print("AI:", answer)

        return answer

    except Exception as e:
        print("AI ERROR:", e)
        traceback.print_exc()
        return ""


# WAKE WORD

def clean_token(tok):
    return tok.strip(PUNCT + " ").lower()


def normalize_phrase(text):
    return " ".join(clean_token(t) for t in text.split()).strip()


def extract_wake_word(transcript):

    tokens = transcript.split()
    cleaned = [clean_token(t) for t in tokens]

    # Victor at the beginning
    for i, tok in enumerate(cleaned):
        if tok in WAKE_WORDS and i < WAKE_MAX_POSITION:
            command = " ".join(tokens[i + 1:])
            return True, command.strip(PUNCT + " -")

    # Victor at the end
    for i in range(max(0, len(cleaned) - 2), len(cleaned)):
        if cleaned[i] in WAKE_WORDS:
            command = " ".join(tokens[:i])
            return True, command.strip(PUNCT + " -")

    return False, ""


# PLAY COMMAND

def parse_play_command(command):

    m = PLAY_RE.match(command.strip(PUNCT + " "))

    if not m:
        return None

    query = m.group(1).strip(PUNCT + " ")

    for _ in range(3):
        query = re.sub(
            r"\s*\b(?:for me|please|now)\s*$",
            "",
            query,
            flags=re.IGNORECASE,
        ).strip(PUNCT + " ")

    query = re.sub(
        r"\s+(?:on|from|in)\s+(?:youtube|yt)\b.*$",
        "",
        query,
        flags=re.IGNORECASE,
    )

    query = re.sub(
        r"^(?:the |some |a )?(?:song|songs|music|track)\b\s*",
        "",
        query,
        flags=re.IGNORECASE,
    )

    query = query.strip(PUNCT + " ")

    return query or DEFAULT_MUSIC_QUERY


# AUDIO HELPERS

def to_mono(samples, channels):

    x = samples.astype(np.float32)

    if channels == 1:
        return x

    if channels == 2:
        return x.reshape(-1, 2).mean(axis=1)

    raise RuntimeError(f"Unsupported TTS channels: {channels}")


def resample(x, src_rate, dst_rate):

    if src_rate == dst_rate:
        return x

    print(f"Resampling {src_rate} -> {dst_rate} Hz")

    if HAVE_SCIPY:
        g = gcd(src_rate, dst_rate)
        return resample_poly(
            x, dst_rate // g, src_rate // g
        ).astype(np.float32)

    n_out = int(round(len(x) * dst_rate / src_rate))
    t_out = np.linspace(0, len(x) - 1, n_out)

    return np.interp(t_out, np.arange(len(x)), x).astype(np.float32)


def normalize(x):

    if len(x) == 0:
        return x

    x = x - np.mean(x)

    peak = np.max(np.abs(x))

    if peak < 1.0:
        return x

    return x * (TTS_PEAK_TARGET * 32767.0 / peak)


# TTS

def text_to_speech_pcm(text):

    print()
    print("================================")
    print("SARVAM BULBUL V3")
    print("================================")

    response = sarvam.text_to_speech.convert(
        text=text,
        model="bulbul:v3",
        language_code="en-IN",
        speaker="shubh",
        speech_sample_rate=TTS_SAMPLE_RATE,
        output_audio_codec="wav",
    )

    if not response.audios:
        raise RuntimeError("Sarvam TTS returned no audio.")

    wav_bytes = base64.b64decode(response.audios[0])

    with wave.open(io.BytesIO(wav_bytes), "rb") as wav:
        sample_rate = wav.getframerate()
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        pcm = wav.readframes(wav.getnframes())

    print(
        "TTS from Sarvam:",
        sample_rate, "Hz |",
        channels, "channels |",
        sample_width, "bytes/sample",
    )

    if sample_width != 2:
        raise RuntimeError("TTS audio is not 16-bit PCM.")

    samples = np.frombuffer(pcm, dtype="<i2")

    x = to_mono(samples, channels)
    x = resample(x, sample_rate, TTS_SAMPLE_RATE)
    x = normalize(x)

    out = np.clip(x, -32768, 32767).astype("<i2").tobytes()

    print("PCM bytes:", len(out))

    return out


# SAVE MIC PCM

def save_pcm_as_wav(audio_data):

    temp_file = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    path = temp_file.name
    temp_file.close()

    with wave.open(path, "wb") as wav:
        wav.setnchannels(INPUT_CHANNELS)
        wav.setsampwidth(INPUT_SAMPLE_WIDTH)
        wav.setframerate(INPUT_SAMPLE_RATE)
        wav.writeframes(audio_data)

    return path


# JSON

async def send_json(websocket, data):

    try:
        await websocket.send(json.dumps(data))
        return True

    except Exception as e:
        print("JSON send failed:", e)
        return False


# PCM CHUNKS

async def pcm_chunks(pcm):

    for i in range(0, len(pcm), CHUNK_SIZE):
        yield pcm[i:i + CHUNK_SIZE]



# PACED AUDIO STREAM

# Prebuffer goes out at PREBUFFER_SPEED x real time, afterwards

# we stay LEAD_SECONDS ahead of playback. Always yields to the
# event loop so pings/stop events are handled.

async def stream_paced(websocket, chunks):

    position = 0
    start_time = time.monotonic()
    leftover = b""

    async for chunk in chunks:

        chunk = leftover + chunk

        if len(chunk) % 2:
            leftover = chunk[-1:]
            chunk = chunk[:-1]
        else:
            leftover = b""

        if not chunk:
            continue

        await websocket.send(chunk)

        position += len(chunk)

        seconds_of_audio = position / BYTES_PER_SECOND

        if position <= PREBUFFER_BYTES:
            due = seconds_of_audio / PREBUFFER_SPEED
        else:
            due = seconds_of_audio - LEAD_SECONDS

        wait = due - (time.monotonic() - start_time)

        await asyncio.sleep(max(wait, 0.002))


# SEND AUDIO

async def send_audio(websocket, chunks, label="audio"):

    print()
    print("================================")
    print(f"SENDING {label.upper()} TO ESP32")
    print("================================")

    if not await send_json(
        websocket,
        {
            "type": "tts_start",
            "sample_rate": TTS_SAMPLE_RATE,
            "channels": TTS_CHANNELS,
            "sample_width": TTS_SAMPLE_WIDTH,
        },
    ):
        return False

    # Let the ESP32 switch I2S from mic to DAC
    await asyncio.sleep(0.4)

    try:
        await stream_paced(websocket, chunks)

    except asyncio.CancelledError:

        print(f"{label.upper()} INTERRUPTED")

        try:
            await send_json(websocket, {"type": "tts_stop"})
        except Exception:
            pass

        raise

    except (
        websockets.exceptions.ConnectionClosed,
        ConnectionResetError,
    ) as e:
        print(f"ESP32 disconnected during {label}:", e)
        return False

    except Exception as e:
        print(f"{label} send error:", e)
        return False

    await send_json(websocket, {"type": "tts_end"})

    # Let the buffered tail finish playing
    await asyncio.sleep(LEAD_SECONDS + 0.8)

    print(f"{label} transmission complete.")

    return True


# SEND TTS

async def send_tts_audio(websocket, pcm_audio):

    total = len(pcm_audio)
    total -= total % 2
    pcm_audio = pcm_audio[:total]

    print("Total bytes:", total)
    print("Duration:", round(total / BYTES_PER_SECOND, 2), "seconds")

    return await send_audio(websocket, pcm_chunks(pcm_audio), "tts")


# SPEAK

async def speak(websocket, text):

    try:
        pcm_audio = await asyncio.to_thread(text_to_speech_pcm, text)

        if not pcm_audio:
            return False

        return await send_tts_audio(websocket, pcm_audio)

    except asyncio.CancelledError:

        print("TTS INTERRUPTED")

        try:
            await send_json(websocket, {"type": "tts_stop"})
        except Exception:
            pass

        raise

    except Exception as e:

        print("TTS ERROR:", e)
        traceback.print_exc()

        await send_json(
            websocket,
            {"type": "error", "message": "TTS failed."},
        )

        return False


# STOP SPEAKING

async def stop_speaking(state):

    task = state.get("tts_task")

    if task is None:
        return False

    if task.done():
        state["tts_task"] = None
        return False

    print("Stopping Victor speech...")

    task.cancel()

    try:
        await asyncio.wait_for(
            asyncio.gather(task, return_exceptions=True), timeout=5
        )
    except Exception:
        print("(speech task did not stop in time)")

    state["tts_task"] = None

    try:
        await send_json(state["websocket"], {"type": "tts_stop"})
    except Exception:
        pass

    print("Victor speech stopped.")

    return True


# START SPEAKING

async def start_speaking(websocket, state, text):

    await stop_speaking(state)

    task = asyncio.create_task(speak(websocket, text))

    state["tts_task"] = task

    try:
        return await task

    except asyncio.CancelledError:
        return False

    finally:
        if state.get("tts_task") is task:
            state["tts_task"] = None

