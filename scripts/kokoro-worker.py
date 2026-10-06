#!/usr/bin/env python3
"""Kokoro synthesis worker.

Usage: kokoro-worker.py MODEL.onnx VOICES.bin

Reads one JSON request per stdin line: {"id", "text", "voice", "speed"}.
Replies with one JSON header line {"id", "rate", "bytes"} followed by exactly
`bytes` bytes of mono signed 16-bit little-endian PCM, or {"id", "error"}.
"""

import json
import os
import sys

import numpy as np
import onnxruntime as ort
from kokoro_onnx import Kokoro


def main():
    out = sys.stdout.buffer
    # Anything a library prints must not corrupt the binary stream.
    sys.stdout = sys.stderr

    # The full-precision model is several times faster than the int8 one on
    # CPUs without int8 acceleration, and gains nothing from more than 8 threads.
    options = ort.SessionOptions()
    options.intra_op_num_threads = min(8, os.cpu_count() or 1)
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(sys.argv[1], sess_options=options,
                                   providers=["CPUExecutionProvider"])
    kokoro = Kokoro.from_session(session, sys.argv[2])
    voices = set(kokoro.get_voices())

    out.write((json.dumps({"ready": True, "voices": sorted(voices)}) + "\n").encode())
    out.flush()

    for line in sys.stdin:
        request_id = None
        try:
            request = json.loads(line)
            request_id = request.get("id")
            voice = request.get("voice") or "af_heart"
            if voice not in voices:
                voice = "af_heart"
            samples, rate = kokoro.create(
                request["text"], voice=voice,
                speed=float(request.get("speed", 1.0)), lang="en-us",
            )
            pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
            out.write((json.dumps({"id": request_id, "rate": rate, "bytes": len(pcm)},
                                  separators=(",", ":")) + "\n").encode())
            out.write(pcm)
        except Exception as error:  # keep serving after a bad request
            out.write((json.dumps({"id": request_id, "error": str(error)}) + "\n").encode())
        out.flush()


if __name__ == "__main__":
    main()
