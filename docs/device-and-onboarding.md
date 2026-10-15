# 设备信息与接入手册

最后更新：2026-10-15（Asia/Shanghai）。这份手册说明如何把一块立创·实战派 ESP32-S3 状态屏接入 bridge，以及如何升级已经接入的设备。编译环境的安装和固件构建单独写在[编译与烧录](building.md)。数据路径如下：

```text
sub2api 管理 API → bridge（采集、缓存、设备 Token）→ ESP32-S3（Wi-Fi / LVGL）
```

bridge 只调用 sub2api 已有的 HTTP API，不修改 sub2api，设备也不持有管理员 Key。

## 接入步骤一览

| 步骤 | 做什么 | 结果 |
| --- | --- | --- |
| 1 | `python3 -m bridge init`，填写管理员 Key 和 `config.local.json` | 生成设备 Token |
| 2 | `python3 -m bridge check`，然后本机运行，或用 Docker 部署到常开的主机 | `/health` 返回 `status=ok` |
| 3 | 按[编译与烧录](building.md)安装工具链，构建并刷写固件 | 屏幕显示“演示数据” |
| 4 | 填写 `firmware/device.local.json`，用 `provision.py` 写入 | 设备重启，连接 Wi-Fi |
| 5 | 在系统信息页做一次触摸校准 | 点按位置准确 |
| 6 | 运行 `device-check.py`，或查看 monitor 日志 | `snapshot accepted ... partial=0` |

## 硬件参数

| 项目 | 参数 |
| --- | --- |
| 主控 | ESP32-S3-WROOM-1-N16R8，固件运行在 240 MHz |
| Flash / PSRAM | 16 MB（QIO）/ 8 MB Octal PSRAM |
| 屏幕 | 2.0 寸 IPS，ST7789，320×240，横屏，RGB565 |
| LCD SPI | SPI3，mode 2，80 MHz；MOSI GPIO40，SCLK GPIO41，DC GPIO39 |
| LCD CS | PCA9557（I²C 地址 `0x19`）IO0，保持低电平 |
| 背光 | GPIO42，低电平点亮；LEDC PWM 调光，范围 1–100%，保存在 NVS |
| 用户按键 | SW2（丝印 BOOT，外壳标为“自定义按键”），GPIO0，按下为低电平；单击熄屏或亮屏；复位时按住会进入下载模式 |
| 触摸 | FT6336/FT5x06（I²C 地址 `0x38`），中断和复位脚未接出，固件轮询读取 |
| I²C | I²C1，SDA GPIO1，SCL GPIO2，400 kHz |
| USB | USB Serial/JTAG，同时用于刷写、配置和日志 |

固件默认 LCD 使用 `swap_xy=1`、`mirror_x=1`、`mirror_y=0` 并开启反色，触摸使用相同的坐标变换。`board_init()` 会写入 ST7789 的电压和 Gamma 调校参数，用来消除文字残影。板级实现见 [board.c](../firmware/main/board.c)，面板参数的来历见[屏幕界面与面板调校](display-ui-and-panel.md)。

官方资料：[立创·实战派 ESP32-S3 Wiki](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/)。原理图、引脚分配、外设教程和官方例程都在这里，排查底层问题（引脚、PCA9557、ST7789、触摸、背光、音频等）时先查这里。其中[液晶显示教程](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/lcd-display.html)只调用默认初始化，没有提供面板的 VCOM/Gamma 参数。

软件版本已经锁定：ESP-IDF 6.1、LVGL 9.4.0、esp_lvgl_port 2.9.0、esp_lcd_touch_ft5x06 1.1.1、cJSON 1.7.19。bridge 只需要 Python 3.9+ 标准库；固件工具链使用 `.tools/` 中的 Python 3.12 环境。

## 1. 配置 bridge

在项目根目录执行：

```sh
python3 -m bridge init
```

把现有的 sub2api **管理员** API Key 写入 `bridge/secrets/admin-api-key`。这个 Key 在 sub2api 管理后台的“系统设置 → 管理员 API Key”中创建；它不是用户的网关 Key，bridge 也不会生成或替换它。也可以改用环境变量 `SUB2API_ADMIN_API_KEY`。

> 管理员 API Key 是全局凭据，拥有完整的管理员权限（sub2api 没有只读 Key）。bridge 只调用下文列出的只读接口，但请把它部署在你信任的主机上，并保持 `secrets/` 目录权限为 700、文件为 600（`bridge init` 已经这样设置）。

