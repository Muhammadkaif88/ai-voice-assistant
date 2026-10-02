import os
import io
import wave
import json
import asyncio
from dotenv import load_dotenv
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request, Body
from fastapi.responses import HTMLResponse, StreamingResponse, Response
import google.generativeai as genai
from groq import Groq
import edge_tts

# Load environment variables
load_dotenv()

GROQ_API_KEY = os.getenv("GROQ_API_KEY", "")
GEMINI_API_KEY = os.getenv("GEMINI_API_KEY", "")
TTS_VOICE = os.getenv("TTS_VOICE", "ml-IN-SobhanaNeural")  # Malayalam Voice

# Initialize AI Clients
if GROQ_API_KEY:
    groq_client = Groq(api_key=GROQ_API_KEY)
else:
    groq_client = None

if GEMINI_API_KEY:
    genai.configure(api_key=GEMINI_API_KEY)
    gemini_model = genai.GenerativeModel(
        model_name="gemini-1.5-flash",
        system_instruction=(
            "You are a friendly, witty, and helpful AI voice assistant like Xiaozhi AI. "
            "Your name is 'Kalyani' or 'Smart AI'. You interact primarily in Malayalam or English "
            "(respond in whichever language the user speaks). "
            "Keep your responses short, natural, conversational, and concise (1 to 3 sentences maximum) "
            "so that it sounds great when spoken out loud. Avoid bullet points, code blocks, or markdown formatting."
        )
    )
else:
    gemini_model = None

app = FastAPI(title="ESP32 AI Voice Assistant Backend")

# Simple Web Dashboard to test and monitor
@app.get("/")
async def root():
    return HTMLResponse("""
    <!DOCTYPE html>
    <html>
    <head>
        <title>ESP32 AI Voice Assistant Server</title>
        <style>
            body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #0f172a; color: #f8fafc; padding: 40px; text-align: center; }
            .card { max-width: 600px; margin: 0 auto; background: #1e293b; padding: 30px; border-radius: 16px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); }
            h1 { color: #38bdf8; margin-bottom: 8px; }
            p { color: #94a3b8; }
            .status { display: inline-block; padding: 8px 16px; border-radius: 20px; font-weight: bold; margin-top: 15px; }
            .online { background: #065f46; color: #34d399; }
            .badge { background: #334155; padding: 6px 12px; border-radius: 8px; margin: 5px; font-size: 14px; display: inline-block; }
        </style>
    </head>
    <body>
        <div class="card">
            <h1>🎙️ ESP32 AI Voice Assistant</h1>
            <p>Python Backend Server is Running</p>
            <div class="status online">● Server Online</div>
            <div style="margin-top: 25px; text-align: left;">
                <div class="badge">WebSocket Endpoint: <b>ws://&lt;YOUR_PC_IP&gt;:8000/ws</b></div>
                <div class="badge">STT: Groq Whisper (Blazing Fast)</div>
                <div class="badge">LLM: Google Gemini 1.5 Flash</div>
                <div class="badge">TTS: Edge-TTS (Malayalam/English)</div>
            </div>
        </div>
    </body>
    </html>
    """)

def pcm_to_wav_bytes(pcm_data: bytes, sample_rate: int = 16000, channels: int = 1, sampwidth: int = 2) -> bytes:
    """Wrap raw 16-bit PCM data into a valid WAV format in memory."""
    wav_io = io.BytesIO()
    with wave.open(wav_io, 'wb') as wav_file:
        wav_file.setnchannels(channels)
        wav_file.setsampwidth(sampwidth)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_data)
    return wav_io.getvalue()

async def transcribe_audio_groq(wav_bytes: bytes) -> str:
    """Transcribe speech WAV audio to text using Groq Whisper."""
    if not groq_client:
        return "Groq API key missing in .env"
    
    audio_file = ("audio.wav", wav_bytes, "audio/wav")
    transcription = groq_client.audio.transcriptions.create(
        model="whisper-large-v3",
        file=audio_file,
        response_format="text"
    )
    return str(transcription).strip()

async def generate_llm_response(prompt: str) -> str:
    """Generate intelligent response using Gemini LLM."""
    if not gemini_model:
        return "Gemini API key is not configured."
    try:
        response = await asyncio.to_thread(gemini_model.generate_content, prompt)
        return response.text.strip()
    except Exception as e:
        print(f"[LLM Error] {e}")
        return "ക്ഷമിക്കണം, എനിക്ക് ഉത്തരം കണ്ടെത്താൻ സാധിച്ചില്ല."

async def synthesize_speech_edge_tts(text: str, voice: str = TTS_VOICE) -> bytes:
    """Convert text to 16kHz 16-bit mono PCM audio bytes."""
    communicate = edge_tts.Communicate(text, voice)
    audio_stream = io.BytesIO()
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            audio_stream.write(chunk["data"])
    
    mp3_bytes = audio_stream.getvalue()
    
    try:
        from pydub import AudioSegment
        audio_seg = AudioSegment.from_file(io.BytesIO(mp3_bytes), format="mp3")
        audio_seg = audio_seg.set_frame_rate(16000).set_channels(1).set_sample_width(2)
        return audio_seg.raw_data
    except Exception as e:
        print(f"[Audio Conversion Warning] {e}")
