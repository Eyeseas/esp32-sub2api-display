#!/usr/bin/env bash
# Adapted from diagnosing-bugs/scripts/hitl-loop.template.sh for a physical LCD.
set -euo pipefail
task_root="$(cd "$(dirname "$0")/.." && pwd)"
task_python="$task_root/.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python"
cd "$task_root"
task_port=""
task_pattern=gray
while [ "$#" -gt 0 ]; do
    case "$1" in
        --port|--pattern)
            if [ "$#" -lt 2 ]; then exit 2; fi
            if [ "$1" = --port ]; then task_port="$2"; else task_pattern="$2"; fi
            shift 2
            ;;
        *) printf '用法: bash scripts/display-hitl.sh --port PORT [--pattern gray|black|white|red|green|blue]\n'; exit 2 ;;
    esac
done
if [ -z "$task_port" ]; then exit 2; fi
case "$task_pattern" in
    gray|black|white|red|green|blue) ;;
    *) exit 2 ;;
esac

# Leave the backlight off after the observation, including failures and timeouts.
trap '"$task_python" scripts/display-test.py --port "$task_port" --pattern off || true' EXIT
"$task_python" scripts/display-test.py --port "$task_port" --pattern "$task_pattern"
printf '\n观察无文字 %s 屏：颜色正确且无残影输入 clear，仍有旧字输入 ghost，颜色或覆盖异常输入 wrong_color。\n' "$task_pattern"
VISUAL=pending
read -r -t 60 VISUAL || VISUAL=pending
case "$VISUAL" in
    clear|ghost|wrong_color) ;;
    *) VISUAL=pending ;;
esac
"$task_python" -c '
import datetime, json, sys
from pathlib import Path
result = {"pattern": sys.argv[2], "visual_result": sys.argv[1],
          "observed_at": datetime.datetime.now(datetime.timezone.utc).isoformat()}
path = Path("artifacts/display-diagnostic.json")
path.parent.mkdir(exist_ok=True)
path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
print(json.dumps(result, ensure_ascii=False))
' "$VISUAL" "$task_pattern"
case "$VISUAL" in
    clear) exit 0 ;;
    ghost|wrong_color) exit 1 ;;
    *) exit 2 ;;
esac
