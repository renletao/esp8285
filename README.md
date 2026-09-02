# esp8285-hello

ESP8285 + Arduino 框架的 PlatformIO 工程模板，开箱即可编译烧录。
固件功能：上电打印芯片自检信息，之后每秒输出一行 Hello World。

## 目录结构

```
esp8285-hello/
├── platformio.ini          工程配置（板型、串口、Flash 模式都在这）
├── src/
│   └── main.cpp            主程序
├── include/                共享头文件（自动在 include 搜索路径里）
├── lib/                    自己写的私有库，一个库一个子目录
├── test/
│   └── test_chip/
│       └── test_main.cpp   示例单元测试，`pio test` 运行
└── .gitignore
```

## 环境准备

装 PlatformIO Core（命令行）：

```bash
pip3 install -U platformio
```

或者在 VS Code 里装 **PlatformIO IDE** 扩展，打开这个文件夹即可，
下方状态栏有编译（✓）、烧录（→）、串口监视（插头图标）按钮。

第一次编译时 PlatformIO 会自动下载 espressif8266 平台和 xtensa 工具链，
约几百 MB，需要等一会儿。

## 常用命令

在工程根目录执行：

```bash
pio run                        # 编译
pio run -t upload              # 编译并烧录
pio device monitor             # 打开串口监视器，Ctrl+C 退出
pio run -t upload -t monitor   # 烧录完直接进监视器
pio run -t clean               # 清理编译产物
pio device list                # 列出可用串口
pio test                       # 在真实板子上跑单元测试
```

## 烧录接线

如果用的是 ESP-01/ESP-M2 这类裸模块（没有 USB 口），需要外接 USB-TTL：

| ESP8285 | 接到 | 说明 |
| --- | --- | --- |
| VCC | 3.3V | **不能接 5V**，且电源要能供 500mA 以上 |
| GND | GND | |
| TX | USB-TTL 的 RX | 注意是交叉的 |
| RX | USB-TTL 的 TX | |
| EN / CH_PD | 3.3V | 经 10kΩ 上拉，悬空不启动 |
| GPIO15 | GND | 经 10kΩ 下拉 |
| GPIO2 | 3.3V | 上拉或悬空 |
| GPIO0 | **GND** | 拉低 = 进入烧录模式 |

烧录流程：GPIO0 接地 → 上电（或按一下 RST）→ 执行 `pio run -t upload`
→ 烧完把 GPIO0 断开 → 再复位一次，程序开始运行。

带 USB 口的开发板（NodeMCU、Wemos D1 mini 等）会自动处理进入烧录模式，
直接 `pio run -t upload` 就行。

## 关于 ESP8285 的两个要点

**Flash 必须是 DOUT 模式。** ESP8285 把 1MB Flash 封在芯片内部，
占用了 SD_D1/SD_D2 引脚，所以只能用 DOUT。设成 qio 或 dio 能烧进去但起不来。
`platformio.ini` 里已经写死了 `board_build.flash_mode = dout`。

**只有 1MB Flash，空间紧。** 默认分区（`eagle.flash.1m64.ld`）给程序约 960KB、
文件系统 64KB。用不上文件系统就换成 `eagle.flash.1m0.ld` 多腾出 64KB。
注意这个容量做 OTA 空中升级会很吃紧，因为 OTA 需要同时容纳新旧两份固件。

## 排错

**串口一开始是乱码** — 正常。ESP8285 的 bootloader 固定用 74880 波特率打印启动信息，
115200 下看就是乱码，之后我们程序的输出是正常的。

**`Timed out waiting for packet header`** — 没进烧录模式（检查 GPIO0），
或者波特率太高。把 `platformio.ini` 里的 `upload_speed` 调回 `115200`。

**反复重启 / 打印 `wdt reset`** — 电源带不动。ESP8285 发射时瞬时电流可达 300mA 以上，
别用 USB-TTL 模块自带的 3.3V 输出供电，单独接个电源，并在 VCC-GND 间加 100µF 电容。

**崩溃后是一串十六进制地址** — 工程已经启用了 `esp8266_exception_decoder`，
串口监视器会自动把它翻译成文件名和行号，直接看解码后的内容。
