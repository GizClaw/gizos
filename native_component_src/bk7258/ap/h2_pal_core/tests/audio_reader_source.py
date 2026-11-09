"""Materialize the board's real microphone stop body for an SDK-free test."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
source = args.source.read_text()
signature = "static int bk_audio_stop_mic(void* user) {"
start = source.index(signature)
end = source.index("{", start) + 1
depth = 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
args.output.write_text(source[start:end] + "\n")
