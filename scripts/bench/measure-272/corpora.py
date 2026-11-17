#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Writes the two compile databases of the measurement, under /work/corpus.

analyzer: the analyzer's own sources, as the build of /work/main compiled them.
lua:      the C files of /work/lua, the src directory of Lua 5.4.6.
"""
import glob
import json
import os

os.makedirs("/work/corpus/analyzer", exist_ok=True)
os.makedirs("/work/corpus/lua", exist_ok=True)

built = json.load(open("/work/main/build/compile_commands.json"))
own = [entry for entry in built
       if entry["file"].startswith("/work/main/src/") or entry["file"] == "/work/main/main.cpp"]
json.dump(own, open("/work/corpus/analyzer/compile_commands.json", "w"), indent=1)

units = sorted(os.path.basename(path) for path in glob.glob("/work/lua/*.c"))
lua = [{"directory": "/work/lua", "file": unit,
        "arguments": ["cc", "-std=gnu99", "-DLUA_COMPAT_5_3", "-DLUA_USE_LINUX", "-Wall", "-Wextra",
                      "-c", unit, "-o", unit[:-2] + ".o"]} for unit in units]
json.dump(lua, open("/work/corpus/lua/compile_commands.json", "w"), indent=1)
print("analyzer: %d sources, lua: %d sources" % (len(own), len(lua)))
if not own or not lua:
    raise SystemExit("an empty corpus")
