# sub2api 账号状态屏 · ESP32-S3

一块放在桌面上的 2 寸触摸屏，实时显示 [sub2api](https://github.com/Wei-Shaw/sub2api) 中账号的健康状态、额度窗口和今日用量。

项目由两部分组成：

- **bridge**：一个只用 Python 标准库的小服务。它用管理员 API Key 定时读取 sub2api **已有的**管理 API，整理成精简的快照，再用单独的设备 Token 提供给屏幕。不需要修改 sub2api，管理员 Key 也不会进入设备。
- **固件**：运行在立创·实战派 ESP32-S3 开发板上，基于 ESP-IDF 6.1 和 LVGL 9.4，通过 Wi-Fi 拉取 bridge 的快照并显示。

```text
sub2api 管理 API ──HTTPS──▶ bridge（采集、缓存、设备 Token） ──HTTP(S)──▶ ESP32-S3 屏幕
      管理员 API Key                 电脑 / NAS / 树莓派                   只持有设备 Token
```

## 功能

- **桌面**：上下滑动的卡片轮播。时钟 + [一言](https://hitokoto.cn/)、Sub2API 摘要（正常账号数、今日费用、最紧张的额度窗口）、系统信息。
- **Sub2API 首页**：正常 X/Y 与告警数、今日账号费用 / 请求数 / Token；每页 3 个账号，每个账号显示前两个额度窗口和重置倒计时。
- **今日详情**：今日总数和按 Token 排序的模型分布。
- **账号详情**：额度窗口的百分比、倒计时、数据质量，以及窗口内的请求数、Token 和费用；并发和该账号的今日用量。
- 支持 Anthropic、Codex、Grok、CN、Ollama/OpenCode、Antigravity/Gemini 等平台的额度数据（具体取决于 sub2api 已保存的数据，见[数据口径与接口](docs/data-and-api.md)）。
- 颜色只表示状态：绿色正常，琥珀色达到 80% 或限流，红色达到 95% 或出错，灰色表示过期或未知。数据超过 120 秒没有更新时整屏变灰。
- 用户键单击熄屏 / 亮屏，背光 1–100% 可调；9 点触摸校准；中文字库覆盖 GB2312 全集。
- 一个 bridge 可以服务多块屏，每块屏有自己的 Token 和账号范围，可以单独撤销。
- 浏览器预览页：不接开发板也能看到三个页面的效果。

## 硬件

| 项目 | 型号 |
| --- | --- |
| 开发板 | [立创·实战派 ESP32-S3](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/)（ESP32-S3-WROOM-1-N16R8：16 MB Flash，8 MB PSRAM） |
| 屏幕 | 2.0 寸 IPS，ST7789，320×240，SPI |
| 触摸 | FT6336（FT5x06 兼容），I²C |
| 连接 | USB-C 数据线（刷写、配置和日志都走这一根线）；2.4 GHz Wi-Fi |

固件按这块板的接线编写：LCD 片选接在 PCA9557 扩展芯片上，并针对这块面板做了电压 / Gamma 调校。换用其他 ESP32-S3 板需要修改 [board.c](firmware/main/board.c)。完整引脚见[设备信息与接入手册](docs/device-and-onboarding.md#硬件参数)。

## 先看看效果（不需要开发板，也不需要 sub2api）

只要有 Python 3.9 以上：

```sh
git clone https://github.com/Eyeseas/sub2api-display.git
cd sub2api-display
python3 -m bridge demo --port 8788
```

浏览器打开 <http://127.0.0.1:8788>，点击“使用演示 Token”，即可浏览模拟数据下的首页、今日详情和账号详情。

## 完整安装

需要：一个可以登录管理后台的 sub2api 实例、一台能一直运行 bridge 的机器、开发板和一根 USB-C 数据线。

### 1. 配置并运行 bridge

```sh
python3 -m bridge init          # 生成 bridge/config.local.json 和设备 Token
```

1. 在 sub2api 管理后台的“系统设置 → 管理员 API Key”中创建 Key，写入 `bridge/secrets/admin-api-key`（不是用户的网关 Key）。
2. 编辑 `bridge/config.local.json`：`upstream_url` 填 sub2api 地址，`sub2api_timezone` 填 sub2api 实际使用的时区，`accounts` 填要显示的账号 ID（或把 `selection` 改为 `all`），`listen_host` 改为 `0.0.0.0` 让设备能访问。
3. 检查并启动：

```sh
python3 -m bridge check         # 采集一次并打印结果，有问题时返回非零
python3 -m bridge run           # 浏览器打开 http://127.0.0.1:8787 可以预览
```

长期运行建议用 Docker：`docker compose up -d --build`。全部字段、多设备和 Docker 部署见[设备信息与接入手册](docs/device-and-onboarding.md)。

### 2. 编译并刷写固件

macOS / Linux 上，仓库脚本会把 ESP-IDF 6.1 工具链装进仓库内的 `.tools/`，不改动全局环境（需要 Python 3.12 和 git）：

```sh
scripts/setup.sh                       # 首次：安装工具链，较慢
scripts/idf.sh set-target esp32s3      # 首次：生成 sdkconfig
scripts/idf.sh build
scripts/idf.sh -p <串口> flash          # macOS: /dev/cu.usbmodem*，Linux: /dev/ttyACM0
```

Windows、已有 ESP-IDF 环境、国内网络加速、合并镜像分发等见[编译与烧录](docs/building.md)。刷写后屏幕显示标有“演示数据”的界面。

### 3. 写入 Wi-Fi 和 bridge 配置

```sh
cp firmware/device.example.json firmware/device.local.json
# 编辑：Wi-Fi 名称和密码、bridge 地址（运行 bridge 的机器的局域网 IP）、设备 Token（bridge/secrets/device-token 的内容）
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python scripts/provision.py --port <串口>
```

设备回复 `CONFIG_OK` 后会重启并联网。只支持 2.4 GHz Wi-Fi；bridge 地址用 `http://` 时必须保留 `"allow_http": true`。

### 4. 校准触摸并验证

在桌面滑到“系统信息”卡片，点进去后点最下面的“校准”，按提示依次点 9 个十字。然后点开 Sub2API 卡片，右上角显示“在线 Ns”即表示设备已经拿到数据。也可以运行 `scripts/device-check.py` 自动检查，见[接入手册的验证一节](docs/device-and-onboarding.md#6-验证)。

## 目录结构

```text
bridge/                 采集与快照服务（Python 标准库）
  config.example.json   配置模板；bridge init 据此生成 config.local.json
  preview.html          浏览器预览页
firmware/               ESP-IDF 工程
  main/                 固件源码：board.c 板级驱动，ui.c 界面，network.c 联网，model.c 快照解析
  sdkconfig.defaults    项目的 ESP-IDF 默认配置
  partitions.csv        分区表
  device.example.json   设备配置模板
scripts/
  setup.sh / idf.sh     安装和调用仓库内的 ESP-IDF 工具链
  provision.py          通过 USB 写入设备配置，不打印密码和 Token
  device-check.py       通过 USB 检查配置状态、视图和快照接收
  display-test.py       屏幕测试图、背光、面板寄存器和触摸校准
  generate_font.py      重新生成中文字库
tests/                  bridge 单元测试
prototypes/             界面原型（直接用浏览器打开）
docs/                   文档
Dockerfile, compose.yaml  bridge 的容器部署
```

## 文档

| 文档 | 内容 |
| --- | --- |
| [编译与烧录](docs/building.md) | 各平台的环境安装、构建、刷写、menuconfig 选项、分发固件、构建问题 |
| [设备信息与接入手册](docs/device-and-onboarding.md) | 硬件参数、bridge 配置字段、Docker 部署、设备配置、触摸校准、升级、USB 命令、故障排查 |
| [数据口径与接口](docs/data-and-api.md) | 采集节奏、额度来源、各项数字的含义和局限、设备接口 |
| [屏幕界面与面板调校](docs/display-ui-and-panel.md) | 界面设计、渲染方式、残影和触摸问题的排查过程 |
| [方案](docs/sub2api-esp32-s3-plan.md) / [证据笔记](docs/research/sub2api-backend-evidence.md) | 设计推导，以及对 sub2api 源码的调查（链接到对应提交） |
| [实施与验收记录](docs/implementation-status.md) | 开发过程中的验证结果和实物测试记录 |

## 安全说明

- 管理员 API Key 是 sub2api 的全局管理凭据（sub2api 没有只读 Key），只保存在 bridge 所在的机器上。bridge 只调用只读接口，但请把它部署在你信任的机器上。
- 设备只持有设备 Token。bridge 按每个 Token 的账号范围过滤数据，不下发凭据、邮箱、完整的 `extra` 或原始错误信息。
- 局域网内可以用 HTTP；经过公网时请让 bridge 提供 HTTPS（`tls_cert_file` / `tls_key_file`，或放在反向代理后面），并把设备的 `allow_http` 设为 `false`。固件使用证书包校验证书。
- 以下本地文件含有凭据，已被 `.gitignore` 排除，请不要提交：`bridge/config.local.json`、`bridge/secrets/`、`firmware/device.local.json`、`artifacts/`。

## 开发

```sh
python3 -m unittest discover -s tests -v   # bridge 测试
scripts/idf.sh build                        # 固件构建
```

固件依赖的版本已经锁定（ESP-IDF 6.1、LVGL 9.4.0、esp_lvgl_port 2.9.0、esp_lcd_touch_ft5x06 1.1.1、cJSON 1.7.19）。板卡原理图和官方例程见[立创·实战派 ESP32-S3 Wiki](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/)。

## 致谢

- [sub2api](https://github.com/Wei-Shaw/sub2api)：本项目读取它的管理 API，不包含它的代码。
- [LVGL](https://lvgl.io/)、[ESP-IDF](https://github.com/espressif/esp-idf)、[esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port)。
- 中文字库由 [Noto Sans CJK SC](https://github.com/notofonts/noto-cjk) 生成，采用 SIL Open Font License 1.1，见 [FONT-LICENSE.txt](firmware/main/FONT-LICENSE.txt)。
- 一言数据来自 [hitokoto.cn](https://hitokoto.cn/)。

## 许可证

本项目代码以 [MIT 许可证](LICENSE) 发布。中文字库文件 `firmware/main/display_font*.c` 由 Noto Sans CJK SC 生成，适用 SIL Open Font License 1.1。
