#!/usr/bin/env bash
# 编译 RenderServer 与示例 apps 为 wasm32 模块。
# 产物 (out/):
#   render_server.wasm      —— 仅 RenderServer 本体 (导出 memory + rs_*)
#   app_a/b/c.wasm          —— 独立 app (从 "render_server" 导入 rs_*, 共享 server 内存)
#   render_server_demo.wasm —— 单模块整合版 (server+apps), 供 WASM3 在设备上验证
# 用法: bash render_server/build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/out"
mkdir -p "$OUT"
CC=clang
CFLAGS="--target=wasm32 -nostdlib -O2 -I$ROOT -Wall -Wextra"
LD_FLAGS="-Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined"
echo "==> render_server.wasm (server only, exports memory)"
clang $CFLAGS -DRS_COMPILE_SERVER $LD_FLAGS -Wl,--export-memory \
    -o "$OUT/render_server.wasm" "$ROOT/render_server.c"
for a in a b c; do
    echo "==> app_$a.wasm (imports rs_* from render_server, shares memory)"
    clang $CFLAGS $LD_FLAGS -Wl,--import-memory \
        -o "$OUT/app_$a.wasm" "$ROOT/apps/app_$a.c"
done
echo "==> render_server_demo.wasm (monolithic: server + apps)"
clang $CFLAGS -DRS_MONOLITHIC $LD_FLAGS \
    -o "$OUT/render_server_demo.wasm" \
    "$ROOT/render_server.c" "$ROOT/rs_demo.c" \
    "$ROOT/apps/app_a.c" "$ROOT/apps/app_b.c" "$ROOT/apps/app_c.c"
echo "==> build done -> $OUT"
ls -l "$OUT"
