"""Show an unlabelled LCD pattern; a USB acknowledgement cannot verify the panel."""
import argparse
import json
import time

import serial


def register_step(values):
    return ({"command": "lcd", "cmd": values[0], "data": values[1:]},
            f"LCD_OK 0x{values[0]:02x}".encode(), "register " + " ".join(f"0x{v:02X}" for v in values), 0)


def main():
    parser = argparse.ArgumentParser(description="通过 USB 直接全屏填色，辅助检查颜色与文字残影")
    parser.add_argument("--port", required=True)
    parser.add_argument("--pattern", choices=("black", "gray", "white", "red", "green", "blue", "ramp", "flicker", "off", "ui", "touch"))
    parser.add_argument("--register", nargs="+", action="append", metavar="BYTE", default=[],
                        help="改写面板寄存器，如 --register 0xBB 0x19（仅限 Gamma/电压/反色寄存器），可重复")
    parser.add_argument("--backlight", type=int, metavar="PERCENT", help="设置背光亮度 1–100，并保存到设备")
    parser.add_argument("--vcom-sweep", nargs="+", metavar="BYTE",
                        help="在闪烁测试图上依次写入这些 VCOM（BB）值，顶部格子标出当前是第几档，如 0x11 0x13 0x15")
    parser.add_argument("--hold", type=float, default=6, help="扫描时每档停留的秒数，默认 6")
    parser.add_argument("--rounds", type=int, default=2, help="扫描轮数，默认 2")
    parser.add_argument("--level", type=lambda v: int(v, 0), default=0x80, help="闪烁测试图亮像素的灰度，默认 0x80")
    parser.add_argument("--frame-rate", type=lambda v: int(v, 0), default=0x1F, metavar="C6",
                        help="扫描时写入的 C6 帧率参数，默认 0x1F（约 39 Hz，让闪烁更明显）；复位后恢复")
    args = parser.parse_args()
    if not args.pattern and not args.register and args.backlight is None and not args.vcom_sweep:
        args.pattern = "gray"
    # Opening the USB serial port resets the board, so every step must run in this one session.
    # Each step is (command, expected reply, label, seconds to hold before the next step).
    steps = []
    if args.pattern:
        command = {"command": "display_test", "pattern": args.pattern}
        if args.pattern == "flicker":
            command["level"] = args.level
        steps.append((command, f"DISPLAY_TEST_OK {args.pattern}".encode(), args.pattern, 0))
    if args.backlight is not None:
        steps.append(({"command": "backlight", "level": args.backlight},
                      f"BACKLIGHT_OK {args.backlight}".encode(), f"backlight {args.backlight}%", 0))
    for register in args.register:
        values = [int(v, 0) for v in register]
        steps.append(register_step(values))
    if args.vcom_sweep:
        # A lower frame rate slows the flicker from an off-centre VCOM to where the eye catches it.
        steps.append(register_step([0xC6, args.frame_rate]))
        values = [int(v, 0) for v in args.vcom_sweep]
        for round_index in range(args.rounds):
            for index, value in enumerate(values):
                steps.append(register_step([0xBB, value]))
                steps.append(({"command": "display_test", "pattern": "flicker", "step": index, "steps": len(values),
                               "level": args.level}, b"DISPLAY_TEST_OK flicker",
                              f"round {round_index + 1} cell {index + 1}: VCOM 0x{value:02X}", args.hold))
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
        for command, expected, label, hold in steps:
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
            print(json.dumps(result, ensure_ascii=False), flush=True)
            if not result["acknowledged"]:
                return 1
            time.sleep(hold)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