Windows 上的 Python 不带 IANA 时区数据库，运行 bridge 前需要 `pip install tzdata`；macOS 和 Linux 不需要。

编辑 `bridge/config.local.json`，完整字段见 [config.example.json](../bridge/config.example.json)：

```json
{
  "upstream_url": "https://sub2api.example.com",
  "admin_api_key_file": "secrets/admin-api-key",
  "sub2api_timezone": "Asia/Shanghai",
  "selection": "ids",
  "accounts": [
    {"id": "20", "alias": "Claude-20", "quota_mode": "saved"}
  ],
  "listen_host": "0.0.0.0",
  "listen_port": 8787,
  "devices": [
    {"name": "重点账号", "token_sha256": "", "account_ids": null}
  ]
}
```

| 字段 | 说明 |
| --- | --- |
| `upstream_url` | 必须使用 HTTPS，并且不能带凭据或查询参数 |
| `sub2api_timezone` | 必须与线上 sub2api 的实际时区一致，否则“今日”统计的口径会错开 |
| `selection` | `ids` 只采集 `accounts` 中的账号；`all` 会扫描全部账号，此时 `accounts` 只用来设置别名和额度策略 |
| `accounts[].alias` | 设备上显示的名称，建议使用 ASCII 短名；其他字符会回退显示账号 ID |
| `accounts[].quota_mode` | 默认 `saved`，只读取已保存的额度；`existing_usage` 会定期调用已有的 usage 接口，该接口可能主动查询供应商。CN 账号另外设置 `cn_mode=quota` 或 `balance` |
| `devices[].account_ids` | `null` 表示使用全部采集范围，填数组则进一步缩小范围；设备的请求参数不能扩大范围 |
| `listen_host` | 设备需要访问时设为 `0.0.0.0`；只在本机预览时用 `127.0.0.1` |
| `use_monitors` | 开启后读取 channel monitor 的 `latest_quota`，适用于 Antigravity/Gemini 等账号 |
| 采集间隔 | `state_interval` 30 秒，`today_interval` 60 秒，`models_interval` 60 秒，`window_interval` 60 秒，`quota_interval` 300 秒，`quota_stale_seconds` 900 秒，`request_timeout` 15 秒；取值必须在 1–86400 秒之间 |
| `tls_cert_file` / `tls_key_file` | 让 bridge 直接提供 HTTPS；也可以留空，放在已有的 HTTPS 反向代理后面 |

`bridge init` 会生成 `bridge/secrets/device-token`，并把它的 SHA-256 写入 `devices[].token_sha256`。需要多块屏使用不同的 Token 和账号范围时，在 `devices` 中增加条目，然后给每块屏写入各自的 Token。

管理员 Key 需要能读取以下接口（都是只读调用）：

- `/admin/accounts`（列表和详情）
- `/admin/accounts/<id>/today-stats`
- `/admin/dashboard/models?account_id=…`：今日页的模型分布
- `/admin/accounts/<id>/usage?source=passive`：Anthropic OAuth/Setup Token 账号的用量窗口统计，不会请求上游
- 可选：`/admin/channel-monitors`、`/admin/cn-providers/...`

先检查采集，再启动服务：

```sh
python3 -m bridge check
python3 -m bridge run
```

在浏览器打开 `http://127.0.0.1:8787`，输入 `bridge/secrets/device-token` 中的 Token，即可预览首页、今日详情和账号详情。预览只读取 bridge 的缓存，翻页或 304 都不会触发新的上游采集。没有线上凭据时，可以用 `python3 -m bridge demo --port 8788` 启动演示服务。

修改配置后需要重启 bridge。重启后，新的账号范围和 Token 撤销会立即生效。

## 2. 部署 bridge

bridge 需要一直运行，并且 ESP32 所在的网络能访问到它。可以直接在电脑上 `python3 -m bridge run`，也可以用 Docker 部署到 NAS、树莓派或服务器等常开的机器。

### 用 Docker 部署

镜像只包含 `bridge/` 的代码、预览页和示例配置（`.dockerignore` 采用白名单）。`config.local.json` 和 `secrets/` 由 `compose.yaml` 以只读方式挂载到 `/config`，不会打进镜像。容器的重启策略为 `unless-stopped`，只要 Docker 开机自启，主机重启后服务会自动恢复。镜像基于 `python:3.12-slim-bookworm`，支持 amd64 和 arm64。

