#!/bin/sh
set -eu

runtime_dir=${1:?usage: setup-rapidocr.sh RUNTIME_DIR}
python=${GLANCE_PYTHON:-}

if [ -z "$python" ]; then
    for candidate in python3.12 python3.11 python3.10; do
        if command -v "$candidate" >/dev/null 2>&1; then
            python=$candidate
            break
        fi
    done
fi

if [ -z "$python" ] && command -v mise >/dev/null 2>&1; then
    python=$(mise where python@3.11)/bin/python
fi

if [ -z "$python" ]; then
    printf '%s\n' 'RapidOCR requires Python 3.10-3.12. Set GLANCE_PYTHON to a compatible interpreter.' >&2
    exit 1
fi

rm -rf "$runtime_dir"
"$python" -m venv "$runtime_dir"
"$runtime_dir/bin/python" -m pip install --disable-pip-version-check \
    'rapidocr==3.9.2' 'onnxruntime==1.30.0'
