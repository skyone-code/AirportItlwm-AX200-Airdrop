# AirportItlwm + AWDL（AirDrop for Intel Wi-Fi）

在 Intel 无线网卡上把 **AirDrop（AWDL）** 跑起来的 AirportItlwm 分支。普通 Wi-Fi 照常工作，AirDrop 与它并行。

基于 [laobamac/itlwm](https://github.com/laobamac/itlwm) 的 AirportItlwm（原生 Wi-Fi 接口），AWDL 层为本仓库新增。当前版本 **2.5.15**。

---

## 现在能做什么

| 能力 | 状态 |
| --- | --- |
| 未连 Wi-Fi 时 AirDrop 双向传输 | 可用 |
| 连着 Wi-Fi 时 AirDrop | 可用 |
| 传输中同时扫描全频段 SSID | 可用，扫描面板不再空白 |
| 关闭再打开 Wi-Fi | 可用，自愈重建约 6 秒，无需重启 |
| 关联/未关联来回切换 | 可用，不再出现"一直等待中" |
| 先传过文件、再去连 Wi-Fi | 首次传输即通，不用等 30 秒超时 |
| 多设备并发（3 台手机） | 可用 |

## 现在还不能做什么

- **只支持这三条系统线**：macOS 26.7.1/26.7.2/26.7.3（Tahoe）、15.8 与 15.8.3（Sequoia，其他 15.8.x 按 "expected" 放行）、14.8.9（Sonoma）。其他构建由运行时校验挡住，驱动退化成普通 Wi-Fi。
- **AWDL 时间窗对齐偏粗**，靠单帧堆速率，长传输每 20 秒左右有一次扫描放行导致的短暂速率抖动。
- **不支持** WPA3-only、强制 PMF、MLO。
- 只在**部分机型**上验证过，见 [docs/LIMITATIONS.md](docs/LIMITATIONS.md)。

## 兼容性

| 项 | 要求 |
| --- | --- |
| 系统 | macOS 26.7.x / 15.8.x / 14.8.9，仅 x86_64 |
| 网卡 | Intel AX200 / AX210 / AX211 / AX201 一类（与上游硬件表一致） |
| 引导 | OpenCore，需在 `Kernel > Add` 中挂载 |
| 依赖 | 无。不需要 Lilu |

## 安装

1. 把 `AirportItlwm.kext` 放到 `EFI/OC/Kexts/`。
2. 在 `config.plist` 的 `Kernel > Add` 里加一条：

   | 字段 | 值 |
   | --- | --- |
   | BundlePath | `AirportItlwm.kext` |
   | ExecutablePath | `Contents/MacOS/AirportItlwm` |
   | PlistPath | `Contents/Info.plist` |
   | MinKernel | Tahoe `25.0.0` / Sequoia `24.0.0` / Sonoma `23.0.0` |
   | MaxKernel | Tahoe `25.99.99` / Sequoia `24.99.99` / Sonoma `23.99.99` |

3. 删掉旧的 `AirportItlwm.kext`（上游原生版）或 `itlwm.kext` + HeliPort：两者不能共存。
4. 重启。**不需要任何 boot-arg。**

细节、卸载与回滚见 [docs/INSTALL.md](docs/INSTALL.md)。

## 构建

需要 Xcode 与 MacKernelSDK（commit `3f750085caa17ec3a7880f11c11bf4f48cd6a164` 放在 `MacKernelSDK/`）。

```bash
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build
```

三个目标：`AirportItlwm-Tahoe`、`AirportItlwm-Sequoia` 走 scheme；`AirportItlwm-Sonoma14.4` 没有独立 scheme，用 `-target AirportItlwm-Sonoma14.4`。产物为 `AirportItlwm.kext`，版本号来自工程里的 `MODULE_VERSION`。

完整说明见 [docs/BUILD.md](docs/BUILD.md)。

## 验证装好了没有

```bash
ioreg -rn AirportItlwm | grep -E "AWDLBuildCheck|AWDLActivationBlocked|AWDLNativeUp|AWDLMiState"
```

正常应看到 `r2vhjBuildCheck = verified`、`r2vhjNativeUp = Yes`。判读表与抓日志方法见 [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md)。

## 它是怎么工作的

AWDL 接口继承 Apple 的 `IO80211AWDLProtocol`，在 5 GHz 上用 16 个 legacy 时隙轮转广播与收发，同时把 Wi-Fi 的扫描、关联、数据路径让开射频。三条主线：多槽并发发送、扫描仲裁与让位、关联时的路径交接。详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 仓库结构

```
AirportItlwm/          kext 主体，AWDL 接口与集成点在这里
  hf58f.cpp   运行时校验：系统构建 + Apple 虚表指纹
include/               AWDL 协议实现（帧、调度、探测、监管域……）与 Apple 私有头
itl80211/ itlwm/       net80211 与 iwlwifi 移植（上游）
itlwm.xcodeproj        构建工程
scripts/               上游脚本
docs/                  安装、构建、架构、诊断、限制
```

## 致谢

本仓库建立在两条上游血脉上：

- [laobamac/itlwm](https://github.com/laobamac/itlwm) —— 原生 AirportItlwm（Tahoe / Sequoia），本仓库的起点
- [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm) —— zxystd 的 itlwm，AirportItlwm 的源头

以及上游 README 中列出的 Acidanthera、Apple、OpenBSD、Intel、Linux 等贡献者。

## 许可

GPL-2.0，见 [LICENSE](LICENSE)。Intel 固件的分发许可见 [iwlwifi-firmware-license](iwlwifi-firmware-license)。

> ⚠️ AWDL 层直接子类化 Apple 私有类并按虚表布局调用，只在经过校验的系统构建上启动。在未验证的系统上强行用 `-itlawdlanybuild` 打开，可能导致内核崩溃。
