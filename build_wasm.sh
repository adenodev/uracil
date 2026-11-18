#!/bin/bash
# Build the Uracil WASM kernel with the iris shell (Nim) linked in.
#
#   bash uracil/build_wasm.sh
#
# Steps:
#   1. Nim compiles examples/iris to C (wasm32 standalone profile)
#   2. emcc links the kernel + platform + iris glue + Nim C into
#      adeno-site/app/public/uracil/{uracil.js,uracil.wasm}
#
# Requires: emsdk activated in this shell, Nim ≥ 2.0 on PATH (or $NIM).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/../adeno-site/app/public/uracil"
BUILD_DIR="${SCRIPT_DIR}/build_wasm"
NIMCACHE="${BUILD_DIR}/nimcache"
NIM="${NIM:-$(command -v nim || echo "$HOME/.nimble/bin/nim")}"

if ! command -v emcc >/dev/null 2>&1; then
    echo "emcc not found — activate emsdk first: source ~/emsdk/emsdk_env.sh" >&2
    exit 1
fi
if ! "$NIM" --version >/dev/null 2>&1; then
    echo "nim not found — set NIM=/path/to/nim" >&2
    exit 1
fi

NIM_LIB="$(ls -d "$HOME"/.choosenim/toolchains/nim-*/lib 2>/dev/null | sort -V | tail -1)"
if [ -z "$NIM_LIB" ]; then
    NIM_LIB="$("$NIM" dump 2>/dev/null | grep -m1 -oE '/[^ "]+/lib' || true)"
fi
if [ -z "$NIM_LIB" ] || [ ! -f "$NIM_LIB/nimbase.h" ]; then
    echo "cannot locate the Nim standard library (nimbase.h)" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}" "${BUILD_DIR}" "${NIMCACHE}"

echo "==> [1/3] compiling iris (Nim -> C, wasm32 standalone)"
(
    cd "${SCRIPT_DIR}/examples/iris"
    "$NIM" c --os:standalone --cpu:wasm32 -d:release --compileOnly \
        --mm:arc -d:useMalloc --threads:off --exceptions:goto --noMain:on \
        --nimcache:"${NIMCACHE}" -o:"${NIMCACHE}/iris" iris.nim
)

echo "==> [2/3] linking the kernel + iris (non-GPL module)"
emcc -O2 \
    -I "${SCRIPT_DIR}/include" -I "${SCRIPT_DIR}/arch/wasm" -I "${NIMCACHE}" -I "${NIM_LIB}" \
    "${SCRIPT_DIR}/arch/wasm/platform.c" \
    "${SCRIPT_DIR}/arch/wasm/host.c" \
    "${SCRIPT_DIR}/kernel/bundle.c" \
    "${SCRIPT_DIR}/kernel/campaign.c" \
    "${SCRIPT_DIR}/kernel/json.c" \
    "${SCRIPT_DIR}/kernel/loader.c" \
    "${SCRIPT_DIR}/kernel/permissions.c" \
    "${SCRIPT_DIR}/kernel/process.c" \
    "${SCRIPT_DIR}/kernel/runtime.c" \
    "${SCRIPT_DIR}/kernel/ura.c" \
    "${SCRIPT_DIR}/app/iris_bundle.c" \
    "${SCRIPT_DIR}/app/service_host.c" \
    "${NIMCACHE}"/*.c \
    -lidbfs.js \
    -o "${OUTPUT_DIR}/uracil.js" \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=UracilModule \
    -sENVIRONMENT=web,node,worker \
    -sALLOW_MEMORY_GROWTH=1 \
    -sASYNCIFY=1 \
    -sASYNCIFY_STACK_SIZE=131072 \
    -sFORCE_FILESYSTEM=1 \
    -sSTACK_SIZE=1048576 \
    -sASSERTIONS=0 \
    --no-entry \
    -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8","HEAPU8","setValue","getValue","FS"]' \
    -sEXPORTED_FUNCTIONS='["_malloc","_free","_iris_wasm_boot","_iris_wasm_exec","_iris_wasm_key","_iris_wasm_screen","_iris_wasm_tick","_iris_wasm_doom_key","_iris_wasm_set_size","_iris_output","_iris_doom_closed","_iris_glue_campaign_stage","_iris_glue_campaign_load","_iris_glue_campaign_complete","_iris_glue_campaign_destroy","_iris_wasm_service_open","_iris_wasm_service_call","_iris_wasm_service_close","_iris_wasm_service_swap","_iris_wasm_service_swap_bytes","_iris_wasm_grant","_iris_wasm_revoke","_iris_wasm_perms"]'

echo "==> [3/3] building the doom engine (GPL-2.0 standalone module, hosted by the kernel)"
DOOM_DIR="${SCRIPT_DIR}/examples/iris/doomgeneric/doomgeneric"
DOOM_OUT="${SCRIPT_DIR}/../adeno-site/app/public/doom"
DOOM_SRC=(
    dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c
    d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c
    hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c
    i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c
    m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c
    p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c
    p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c
    p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c
    r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c
    st_lib.c st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c
    w_file.c w_main.c w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c
    doomgeneric.c
)
DOOM_PATHS=()
for f in "${DOOM_SRC[@]}"; do
    DOOM_PATHS+=("${DOOM_DIR}/${f}")
done

mkdir -p "${DOOM_OUT}"
emcc -O2 \
    -I "${DOOM_DIR}" -I "${SCRIPT_DIR}/sdk" \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
    -Wno-implicit-function-declaration -Wno-int-conversion \
    "${SCRIPT_DIR}/app/dg_iris.c" \
    "${SCRIPT_DIR}/sdk/ura_guest.c" \
    "${DOOM_PATHS[@]}" \
    -o "${DOOM_OUT}/doom.wasm" \
    -sSTANDALONE_WASM=1 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sSTACK_SIZE=1048576 \
    --no-entry \
    -sEXPORTED_FUNCTIONS='["_doom_start","_doom_tick","_doom_key","_doom_frame","_doom_state"]'

# package the service binary as a .ura (the official Uracil container:
# [module][payload: .ura.meta + doom1.wad + manifest.json][trailer])
DOOM_STAGE="$(mktemp -d)"
cp "${SCRIPT_DIR}/examples/iris/doom-service/manifest.json" "${DOOM_STAGE}/manifest.json"
cp "${DOOM_OUT}/doom1.wad" "${DOOM_STAGE}/doom1.wad"
python3 "${SCRIPT_DIR}/tools/ura_builder.py" \
    --package-dir "${DOOM_STAGE}" \
    --target wasm32 \
    --stub-wasm "${DOOM_OUT}/doom.wasm" \
    -o "${DOOM_OUT}/doom-engine-1.9.0.wasm32.ura"
rm -rf "${DOOM_STAGE}"
rm -f "${DOOM_OUT}/doom.js" "${DOOM_OUT}/doom.wasm32.ura" "${DOOM_OUT}/doom.manifest.json"
cp "${SCRIPT_DIR}/examples/iris/doomgeneric/LICENSE" "${DOOM_OUT}/LICENSE-GPL-2.0.txt"

echo "==> done"
ls -la "${OUTPUT_DIR}" "${DOOM_OUT}"
