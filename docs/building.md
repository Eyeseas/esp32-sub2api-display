# 编译与烧录

最后更新：2026-10-15（Asia/Shanghai）。本文说明怎样从零搭建固件的编译环境、构建并刷写到开发板。bridge 的配置和设备接入见[设备信息与接入手册](device-and-onboarding.md)。

固件基于 ESP-IDF v6.1，组件版本在 `firmware/main/idf_component.yml` 和 `firmware/dependencies.lock` 中锁定：LVGL 9.4.0、esp_lvgl_port 2.9.0、esp_lcd_touch_ft5x06 1.1.1、cJSON 1.7.19。首次构建时，ESP-IDF 的组件管理器会把它们下载到 `firmware/managed_components/`。

有两种搭建方式：

| 方式 | 适用 | 说明 |
| --- | --- | --- |
| A. 仓库脚本（推荐） | macOS、Linux | `scripts/setup.sh` 把 ESP-IDF 和工具链装进仓库内的 `.tools/`，不改动全局环境；之后用 `scripts/idf.sh` 代替 `idf.py` |
| B. 已安装的 ESP-IDF v6.1 | Windows，或已经装过 ESP-IDF 的机器 | 激活 ESP-IDF 环境后，在 `firmware/` 目录直接运行 `idf.py` |

## 需要准备

