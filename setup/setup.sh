#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -lt 2 ]; then echo "Usage: bash setup.sh ISO EXTERNAL_WORK [--optimise]" >&2; exit 2; fi
iso="$1"; work="$2"; shift 2
nice -n 10 python3 "$(dirname "$0")/setup.py" --iso "$iso" --work "$work" "$@"
