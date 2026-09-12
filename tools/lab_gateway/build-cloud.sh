#!/usr/bin/env bash
set -euo pipefail
: "${LAB_IDF_PATH:?Configure LAB_IDF_PATH in the cloud environment}"
source "$LAB_IDF_PATH/export.sh" >/dev/null
python "$(dirname "$0")/release.py" "$@"
