"""Check the flashed device through USB without printing its credentials."""
import argparse
import json
import time
from pathlib import Path

import serial


def main():
    parser = argparse.ArgumentParser(description="验证 USB 配置状态、三个视图和真实快照接收")
    parser.add_argument("--port", required=True)
    parser.add_argument("--config", default="firmware/device.local.json")
    parser.add_argument("--log", default=".tools/device-monitor.log")
    parser.add_argument("--seconds", type=int, default=45)
    args = parser.parse_args()
    if not 5 <= args.seconds <= 60:
        parser.error("--seconds 必须在 5 到 60 之间")

    config = json.loads(Path(args.config).read_text(encoding="utf-8"))
    commands = [{"command": "status"}] + [{"command": "view", "view": view} for view in range(3)]
    replies = [b"CONFIGURED", b"VIEW_OK", b"VIEW_OK", b"VIEW_OK"]
    received = bytearray()
    pending = bytearray()
    accepted = 0
    command_index = 0
    sent_at = -float("inf")
    configured = False

    with serial.Serial() as connection:
        connection.port = args.port
        connection.baudrate = 115200
        connection.timeout = 0.25
        connection.write_timeout = 2
        connection.dtr = False
        connection.rts = False
        connection.open()
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            if command_index < len(commands) and time.monotonic() - sent_at >= 3:
                connection.write(json.dumps(commands[command_index]).encode() + b"\n")
                # Device acknowledgements confirm delivery; tcdrain can hang across USB resets.
                sent_at = time.monotonic()
            chunk = connection.read(4096)
            received.extend(chunk)
            pending.extend(chunk)
            while b"\n" in pending:
                line, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                line = line.strip()
                if line == b"NOT_CONFIGURED":
                    break
                if command_index < len(replies) and line == replies[command_index]:
                    if command_index == 0:
                        configured = True
                    command_index += 1
                    sent_at = -float("inf")
                if b"network: snapshot accepted:" in line:
                    accepted += 1
            if len(received) > 1024 * 1024:
                break

    log = received.decode("utf-8", errors="replace")
    for name in ("wifi_ssid", "wifi_password", "device_token"):
        secret = config.get(name)
        if secret:
            log = log.replace(secret, "[REDACTED]")
    log_path = Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(log, encoding="utf-8")
    crashes = any(marker in log for marker in ("Guru Meditation Error", "panic'ed", "abort() was called"))
    summary = {"configured": configured, "views_confirmed": max(0, command_index - 1),
               "snapshots_accepted": accepted, "crash_detected": crashes, "log": str(log_path)}
    print(json.dumps(summary, ensure_ascii=False))
    return 0 if command_index == len(commands) and accepted and not crashes else 1


if __name__ == "__main__":
    raise SystemExit(main())
