import argparse
import hashlib
import json
import logging
import os
import secrets
import threading
from pathlib import Path

from .api import Sub2API
from .collector import Collector
from .config import load_config, read_admin_key
from .demo import DemoAPI
from .server import make_server


def initialize(path):
    path = Path(path)
    if path.exists():
        raise ValueError("配置已存在，未覆盖：" + str(path))
    cfg = json.loads((Path(__file__).parent / "config.example.json").read_text())
    secret_dir = path.parent / "secrets"
    secret_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(secret_dir, 0o700)
    token = secrets.token_urlsafe(32)
    token_file = secret_dir / "device-token"
    if token_file.exists():
        token = token_file.read_text().strip()
    else:
        token_file.write_text(token + "\n")
        os.chmod(token_file, 0o600)
    key_file = secret_dir / "admin-api-key"
    if not key_file.exists():
        key_file.touch(mode=0o600)
    cfg["devices"][0]["token_sha256"] = hashlib.sha256(token.encode()).hexdigest()
    path.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")
    os.chmod(path, 0o600)
    print("本机配置已创建：" + str(path.resolve()))
    print("现有 Admin API Key 填入：" + str(key_file.resolve()))
    print("设备 Token 已生成并保存在：" + str(token_file.resolve()))


def main():
    parser = argparse.ArgumentParser(description="sub2api 独立 ESP32 展示适配服务")
    parser.add_argument("command", choices=("init", "run", "check", "demo"))
    parser.add_argument("--config", default="bridge/config.local.json")
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    try:
        if args.command == "init":
            initialize(args.config)
            return
        if args.command == "demo":
            cfg = json.loads((Path(__file__).parent / "config.example.json").read_text())
            cfg["accounts"] = [{"id": str(i)} for i in (101, 102, 103)]
            cfg["devices"][0]["token_sha256"] = hashlib.sha256(b"demo-device").hexdigest()
            cfg["devices"][0]["name"] = "演示账号"
            cfg.update(config_dir=str(Path.cwd()), revision="demo", listen_port=args.port)
            from zoneinfo import ZoneInfo
            cfg["timezone"] = ZoneInfo(cfg["sub2api_timezone"])
            api = DemoAPI()
        else:
            cfg = load_config(args.config)
            api = Sub2API(cfg, read_admin_key(cfg))
        collector = Collector(cfg, api)
        if args.command == "check":
            collector.tick(force=True)
            result = collector.snapshot(cfg["devices"][0])["data"]
            print(json.dumps({"summary": result["summary"], "today": result["today"], "models": result["models"],
                "partial": result["partial"],
                "source_status": {a["id"]: a["sources"] for a in result["accounts"]}}, ensure_ascii=False, indent=2))
            if result["partial"]:
                raise SystemExit(1)
            return
        if args.command == "demo":
            collector.tick(force=True)
        server = make_server(cfg, collector)
        thread = threading.Thread(target=collector.run, daemon=True)
        thread.start()
        print("展示服务：%s:%s；浏览器打开同一地址可预览" % server.server_address, flush=True)
        if args.command == "demo":
            print("演示设备 Token：demo-device（只用于模拟数据）", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            collector.stop.set()
            server.server_close()
    except (ValueError, OSError) as error:
        parser.exit(2, "配置/启动失败：" + str(error) + "\n")


if __name__ == "__main__":
    main()
