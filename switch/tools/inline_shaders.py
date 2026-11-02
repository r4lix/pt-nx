#!/usr/bin/env python3
"""Expands the shaders' quoted #include lines literally (as glslc does) into flat files, for compilers without the include extension.

usage: inline_shaders.py <shaders dir> <output dir>
"""
import pathlib
import re
import sys

include = re.compile(r'^\s*#\s*include\s+"([^"]+)"\s*$')


def expand(path, depth=0):
    assert depth < 16, f"include loop at {path}"
    lines = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = include.match(line)
        lines.append(expand((path.parent / match.group(1)).resolve(), depth + 1) if match else line)
    return "\n".join(lines)


def main():
    source, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for path in sorted(source.iterdir()):
        if path.suffix in (".vert", ".frag", ".comp"):
            (out / path.name).write_text(expand(path.resolve()), encoding="utf-8")
            count += 1
    print(f"{count} shaders expanded into {out}")


main()
