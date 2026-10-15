"""Show an unlabelled LCD pattern; a USB acknowledgement cannot verify the panel."""
import argparse
import json
import time

import serial


def main():
    parser = argparse.ArgumentParser(description="通过 USB 直接全屏填色，辅助检查颜色与文字残影")
    parser.add_argument("--port", required=True)
    parser.add_argument("--pattern", choices=("black", "gray", "white", "red", "green", "blue", "ramp", "off", "ui", "touch"))
    parser.add_argument("--register", nargs="+", action="append", metavar="BYTE", default=[],
                        help="改写面板寄存器，如 --register 0xBB 0x19（仅限 Gamma/电压/反色寄存器），可重复")
    parser.add_argument("--backlight", type=int, metavar="PERCENT", help="设置背光亮度 1–100，并保存到设备")
    args = parser.parse_args()
    if not args.pattern and not args.register and args.backlight is None:
        args.pattern = "gray"
    # Opening the USB serial port resets the board, so every step must run in this one session.
    steps = []
    if args.pattern:
        steps.append(({"command": "display_test", "pattern": args.pattern},
                      f"DISPLAY_TEST_OK {args.pattern}".encode(), args.pattern))
    if args.backlight is not None:
        steps.append(({"command": "backlight", "level": args.backlight},
                      f"BACKLIGHT_OK {args.backlight}".encode(), f"backlight {args.backlight}%"))
    for register in args.register:
        values = [int(v, 0) for v in register]
        steps.append(({"command": "lcd", "cmd": values[0], "data": values[1:]},
                      f"LCD_OK 0x{values[0]:02x}".encode(), "register " + " ".join(f"0x{v:02X}" for v in values)))
    with serial.Serial() as connection:
        connection.port = args.port
        connection.baudrate = 115200
        connection.timeout = 0.25
        connection.write_timeout = 2
        connection.dtr = False
        connection.rts = False
        connection.open()
        connection.reset_input_buffer()
        pending = bytearray()
        for command, expected, label in steps:
            payload = json.dumps(command).encode() + b"\n"
            sent_at = -float("inf")
            deadline = time.monotonic() + 15
            result = None
            while result is None and time.monotonic() < deadline:
                if time.monotonic() - sent_at >= 3:
                    connection.write(payload)
                    sent_at = time.monotonic()
                pending.extend(connection.read(4096))
                while b"\n" in pending and result is None:
                    line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    line = line.strip()
                    if line == expected:
                        result = {"step": label, "acknowledged": True, "visual_check": "pending"}
                    elif line.startswith((b"BACKLIGHT_FAILED", b"LCD_FAILED", b"DISPLAY_TEST_FAILED", b"DISPLAY_TEST_INVALID", b"CONFIG_INVALID")):
                        result = {"step": label, "acknowledged": False, "reason": "device_rejected_command"}
            result = result or {"step": label, "acknowledged": False, "reason": "no_acknowledgement"}
            print(json.dumps(result, ensure_ascii=False))
            if not result["acknowledged"]:
                return 1
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
