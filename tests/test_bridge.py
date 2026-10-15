import copy
import hashlib
import json
import threading
import unittest
from datetime import datetime, timedelta
from decimal import Decimal
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from zoneinfo import ZoneInfo

from bridge.api import APIError, Sub2API
from bridge.collector import Collector
from bridge.normalize import UTC, iso, saved_quota, today_stats
from bridge.server import make_server


class FakeAPI:
    def __init__(self, now):
        self.calls = []
        self.fail = set()
        self.accounts = {
            "101": {"id": 101, "name": "Claude", "platform": "anthropic", "type": "oauth", "status": "active",
                "schedulable": True, "current_concurrency": 0, "concurrency": 5,
                "session_window_end": iso(now + timedelta(hours=2)),
                "credentials": {"access_token": "MUST_NOT_LEAK"}, "error_message": "MUST_NOT_LEAK",
                "extra": {"private_secret": "MUST_NOT_LEAK", "session_window_utilization": Decimal("0.85"),
                    "passive_usage_sampled_at": iso(now), "passive_usage_7d_utilization": Decimal("0.2"),
                    "passive_usage_7d_reset": int((now + timedelta(days=2)).timestamp())}},
            "102": {"id": 102, "name": "Codex", "platform": "openai", "type": "oauth", "status": "error",
                "schedulable": False, "current_concurrency": 0, "concurrency": 3,
                "extra": {"codex_5h_used_percent": 110, "codex_5h_reset_at": iso(now + timedelta(hours=1)),
                    "codex_usage_updated_at": iso(now)}}}
        self.models = {"101": [("claude-opus-5-5", 190, 25165094, Decimal("16.57475"))],
                       "102": [("gpt-6.1-sol", 228, 29451058, Decimal("9.2654215")),
                               ("gpt-5.6-luna", 26, 3210946, Decimal("0.15992508"))]}

    def get(self, path, params=None):
        self.calls.append((path, params))
        if path in self.fail or (path, params.get("account_id") if params else None) in self.fail:
            raise APIError("http_503")
        if path == "/admin/dashboard/models":
            return {"start_date": params["start_date"], "end_date": params["end_date"], "models": [
                {"model": name, "requests": requests, "total_tokens": tokens, "account_cost": cost,
                 "actual_cost": cost, "cost": cost * 2} for name, requests, tokens, cost in self.models[params["account_id"]]]}
        account_id = path.split("/")[3]
        if path.endswith("/usage"):
            return {"source": "passive", "five_hour": {"utilization": 85, "window_stats": {
                "requests": 127, "tokens": 9509863, "cost": 6.653728, "standard_cost": 6.653728,
                "user_cost": Decimal("7.5")}}, "seven_day": {"utilization": 20}}
        if path.endswith("today-stats"):
            return {"requests": 9007199254740993, "tokens": 18446744073709551615,
                    "cost": Decimal("1.000001"), "standard_cost": Decimal("2.25"), "user_cost": Decimal("3.125")}
        return copy.deepcopy(self.accounts[account_id])

    def pages(self, path, params=None):
        self.calls.append((path, params))
        if path in self.fail:
            raise APIError("http_503")
        return copy.deepcopy(list(self.accounts.values()))


def config():
    return {"selection": "ids", "accounts": [{"id": "101", "alias": "Claude-01"}, {"id": "102"}],
        "devices": [{"name": "desk", "token_sha256": hashlib.sha256(b"device-secret").hexdigest(), "account_ids": ["101"]}],
        "state_interval": 30, "today_interval": 60, "models_interval": 60, "window_interval": 60, "quota_interval": 300, "quota_stale_seconds": 900,
        "timezone": ZoneInfo("Asia/Shanghai"), "sub2api_timezone": "Asia/Shanghai", "revision": "test",
        "listen_host": "127.0.0.1", "listen_port": 0}