在本机完成第 1 步后，把需要的文件同步到目标主机并启动（下面的 `myhost` 换成你的 SSH 主机名，目录可以自定）；更新代码或配置后重复执行同一组命令即可：

```sh
ssh myhost 'mkdir -p ~/sub2api-display && chmod 700 ~/sub2api-display'
rsync -a --delete --exclude '__pycache__/' Dockerfile compose.yaml .dockerignore myhost:~/sub2api-display/
rsync -a --delete --exclude '__pycache__/' bridge myhost:~/sub2api-display/
ssh myhost 'cd ~/sub2api-display && chmod 600 bridge/config.local.json bridge/secrets/* && docker compose up -d --build'
```

也可以在目标主机上直接 `git clone` 本仓库，在那里执行第 1 步，再运行 `docker compose up -d --build`。

用 Docker 运行时，配置里的 `listen_host` 必须设为 `0.0.0.0`（否则容器外访问不到），`listen_port` 保持 `8787`；要换对外端口，只改 `compose.yaml` 中 `ports` 冒号左边的数字。

检查方法：`curl http://<主机 IP>:8787/health`，应返回 `{"status":"ok",...}`；查看日志用 `docker compose logs --tail 20`。设备的 `bridge_url` 填 `http://<主机 IP>:8787`。建议在路由器上给这台主机分配固定 IP，否则 IP 变化后设备会连不上。

## 3. 构建并刷写固件

完整的环境安装、构建、刷写和常见问题见[编译与烧录](building.md)。已经装好工具链时，只需要：

```sh
scripts/idf.sh build
scripts/idf.sh -p <串口> flash
```

刷写后、写入配置前，屏幕会显示标有“演示数据”的界面。

## 4. 写入设备配置

在本机创建 `firmware/device.local.json`（已被 git 忽略）：

```json
{
  "wifi_ssid": "你的 2.4 GHz Wi-Fi",
  "wifi_password": "Wi-Fi 密码",
  "bridge_url": "http://192.168.1.10:8787",
  "device_token": "从 bridge/secrets/device-token 读取",
  "allow_http": true
}
```

仓库里有一份模板 [firmware/device.example.json](../firmware/device.example.json)，可以复制后修改：`cp firmware/device.example.json firmware/device.local.json`。`device_token` 填 `bridge/secrets/device-token` 文件的内容（一行字符串）；`bridge_url` 填运行 bridge 那台机器的局域网地址，不能填 `127.0.0.1`。

- 只支持 2.4 GHz Wi-Fi。有密码时最低要求 WPA2，留空密码表示开放网络。
- `bridge_url` 末尾的 `/` 会被去掉，不能包含 `?`。使用 `http://` 时必须同时设置 `allow_http=true`，否则设备会拒绝这份配置。
- 整份 JSON 不能超过 4095 字节。

通过 USB 写入：

```sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python scripts/provision.py \
  --port /dev/cu.usbmodemXXXX --config firmware/device.local.json
```

脚本会自动加上 `"command":"configure"`。打开串口会让开发板复位，所以脚本每 3 秒重发一次，最多等待 20 秒，直到设备回复 `CONFIG_OK`。之后设备把配置保存到 NVS 并重启联网。脚本不会打印 Wi-Fi 密码或 Token。如果输出“设备拒绝配置”，说明字段不合法，常见原因是用了 `http://` 却没有设置 `allow_http`。

## 5. 校准触摸

