# 数据口径与接口

本文说明 bridge 采集什么、怎样下发给设备，以及各项数字的含义和局限。设计推导见[方案](sub2api-esp32-s3-plan.md)，sub2api 源码依据见[证据笔记](research/sub2api-backend-evidence.md)，快照格式示例见 [device-snapshot.example.json](examples/device-snapshot.example.json)。

## 设备接口

设备调用 bridge 自己的 `GET /display/v1/snapshot?page=1&page_size=3`，使用 `Authorization: Bearer <设备 Token>`。`GET /display/v1/accounts/<id>?window_page=1` 返回缓存详情，每页四个窗口。服务支持 ETag/304，响应上限 32 KiB；设备 GET、翻页和重试不会即时访问 sub2api。

## 采集节奏

默认每 30 秒读取状态、60 秒读取单账号今日统计，额度随已有状态快照读取。选定账号模式读账号详情；全站模式完整分页扫描列表。跨页总数变化或查询失败保留上次结果并标不完整，分页列表不是事务快照。

## 额度来源

额度覆盖 Anthropic 被动窗口、Codex 保存窗口、CN 计划/余额、Ollama/OpenCode 保存状态、Grok 响应头/计费窗口和本地日/周/总额度。开启 `use_monitors` 时读取已有管理监控列表中的 `account_id/latest_quota`；功能未开启或无样本时正常回退。Antigravity/Gemini 可使用已有 monitor 快照。

## 窗口统计

窗口统计（窗口开始至今的请求、Token、费用，即 sub2api“用量窗口”里的 `req · Token · A $ · U $`）随窗口下发为 `stats`。Anthropic OAuth/Setup Token 账号每 60 秒（`window_interval`）读取只读的 `usage?source=passive`，不会请求上游；其他平台只有 `existing_usage` 账号随 usage 接口一并获得。统计早于当前窗口开始或超过 `quota_stale_seconds` 时不展示。设备详情页在窗口下方显示请求数、Token 和账号费用。

## 今日模型分布

今日详情在总数下方显示模型分布，对应 sub2api `/admin/usage` 的“模型分布”（请求模型、按 Token 排序）。bridge 每 60 秒（`models_interval`）按账号调用只读的 `/admin/dashboard/models?account_id=…`，日期和时区与今日统计一致，再在设备范围内合并，因此行数据之和与今日总数同口径。横条为各行 Token 占比；超过四个模型时前三行之外合并为“其余 N 个”。按账号过滤时 sub2api 的 `actual_cost` 被替换为账号费用，所以只下发账号费用和标准费用。任一账号查询失败或跨日即标为不完整，不显示部分结果。

## 额度模式

`quota_mode=saved` 是默认值，缺样本显示未采样，旧样本显示过期/待新样本。配置为 `existing_usage` 后，后台低频调用已有 usage 接口；CN 账号另设 `cn_mode=quota` 或 `balance`。这些现有接口可能主动查询供应商、probe 或回写状态；`force=false` 不保证全平台被动。首版没有启用主动额度查询。

## 状态与今日统计的局限

总览的“状态正常”是列表字段推断。本站并发的内部 Redis 失败可能已被 sub2api 转成零，外部程序无法识别，因此标为 API 报告。今日使用单账号接口传播查询错误；失败保留旧值但不计算新的完整总数，跨日不混合昨天的数据。Token 含输入、输出和缓存；费用分别为账号、用户计费、标准费用，不是供应商账单。

## 设备的其他网络请求

除了 bridge，设备还会直接访问：

- `pool.ntp.org`：SNTP 校时。HTTPS 证书校验依赖正确的时间，校时完成前屏幕显示“校时中”。
- `https://v1.hitokoto.cn`：桌面时钟卡片上的“一言”，成功后每 10 分钟换一句。访问不到时保留内置的一句，不影响其他功能。

屏幕上的时间按 bridge 快照中的 sub2api 时区显示，收到第一份快照之前显示 `--:--`。