- 立创·实战派 ESP32-S3 开发板（ESP32-S3-WROOM-1-N16R8，2.0 寸 ST7789 320×240 屏，FT6336 触摸）。其他板卡的屏幕、触摸和 LCD 片选接法不同，不能直接使用。
- 一根**能传数据**的 USB-C 线，直接插开发板。很多充电线只有供电；电池仓、充电底座也可能不转发 USB 数据，经过它们时电脑上看不到串口。
- 约 6 GB 磁盘空间（ESP-IDF 源码约 0.7 GB，工具链约 5 GB），以及能访问 GitHub、`dl.espressif.com`、PyPI 和 `components.espressif.com` 的网络。国内网络见下文“[网络较慢时](#网络较慢时)”。

## 方式 A：用仓库脚本（macOS / Linux）

### 1. 安装系统依赖

`scripts/setup.sh` 要求 **Python 3.12**（不是 3.12 以上，`scripts/idf.sh` 按 `idf6.1_py3.12_env` 这个虚拟环境名查找解释器），另外需要 git。CMake 和 Ninja 由脚本装进虚拟环境，不需要另装。

macOS（需要 [Homebrew](https://brew.sh/)）：

```sh
xcode-select --install          # 已安装命令行工具时会提示无需安装
brew install python@3.12 git
```

Debian / Ubuntu：

```sh
sudo apt-get install git wget flex bison gperf python3.12 python3.12-venv \
  ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
sudo usermod -aG dialout "$USER"  # 允许访问串口，执行后需要重新登录
```

Ubuntu 24.04 自带 Python 3.12；更早的版本需要从 deadsnakes PPA 等来源安装 `python3.12`。其他发行版请参考 [ESP-IDF 官方的 Linux 依赖说明](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.1/esp32s3/get-started/linux-macos-setup.html)，Arch 系的串口用户组是 `uucp`。

### 2. 获取代码并安装工具链

```sh
git clone https://github.com/Eyeseas/sub2api-display.git
cd sub2api-display
scripts/setup.sh
```

`setup.sh` 依次完成：

1. 把 ESP-IDF v6.1 浅克隆到 `.tools/esp-idf`（含子模块；该目录已存在时跳过）。
2. 把 ESP32-S3 的编译器、调试器等工具装进 `.tools/idf-tools`。
3. 创建 ESP-IDF 的 Python 虚拟环境 `.tools/idf-tools/python_env/idf6.1_py3.12_env`，并装入 `cmake>=3.22,<4` 和 `ninja`。

第一次运行需要较长时间，主要花在下载上。完成后输出“工具链准备完成”。脚本可以重复运行，已经装好的部分会跳过。

### 3. 构建

```sh
scripts/idf.sh set-target esp32s3   # 只在第一次，或需要重新生成 sdkconfig 时运行
scripts/idf.sh build
```

`scripts/idf.sh` 会设置 `IDF_PATH`、`IDF_TOOLS_PATH`，加载 ESP-IDF 环境，切换到 `firmware/` 后把参数原样交给 `idf.py`，所以 `idf.py` 的子命令都可以这样用（`menuconfig`、`size`、`fullclean` 等）。

第一次构建会下载组件并编译 LVGL，耗时几分钟；之后是增量构建。成功时最后几行会给出刷写命令，应用文件为 `firmware/build/sub2api_display.bin`，约 2.4 MB。

### 4. 找到串口

把开发板直接用 USB 线连到电脑。这块板的 USB 口走 ESP32-S3 内置的 USB Serial/JTAG，不需要安装驱动。

| 系统 | 端口名 | 查看方法 |
| --- | --- | --- |
| macOS | `/dev/cu.usbmodemXXXX` | `ls /dev/cu.usbmodem*` |
| Linux | `/dev/ttyACM0` | `ls /dev/ttyACM*`；没有权限时检查 `dialout` 组 |
| Windows | `COM3` 等 | 设备管理器 → 端口（COM 和 LPT），名称为“USB 串行设备” |

下文用 `<串口>` 代替实际端口名。

### 5. 刷写并查看日志

```sh
scripts/idf.sh -p <串口> flash
scripts/idf.sh -p <串口> monitor    # 按 Ctrl+] 退出
# 或者合在一起：
scripts/idf.sh -p <串口> flash monitor
```

刷写只写入 bootloader、分区表和应用，不会擦除 NVS 分区，所以已经保存的 Wi-Fi、bridge 地址、Token、背光亮度和触摸校准都会保留。需要恢复出厂状态时运行 `scripts/idf.sh -p <串口> erase-flash`，之后重新刷写并重新配置。

刷写成功后屏幕会进入桌面，Sub2API 页面显示标有“演示数据”的界面。下一步是写入 Wi-Fi 和 bridge 配置，见[设备信息与接入手册](device-and-onboarding.md#4-写入设备配置)。

串口同一时间只能被一个程序打开。运行 `provision.py`、`device-check.py` 或 `display-test.py` 之前，先退出 `monitor`。这些脚本需要 pyserial，ESP-IDF 的虚拟环境里已经有，所以用这个解释器运行：

```sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python scripts/provision.py --port <串口>
```

## 方式 B：使用已安装的 ESP-IDF（含 Windows）

`scripts/*.sh` 是 bash 脚本，Windows 上请用 Espressif 官方的 [ESP-IDF 安装管理器（EIM）](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.1/esp32s3/get-started/windows-setup.html)安装 **v6.1**，或在 macOS / Linux 上沿用已有的 v6.1 安装。版本必须是 6.1.x，`idf_component.yml` 限定了 `>=6.1,<6.2`。

打开 ESP-IDF 终端（Windows 上是 EIM 创建的“ESP-IDF PowerShell”；macOS / Linux 上先 `. $IDF_PATH/export.sh`），然后：

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <串口> flash monitor
```

配置和检查脚本在仓库根目录运行，使用 ESP-IDF 环境里的 `python`（已带 pyserial）：

```sh
cd ..
python scripts/provision.py --port COM3 --config firmware/device.local.json
```

Windows 上也可以把 bridge 跑在同一台电脑上，但需要先 `pip install tzdata`，见[设备信息与接入手册](device-and-onboarding.md#1-配置-bridge)。

## 网络较慢时

ESP-IDF 官方提供国内下载服务器。运行 `setup.sh` 前设置以下环境变量：

```sh
export IDF_GITHUB_ASSETS="dl.espressif.cn/github_assets"            # 工具链从乐鑫国内服务器下载
export PIP_INDEX_URL="https://mirrors.tuna.tsinghua.edu.cn/pypi/web/simple"  # Python 包走清华镜像
scripts/setup.sh
```

`IDF_GITHUB_ASSETS` 只影响工具链下载，不影响克隆 ESP-IDF 本身。如果 `git clone` 很慢或失败，可以先用代理或其他镜像把 ESP-IDF v6.1 连同子模块克隆到 `.tools/esp-idf`，`setup.sh` 发现 `.tools/esp-idf/tools/idf.py` 已存在就会跳过克隆：

```sh
git clone --branch v6.1 --depth 1 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git .tools/esp-idf
```

## 固件配置

### menuconfig 中的板级选项

`scripts/idf.sh menuconfig` → `Display board`：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| Show demo data until provisioned | 开 | 未写入配置时显示演示数据 |
| LCD swap X/Y、mirror X、mirror Y | 开、开、关 | 横屏方向；画面方向不对时调整 |
| Invert LCD colors | 开 | 这块 IPS 屏需要反色 |
| LCD SPI clock (MHz) | 80 | 10–80；出现花屏时可以调低 |
| LCD X/Y offset | 0 | 画面整体偏移时调整 |
| Touch swap X/Y、mirror X、mirror Y | 开、开、关 | 触摸方向；只解决方向问题，偏差用屏幕上的触摸校准修正 |

### sdkconfig 和 sdkconfig.defaults

- `firmware/sdkconfig.defaults` 进入版本库，记录项目需要的设置：16 MB QIO Flash、8 MB Octal PSRAM、自定义分区表、USB Serial/JTAG 控制台、证书包、LVGL 字体，以及 `-O2`、240 MHz、32 KB 指令缓存和 64 KB 数据缓存等性能设置。
- `firmware/sdkconfig` 由构建生成，已被 git 忽略。`sdkconfig.defaults` 中的值只在**生成新的** `sdkconfig` 时生效。
- 拉取了修改 `sdkconfig.defaults` 的新代码后，运行 `scripts/idf.sh set-target esp32s3` 重新生成（旧文件保存为 `sdkconfig.old`）。这会丢掉你在 menuconfig 中做的改动，需要长期保留的设置请写进 `sdkconfig.defaults`。
- 确认性能设置已生效：`grep CPU_FREQ_MHZ= firmware/sdkconfig`，结果应为 240。

### 分区表

`firmware/partitions.csv`：

| 名称 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| nvs | 0x9000 | 24 KB | Wi-Fi、bridge 地址、Token、背光、触摸校准 |
| phy_init | 0xf000 | 4 KB | 射频校准数据 |
| factory | 0x10000 | 4 MB | 应用（当前约 2.4 MB） |

用 `scripts/idf.sh size` 查看应用占用；16 px 中文字库（GB2312 全集）约占 0.9 MB。

## 分发编译好的固件

想把固件给没有编译环境的人使用时，可以合并成一个镜像：

```sh
scripts/idf.sh merge-bin      # 生成 firmware/build/merged-binary.bin
```

对方只需要 esptool（`pip install esptool`），从 0 地址写入：

```sh
python -m esptool --chip esp32s3 -p <串口> write-flash 0x0 merged-binary.bin
```

合并镜像会把各段之间的空隙填成 0xFF，**写入后 NVS 中的配置和触摸校准会被清空**，所以它适合首次安装。已经配置过的设备请用 `flash`，或者分别写入 `build/flash_args` 中列出的三个文件。

## 重新生成中文字库（可选）

`firmware/main/display_font.c`（16 px，GB2312 全集加源码用字）和 `display_font_14.c`（14 px，源码用字）已经生成并提交，只有在源码里新增了 14 px 字库没有的汉字、或者想换字体时才需要重新生成。需要 Node.js：

```sh
mkdir -p .tools/font && (cd .tools/font && npm install lv_font_conv@1.5.3)
```

再从 [Noto CJK](https://github.com/notofonts/noto-cjk) 下载 `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`，放到 `.tools/NotoSansCJKsc-Regular.otf`，然后运行：

```sh
python3 scripts/generate_font.py
```

字体采用 SIL Open Font License 1.1，许可证见 [firmware/main/FONT-LICENSE.txt](../firmware/main/FONT-LICENSE.txt)。

## 主机上的测试

不需要开发板：

```sh
python3 -m unittest discover -s tests -v    # bridge 单元测试，只用标准库
scripts/idf.sh build                         # 固件能否编译
```

`scripts/preview-check.mjs` 用无头 Chrome 打开 bridge 预览页并截图，目前写死了 macOS 上 Chrome 的路径，是可选的开发工具。

## 构建常见问题

| 现象 | 处理 |
| --- | --- |
| `需要 Python 3.12，请先安装后重试` | 安装 Python 3.12，确认 `python3.12` 在 `PATH` 中（Homebrew 装在 `/opt/homebrew/opt/python@3.12/bin` 或 `/usr/local/opt/python@3.12/bin`，脚本也会自动查找这两处） |
| `scripts/idf.sh` 报找不到 `idf6.1_py3.12_env` | `setup.sh` 没有完整跑完，重新运行；用其他 Python 版本装过时，删除 `.tools/idf-tools/python_env` 后重来 |
| 下载工具或 Python 包超时 | 设置 `IDF_GITHUB_ASSETS` 和 `PIP_INDEX_URL`，见“网络较慢时” |
| 构建时下载组件失败 | 需要访问 `components.espressif.com`；网络恢复后重新 `build`，不要运行 `update-dependencies`，以免改动锁定的版本 |
| CMake 报版本或策略错误 | ESP-IDF 6.1 需要 CMake 3.22 以上；本项目只在 CMake 3.x 下验证过，脚本在虚拟环境里固定了 `cmake<4`，确认 `PATH` 中没有更早出现的系统 CMake 4 |
| 改了 `sdkconfig.defaults` 却没效果 | 运行 `scripts/idf.sh set-target esp32s3` 重新生成 `sdkconfig` |
| `Permission denied: /dev/ttyACM0` | 把用户加入 `dialout` 组并重新登录 |
| 找不到串口或刷写连不上 | 换数据线、直接插开发板；仍不行时按住用户键（BOOT）重新插 USB 进入下载模式后再刷写 |
| 刷写成功但屏幕全黑 | 先看 `monitor` 中两行 `board:` 日志；更多排查见[设备信息与接入手册](device-and-onboarding.md#故障排查) |
