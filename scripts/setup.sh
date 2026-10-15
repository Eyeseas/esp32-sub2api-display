#!/usr/bin/env bash
set -euo pipefail
task_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$task_root"
task_python="$(command -v python3.12 || true)"
if [ -z "$task_python" ] && [ -x /usr/local/opt/python@3.12/bin/python3.12 ]; then task_python=/usr/local/opt/python@3.12/bin/python3.12; fi
if [ -z "$task_python" ] && [ -x /opt/homebrew/opt/python@3.12/bin/python3.12 ]; then task_python=/opt/homebrew/opt/python@3.12/bin/python3.12; fi
if [ -z "$task_python" ]; then printf '%s\n' '需要 Python 3.12，请先安装后重试。'; exit 1; fi
mkdir -p .tools
if [ ! -f .tools/esp-idf/tools/idf.py ]; then
    git clone --branch v6.1 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .tools/esp-idf
fi
export IDF_TOOLS_PATH="$task_root/.tools/idf-tools"
"$task_python" .tools/esp-idf/tools/idf_tools.py install --targets esp32s3
"$task_python" .tools/esp-idf/tools/idf_tools.py install-python-env
"$IDF_TOOLS_PATH/python_env/idf6.1_py3.12_env/bin/python" -m pip install 'cmake>=3.22,<4' ninja
printf '%s\n' '工具链准备完成；scripts/idf.sh build 可构建。'
