#!/bin/sh
# Opt-in read-aloud setup: an isolated Kokoro runtime plus the model files.
set -eu

tts_dir=${1:?usage: setup-tts.sh TTS_DIR}
python=${GLANCE_PYTHON:-}

if [ -z "$python" ]; then
    for candidate in python3.12 python3.11 python3.10; do
        if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c 'import sys' >/dev/null 2>&1; then
            python=$candidate
            break
        fi
    done
fi

if [ -z "$python" ] && command -v mise >/dev/null 2>&1; then
    python=$(mise where python@3.11)/bin/python
fi

if [ -z "$python" ]; then
    printf '%s\n' 'Read aloud requires Python 3.10-3.12. Set GLANCE_PYTHON to a compatible interpreter.' >&2
    exit 1
fi

base=https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0
mkdir -p "$tts_dir"

if [ ! -x "$tts_dir/runtime/bin/python" ] \
        || ! "$tts_dir/runtime/bin/python" -c 'import kokoro_onnx' >/dev/null 2>&1; then
    rm -rf "$tts_dir/runtime"
    "$python" -m venv "$tts_dir/runtime"
    "$tts_dir/runtime/bin/python" -m pip install --disable-pip-version-check \
        'kokoro-onnx>=0.4' numpy
fi

# Page-layout model: tells tables, figures, captions, headings, headers and footers
# apart by looking at the page, so no rule has to guess.
if ! "$tts_dir/runtime/bin/python" -c 'import rapid_layout, cv2' >/dev/null 2>&1; then
    "$tts_dir/runtime/bin/python" -m pip install --disable-pip-version-check \
        rapid-layout opencv-python-headless
fi
layout_model=pp_doc_layoutv2.onnx
layout_sha=0bd2ea0997fe0789f0300292291f8bbf897d890b44a9a3bd5be72afd6198aa90
if [ ! -s "$tts_dir/$layout_model" ]; then
    curl -fL --retry 3 -o "$tts_dir/$layout_model.part" \
        "https://www.modelscope.cn/models/RapidAI/RapidLayout/resolve/v1.2.0/onnx/pp_doc_layout/$layout_model"
    if [ "$(sha256sum "$tts_dir/$layout_model.part" | cut -d' ' -f1)" != "$layout_sha" ]; then
        rm -f "$tts_dir/$layout_model.part"
        echo "Layout model download is corrupt" >&2
        exit 1
    fi
    mv "$tts_dir/$layout_model.part" "$tts_dir/$layout_model"
fi

for file in kokoro-v1.0.onnx voices-v1.0.bin; do
    if [ ! -s "$tts_dir/$file" ]; then
        curl -fL --retry 3 -o "$tts_dir/$file.part" "$base/$file"
        mv "$tts_dir/$file.part" "$tts_dir/$file"
    fi
done
echo "OK: $tts_dir"
