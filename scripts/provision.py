"""Send a local JSON config through USB without echoing secrets."""
import argparse
import json
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser(description="通过 USB 配置 ESP32，凭据不打印到日志")
parser.add_argument("--port", required=True)
parser.add_argument("--config", default="firmware/device.local.json")
args = parser.parse_args()
config = json.loads(Path(args.config).read_text(encoding="utf-8"))
if not config.get("wifi_ssid") or not config.get("bridge_url") or not config.get("device_token"):
    parser.error("请先在本机配置文件填写 wifi_ssid、bridge_url、device_token")
config["command"] = "configure"
payload = json.dumps(config, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
if len(payload) > 4095:
    parser.error("配置超过设备接收上限")
with serial.Serial() as connection:
    connection.port = args.port
    connection.baudrate = 115200
    connection.timeout = 0.5
    connection.write_timeout = 2
    connection.dtr = False
    connection.rts = False
    connection.open()
    connection.reset_input_buffer()
    # Opening the port resets the board, so resend until the console has started and answers.
    sent_at = -float("inf")
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if time.monotonic() - sent_at >= 3:
            connection.write(payload)
            sent_at = time.monotonic()
        line = connection.readline()
        if b"CONFIG_OK" in line:
            print("配置已保存，设备正在重启联网。")
            break
        if b"CONFIG_INVALID" in line or b"CONFIG_SAVE_FAILED" in line:
            raise SystemExit("设备拒绝配置，请检查字段；未输出凭据。")
    else:
        raise SystemExit("设备未确认配置，请检查串口连接。")
