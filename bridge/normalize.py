"""Convert only documented response fields; no sub2api internal services."""
from datetime import datetime, timezone, timedelta
from decimal import Decimal, InvalidOperation

UTC = timezone.utc


def iso(value):
    return value.astimezone(UTC).isoformat(timespec="seconds").replace("+00:00", "Z")


def timestamp(value):
    try:
        if isinstance(value, (int, float, Decimal)):
            return datetime.fromtimestamp(float(value), UTC)
        if isinstance(value, str) and value:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
            return parsed.astimezone(UTC) if parsed.tzinfo else None
    except (ValueError, OverflowError, OSError):
        pass
    return None


def number(value):
    if isinstance(value, bool) or value is None:
        return None
    try:
        parsed = Decimal(str(value))
        return parsed if parsed.is_finite() else None
    except (InvalidOperation, ValueError):
        return None


def short(value, maximum=48):
    return "".join(c for c in str(value or "") if c.isprintable())[:maximum]


def account_state(raw, alias, now):
    flags = []
    status = raw.get("status")
    schedulable = raw.get("schedulable")
    if status == "error":
        flags.append("error")
    elif status != "active":
        flags.append("paused" if status is not None else "unknown")
    if schedulable is False:
        flags.append("paused")
    if schedulable is None:
        flags.append("unknown")
    for key, flag in (("rate_limit_reset_at", "rate_limited"), ("overload_until", "overloaded"),
                      ("temp_unschedulable_until", "temp_unschedulable")):
        value = timestamp(raw.get(key))
        if value and value > now:
            flags.append(flag)
    expires = timestamp(raw.get("expires_at"))
    if expires and expires <= now:
        flags.append("expired")
    for prefix in ("quota", "quota_daily", "quota_weekly"):
        limit, used = number(raw.get(prefix + "_limit")), number(raw.get(prefix + "_used"))
        if limit and limit > 0 and used is not None and used >= limit:
            flags.append("local_quota_exceeded")
    blocking = set(flags) - ({"expired"} if not raw.get("auto_pause_on_expired") else set())
    extra = raw.get("extra") or {}
    model_limits = extra.get("model_rate_limits") or {}
    if isinstance(model_limits, dict) and any(timestamp(v.get("rate_limit_reset_at")) and
          timestamp(v["rate_limit_reset_at"]) > now for v in model_limits.values() if isinstance(v, dict)):
        flags.append("model_rate_limited")
    return {
        "id": str(raw["id"]), "alias": short(alias or raw.get("name") or "#" + str(raw["id"])),
        "platform": short(raw.get("platform"), 24), "type": short(raw.get("type"), 24),
        "status": short(status, 24), "schedulable": schedulable,
        "observed_normal": not blocking, "state_basis": "admin_list_fields",
        "flags": sorted(set(flags)), "expires_at": iso(expires) if expires else None,
        "runtime": {"concurrency": raw.get("current_concurrency"),
                    "concurrency_limit": raw.get("concurrency"),
                    "active_sessions": raw.get("active_sessions"), "max_sessions": raw.get("max_sessions"),
                    "rpm": raw.get("current_rpm"), "rpm_limit": raw.get("base_rpm")},
    }


def window(key, label, used, reset, sampled, now, ttl, kind="upstream_utilization",
           quality="observed", dimension="global", source="saved_extra", time_basis="source_sample"):
    used = number(used)
    if used is None or used < 0:
        return None
    reset, sampled = timestamp(reset), timestamp(sampled)
    stale_at = sampled + timedelta(seconds=ttl) if sampled else None
    if reset and reset <= now:
        quality = "waiting_sample"
    elif stale_at and stale_at <= now:
        quality = "stale"
    elif not sampled and quality == "observed":
        quality = "api_reported"
    return {"key": short(key, 64), "label": short(label, 32), "kind": kind,
            "dimension": short(dimension, 64), "used_percent": float(used),
            "remaining_percent": float(max(Decimal(0), 100 - used)),
            "reset_at": iso(reset) if reset else None,
            "source_updated_at": iso(sampled) if sampled else None,
            "stale_at": iso(stale_at) if stale_at else None,
            "quality": quality, "refresh_source": source, "time_basis": time_basis}


