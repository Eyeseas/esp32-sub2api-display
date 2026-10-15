import hashlib
import json
import os
from pathlib import Path
from urllib.parse import urlsplit
from zoneinfo import ZoneInfo


def load_config(path):
    path = Path(path).resolve()
    cfg = json.loads(path.read_text(encoding="utf-8"))
    cfg["config_dir"] = str(path.parent)
    url = urlsplit(cfg.get("upstream_url", ""))
    if url.scheme != "https" and not (cfg.get("allow_http_upstream") and url.scheme == "http"):
        raise ValueError("upstream_url 必须为 HTTPS（本地测试可显式 allow_http_upstream）")
    if not url.netloc or url.username or url.password or url.query or url.fragment:
        raise ValueError("upstream_url 必须是没有凭据、查询参数的服务基址")
    cfg["upstream_url"] = cfg["upstream_url"].rstrip("/")
    cfg["timezone"] = ZoneInfo(cfg["sub2api_timezone"])
    if cfg.get("selection") not in ("ids", "all"):
        raise ValueError("selection 应为 ids 或 all")
    ids = set()
    for account in cfg.get("accounts", []):
        account["id"] = str(int(account["id"]))
        if int(account["id"]) <= 0 or account["id"] in ids:
            raise ValueError("accounts 中的 ID 必须为唯一正整数")
        ids.add(account["id"])
        if account.get("quota_mode", "saved") not in ("saved", "existing_usage"):
            raise ValueError("quota_mode 应为 saved 或 existing_usage")
    if cfg["selection"] == "ids" and not ids:
        raise ValueError("请先配置 accounts，或显式设 selection=all")
    for key, default in (("state_interval", 30), ("today_interval", 60), ("models_interval", 60), ("window_interval", 60),
                         ("quota_interval", 300), ("quota_stale_seconds", 900), ("request_timeout", 15)):
        cfg[key] = float(cfg.get(key, default))
        if not 1 <= cfg[key] <= 86400:
            raise ValueError(key + " 必须在 1–86400 秒内")
    for device in cfg.get("devices", []):
        digest = device.get("token_sha256", "")
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise ValueError("设备 token_sha256 应为小写 SHA256；用 python3 -m bridge init 初始化")
        if device.get("account_ids") is not None:
            device["account_ids"] = [str(int(i)) for i in device["account_ids"]]
    if not cfg.get("devices"):
        raise ValueError("至少需要配置一个设备")
    cfg["revision"] = hashlib.sha256(json.dumps({k: cfg.get(k) for k in
        ("selection", "accounts", "devices", "sub2api_timezone")}, sort_keys=True).encode()).hexdigest()[:16]
    return cfg


def resolve_file(cfg, name):
    return Path(cfg["config_dir"]) / cfg[name]


def read_admin_key(cfg):
    key = os.environ.get("SUB2API_ADMIN_API_KEY")
    if not key:
        key = resolve_file(cfg, "admin_api_key_file").read_text(encoding="utf-8").strip()
    if not key or "\n" in key or "\r" in key:
        raise ValueError("请在 admin_api_key_file 中填写现有 Admin API Key")
    return key