这块板的触摸面板线性度不好，左边缘的上报坐标可能偏 40–50 px，交换或镜像坐标轴解决不了，所以每块板第一次使用时建议校准一次（原因和算法见[屏幕界面与面板调校](display-ui-and-panel.md#8-触摸校准)）。

1. 在桌面上下滑到“系统信息”卡片，点按进入，点最下面一行的“校准”。也可以在电脑上运行 `scripts/display-test.py --port <串口> --pattern touch` 直接打开校准画面。
2. 屏幕依次显示 9 个十字，当前目标为橙色；每个十字点 3 下。
3. 9 个目标中误差最大的不超过 15 px 才会保存，否则从头再来。保存后进入检查模式，红点显示校正后的位置，点“完成”返回。

校准结果保存在 NVS，刷写新固件不会丢失；启动日志 `board:` 一行末尾显示 `touch calibrated` 即表示已经生效。

## 6. 验证

```sh
scripts/idf.sh -p /dev/cu.usbmodemXXXX monitor
# 或者：
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python scripts/device-check.py \
  --port /dev/cu.usbmodemXXXX --seconds 45
```

`device-check.py` 会确认配置状态、三个视图命令、快照接收以及有无 panic，日志写入被忽略的 `.tools/device-monitor.log`。串口同一时间只能被一个程序打开，运行配置或检查脚本前要先退出 `monitor`。

正常启动时依次出现以下日志：

```text
board: PCA9557 0x19 ready; LCD CS is controlled by IO0
board: ST7789 320x240 SPI3 mode=2 80 MHz / FT5x06 0x38 initialized; backlight PWM 100%; touch calibrated
display: Ready; free internal=... PSRAM=...; USB accepts JSON commands
network: snapshot accepted: page=1 accounts=3 partial=0
```

开机进入桌面的时钟卡片；向上滑到 Sub2API 卡片并点按进入，右上角显示“在线 Ns”，说明设备已取得完整快照。

## 升级已接入的设备

- **只更新 bridge**：重新执行第 2 节的 rsync 和 `docker compose up -d --build`。设备不需要任何操作；快照中新增的字段，旧固件会直接忽略。
- **只更新固件**：`scripts/idf.sh build` 后执行 `flash`。`flash` 不会擦除 NVS 分区（`0x9000`），所以 Wi-Fi、bridge 地址、Token 和背光亮度都会保留，不需要重新配置。只有执行 `erase-flash` 后才需要重新运行 `provision.py`。
- **两者都更新**：先更新 bridge，再刷写固件。今日页的模型分布和详情页的窗口统计需要两边都是 2026-10-05 及以后的版本。
- **更换 Wi-Fi、bridge 地址或 Token**：修改 `device.local.json` 后重新运行 `provision.py`。

## 设备使用

| 页面 | 内容 | 操作 |
| --- | --- | --- |
| 桌面 | 顶部日期和 Wi-Fi；下面是竖排卡片：时钟 + 一言、Sub2API 摘要（健康数、今日账号费用、前 3 个账号最紧的额度窗口）、系统信息摘要；左侧位置点 | 上下滑动或点上下露出的卡片切换；点时钟卡片换一句，点模块卡片进入模块 |
| Sub2API 首页 | 状态栏（正常 X/Y、告警数、连接状态和数据新鲜度）；今日账号费用、请求数、Token；每页 3 个账号，每个账号显示前两个额度窗口和倒计时 | 点今日摘要进入今日详情；点账号行进入账号详情；左右滑动或点页脚两侧翻页；点状态栏或在第 1 页右滑回到桌面 |
| 今日详情 | 账号费用、请求数、Token；模型分布（按 Token 排序，横条表示 Token 占比，超过 4 个模型时把第 3 名之后合并为“其余 N 个”） | 点顶部或右滑返回 |
| 账号详情 | 最多两个额度窗口（百分比、进度条、倒计时、数据质量，以及窗口内的请求数 · Token · 账号费用）；并发；该账号的今日用量；未显示的窗口数或平台 | 点顶部或右滑返回 |

- 单击用户按键熄屏或亮屏。熄屏时只关闭背光并暂停绘制，联网刷新照常进行。熄屏后触摸任意位置也能亮屏，这一下触摸不会点到下面的页面。
- 颜色只表示状态：绿色为正常；琥珀色表示用量达到 80% 或有限流类标记；红色表示达到 95% 或有错误类标记；灰色表示过期或未知。快照超过 120 秒没有更新时，整屏数值变灰。
- 设备每 15 秒请求一次快照，并使用 ETag/304。请求失败时按 30 秒、60 秒……逐步退避，最长 300 秒；遇到 401/403 时直接等待 300 秒。

## USB 命令

设备从 USB 接收单行 JSON，每条命令以换行结束。

| 命令 | 示例 | 回复 |
| --- | --- | --- |
| 写入配置 | 由 `provision.py` 发送 | `CONFIG_OK` / `CONFIG_INVALID` / `CONFIG_SAVE_FAILED` |
| 查询配置 | `{"command":"status"}` | `CONFIGURED` / `NOT_CONFIGURED` |
| 切换视图 | `{"command":"view","view":0}`，其中 0 为 Sub2API 首页，1 为今日详情，2 为当前页第一个账号的详情，3 为桌面，4 为系统信息 | `VIEW_OK` |
| 背光亮度 | `{"command":"backlight","level":40}`（1–100，保存在 NVS） | `BACKLIGHT_OK 40` |
| 测试图 | `{"command":"display_test","pattern":"ramp"}`；`"touch"` 打开触摸校准 | `DISPLAY_TEST_OK ramp` |
| 写面板寄存器 | `{"command":"lcd","cmd":187,"data":[25]}`（只接受白名单寄存器） | `LCD_OK 0xbb` |

测试图、背光和寄存器调试建议使用 `scripts/display-test.py`（选项为 `--pattern`、`--backlight`、`--register`）。**在 macOS 上打开 USB 串口会复位开发板**，所以测试图和寄存器写入必须放在同一条命令里完成。详见[屏幕界面与面板调校](display-ui-and-panel.md#4-诊断工具)。

## 网络和安全边界

局域网调试时，bridge 监听 `0.0.0.0:8787`，设备直接访问 bridge 所在机器的局域网地址，并需要显式设置 `allow_http=true`。部署到公网时，应使用 HTTPS 的 bridge 地址，并把 `allow_http` 设为 `false`。固件使用证书包校验证书并通过 SNTP 校时，不会跳过证书验证。

管理员 Key 只保存在 bridge 上，设备只持有设备 Token。bridge 在返回数据前会按设备的账号范围做校验，不返回 credentials、完整的 `extra`、邮箱或原始错误，响应上限为 32 KiB。以下本地敏感文件已被 `.gitignore` 排除，不能提交：

- `bridge/config.local.json`
- `bridge/secrets/admin-api-key`
- `bridge/secrets/device-token`
- `firmware/device.local.json`
- `artifacts/original-firmware.bin`（烧录前备份的原厂 Flash，可能包含原设备配置）

## 故障排查

| 现象 | 检查方向 |
| --- | --- |
| 电脑上找不到串口 | 换一根能传数据的 USB 线（很多充电线只有供电）；直接插开发板，不要经过电池仓、充电底座或不转发数据的 Hub；Linux 确认用户在 `dialout`（或 `uucp`）组中 |
| 刷写时提示端口被占用 | 关闭 `monitor`、串口助手或其他占用串口的程序；同一时间只能有一个程序打开串口 |
| 刷写失败或连不上芯片 | 按住用户键（BOOT，GPIO0）的同时重新插上 USB，进入下载模式后重试；刷写完成后再拔插一次回到正常运行 |
| 黑屏 | 背光 GPIO42 是否为低电平点亮、SPI3 mode 2、PCA9557 IO0 上的 CS（它不是 ESP32 的普通 GPIO），以及启动日志中前两行 `board:` 是否出现；另外确认亮度没有被设得太低 |
| 画面方向或触摸方向不对 | 在 `menuconfig → Display board` 中调整 LCD 或触摸的 swap/mirror 设置，然后重新构建并刷写 |
| 背景发灰或有旧字残影 | 确认固件包含 `board_init()` 中的面板调校参数；用 `display-test.py --pattern ramp` 检查灰阶 |
| 屏幕显示“演示数据” | 设备尚未配置，或者 NVS 中的配置无效；用 `{"command":"status"}` 确认 |
| 一直显示“连接 Wi-Fi”或“Wi-Fi 断线” | 是否为 2.4 GHz 网络、SSID 和密码是否正确、路由器是否要求 WPA3 |
| 一直显示“校时中” | 设备访问不到 `pool.ntp.org`，需要检查网络出口 |
| 读取失败 | bridge 地址或端口是否可达；运行 `curl <bridge_url>/health` 检查；确认 `listen_host` 不是 `127.0.0.1` |
| 授权失效 | 设备 Token 与 `devices[].token_sha256` 不对应，或者该设备已被撤销；修正后重新运行 `provision.py` |
| 部分数据过期 | 设备收到的快照被标为不完整，原因在 bridge 的上游采集；运行 `python3 -m bridge check` 或查看容器日志，常见原因是管理员 Key 失效或接口超时 |
| 今日页显示“模型数据不完整” | 有账号的 `/admin/dashboard/models` 查询失败，或者刚好跨日；bridge 不会显示部分结果 |
| 详情页没有窗口统计 | 只有 Anthropic OAuth/Setup Token 账号，以及配置了 `quota_mode=existing_usage` 的账号才有统计；统计早于窗口开始时间或超过 `quota_stale_seconds` 时会被隐藏 |
| 响应过大 | 快照超过 32 KiB；需要减小 `page_size` 或账号范围 |