def saved_quota(raw, now, ttl):
    extra, windows, balances = raw.get("extra") or {}, [], []

    def add(key, label, used, reset=None, sample=None, **kwargs):
        value = window(key, label, used, reset, sample, now, ttl, **kwargs)
        if value:
            windows.append(value)

    platform = raw.get("platform")
    if platform == "anthropic":
        sample = extra.get("passive_usage_sampled_at")
        fraction = number(extra.get("session_window_utilization"))
        if raw.get("session_window_end") and fraction is not None:
            add("five_hour", "5h", fraction * 100, raw["session_window_end"], sample, source="passive")
        elif raw.get("session_window_end") and raw.get("session_window_status") in ("rejected", "allowed_warning"):
            add("five_hour", "5h", 100 if raw["session_window_status"] == "rejected" else 80,
                raw["session_window_end"], sample, quality="estimated", source="passive")
        for key, prefix, label in (("seven_day", "passive_usage_7d", "7d"),
                                   ("seven_day_fable", "passive_usage_7d_oi", "7d Fable")):
            # Header utilization stored by sub2api is a fraction (0..1).
            percent = number(extra.get(prefix + "_utilization"))
            if percent is not None:
                add(key, label, percent * 100, extra.get(prefix + "_reset"), sample, source="passive")
    if platform == "openai":
        for key, label in (("five_hour", "5h"), ("seven_day", "7d")):
            prefix = "codex_" + label
            sample = extra.get("codex_usage_updated_at")
            reset = extra.get(prefix + "_reset_at")
            seconds, parsed = number(extra.get(prefix + "_reset_after_seconds")), timestamp(sample)
            if not reset and parsed and seconds and seconds > 0:
                reset = iso(parsed + timedelta(seconds=float(seconds)))
            add(key, label, extra.get(prefix + "_used_percent"), reset, sample,
                dimension=raw.get("quota_dimension") or "global")
    for provider in ("kimi", "zhipu", "deepseek", "minimax", "opencode_go"):
        if platform != provider:
            continue
        for key, label, suffix in (("five_hour", "5h", "5h"), ("weekly", "weekly", "weekly"),
                                    ("monthly", "monthly", "monthly")):
            add(key, label, extra.get(provider + "_" + suffix + "_used_percent"),
                extra.get(provider + "_" + suffix + "_reset_at"), extra.get(provider + "_usage_updated_at"))
        entries = extra.get(provider + "_balances")
        if not isinstance(entries, list):
            entries = [{"currency": extra.get(provider + "_balance_currency"),
                        "balance": extra.get(provider + "_balance")}]
        for entry in entries:
            balance = number(entry.get("balance")) if isinstance(entry, dict) else None
            if balance is not None and entry.get("currency"):
                balances.append({"currency": short(entry["currency"], 8), "balance": str(balance),
                    "source_updated_at": extra.get(provider + "_balance_updated_at")})
    for field, mapping in (("ollama_cloud_usage", (("five_hour", "5h"), ("seven_day", "7d"))),
                           ("opencode_go_usage", (("rolling", "rolling"), ("weekly", "weekly"), ("monthly", "monthly")))):
        snapshot = (raw.get(field) or {}).get("snapshot") or {}
        data = snapshot.get("data") or {}
        for key, label in mapping:
            progress = data.get(key) or {}
            add(key, label, progress.get("used_percent", progress.get("percent")),
                progress.get("reset_at", progress.get("resets_at")), snapshot.get("fetched_at"))
    if platform == "grok":
        billing = extra.get("grok_billing_snapshot") or {}
        if billing.get("status_code") in (None, 200):
            add("seven_day", "7d", billing.get("usage_percent"), billing.get("period_end"),
                billing.get("weekly_updated_at") or billing.get("updated_at"))
            add("thirty_day", "30d", billing.get("used_percent"), billing.get("billing_period_end"),
                billing.get("monthly_updated_at") or billing.get("updated_at"))
        snapshot = extra.get("grok_usage_snapshot") or {}
        for dimension in ("requests", "tokens"):
            quota = snapshot.get(dimension) or {}
            limit, remaining = number(quota.get("limit")), number(quota.get("remaining"))
            if limit and limit > 0 and remaining is not None:
                add(dimension, dimension, (limit - remaining) * 100 / limit,
                    quota.get("reset_at") or quota.get("reset_unix"), snapshot.get("updated_at"), dimension=dimension)
    for prefix, label in (("quota_daily", "daily"), ("quota_weekly", "weekly"), ("quota", "total")):
        limit, used = number(raw.get(prefix + "_limit")), number(raw.get(prefix + "_used"))
        if limit and limit > 0 and used is not None:
            add(prefix, label, used * 100 / limit, raw.get(prefix + "_reset_at"), iso(now),
                kind="local_limit", time_basis="api_read_time", dimension="local_cost")
    return {"windows": windows, "balances": balances}


