#!/usr/bin/env python3
"""Page-layout worker.

Usage: layout-worker.py MODEL.onnx [THREADS]

Reads one JSON request per stdin line: {"id", "path"} (an image of one page).
Replies with one JSON line: {"id", "dets": [{"label", "score", "box": [x0, y0, x1, y1]}]}
(pixels of the given image), or {"id", "error"}. The first line is {"ready": true}.
"""

import json
import sys

from rapid_layout import ModelType, RapidLayout


def main():
    out = sys.stdout
    # Anything a library prints must not corrupt the JSON stream.
    sys.stdout = sys.stderr

    threads = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    engine = RapidLayout(
        model_type=ModelType.PP_DOC_LAYOUTV2,
        model_dir_or_path=sys.argv[1],
        engine_cfg={"intra_op_num_threads": threads, "inter_op_num_threads": 1},
        conf_thresh=0.4,
    )
    out.write(json.dumps({"ready": True}) + "\n")
    out.flush()

    for line in sys.stdin:
        request_id = None
        try:
            request = json.loads(line)
            request_id = request.get("id")
            result = engine(request["path"])
            dets = []
            boxes = result.boxes if result.boxes is not None else []
            names = result.class_names if result.class_names is not None else []
            scores = result.scores if result.scores is not None else []
            for box, name, score in zip(boxes, names, scores):
                dets.append({"label": str(name), "score": float(score),
                             "box": [float(v) for v in box]})
            out.write(json.dumps({"id": request_id, "dets": dets}, separators=(",", ":")) + "\n")
        except Exception as error:  # keep serving after a bad page
            out.write(json.dumps({"id": request_id, "error": str(error)}) + "\n")
        out.flush()


if __name__ == "__main__":
    main()
