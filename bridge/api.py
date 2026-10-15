import json
import ssl
from decimal import Decimal
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, build_opener, HTTPSHandler, HTTPRedirectHandler


class APIError(Exception):
    """Only a sanitized category is logged or exposed; never an upstream body."""

    def __init__(self, code):
        self.code = code
        super().__init__(code)


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Sub2API:
    def __init__(self, cfg, key):
        self.base = cfg["upstream_url"] + "/api/v1"
        self.key = key
        self.timeout = cfg["request_timeout"]
        self.opener = build_opener(HTTPSHandler(context=ssl.create_default_context()), NoRedirect())

    def get(self, path, params=None):
        url = self.base + path + ("?" + urlencode(params) if params else "")
        request = Request(url, headers={"x-api-key": self.key, "Accept": "application/json",
                                       "User-Agent": "esp32-sub2api-display/1"})
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                body = response.read(8 * 1024 * 1024 + 1)
        except HTTPError as error:
            error.close()
            raise APIError("http_" + str(error.code)) from None
        except (URLError, TimeoutError, OSError):
            raise APIError("network_error") from None
        if len(body) > 8 * 1024 * 1024:
            raise APIError("response_too_large")
        try:
            result = json.loads(body, parse_float=Decimal)
        except (ValueError, UnicodeError):
            raise APIError("invalid_json") from None
        if not isinstance(result, dict) or result.get("code") != 0 or "data" not in result:
            raise APIError("invalid_api_envelope")
        return result["data"]

    def pages(self, path, params=None):
        records, seen, expected = [], set(), None
        for page in range(1, 10001):
            payload = self.get(path, dict(params or {}, page=page, page_size=100))
            if not isinstance(payload, dict) or not isinstance(payload.get("items"), list):
                raise APIError("invalid_pagination")
            total = payload.get("total")
            if not isinstance(total, int) or total < 0:
                raise APIError("invalid_pagination")
            if expected is None:
                expected = total
            if expected != total:
                raise APIError("pagination_changed")
            for item in payload["items"]:
                if not isinstance(item, dict) or not isinstance(item.get("id"), int) or item["id"] in seen:
                    raise APIError("invalid_pagination")
                seen.add(item["id"])
                records.append(item)
            if len(records) == total:
                return records
            if not payload["items"] or len(records) > total:
                raise APIError("incomplete_pagination")
        raise APIError("pagination_limit")
