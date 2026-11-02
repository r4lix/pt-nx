#!/bin/bash
# Compiles the flat shaders (inline_shaders.py output) in /in to optimized SPIR-V in /out with glslang and spirv-opt. Meant to run in
# the nxvk toolchain image, which has both:
#   docker run --rm -v <flat shaders>:/in:ro -v <sdmc>/switch/pt/shaders:/out -v <this file>:/run.sh:ro nxvk bash /run.sh
# With the Vulkan SDK installed the CMake build compiles the shaders itself (glslc) and this is not needed.
cd /in
ok=0; bad=0
: > /out/_compile.log
for f in *.vert *.frag *.comp; do
  if glslangValidator -V --target-env vulkan1.3 "$f" -o "/out/$f.spv" >> /out/_compile.log 2>&1; then
    spirv-opt -O "/out/$f.spv" -o "/out/$f.spv" >> /out/_compile.log 2>&1 || true
    ok=$((ok+1))
  else
    bad=$((bad+1))
    echo "FAILED: $f" >> /out/_compile.log
  fi
done
echo "compiled=$ok failed=$bad"
grep -a "^FAILED" /out/_compile.log
[ "$bad" -eq 0 ]
