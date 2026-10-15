# sub2api 账户页现有 API 证据与 ESP32-S3 外部适配方案

调查日期：2026-10-04。代码位置：[Wei-Shaw/sub2api](https://github.com/Wei-Shaw/sub2api)。当前提交：`b8dece9000c68815a5b867ca5a1e6f236e173905`。调查时 `git status --short` 为空；未修改该项目，未读取 `.env`、数据库内容或真实凭据。项目与父级未发现额外 `AGENTS.md`；遵循任务提供的中文与 Context7 规则。本文件依据项目源代码调查，未调用线上管理接口，不能据此断言当前部署的账号数量、配置开关、数据新鲜度或实测响应大小。

## 1. 可以复用的数据路径

`/admin/accounts` 的设备数据可从现有 JSON API 取得。按用户明确约束，当前方案只由独立 bridge 或 ESP32 调用这些 API，不修改 sub2api、不新增其接口。服务器把面板 API 挂在 `/api/v1` 下，管理接口统一经过 AdminAuth、面板限流、审计和 AdminComplianceGuard；现有管理认证没有设备只读 scope。[路由入口](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/router.go#L119)，[管理中间件](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/routes/admin.go#L26)。

| 已有接口 | 实际方法与响应 `data` | 可以提供什么 | 读取边界 |
| --- | --- | --- | --- |
| `/api/v1/admin/accounts` | GET；`{items,total,page,page_size,pages}` | 每账号配置、状态、并发、部分本地配额和已保存的供应商快照 | 数据库列表与关联读取；特定账号窗口费用另查 SQL；可关闭调度分 |
| `/api/v1/admin/accounts/:id` | GET；单账号 DTO，带运行字段 | 指定账号详情及当前并发、可选 RPM/会话/窗口费用 | 无通用 `:id/runtime` 路由；运行字段由处理器组合 |
| `/api/v1/admin/accounts/usage/batch` | **POST**；请求 `{account_ids:[...],force:false}`；响应 `{usage:{"ID":...},errors:{"ID":"..."}}` | 多平台用量 | Anthropic OAuth/SetupToken 被动读取；其他平台可能主动访问上游、写状态 |
| `/api/v1/admin/accounts/:id/usage` | GET；`source=passive` 或默认 `active`，`force=true` 可选；单个 UsageInfo | 平台用量窗口、重置时间、某些状态 | `source=passive` 仅支持 Anthropic OAuth/SetupToken |
| `/api/v1/admin/accounts/today-stats/batch` | **POST**；请求 `{account_ids:[...]}`；响应 `{stats:{"ID":WindowStats}}` | 今日请求、Token、三种费用 | 本地 SQL；处理器有 30 秒快照缓存，失败可能被补为零 |
| `/api/v1/admin/accounts/:id/today-stats` | GET；WindowStats | 单账号今日统计 | 本地 SQL；查询错误会返回错误 |
| `/api/v1/admin/accounts/:id/stats?days=30` | GET；历史统计 | 1–90 天统计，默认 30 天 | 时间区间按服务配置时区计算 |
| `/api/v1/admin/channel-monitors` | GET；分页列表，含 account_id/latest_quota | 已绑定账号监控的持久化额度快照 | 功能开启；列表读已有 latest，不执行 RunCheck；缺失快照不补零 |
| `/api/v1/admin/ops/concurrency` | GET；`{enabled,platform,group,account,timestamp}` | 平台、组、账号并发、容量、排队 | 管理鉴权及 Ops monitoring/realtime 开关；适合已有运维总览 |
| `/api/v1/admin/ops/account-availability` | GET；同类集合 | 状态、限流、过载、临时冷却 | 管理鉴权及 Ops 开关；可用性定义有局限 |

接口方法来自[账户路由](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/routes/admin.go#L360)，usage 请求和返回见[批量处理器](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L2682)、[usage 处理器](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L2733)。统一成功包为 `{code:0,message:"success",data:...}`，设备不能照抄前端已解包的数据结构。[响应封装](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/pkg/response/response.go#L14)。

## 2. 列表参数、返回字段和开销

列表支持 `page`、`page_size`（或 `limit`）、`platform`、`type`、`status`、`search`、`group`、`privacy_mode`、`sort_by`、`sort_order`、`lite`、`include_scheduler_score`。分页默认第 1 页、20 条；接受 1–1000 的页大小，非法页大小退回默认值。`search` 去空格并限制为 100 字节；`group=ungrouped` 可筛未分组账号，负数/无法解析的组 ID 返回错误。[处理器参数](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L643)，[分页解析](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/pkg/response/response.go#L169)。

默认按 `name asc` 排序，可按 id、status、schedulable、priority、rate_multiplier、last_used_at、expires_at、created_at、upstream_billing_rate 排序，并以 ID 作为次序键。搜索只匹配账号名。`status=active` 额外要求 schedulable、当前未限流、未临时冷却；其筛选不等于只看数据库 `status`，也不等于完整调度器可用性。[筛选](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/repository/account_repo.go#L1006)，[排序](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/repository/account_repo.go#L1175)。

列表核心运行字段包括 `current_concurrency`、`status`、`schedulable`、`error_message`、`rate_limited_at`、`rate_limit_reset_at`、`overload_until`、`temp_unschedulable_until/reason`、`last_used_at`、`expires_at`、`session_window_start/end/status`。`expires_at` 是 Unix 秒；其他上述时间多数为 `time.Time` 的 JSON 时间字符串，不能统一当毫秒整数解析。[DTO](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/dto/types.go#L333)，[时间转换](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/dto/mappers.go#L499)。

`current_window_cost`、`active_sessions`、`current_rpm` 只对启用相应控制的 Anthropic OAuth/SetupToken 有意义。列表的 `current_window_cost` 使用 **StandardCost**，不等于账号费用 Cost。并发/RPM/会话数读 Redis；窗口费用按账号查 PostgreSQL，最多 10 个并行。调度分默认不计算，开启时会跨候选池读取负载并评分；设备请求应显式 `include_scheduler_score=false`。[运行字段](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L703)。

`lite=1` 省略完整 groups/account_groups，**仍包含**脱敏后的 `credentials`、`credentials_status`、`extra`、proxy、备注、ParentEmail/ParentChatGPTAccountID 等配置/身份字段；它不是设备字段白名单。敏感 Credential 键按清单移除，以 `has_<key>` 标记存在性；未知 Credential 键仍会原样保留。Extra 仅去掉 Ollama/OpenCode 的若干内部管理键，其他 Extra 保留。Proxy DTO 省略密码，但含主机、端口、用户名。[lite 投影](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/dto/mappers.go#L461)，[Credential 脱敏](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/dto/credentials_redact.go#L11)，[敏感键清单](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_credentials_redact.go#L5)，[Extra 脱敏](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/dto/mappers.go#L416)。因此 bridge 应从 API 响应逐字段构造设备 DTO，不能转发完整 Account 后只删除少数字段。

列表每次先查数据库 COUNT、分页列表、批量代理/组关系，并给 Ollama/OpenCode 覆盖同凭据组已有快照；这些覆盖过程是本地读取，不触发抓取。[列表仓储](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/repository/account_repo.go#L1102)，[关联加载](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/repository/account_repo.go#L3370)，[Ollama 覆盖](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/ollama_cloud_usage.go#L530)，[OpenCode 覆盖](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/opencode_go_usage.go#L475)。列表支持 ETag/304，但是在上述读取、运行数据组合之后才计算 ETag；304 节省传输，不消除列表查询成本。[列表条件响应](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L843)。

## 3. Usage 不是全平台只读缓存 API

批量 usage 去重且丢弃非正 ID，一次加载账号，最多 6 个账号并行；缺失账号及单账号失败按 ID 放入 errors，不中断整批。Anthropic OAuth/SetupToken 强制走被动分支，`force` 在这条分支不触发主动刷新；其他账号调用通用 `getUsageForAccount`。[批量服务](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L514)。

下表的内部方法用于解释现有接口行为，独立 bridge 不能直接调用这些 Go 方法或读取进程私有缓存。

| 平台/类型 | 当前代码的数据源 | 不改 sub2api 的读取方法 |
| --- | --- | --- |
| Anthropic OAuth / SetupToken | GetPassiveUsage 从 Extra 构造 session_window、passive_usage_7d/7d_oi 及采样时间，并附本地窗口统计 | 已有 GET usage?source=passive；结合列表原始字段区分真实 utilization、估算和无样本；没有 Sonnet 被动映射 |
| OpenAI OAuth，含 Spark shadow | buildCodexUsageProgressFromExtra 从 codex_5h/7d_*、codex_usage_updated_at 构造窗口 | 在 bridge 解析列表 extra 字段；canonical 优先，primary/secondary 按窗口时长映射；shadow 维度独立；默认不调用可能 probe 的 usage |
| Gemini | getGeminiUsage 以本地 policy 套餐为分母，以 usage_logs 日/分钟统计为分子 | 优先已有 monitor latest_quota；否则调用已有 usage，并承认其 wrapper 可能 ClearError；保存模式无快照时 unknown，结果标 local_estimate |
| Grok | GrokQuotaFetcher.BuildUsageInfo 从 grok_usage_snapshot、grok_billing_snapshot 构造；本地日志可附消费 | 解析列表 extra 快照或已有 monitor quota；原有 usage 可能 ProbeBilling，不宣称严格被动；保留 unknown_until_first_response |
| Antigravity | AccountUsageService 有私有 antigravityCache；monitor 持久化 latest quota 可经 HTTP 获取 | 已有管理监控列表的 account_id/latest_quota；无样本时 unknown，或配置调用现有 usage；不能直接读内部缓存，也不新增缓存入口 |
| CN Coding Plan | Extra 保存 `<provider>_5h/weekly/monthly_used_percent`、reset_at、usage_updated_at | 解析列表已返回的 Extra，或 monitor quota；主动刷新使用原有 /cn-providers/accounts/:id/quota，保留其查询/回写行为 |
| CN 按量余额 | Extra 保存 `<provider>_balance`、balance_currency、balances、balance_available、balance_updated_at | 解析列表 Extra，或 monitor balance；需要时用原有 /balance；保留币种，不将不同币种相加 |
| Ollama Cloud / OpenCode Go | 列表独立 usage state/snapshot，组归属覆盖已有快照；含 fetched_at/status/last_error | 映射列表 ollama_cloud_usage/opencode_go_usage；不为设备请求调用 refresh POST |
| API Key / Bedrock 本地控制 | DTO 的 quota_limit/used、quota_daily_limit/used、quota_weekly_limit/used 来自 Extra | 读列表字段，标 local_limit；limit<=0 为未启用，不是上游余额或已用 0% |

对应代码证据：被动读取见[GetPassiveUsage](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L586)，OpenAI 的 pure helper 见[Codex Extra 构造](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L1481)，Gemini 见[本地用量计算](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L971)，Grok 见[BuildUsageInfo](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/grok_quota_fetcher.go#L21)，Antigravity 见[缓存与抓取](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L1028)，CN 见[配额写快照](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/cn_provider_quota_service.go#L615)与[余额写快照](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/cn_provider_balance_service.go#L230)，独立状态读取见[Ollama](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/ollama_cloud_usage.go#L511)与[OpenCode](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/opencode_go_usage.go#L458)，本地额度见[配额 Getter](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account.go#L2556)。

通用 active 路径有明确副作用：Anthropic 缓存失效后访问上游，随后 `syncActiveToPassive` 写入 Extra；Gemini/Antigravity/Grok/Anthropic 成功后可能调用 `tryClearRecoverableAccountError`；OpenAI 缺失/过期或限流时可能请求 `/responses` 进行 probe，Spark shadow 可请求 `/wham/usage`；Grok 可能 ProbeBilling 并合并/持久化计费快照。OpenAI probe 是生成请求，不能只因管理接口是 GET 就称其为无上游调用。[通用分发](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L348)，[OpenAI 刷新分支](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L719)，[probe 请求](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L840)，[清错误](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L1608)，[Grok probe](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L1100)。

实际缓存常量：Anthropic 成功 3 分钟、失败 1 分钟，窗口统计 1 分钟；OpenAI probe 10 分钟；Grok retry 1 分钟。Antigravity 成功/403 3 分钟，其他错误 1 分钟。某些函数旧注释写 Anthropic 10 分钟，应以常量和分支为准。[缓存常量](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L107)，[Antigravity TTL](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L1271)。

## 4. 不能把无样本、失败或缺字段当成零

- **Anthropic 被动 5h 无样本仍返回 0**：`estimateSetupTokenUsage` 在没有 session_window_end 时创建 utilization=0 对象。有窗口但无 utilization 时，它按 `rejected→100`、`allowed_warning→80` 估算，其他情况保持 0；重置时间过去后也归零。bridge 输出应区分 `observed`、`estimated`、`unknown`，并保留源采样时间。[被动估算](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L1705)。被动实现仅构造 SevenDay 与 SevenDayFable，没有 SevenDaySonnet，不能因为字段缺失显示 Sonnet=0。[被动窗口映射](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L615)。
- **并发读取错误被吞掉**：列表初始化空 map，仅在 Redis 读取成功时替换，最终按 ID 取值；失败会得到 `current_concurrency=0`。详情也以 0 初始化。RPM/会话数同样忽略部分批量错误。外部 bridge 只能看到成功响应中的零值，无法判断这次 Redis 是否失败；应标为 API 报告，不能承诺识别隐藏错误。显式 API 错误才可标 null/unknown 或保留旧值过期。[列表并发读取](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L709)，[详情并发](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L368)。Redis 实际批量读取含过期租约清理和两个 ZCARD 相加，是 Sub2API 占槽/租约并发，不是提供商全局并发。[Redis 计数](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/repository/concurrency_cache.go#L665)。
- **今日统计失败可能被补为 0**：GetTodayStatsBatch 优先一条批量 SQL；失败后最多 8 个单账号查询，单个失败吞错，缺失结果最终补 `&WindowStats{}`。因此沿用其成功响应无法区分“确实无使用”与“读失败”；不改后端时，少量重点账号优先调用已有单账号 GET today-stats，其 SQL 错误会传播至 HTTP 层。依据见[单账号处理器](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L2662)和[单账号 service](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L1394)。[批量降级](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L1429)。
- **更新时间不等于源采样时间**：部分 UsageInfo 的 UpdatedAt 直接设为本次计算时间。例如 Gemini/OpenAI 构造时间不能证明上游额度刚采到。应另用 `passive_usage_sampled_at`、`codex_usage_updated_at`、snapshot.fetched_at 等作为 source_updated_at。[OpenAI 构造](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L709)，[被动采样时间](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account_usage_service.go#L607)。

## 5. 今日请求、Token、费用和时间的准确口径

今日统计从**服务配置时区的当日 00:00**开始，不接受设备时区参数。应用启动通过 `timezone.Init(cfg.Timezone)` 初始化，未给时区时 helper 默认 Asia/Shanghai。现有账号统计 API 不返回 day/timezone；bridge 必须配置与实际 sub2api 部署一致的时区来补充显示日界线，不能从设备时区或 HTTP Date 推断。跨日的一轮采集应丢弃并重采。[时区初始化](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/repository/ent.go#L107)，[Today](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/pkg/timezone/timezone.go#L89)。

SQL 读 `usage_logs`，按 account_id 和 `created_at>=today` 汇总；它是本服务记录的使用量，不能推断供应商账号在其他客户端上的使用量。精确表达如下。[今日 SQL](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/repository/usage_log_repo_stats.go#L275)。

| 返回字段 | SQL 口径 | 设备标签建议 |
| --- | --- | --- |
| requests | `COUNT(*)` | 本站今日请求记录 |
| tokens | `SUM(input_tokens + output_tokens + cache_creation_tokens + cache_read_tokens)` | 本站今日 Token（含缓存） |
| cost | `SUM(COALESCE(account_stats_cost,total_cost) * COALESCE(account_rate_multiplier,1))` | 本站账号费用 |
| standard_cost | `SUM(total_cost)` | 本站标准费用 |
| user_cost | `SUM(actual_cost)` | 本站用户计费 |

Cost 的实际实现不只是 `total_cost*倍率`，它优先 `account_stats_cost`。这三种费用不能混作同一指标，也不能将本站计算费用直接称为上游账单。批量接口使用同口径 GROUP BY account_id；处理器缓存 30 秒且支持 ETag/304。[批量 SQL](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/repository/usage_log_repo_stats.go#L339)，[缓存 TTL](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_today_stats_cache.go#L9)，[缓存条件响应](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/account_handler.go#L2702)。

## 6. 状态、健康与现成运维聚合

列表 `schedulable` 是持久化配置。`Account.IsSchedulable()` 还检查 active、到期自动暂停、过载、限流、临时冷却、APIKey/Bedrock 本地 quota；它仍不代表某个模型实时上游可用，也没有作为列表 DTO 的现成布尔字段返回。bridge 的 observed_normal 仅是列表状态推断，不能声称调用了内部 IsSchedulable。OpenAI 等调度器还有具体模型/候选池策略，设备页面不要将一个布尔值命名为“所有模型健康”。[基本调度条件](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/account.go#L182)。

已有 Ops account-availability 提供账号状态映射和平台/组计数，但其 `is_available` 仅是 `active && schedulable && !rateLimited && !overloaded && !tempUnsched`，没有检查上文的到期/本地 quota。一个账号可属于多个组，不能相加各组汇总当成全局账号数；并发 DTO 已明确这一点。[Ops 可用性算法](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/ops_account_availability.go#L53)，[Ops 并发 DTO](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/ops_realtime_models.go#L14)。Ops monitoring/realtime 关闭时返回 enabled=false 的空集合，不能显示为“0 个账号”。[开关行为](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/ops_realtime_handler.go#L117)。

`GET /health` 只固定返回 `{status:"ok"}`，没有数据库/Redis检查，也没有账号健康语义。[健康检查](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/routes/common.go#L12)。

## 7. 鉴权与凭据生命周期

管理路由支持 `x-api-key: <Admin API Key>` 或 `Authorization: Bearer <管理员 JWT>`；普通网关 API Key 并不是这里的管理员凭据。x-api-key 非空时优先验证该路径，失败不会再回退 Bearer。[AdminAuth](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/middleware/admin_auth.go#L24)。

Admin API Key 是 settings 表里的**单个全局值**：生成 32 字节随机数并存储，重新生成覆盖原值，删除撤销。GET 设置接口只返回 exists/masked_key，生成时返回完整 key。此实现没有过期时间、账号选择范围或只读 scope；通过管理员认证后可以进入账户修改/删除等路由。应由服务器/桥接程序持有，不能把它当设备展示令牌。[生成与读取](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/setting_features.go#L569)，[管理处理器](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/setting_handler_runtime.go#L13)。

JWT 验证 token、最新用户 active、TokenVersion、管理员角色；开启会话绑定时还检查 IP/UA，变化可撤销会话。不能保证复制浏览器 JWT 到 ESP32 或桥接程序可持续工作。[管理员 JWT 校验](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/middleware/admin_auth.go#L156)，[IP/UA 绑定](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/middleware/session_binding.go#L55)。

Access Token 过期用 `access_token_expire_minutes`，为 0 时回退 `expire_hour`；代码默认分别为 0/24 小时，Refresh Token 默认 30 天，实际部署可能不同。POST `/api/v1/auth/refresh` 接收 refresh_token 并返回新 access_token、refresh_token、expires_in；刷新会轮转并删除旧 Refresh Token、保留 familyID。[过期计算](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/auth_service.go#L1420)，[代码默认](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/config/config.go#L2319)，[刷新处理器](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/auth_handler.go#L670)，[轮转](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/auth_service.go#L1837)。

管理路径还可能因 AdminComplianceGuard 未确认返回 HTTP 423；这是现有路由运行前提。默认面板限流豁免管理员，但可配置关闭；不应以默认豁免作为高频拉取设计的依据。[423 行为](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/middleware/admin_compliance.go#L12)，[面板限流](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/middleware/panel_rate_limit.go#L59)。

## 8. 已有渠道监控能提供哪些额度数据

**独立 bridge 优先使用管理端列表 `GET /api/v1/admin/channel-monitors`。** 它通过已有 AdminAuth，返回 account_id、check_mode 和 latest_quota，可按账号 ID 关联；管理 DTO 不受 show_quota 限制。List 批量读已有 latest/可用率后附快照；单条 Get 只返回配置，没有同样的聚合。管理 guard 只检查 Enabled、不检查 V1/V2 模式，但当前模式未必仍在采集旧监控，必须判断过期。无需新增路由或从用户视图按名称猜账号。[管理路由](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/routes/admin.go#L794)、[列表/单条处理器](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/channel_monitor_handler.go#L250)、[DTO](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/admin/channel_monitor_handler.go#L117)、[guard](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/routes/admin.go#L868)。缺失/聚合失败时 latest_quota 可能为空，应显示未采样/未知，不能推断余额为零。


**V1 可以覆盖“额度展示”子需求。** `GET /api/v1/channel-monitors` 需要普通用户 JWT，channel monitor 功能开启且 mode=v1；show_quota=true 才在服务器响应中保留 latest_quota。返回监控名称、provider、状态、延迟、可用率、timeline、latest_quota，不返回管理凭据/账号 account_id，也不包含账户页并发、schedulable、本地 quota 控制或今日费用。[用户路由](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/routes/user.go#L138)，[用户 DTO/开关](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/channel_monitor_user_handler.go#L35)。

V1 监控可通过 `AccountID` 绑定已有账号，check_mode=quota/quota_probe；最新快照从持久化 latest 读取，设备 GET 不执行上游抓取。后台检测使用已有 ChannelMonitorQuotaFetcher，已把多平台归一为 MonitorQuotaSnapshot/Tiers（window、label、used_percent、used、limit、reset_at、余额/币种、fetched_at）。已有 HTTP 返回中的 MonitorQuotaSnapshot 可作为外部归一的来源，减少多平台字段解析；这不表示 bridge 可调用内部 Fetcher。[绑定](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/channel_monitor_types.go#L52)，[只读聚合](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/channel_monitor_aggregator.go#L58)，[快照 DTO](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/domain/channel_monitor_quota.go#L31)。

但 Fetcher 缓存失效仍调用 active usage/CN quota/balance，可能写状态，不是新的“严格被动”服务。FetchedAt 是 monitor 本轮采集时间，不保证每个窗口均为该时刻的新上游样本。[时间构造](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/channel_monitor_quota_fetcher.go#L232)。其成功快照缓存 5 分钟、失败 1 分钟；quota 模式不测 ping/生成延迟，状态由配额阈值/查询结果推导，90% 用量可被标为 degraded。[采集逻辑](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/channel_monitor_quota_fetcher.go#L169)，[阈值与缓存](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/channel_monitor_const.go#L86)，[状态推导](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/channel_monitor_quota_fetcher.go#L505)。

**V2 可覆盖服务流量总览，不能代替每账号额度。** `/api/v1/channel-monitor-v2/snapshot` 同样需要 JWT、功能开关和 mode=v2，用户范围按 GetAvailableGroups 约束；返回请求/Token/RPM/成功率/延迟、健康评分、coverage/trend 等聚合，过滤维度是 platform/group/model，没有账户配额数组。源码“Public snapshot”注释指用户脱敏视图，不能据此推断匿名公开接口。[V2 路由](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/server/routes/user.go#L145)，[用户范围](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/handler/channel_monitor_v2_handler.go#L193)，[V2 快照结构](https://github.com/Wei-Shaw/sub2api/blob/b8dece9000c68815a5b867ca5a1e6f236e173905/backend/internal/service/channel_monitor_v2.go#L211)。在已检查的 router、routes、管理鉴权与渠道监控服务中未发现可直接授予设备“指定账号只读”的分享/设备 token 机制；bridge 可在自己的配置与展示服务中实现这个范围，sub2api 管理员 Key 权限仍不改变。

## 9. 按“不二次开发 sub2api”约束的方案（方案阶段记录）

推荐在 ESP32 项目内实现独立 bridge，仅调用上述现有 HTTP API。后台读取账号详情/分页列表、重点账号单条今日统计、列表保存额度或已有 monitor 快照，再构造白名单 JSON，供 ESP32 定时读取。bridge 不访问 PostgreSQL/Redis、不链接内部 Go 服务、不新增 sub2api 路由或数据库表。

管理 API Key 保存在 bridge；bridge 本地配置账号范围、短别名、实际服务时区、显示 Token 哈希与刷新策略。设备 Token 只认证 bridge 自己的展示地址，不是 sub2api 的新凭据。示例 scope 为 bridge 配置范围，observed_normal 为列表推断；sources.status=ok 为相关 API 采集成功，不保证内部 Redis 错误已排除。

推荐选 3–10 个重点账号。账号状态约 30 秒、单账号今日统计约 60 秒；额度默认保存模式，Anthropic 可用现有被动 API。额度缺失不补零。需要页面等效主动额度时，配置后台低频调用现有 usage/CN quota/balance，接受并记录既有上游探测及状态回写行为。设备读 bridge 缓存不即时触发这些调用；全平台 force=false 不是只读承诺。

范围内今日统计仅对同日完整成功的一轮求和；有失败标 partial 并保留旧值过期，不能混合日期或缺失账号。全局统计、选择范围与分页须区分。列表/监控接口未暴露的内部错误不作强保证，不用 /health 证明数据库/Redis 正常。

若不运行 bridge，ESP32 也可直接调用相同现有 API，设备持有全局管理 Key，并在固件内处理多接口、分页、大响应和凭据生命周期。V1 用户监控只能独立满足额度子需求；三个主视图仍需组合账号与今日 API。

本节保留方案阶段的证据边界；后续已按此方案实现 bridge/固件并完成自动化实机验收。当前结果与限制见[实施记录](../implementation-status.md)。具体契约和模拟 JSON 仍见[主方案](../sub2api-esp32-s3-plan.md)与[模拟 JSON](../examples/device-snapshot.example.json)。
