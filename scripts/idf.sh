#!/usr/bin/env bash
set -euo pipefail
task_root="$(cd "$(dirname "$0")/.." && pwd)"
export IDF_PATH="$task_root/.tools/esp-idf"
export IDF_TOOLS_PATH="$task_root/.tools/idf-tools"
task_python="$IDF_TOOLS_PATH/python_env/idf6.1_py3.12_env/bin/python"
export PATH="$(dirname "$task_python"):$PATH"
source "$IDF_PATH/export.sh" >/dev/null
cd "$task_root/firmware"
exec "$task_python" "$IDF_PATH/tools/idf.py" "$@"