def usage_quota(raw, usage, now, ttl):
    platform = raw.get("platform")
    if platform == "anthropic":
        # Passive API zeros/estimates are ambiguous; use the accompanying raw evidence.
        return saved_quota(raw, now, ttl)
    windows = []
    for key, label in (("five_hour", "5h"), ("seven_day", "7d"), ("seven_day_sonnet", "7d Sonnet"),
                       ("seven_day_fable", "7d Fable"), ("thirty_day", "30d"),
                       ("gemini_shared_daily", "daily shared"), ("gemini_pro_daily", "daily pro"),
                       ("gemini_flash_daily", "daily flash"), ("gemini_shared_minute", "minute shared"),
                       ("gemini_pro_minute", "minute pro"), ("gemini_flash_minute", "minute flash")):
        progress = usage.get(key) or {}
        value = window(key, label, progress.get("utilization"), progress.get("resets_at"),
                       usage.get("updated_at"), now, ttl,
                       kind="local_estimate" if platform == "gemini" else "upstream_utilization",
                       quality="estimated" if platform == "gemini" else "api_reported",
                       time_basis="api_reported", source="existing_usage")
        if value:
            windows.append(value)
    for model, progress in (usage.get("antigravity_quota") or {}).items():
        value = window("model:" + model, model, progress.get("utilization"), progress.get("reset_time"),
                       usage.get("updated_at"), now, ttl, dimension=model, quality="api_reported",
                       source="existing_usage", time_basis="api_reported")
        if value:
            windows.append(value)
    return {"windows": windows, "balances": []}


def monitor_quota(snapshot, platform, now, ttl):
    if not isinstance(snapshot, dict) or snapshot.get("success") is not True:
        return None
    windows, balances = [], []
    for tier in snapshot.get("tiers") or []:
        value = window(tier.get("window"), tier.get("label") or tier.get("window"),
                       tier.get("used_percent"), tier.get("reset_at"), snapshot.get("fetched_at"), now, ttl,
                       dimension=tier.get("label") or "global", source="existing_monitor",
                       kind="local_estimate" if platform == "gemini" else "upstream_utilization",
                       quality="estimated" if platform == "gemini" else "api_reported", time_basis="monitor_fetch")
        if value:
            windows.append(value)
    entries = snapshot.get("balances") or [{"balance": snapshot.get("balance"), "currency": snapshot.get("currency")}]
    for entry in entries:
        value = number(entry.get("balance"))
        if value is not None and entry.get("currency"):
            balances.append({"currency": short(entry["currency"], 8), "balance": str(value),
                             "source_updated_at": snapshot.get("fetched_at")})
    return {"windows": windows, "balances": balances}


def today_stats(raw):
    out = {}
    for key in ("requests", "tokens"):
        value = number(raw.get(key))
        if value is None or value < 0 or value != value.to_integral_value():
            raise ValueError("invalid_today_stats")
        out[key] = str(int(value))
    for key, target in (("cost", "account_cost_usd"), ("user_cost", "user_cost_usd"),
                        ("standard_cost", "standard_cost_usd")):
        value = number(raw.get(key))
        if value is None or value < 0:
            raise ValueError("invalid_today_stats")
        out[target] = format(value, ".6f")
    return out


def window_stats(usage):
    """Local usage-log totals since each window started; same WindowStats shape as today-stats."""
    out = {}
    for key in ("five_hour", "seven_day"):
        stats = (usage.get(key) or {}).get("window_stats")
        if isinstance(stats, dict):
            try:
                out[key] = today_stats(stats)
            except (ValueError, TypeError):
                pass
    return out


def model_stats(raw):
    """Per-model usage-log totals from the admin dashboard, in today-stats' units.

    Filtered by account, sub2api reports account cost as actual_cost, so no user-billed cost is kept.
    """
    if not isinstance(raw.get("models"), list):
        raise ValueError("invalid_models")
    out = {}
    for item in raw["models"]:
        name = short(item.get("model"), 64) or "unknown"
        stats = today_stats({"requests": item.get("requests"), "tokens": item.get("total_tokens"),
                             "cost": item.get("account_cost"), "user_cost": 0, "standard_cost": item.get("cost")})
        del stats["user_cost_usd"]
        if name in out:
            raise ValueError("duplicate_model")
        out[name] = stats
    return out
