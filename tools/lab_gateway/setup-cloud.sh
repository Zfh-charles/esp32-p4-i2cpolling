#!/usr/bin/env bash
# Codex cloud setup step: cached tools only; never starts a long-running process.
set -euo pipefail
: "${LAB_IDF_PATH:?Set LAB_IDF_PATH to the desired ESP-IDF checkout directory}"
if [[ ! -d "$LAB_IDF_PATH" ]]; then
  git clone --branch v5.4.1 --depth 1 --recursive https://github.com/espressif/esp-idf.git "$LAB_IDF_PATH"
fi
[[ "$(git -C "$LAB_IDF_PATH" describe --tags --exact-match)" == v5.4.1 ]]
[[ -z "$(git -C "$LAB_IDF_PATH" status --porcelain --untracked-files=no)" ]]
"$LAB_IDF_PATH/install.sh" esp32p4
printf '%s\n' 'Setup complete. Configure LAB_IDF_PATH also for the agent phase.'
