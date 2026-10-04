#!/usr/bin/env python3

import json
import re
import sys

from rapidocr import RapidOCR


def bounds(points):
    xs = [point[0] for point in points]
    ys = [point[1] for point in points]
    left = min(xs)
    top = min(ys)
    return float(left), float(top), float(max(xs) - left), float(max(ys) - top)


def recognize(engine, request):
    result = engine(
        request["path"], return_word_box=True, return_single_char_box=True
    )
    words = []
    texts = result.txts if result.txts is not None else ()
    boxes = result.boxes if result.boxes is not None else ()
    character_results = result.word_results if result.word_results is not None else ()
    for line_index, (text, box, chars) in enumerate(
        zip(texts, boxes, character_results)
    ):
        x, y, width, height = bounds(box)
        characters = []
        for character, _confidence, char_box in chars:
            char_x, char_y, char_width, char_height = bounds(char_box)
            characters.append(
                {
                    "text": character,
                    "x": char_x,
                    "y": char_y,
                    "w": char_width,
                    "h": char_height,
                }
            )
        character_index = 0
        for word_index, match in enumerate(re.finditer(r"\S+", text)):
            while character_index < len(characters) \
                    and characters[character_index]["text"].isspace():
                character_index += 1
            token_length = sum(not character.isspace() for character in match.group())
            word_chars = characters[character_index : character_index + token_length]
            character_index += token_length
            if not word_chars:
                continue
            left = min(character["x"] for character in word_chars)
            top = min(character["y"] for character in word_chars)
            right = max(character["x"] + character["w"] for character in word_chars)
            bottom = max(character["y"] + character["h"] for character in word_chars)
            words.append(
                {
                    "text": match.group(),
                    "x": left,
                    "y": top,
                    "w": right - left,
                    "h": bottom - top,
                    "block": 0,
                    "paragraph": 0,
                    "line": line_index,
                    "word": word_index,
                    "chars": word_chars,
                }
            )
    return {"id": request.get("id"), "words": words}


def main():
    engine = RapidOCR()
    for line in sys.stdin:
        try:
            request = json.loads(line)
            response = recognize(engine, request)
        except Exception as error:
            response = {
                "id": request.get("id") if "request" in locals() else None,
                "error": str(error),
            }
        print(json.dumps(response, separators=(",", ":")), flush=True)


if __name__ == "__main__":
    main()
