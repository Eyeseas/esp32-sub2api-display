from datetime import datetime, timedelta
from .normalize import UTC, iso


MODELS = {"101": [("claude-opus-5-5", 120, 1600000, "8.20", "8.20"), ("claude-haiku-4-5-20251001", 3, 2800, "0.003", "0.003")],
          "102": [("gpt-6.1-sol", 230, 2100000, "9.10", "9.10"), ("gpt-5.6-luna", 26, 321000, "0.16", "0.16")],
          "103": [("gpt-6.1-sol", 58, 900000, "3.57", "3.57")]}


class DemoAPI:
    def get(self, path, params=None):
        now = datetime.now(UTC)
        if path == "/admin/dashboard/models":
            return {"start_date": params["start_date"], "end_date": params["end_date"], "models": [
                {"model": name, "requests": requests, "total_tokens": tokens, "account_cost": cost,
                 "actual_cost": cost, "cost": standard} for name, requests, tokens, cost, standard in
                MODELS[params["account_id"]]]}
        account_id = path.split("/")[3]
        if path.endswith("/usage"):
            return {"source": "passive", "five_hour": {"window_stats": {"requests": 97, "tokens": 7312345,
                    "cost": "5.56", "user_cost": "5.56", "standard_cost": "5.56"}}}
        if path.endswith("/today-stats"):
            factor = int(account_id) - 100
            return {"requests": 1234 * factor, "tokens": 2345678 * factor,
                    "cost": "12.34", "user_cost": "18.51", "standard_cost": "15.0"}
        if account_id == "101":
            return {"id": 101, "name": "Claude-01", "platform": "anthropic", "type": "oauth",
                "status": "active", "schedulable": True, "concurrency": 5, "current_concurrency": 2,
                "session_window_end": iso(now + timedelta(hours=2)), "extra": {
                    "session_window_utilization": 0.85, "passive_usage_sampled_at": iso(now),
                    "passive_usage_7d_utilization": 0.42, "passive_usage_7d_reset": iso(now + timedelta(days=3))}}
        if account_id == "102":
            return {"id": 102, "name": "Codex-02", "platform": "openai", "type": "oauth",
                "status": "active", "schedulable": True, "concurrency": 3, "current_concurrency": 0,
                "rate_limit_reset_at": iso(now + timedelta(minutes=18)), "extra": {
                    "codex_5h_used_percent": 100, "codex_5h_reset_at": iso(now + timedelta(minutes=18)),
                    "codex_7d_used_percent": 63, "codex_7d_reset_at": iso(now + timedelta(days=2)),
                    "codex_usage_updated_at": iso(now)}}
        return {"id": 103, "name": "API-03", "platform": "openai", "type": "apikey", "status": "active",
                "schedulable": True, "concurrency": 4, "current_concurrency": 1,
                "quota_daily_limit": 50, "quota_daily_used": 8.5}
