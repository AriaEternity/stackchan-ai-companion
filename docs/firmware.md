# 固件源码与构建

目标硬件是 StackChan K151 / ESP32-S3。上游基线为 M5Stack/StackChan `b72b3ede38b32d54f0b6ba51c62cfcef2ec3ae1e`，公开版本 `1.4.3`。定制源码版本标记为 `companion-0.2.12+base1.4.3`；该标记不是原厂固件，也不代表最终动作验收通过。

## 宿主 C++ 测试

需要 CMake 3.16+ 和支持 C++17 的 GCC/Clang：

```bash
cmake -S firmware/tests -B .cache/host-tests \
  -DPROTOCOL_EXAMPLES_DIR="$PWD/protocol/examples" \
  -DPROTOCOL_V2_EXAMPLES_DIR="$PWD/protocol/examples-v2"
cmake --build .cache/host-tests
ctest --test-dir .cache/host-tests --output-on-failure
```

PowerShell 可把 `$PWD` 换为 `$PWD.Path`，或使用协议样例目录的绝对路径。保持断言启用；不要用 Release/NDEBUG 跳过测试逻辑。

## ESP-IDF 工程

先安装 [ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html) 并激活环境。在短的 ASCII 路径检出仓库，避免 Windows 路径和命令长度限制。

```bash
python tools/fetch_firmware_dependencies.py
cd firmware
idf.py -D CMAKE_NINJA_FORCE_RESPONSE_FILE=1 build
```

下载脚本按 SHA-256 验证六份锁定的上游源码归档，再应用仓库内补丁。存在目标时仅接受已验证的相同依赖，不覆盖未知目录。`dependencies.lock` 继续锁定 ESP-IDF Component Manager 依赖。下载内容不进入 Git。

构建前将 `firmware/main/companion/companion_config.h` 的 `stackchan.example.com` 换为自己的 WSS 域名。示例域名故意不可用。设备令牌和设备 ID 不应编译进固件，通过本地物理配网保存在 NVS。

`tools/provision_companion.py` 是初始物理配网辅助工具，需另外安装 `pyserial`。它只有显式提供串口、设备 ID 文件和令牌文件时才打开串口，等待设备在未配置状态下进入限定的配网输入。运行它属于实际设备写入，使用者需自行决定时机；本次开源发布不运行配网或刷写。

## 构建证据的范围

CI 的必选验证包含服务端和固件宿主测试。ESP-IDF 构建与硬件效果是另外两层验证，不能由宿主测试替代。构建本身也不能解决当前的实机动作问题。

上游图标、声音、字体和驱动仅用于原工程的构建，参见[第三方说明](../THIRD-PARTY-NOTICES.md)。Companion 没有启用上游云 AI、摄像头、麦克风或媒体应用入口。