@app.post("/api/chat-voice")
async def chat_voice_endpoint(request: Request):
    """Receive raw PCM audio from ESP32, process with Whisper + Gemini + Edge-TTS, stream back PCM audio."""
    try:
        pcm_bytes = await request.body()
        print(f"[HTTP Voice] Received {len(pcm_bytes)} bytes audio from ESP32")
        
        if len(pcm_bytes) < 1600:
            return StreamingResponse(io.BytesIO(b""), media_type="audio/pcm")
            
        wav_data = pcm_to_wav_bytes(pcm_bytes)
        user_text = await transcribe_audio_groq(wav_data)
        print(f"[HTTP User Said]: {user_text}")
        
        if not user_text or len(user_text.strip()) == 0:
            user_text = "ഹലോ"
            
        ai_reply = await generate_llm_response(user_text)
        print(f"[HTTP AI Reply]: {ai_reply}")
        
        tts_pcm = await synthesize_speech_edge_tts(ai_reply)
        print(f"[HTTP TTS] Generated {len(tts_pcm)} bytes PCM audio. Streaming to ESP32...")
        
        return StreamingResponse(
            io.BytesIO(tts_pcm), 
            media_type="audio/pcm",
            headers={
                "X-User-Text": user_text[:100],
                "X-AI-Reply": ai_reply[:100]
            }
        )
    except Exception as e:
        print(f"[HTTP Error] {e}")
@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    print("[WebSocket] ESP32 Connected!")
    
    audio_buffer = bytearray()
    
    try:
        # Notify ESP32 that connection is ready
        await websocket.send_json({"type": "state", "value": "idle", "text": "Ready"})
        
        while True:
            message = await websocket.receive()
            
            # If ESP32 sends binary audio chunk
            if "bytes" in message and message["bytes"]:
                chunk = message["bytes"]
                audio_buffer.extend(chunk)
                
            # If ESP32 sends a text command (e.g. "START", "STOP_RECORDING")
            elif "text" in message and message["text"]:
                try:
                    data = json.loads(message["text"])
                    event = data.get("event")
                except Exception:
                    event = message["text"]
                
                if event == "START_RECORDING":
                    audio_buffer.clear()
                    print("[Voice] Started recording...")
                    await websocket.send_json({"type": "state", "value": "listening"})
                    
                elif event == "STOP_RECORDING":
                    print(f"[Voice] Stopped recording. Total audio size: {len(audio_buffer)} bytes")
                    if len(audio_buffer) < 1600:
                        print("[Voice] Audio too short, ignoring.")
                        await websocket.send_json({"type": "state", "value": "idle", "text": "Too short"})
                        audio_buffer.clear()
                        continue
                    
                    # 1. Update State -> Thinking
                    await websocket.send_json({"type": "state", "value": "thinking"})
                    
                    # 2. Convert PCM to WAV
                    wav_data = pcm_to_wav_bytes(bytes(audio_buffer))
                    audio_buffer.clear()
                    
                    # 3. Speech to Text (Whisper)
                    user_text = await transcribe_audio_groq(wav_data)
                    print(f"[User Said]: {user_text}")
                    
                    if not user_text:
                        await websocket.send_json({"type": "state", "value": "idle", "text": "No speech detected"})
                        continue
                    
                    # 4. LLM Generation (Gemini)
                    ai_reply = await generate_llm_response(user_text)
                    print(f"[AI Reply]: {ai_reply}")
                    
                    # 5. Text to Speech (Edge-TTS MP3)
                    await websocket.send_json({
                        "type": "state", 
                        "value": "speaking", 
                        "user_text": user_text, 
                        "ai_reply": ai_reply
                    })
                    
                    tts_audio = await synthesize_speech_edge_tts(ai_reply)
                    print(f"[TTS] Generated {len(tts_audio)} bytes MP3 audio. Sending to ESP32...")
                    
                    # 6. Stream MP3 Audio chunks to ESP32
                    chunk_size = 1024
                    for i in range(0, len(tts_audio), chunk_size):
                        await websocket.send_bytes(tts_audio[i:i+chunk_size])
                        await asyncio.sleep(0.005) # smooth transmission
                        
                    # 7. Notify ESP32 that audio stream is finished
                    await websocket.send_json({"type": "audio_end"})
                    print("[TTS] Audio stream complete.")
                    
    except WebSocketDisconnect:
        print("[WebSocket] ESP32 Disconnected")
    except Exception as e:
        print(f"[WebSocket Error] {e}")
        try:
            await websocket.send_json({"type": "state", "value": "error", "message": str(e)})
        except:
            pass

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
