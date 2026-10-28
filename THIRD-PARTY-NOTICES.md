# 第三方来源与许可证

本项目自有服务端、协议、定制模块、工具与文档按根目录 MIT 发布。以下内容保留其原有版权和许可证；根目录 MIT 不改变其他组件的条款。

| 内容 | 来源与版本 | 许可证与位置 |
| --- | --- | --- |
| 固件原始工程、设备 UI 与原工程资源 | [M5Stack/StackChan](https://github.com/m5stack/StackChan/tree/b72b3ede38b32d54f0b6ba51c62cfcef2ec3ae1e)，基线 1.4.3 | MIT，保留 `firmware/LICENSE` 与文件内署名 |
| FTServo 驱动 | 随上游工程引入 | MIT，`firmware/main/hal/drivers/FTServo_Arduino/LICENSE` |
| BMI270 SensorAPI | Bosch Sensortec，随上游引入 | BSD-3-Clause，`firmware/main/hal/drivers/bmi270/BMI270_SensorAPI/LICENSE` |
| Montserrat SemiBold 派生字体数据 | 上游 `MontserratSemiBold26.c` | SIL OFL 1.1，`docs/licenses/Montserrat-OFL.txt` |
| 六份构建源码依赖 | `firmware/repos.json` 和 `tools/firmware-dependencies.json` | 各依赖自己的许可证；下载时保留，未复制进仓库 |
| ESP-IDF managed components | `firmware/dependencies.lock` | 各组件自己的许可证，使用前检查对应组件 |
| Python 运行依赖 | `server/requirements.txt` | 各软件包自己的许可证；安装内容不在仓库中 |

固件源码里的原始版权、SPDX 与署名注释保留。部分上游工具文件采用 Apache-2.0 等其他许可证，以文件内声明为准。原工程引入的图标、声音和图形资源不表示对第三方商标的授权；本仓库的概览图是新绘制的项目架构示意，网页截图由合成数据本地生成。

本项目与 M5Stack、DeepSeek 及其他上游作者没有官方关联或背书关系。