class CollectorTests(unittest.TestCase):
    def setUp(self):
        self.now = datetime(2026, 10, 4, 8, tzinfo=UTC)
        self.cfg = config()
        self.api = FakeAPI(self.now)
        self.collector = Collector(self.cfg, self.api, lambda: self.now)
        self.collector.tick(force=True)

    def data(self, **kwargs):
        return self.collector.snapshot(self.cfg["devices"][0], **kwargs)["data"]

    def test_source_values_and_large_integer_precision(self):
        result = self.data()
        self.assertEqual(result["summary"]["accounts_total"], 1)
        self.assertEqual(result["today"]["tokens"], "18446744073709551615")
        self.assertEqual(result["today"]["requests"], "9007199254740993")
        self.assertEqual(Decimal(result["today"]["account_cost_usd"]), Decimal("1.000001"))
        self.assertEqual(result["accounts"][0]["windows"][0]["used_percent"], 85)
        self.assertEqual(result["accounts"][0]["windows"][1]["used_percent"], 20)
        self.assertNotIn("MUST_NOT_LEAK", json.dumps(result))
        self.assertNotIn("credentials", json.dumps(result))

    def test_device_scope_cannot_be_expanded(self):
        with self.assertRaises(KeyError):
            self.data(detail="102")
        result = self.data(page=2, page_size=1)
        self.assertEqual(result["accounts"], [])
        self.assertEqual(result["summary"]["accounts_total"], 1)
        self.assertEqual(result["today"]["requests"], "9007199254740993")

    def test_failed_today_preserves_previous_value_but_not_complete_total(self):
        self.api.fail.add("/admin/accounts/101/today-stats")
        self.collector.collect_today()
        result = self.data()
        self.assertTrue(result["partial"])
        self.assertIsNone(result["today"]["requests"])
        self.assertEqual(result["accounts"][0]["today"]["requests"], "9007199254740993")
        self.assertEqual(result["accounts"][0]["sources"]["today"]["error"], "http_503")

    def test_cross_day_does_not_use_yesterdays_values(self):
        self.now = datetime(2026, 10, 4, 16, 0, 1, tzinfo=UTC)
        result = self.data()
        self.assertEqual(result["day"]["date"], "2026-10-05")
        self.assertIsNone(result["today"]["requests"])
        self.assertIsNone(result["accounts"][0]["today"])
        self.assertTrue(result["partial"])

    def test_collection_crossing_midnight_is_discarded(self):
        original = self.api.get
        self.now = datetime(2026, 10, 4, 15, 59, 59, tzinfo=UTC)
        def advancing(path, params=None):
            result = original(path, params)
            self.now += timedelta(seconds=2)
            return result
        self.api.get = advancing
        self.collector.collect_today()
        self.assertEqual(self.collector.records["101"]["today_meta"]["error"], "day_changed")
        self.assertIsNone(self.data()["today"]["requests"])

    def test_zero_concurrency_is_api_reported_not_invented_error(self):
        result = self.data()
        self.assertEqual(result["accounts"][0]["runtime"]["concurrency"], 0)
        self.assertFalse(result["sources"]["runtime"]["internal_errors_exposed"])

    def test_old_source_and_reset_do_not_become_new_zero(self):
        self.now += timedelta(hours=3)
        result = self.data()
        w = result["accounts"][0]["windows"][0]
        self.assertEqual(w["used_percent"], 85)
        self.assertEqual(w["quality"], "waiting_sample")
        self.assertIsNone(result["accounts"][0]["observed_normal"])

    def test_no_sample_is_not_zero_and_known_zero_is_retained(self):
        raw = copy.deepcopy(self.api.accounts["101"])
        raw["extra"] = {}
        raw.pop("session_window_end")
        self.assertEqual(saved_quota(raw, self.now, 900)["windows"], [])
        raw = copy.deepcopy(self.api.accounts["101"])
        raw["extra"]["session_window_utilization"] = 0
        self.assertEqual(saved_quota(raw, self.now, 900)["windows"][0]["used_percent"], 0)

    def test_over_100_is_preserved_and_remaining_is_clipped(self):
        device = dict(self.cfg["devices"][0], account_ids=None)
        w = self.collector.snapshot(device)["data"]["accounts"][1]["windows"][0]
        self.assertEqual(w["used_percent"], 110)
        self.assertEqual(w["remaining_percent"], 0)

    def test_failed_full_inventory_does_not_publish_partial_page(self):
        self.cfg["selection"] = "all"
        self.api.fail.add("/admin/accounts")
        self.collector.collect_state()
        device = dict(self.cfg["devices"][0], account_ids=None)
        result = self.collector.snapshot(device)["data"]
        self.assertTrue(result["partial"])
        self.assertEqual(result["summary"]["accounts_total"], 2)

    def test_window_stats_attach_to_their_own_window(self):
        windows = self.data()["accounts"][0]["windows"]
        self.assertEqual(windows[0]["stats"], {"requests": "127", "tokens": "9509863",
            "account_cost_usd": "6.653728", "user_cost_usd": "7.500000", "standard_cost_usd": "6.653728"})
        self.assertIsNone(windows[1]["stats"])
        self.assertIn(("/admin/accounts/101/usage", {"source": "passive"}), self.api.calls)
        # Saved-mode Codex accounts keep avoiding /usage, which may probe the upstream.
        self.assertNotIn("/admin/accounts/102/usage", [path for path, _ in self.api.calls])

    def test_window_stats_from_previous_window_or_stale_are_hidden(self):
        self.now += timedelta(hours=3)
        self.api.accounts["101"]["session_window_end"] = iso(self.now + timedelta(hours=4))
        self.collector.collect_state()
        self.assertIsNone(self.data()["accounts"][0]["windows"][0]["stats"])
        self.collector.collect_window_stats()
        self.assertIsNotNone(self.data()["accounts"][0]["windows"][0]["stats"])
        self.api.fail.add("/admin/accounts/101/usage")
        self.now += timedelta(seconds=901)
        self.collector.collect_window_stats()
        result = self.data()["accounts"][0]
        self.assertIsNone(result["windows"][0]["stats"])
        self.assertEqual(result["sources"]["quota"]["errors"]["window_stats"], "http_503")

    def test_models_cover_the_device_scope_for_the_sub2api_day(self):
        self.assertIn(("/admin/dashboard/models", {"start_date": "2026-10-04", "end_date": "2026-10-04",
            "timezone": "Asia/Shanghai", "model_source": "requested", "account_id": "101"}), self.api.calls)
        self.assertEqual(self.data()["models"]["items"], [{"model": "claude-opus-5-5", "other_models": 0,
            "requests": "190", "tokens": "25165094", "account_cost_usd": "16.574750", "standard_cost_usd": "33.149500"}])
        device = dict(self.cfg["devices"][0], account_ids=None)
        models = self.collector.snapshot(device)["data"]["models"]
        self.assertTrue(models["complete"])
        self.assertEqual([m["model"] for m in models["items"]], ["gpt-6.1-sol", "claude-opus-5-5", "gpt-5.6-luna"])

    def test_models_beyond_the_rows_fold_into_one(self):
        self.api.models["101"].append(("gpt-6.1-sol", 65, 6489712, Decimal("3.531925")))
        self.api.models["102"] += [("claude-haiku-4-5", 4, 3754, Decimal("0.00401")), ("gpt-5-mini", 1, 1000, Decimal("0.001"))]
        self.collector.collect_models()
        device = dict(self.cfg["devices"][0], account_ids=None)
        models = self.collector.snapshot(device)["data"]["models"]
        self.assertEqual(models["total"], 5)
        self.assertEqual(models["items"][0], {"model": "gpt-6.1-sol", "other_models": 0, "requests": "293",
            "tokens": "35940770", "account_cost_usd": "12.797347", "standard_cost_usd": "25.594693"})
        self.assertEqual(models["items"][3], {"model": None, "other_models": 2, "requests": "5", "tokens": "4754",
            "account_cost_usd": "0.005010", "standard_cost_usd": "0.010020"})

    def test_failed_or_previous_day_models_are_not_partially_shown(self):
        self.api.fail.add(("/admin/dashboard/models", "101"))
        self.collector.collect_models()
        models = self.data()["models"]
        self.assertEqual((models["complete"], models["items"]), (False, []))
        self.assertEqual(self.collector.records["101"]["models_meta"]["error"], "http_503")
        self.api.fail.clear()
        self.collector.collect_models()
        self.now = datetime(2026, 10, 4, 16, 0, 1, tzinfo=UTC)
        self.assertFalse(self.data()["models"]["complete"])

    def test_invalid_stats_are_not_coerced_to_zero(self):
        for value in (None, float("nan"), "NaN", "-1", "1.5"):
            with self.assertRaises(ValueError):
                today_stats({"requests": value})


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.now = datetime(2026, 10, 4, 8, tzinfo=UTC)
        self.cfg, self.api = config(), FakeAPI(self.now)
        self.collector = Collector(self.cfg, self.api, lambda: self.now)
        self.collector.tick(force=True)
        self.server = make_server(self.cfg, self.collector)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = "http://127.0.0.1:" + str(self.server.server_port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, path, token="device-secret", **headers):
        request = Request(self.url + path, headers=dict(headers, Authorization="Bearer " + token))
        try:
            return urlopen(request, timeout=3)
        except HTTPError as error:
            return error

    def test_authorization_precedes_conditional_response(self):
        with self.request("/display/v1/snapshot") as response:
            self.assertEqual(response.status, 200)
            etag = response.headers["ETag"]
        with self.request("/display/v1/snapshot", token="wrong", **{"If-None-Match": etag}) as response:
            self.assertEqual(response.code, 401)

    def test_cached_get_304_and_detail_never_collect(self):
        before = len(self.api.calls)
        with self.request("/display/v1/snapshot") as response:
            etag = response.headers["ETag"]
            self.assertNotIn(b"MUST_NOT_LEAK", response.read())
        with self.request("/display/v1/snapshot", **{"If-None-Match": etag}) as response:
            self.assertEqual(response.code, 304)
            self.assertEqual(response.read(), b"")
        with self.request("/display/v1/accounts/101") as response:
            self.assertEqual(response.status, 200)
        with self.request("/display/v1/accounts/102") as response:
            self.assertEqual(response.code, 404)
        self.assertEqual(before, len(self.api.calls))

    def test_pagination_cannot_expand_scope_or_overload_device(self):
        for path in ("?page=0", "?page_size=11", "?page=1&page=2", "?page_size=no"):
            with self.request("/display/v1/snapshot" + path) as response:
                self.assertEqual(response.code, 400)
        with self.request("/display/v1/snapshot?account_ids=102") as response:
            self.assertEqual([a["id"] for a in json.load(response)["data"]["accounts"]], ["101"])

    def test_staleness_changes_etag_and_cached_data_metadata(self):
        with self.request("/display/v1/snapshot") as response:
            etag = response.headers["ETag"]
        self.now += timedelta(seconds=121)
        with self.request("/display/v1/snapshot", **{"If-None-Match": etag}) as response:
            self.assertEqual(response.status, 200)
            self.assertTrue(json.load(response)["data"]["partial"])


class PaginationTests(unittest.TestCase):
    def test_changing_inventory_rejected(self):
        class API(Sub2API):
            def __init__(self): pass
            def get(self, path, params):
                return {"total": 2 if params["page"] == 1 else 3, "items": [{"id": params["page"]}]}
        with self.assertRaisesRegex(APIError, "pagination_changed"):
            API().pages("/admin/accounts")


if __name__ == "__main__":
    unittest.main()
