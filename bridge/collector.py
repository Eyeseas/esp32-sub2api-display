import copy
import hashlib
import json
import logging
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta
from decimal import Decimal

from .api import APIError
from .normalize import (UTC, iso, timestamp, account_state, saved_quota, usage_quota, monitor_quota, today_stats,
                        window_stats, model_stats)

LOG = logging.getLogger("bridge")
WINDOW_SPANS = {"five_hour": timedelta(hours=5), "seven_day": timedelta(days=7)}
MODEL_ROWS = 4


class Collector:
    def __init__(self, cfg, api, clock=None):
        self.cfg, self.api = cfg, api
        self.clock = clock or (lambda: datetime.now(UTC))
        self.lock, self.stop = threading.RLock(), threading.Event()
        self.records, self.raw, self.monitors = {}, {}, {}
        self.options = {a["id"]: a for a in cfg.get("accounts", [])}
        self.last_run = {"state": -float("inf"), "today": -float("inf"), "models": -float("inf"),
                         "window": -float("inf"), "quota": -float("inf")}
        self.generated_at = None
        self.inventory_error = None

    def selected_ids(self):
        return sorted(self.records, key=int) if self.cfg["selection"] == "all" else list(self.options)

    def _result(self, account_id, source, value=None, error=None, day=None):
        with self.lock:
            record = self.records.setdefault(account_id, {})
            meta = record.setdefault(source + "_meta", {})
            meta["attempted_at"] = iso(self.clock())
            meta["error"] = error
            if error is None:
                record[source] = value
                meta["captured_at"] = iso(self.clock())
                if day is not None:
                    meta["day"] = day
            self.generated_at = iso(self.clock())

    def collect_state(self):
        if self.cfg["selection"] == "all":
            try:
                items = self.api.pages("/admin/accounts", {"lite": 1, "include_scheduler_score": 0,
                                                         "sort_by": "id", "sort_order": "asc"})
                observed = {str(item["id"]): item for item in items}
            except (APIError, ValueError, TypeError, KeyError) as error:
                self.inventory_error = getattr(error, "code", "invalid_account_response")
                for account_id in self.records:
                    self._result(account_id, "state", error=self.inventory_error)
                return
            self.inventory_error = None
            with self.lock:
                for removed in set(self.records) - set(observed):
                    self.records.pop(removed)
                    self.raw.pop(removed, None)
            pairs = [(i, raw, None) for i, raw in observed.items()]
        else:
            def fetch(account_id):
                try:
                    raw = self.api.get("/admin/accounts/" + account_id)
                    if not isinstance(raw, dict) or str(raw.get("id")) != account_id:
                        raise APIError("invalid_account_response")
                    return account_id, raw, None
                except APIError as error:
                    return account_id, None, error.code
            with ThreadPoolExecutor(max_workers=2) as pool:
                pairs = list(pool.map(fetch, self.options))
        for account_id, raw, error in pairs:
            if error:
                self._result(account_id, "state", error=error)
                continue
            try:
                now = self.clock()
                state = account_state(raw, self.options.get(account_id, {}).get("alias"), now)
                quota = saved_quota(raw, now, self.cfg["quota_stale_seconds"])
            except (ValueError, TypeError, KeyError, AttributeError):
                self._result(account_id, "state", error="invalid_account_response")
                continue
            # Credentials are never needed, retained, persisted or sent to the device.
            self.raw[account_id] = {key: value for key, value in raw.items() if key in (
                "id", "platform", "type", "extra", "session_window_end", "session_window_status",
                "quota_dimension", "ollama_cloud_usage", "opencode_go_usage")}
            self._result(account_id, "state", state)
            self._result(account_id, "saved_quota", quota)

    def collect_today(self):
        day = self.clock().astimezone(self.cfg["timezone"]).date().isoformat()
        def fetch(account_id):
            if account_id not in self.raw:
                return account_id, None, "account_unavailable"
            try:
                return account_id, today_stats(self.api.get("/admin/accounts/" + account_id + "/today-stats")), None
            except (APIError, ValueError, TypeError, AttributeError) as error:
                return account_id, None, getattr(error, "code", "invalid_today_response")
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(fetch, self.selected_ids()))
        if day != self.clock().astimezone(self.cfg["timezone"]).date().isoformat():
            for account_id, _, _ in results:
                self._result(account_id, "today", error="day_changed")
            self.last_run["today"] = -float("inf")
            return
        for account_id, value, error in results:
            self._result(account_id, "today", value, error, day)

    def collect_models(self):
        # Same per-account scope and day as today-stats, so the model rows add up to the today totals.
        day = self.clock().astimezone(self.cfg["timezone"]).date().isoformat()
        params = {"start_date": day, "end_date": day, "timezone": self.cfg["sub2api_timezone"],
                  "model_source": "requested"}
        def fetch(account_id):
            if account_id not in self.raw:
                return account_id, None, "account_unavailable"
            try:
                result = self.api.get("/admin/dashboard/models", dict(params, account_id=account_id))
                if not isinstance(result, dict) or result.get("start_date") != day or result.get("end_date") != day:
                    raise APIError("invalid_models_response")
                return account_id, model_stats(result), None
            except (APIError, ValueError, TypeError, AttributeError) as error:
                return account_id, None, getattr(error, "code", "invalid_models_response")
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(fetch, self.selected_ids()))
        if day != self.clock().astimezone(self.cfg["timezone"]).date().isoformat():
            for account_id, _, _ in results:
                self._result(account_id, "models", error="day_changed")
            self.last_run["models"] = -float("inf")
            return
        for account_id, value, error in results:
            self._result(account_id, "models", value, error, day)

    def collect_window_stats(self):
        # Passive usage reads saved samples and local usage logs only; it never queries Anthropic.
        def fetch(account_id):
            try:
                result = self.api.get("/admin/accounts/" + account_id + "/usage", {"source": "passive"})
                if not isinstance(result, dict):
                    raise APIError("invalid_usage_response")
                return account_id, window_stats(result), None
            except (APIError, ValueError, TypeError, AttributeError) as error:
                return account_id, None, getattr(error, "code", "invalid_usage_response")
        ids = [i for i in self.selected_ids() if self.raw.get(i, {}).get("platform") == "anthropic"
               and self.raw[i].get("type") in ("oauth", "setup-token")]
        with ThreadPoolExecutor(max_workers=2) as pool:
            for account_id, value, error in pool.map(fetch, ids):
                self._result(account_id, "window_stats", value, error)

    def collect_quota(self):
        ttl = self.cfg["quota_stale_seconds"]
        if self.cfg.get("use_monitors"):
            try:
                monitors = self.api.pages("/admin/channel-monitors")
                candidates = {}
                for monitor in monitors:
                    account_id = str(monitor.get("account_id"))
                    snapshot = monitor.get("latest_quota")
                    if account_id not in self.raw or not isinstance(snapshot, dict):
                        continue
                    fetched = timestamp(snapshot.get("fetched_at"))
                    if fetched is None:
                        continue
                    if account_id not in candidates or fetched > candidates[account_id][0]:
                        candidates[account_id] = (fetched, snapshot)
                for account_id in self.selected_ids():
                    snapshot = candidates.get(account_id, (None, None))[1]
                    value = monitor_quota(snapshot, self.raw.get(account_id, {}).get("platform"), self.clock(), ttl)
                    if value:
                        self._result(account_id, "monitor_quota", value)
                    else:
                        self._result(account_id, "monitor_quota", error="no_monitor_sample")
            except (APIError, ValueError, TypeError, KeyError, AttributeError) as error:
                for account_id in self.selected_ids():
                    self._result(account_id, "monitor_quota", error=getattr(error, "code", "invalid_monitor_response"))
        for account_id in self.selected_ids():
            if self.options.get(account_id, {}).get("quota_mode", "saved") != "existing_usage":
                continue
            raw = self.raw.get(account_id)
            if not raw:
                continue
            try:
                platform = raw.get("platform")
                if platform in ("kimi", "zhipu", "deepseek", "minimax", "opencode_go"):
                    # Both are existing endpoints; determine coding vs payg from exposed account type.
                    mode = self.options[account_id].get("cn_mode", "quota")
                    if mode not in ("quota", "balance"):
                        raise APIError("invalid_cn_mode")
                    result = self.api.get("/admin/cn-providers/accounts/" + account_id + "/" + mode)
                    snapshot = dict(result, success=result.get("success", True), fetched_at=iso(self.clock()))
                    if "balance" in result:
                        snapshot["source"] = "cn_balance"
                    value = monitor_quota(snapshot, platform, self.clock(), ttl)
                else:
                    params = {"source": "passive"} if platform == "anthropic" else {"force": "false"}
                    result = self.api.get("/admin/accounts/" + account_id + "/usage", params)
                    if not isinstance(result, dict) or result.get("error_code") or result.get("error"):
                        raise APIError("quota_source_error")
                    self._result(account_id, "window_stats", window_stats(result))
                    value = usage_quota(raw, result, self.clock(), ttl)
                if value is None:
                    raise APIError("no_quota_sample")
                self._result(account_id, "active_quota", value)
            except (APIError, ValueError, TypeError, AttributeError) as error:
                self._result(account_id, "active_quota", error=getattr(error, "code", "invalid_usage_response"))

    def tick(self, force=False):
        now = time.monotonic()
        for source, action in (("state", self.collect_state), ("today", self.collect_today),
                               ("models", self.collect_models), ("window", self.collect_window_stats),
                               ("quota", self.collect_quota)):
            if force or now - self.last_run[source] >= self.cfg[source + "_interval"]:
                self.last_run[source] = now
                action()

    def run(self):
        while not self.stop.is_set():
            try:
                self.tick()
            except Exception:
                # Avoid upstream exception bodies, which can contain credentials or account identity.
                LOG.error("collector_cycle_failed")
            self.stop.wait(1)

    def metadata(self, raw, name, now):
        meta = copy.deepcopy(raw.get(name + "_meta", {}))
        captured = timestamp(meta.get("captured_at"))
        ttl = self.cfg[name + "_interval"] * 2 if name in ("today", "models") else self.cfg["state_interval"] * 3
        stale = captured is None or captured + timedelta(seconds=ttl) <= now
        meta["status"] = "unavailable" if captured is None else ("stale" if stale else ("error" if meta.get("error") else "ok"))
        meta["stale_at"] = iso(captured + timedelta(seconds=ttl)) if captured else None
        meta["verification"] = "query_errors_exposed" if name in ("today", "models") else "api_response_only"
        return meta

    @staticmethod
    def model_rows(parts):
        """Today's models across the device scope by tokens, like sub2api's usage page; the tail is one row."""
        complete = bool(parts) and all(part is not None for part in parts)
        merged = {}
        for part in parts if complete else []:
            for name, stats in part.items():
                total = merged.setdefault(name, dict.fromkeys(stats, Decimal(0)))
                for key, value in stats.items():
                    total[key] += Decimal(value)
        ranked = sorted(merged.items(), key=lambda item: (-item[1]["tokens"], item[0]))
        if len(ranked) > MODEL_ROWS:
            rest = ranked[MODEL_ROWS - 1:]
            ranked = ranked[:MODEL_ROWS - 1] + [(None, {key: sum(stats[key] for _, stats in rest)
                                                        for key in rest[0][1]})]
        items = []
        for name, stats in ranked:
            item = {"model": name, "other_models": 0 if name else len(merged) - len(items)}
            for key, value in stats.items():
                item[key] = str(int(value)) if key in ("requests", "tokens") else format(value, ".6f")
            items.append(item)
        return {"complete": complete, "accounts_successful": sum(part is not None for part in parts),
                "basis": "requested_model", "sort": "tokens", "total": len(merged), "items": items}

    def snapshot(self, device, page=1, page_size=5, detail=None, window_page=1):
        now = self.clock()
        with self.lock:
            records, generated = copy.deepcopy(self.records), self.generated_at
            ids = self.selected_ids()
            inventory_error = self.inventory_error
        if device.get("account_ids") is not None:
            ids = [i for i in ids if i in device["account_ids"]]
        if detail is not None and detail not in ids:
            raise KeyError("account_not_in_scope")
        day = now.astimezone(self.cfg["timezone"]).date()
        start = datetime.combine(day, datetime.min.time(), tzinfo=self.cfg["timezone"])
        end = datetime.combine(day + timedelta(days=1), datetime.min.time(), tzinfo=self.cfg["timezone"])
        accounts, model_parts = [], []
        for account_id in ids:
            record = records.get(account_id, {})
            account = copy.deepcopy(record.get("state") or {
                "id": account_id, "alias": self.options.get(account_id, {}).get("alias") or "#" + account_id,
                "platform": "", "type": "", "status": "unknown", "schedulable": None,
                "observed_normal": None, "state_basis": "admin_list_fields", "flags": ["unknown"],
                "runtime": {"concurrency": None, "concurrency_limit": None}})
            state_meta, today_meta = self.metadata(record, "state", now), self.metadata(record, "today", now)
            if state_meta["status"] != "ok":
                account["flags"] = sorted(set(account["flags"] + ["stale" if record.get("state") else "unknown"]))
                account["observed_normal"] = None
            account["sources"] = {"account_state": state_meta, "today": today_meta}
            quota_errors = {source: record.get(source + "_meta", {}).get("error")
                            for source in ("saved_quota", "active_quota", "monitor_quota", "window_stats")
                            if record.get(source + "_meta", {}).get("error")}
            account["sources"]["quota"] = {"errors": quota_errors}
            account["today"] = record.get("today") if today_meta.get("day") == day.isoformat() else None
            if account["today"] is None:
                today_meta["status"] = "unavailable"
            models_meta = self.metadata(record, "models", now)
            fresh_models = models_meta["status"] == "ok" and models_meta.get("day") == day.isoformat()
            model_parts.append(record.get("models") if fresh_models else None)
            merged, balances = {}, {}
            for source in ("monitor_quota", "active_quota", "saved_quota"):
                quota = record.get(source) or {}
                for value in quota.get("windows", []):
                    value = copy.deepcopy(value)
                    reset, stale = timestamp(value.get("reset_at")), timestamp(value.get("stale_at"))
                    if reset and reset <= now:
                        value["quality"] = "waiting_sample"
                    elif stale and stale <= now:
                        value["quality"] = "stale"
                    elif record.get(source + "_meta", {}).get("error"):
                        value["quality"] = "stale"
                    key = (value["key"], value["dimension"])
                    old = merged.get(key)
                    if old is None or (timestamp(value.get("source_updated_at")) or datetime.min.replace(tzinfo=UTC)) >= (
                            timestamp(old.get("source_updated_at")) or datetime.min.replace(tzinfo=UTC)):
                        merged[key] = value
                for value in quota.get("balances", []):
                    value = copy.deepcopy(value)
                    sampled = timestamp(value.get("source_updated_at"))
                    value["quality"] = "stale" if sampled and sampled + timedelta(seconds=self.cfg["quota_stale_seconds"]) <= now else "api_reported"
                    balances[value["currency"]] = value
            windows = list(merged.values())
            stats_captured = timestamp(record.get("window_stats_meta", {}).get("captured_at"))
            fresh = stats_captured and stats_captured + timedelta(seconds=self.cfg["quota_stale_seconds"]) > now
            stats = (record.get("window_stats") or {}) if fresh else {}
            for value in windows:
                # Totals counted before the current window began must not be shown against it.
                reset, span = timestamp(value.get("reset_at")), WINDOW_SPANS.get(value["key"])
                current = value["key"] in stats and value["quality"] != "waiting_sample" and not (
                    reset and span and stats_captured < reset - span)
                value["stats"] = stats[value["key"]] if current else None
            account["quota_status"] = "ok" if windows or balances else "unknown"
            if any(w["used_percent"] >= 80 and w["quality"] not in ("stale", "waiting_sample") for w in windows):
                account["flags"] = sorted(set(account["flags"] + ["quota_warning"]))
            account["windows_total"] = len(windows)
            account["windows"] = windows[(window_page - 1) * 4:window_page * 4] if detail else windows[:4]
            account["windows_truncated"] = len(windows) > len(account["windows"])
            account["balances"] = list(balances.values())
            accounts.append(account)
        complete = bool(accounts) and all(a["today"] and a["sources"]["today"]["status"] == "ok" for a in accounts)
        today = {key: (str(sum(Decimal(a["today"][key]) for a in accounts)) if complete else None) for key in
                 ("requests", "tokens", "account_cost_usd", "user_cost_usd", "standard_cost_usd")}
        today.update(complete=complete, accounts_successful=sum(a["sources"]["today"]["status"] == "ok" for a in accounts))
        models = self.model_rows(model_parts)
        summary = {"accounts_total": len(accounts), "observed_normal": sum(a["observed_normal"] is True for a in accounts),
                   "unknown": sum(a["observed_normal"] is None for a in accounts),
                   "alert_accounts": sum(bool(a["flags"]) for a in accounts), "state_basis": "admin_list_fields"}
        for flag in ("error", "paused", "expired", "rate_limited", "overloaded", "temp_unschedulable", "quota_warning"):
            summary[flag] = sum(flag in a["flags"] for a in accounts)
        selected = [a for a in accounts if a["id"] == detail] if detail else accounts[(page - 1) * page_size:page * page_size]
        data = {"schema_version": 1, "producer": "independent_bridge", "generated_at": generated,
                "partial": bool(inventory_error) or not complete or bool(summary["unknown"]),
                "scope": {"label": device.get("name", "重点账号"), "basis": "bridge_config",
                          "account_count": len(accounts), "revision": self.cfg["revision"]},
                "day": {"date": day.isoformat(), "timezone": self.cfg["sub2api_timezone"],
                        "start_at": iso(start), "end_at": iso(end), "basis": "bridge_config_matches_sub2api_timezone"},
                "summary": summary, "today": today, "models": models, "pagination": {"page": page, "page_size": page_size,
                    "total": len(accounts), "has_more": page * page_size < len(accounts)}, "accounts": selected,
                "sources": {"inventory_error": inventory_error, "runtime": {"verification": "api_response_only",
                    "internal_errors_exposed": False}}}
        if detail:
            data["window_page"] = window_page
        data["snapshot_id"] = hashlib.sha256(json.dumps(data, sort_keys=True, ensure_ascii=False).encode()).hexdigest()[:20]
        return {"code": 0, "message": "success", "data": data}
