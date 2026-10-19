#!/usr/bin/env bash
set -euo pipefail
folder=$(realpath "${1:?Usage: bash play.sh EXTERNAL_GAME_FOLDER}")
while IFS= read -r line; do
    line=${line%$'\r'}
    if [[ "$line" =~ ^[A-Z0-9_]+= ]]; then export "$line"; fi
done < "$(dirname "$0")/play_env.txt"
export PS2X_CD_IMAGE=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["iso"])' "$folder/disc.json")
elf=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["elf"])' "$folder/disc.json")
static=$(python3 -c 'import json,sys; print(int(json.load(open(sys.argv[1])).get("static_vu1",False)))' "$folder/disc.json")
if [ "$static" = 1 ]; then export PS2X_VU1_AOT=1 PS2X_VU1_AOT_STATIC=1; fi
if [ -z "${PS2X_SCRIPT:-}" ]; then
    export PS2X_MC_ROOT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["mc_root"])' "$folder/disc.json")
    python3 "$(dirname "$0")/../setup/cards.py" --disc "$folder/disc.json"
fi
cd "$folder"
if [ -n "${PROTON:-}" ]; then
    : "${STEAM_COMPAT_DATA_PATH:?Set a private Proton prefix path}"
    export PS2X_CD_IMAGE="Z:${PS2X_CD_IMAGE//\//\\}"
    if [ -z "${PS2X_SCRIPT:-}" ]; then export PS2X_MC_ROOT="Z:${PS2X_MC_ROOT//\//\\}"; fi
    elf="Z:${elf//\//\\}"
    exec "$PROTON" run "$folder/ps2EntryRunner.exe" "$elf"
fi
exec "$folder/ps2EntryRunner" "$elf"
