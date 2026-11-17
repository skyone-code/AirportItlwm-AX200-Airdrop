# 构建

## 环境

| 项 | 要求 |
| --- | --- |
| Xcode | 26.x（本仓库在 26.5 上验证） |
| 架构 | x86_64（AWDL 层只覆盖 Intel 机型） |
| 依赖 | 无。不需要 Lilu、不需要 Python |

## MacKernelSDK

工程用 `KERNEL_EXTENSION_HEADER_SEARCH_PATHS = $(PROJECT_DIR)/MacKernelSDK/Headers`，因此仓库根目录下必须有 `MacKernelSDK/`：

```bash
git clone https://github.com/acidanthera/MacKernelSDK.git
cd MacKernelSDK && git checkout 3f750085caa17ec3a7880f11c11bf4f48cd6a164
```

`MacKernelSDK/` 已在 `.gitignore` 中，不会被提交。

## 三个目标

一条系统线一个 kext，不能混装。工程里只有 Tahoe 与 Sequoia 两个共享 scheme，Sonoma14.4 按 target 构建：

```bash
# Tahoe
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build

# Sequoia
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Sequoia \
  -configuration Release ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build

# Sonoma14.4（没有独立 scheme，用 -target）
xcodebuild -project itlwm.xcodeproj -target AirportItlwm-Sonoma14.4 \
  -configuration Release ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build
```

用 scheme 构建时产物在 DerivedData：

```
~/Library/Developer/Xcode/DerivedData/itlwm-*/Build/Products/Release/<Tahoe|Sequoia>/AirportItlwm.kext
```

用 `-target` 构建时产物落在工程目录：

```
build/Release/<Sonoma14.4>/AirportItlwm.kext
```

Debug 把 `-configuration Release` 换成 `Debug`。kext 名固定为 `AirportItlwm.kext`（`PRODUCT_NAME = AirportItlwm`），版本号由 `MODULE_VERSION` 注入 `Info.plist` 的 `CFBundleVersion` / `CFBundleShortVersionString` 与代码里的 `ITLWM_VERSION`。

## AWDL 与目标的关系

AWDL 只在三个现代目标上真正启用，靠编译宏区分：

| 目标 | 宏 | 说明 |
| --- | --- | --- |
| AirportItlwm-Tahoe | `ITL_AWDL_NATIVE` | 虚表 568 槽 |
| AirportItlwm-Sequoia | `ITL_AWDL_NATIVE` + `ITL_AWDL_GUARD_SEQUOIA` | 虚表 563 槽，15.8.x 允许 "expected" |
| AirportItlwm-Sonoma14.4 | `ITL_AWDL_NATIVE` + `ITL_AWDL_GUARD_SONOMA` | 虚表 547 槽，仅 14.8.9 |
| 其余 AirportItlwm 目标 | — | 编成纯 Wi-Fi，`itlAWDLBuildAllowed()` 恒返回 false |

`AirportItlwm/AWDLBuildGuard.cpp` 是唯一新增的编译单元，已加入 Tahoe/Sonoma14.4 共用的 Sources 阶段与 Sequoia 的 Sources 阶段。其余 AWDL 代码都写在既有文件里（`AirportItlwmV2.cpp`、`AirportItlwmSkywalkInterface.cpp`、`WCL.cpp`、`itlwm/hal_iwx/ItlIwx.cpp` 等），以 `#if defined(ITL_AWDL_NATIVE)` 围起来。

## 构建后的自检

```bash
K=~/Library/Developer/Xcode/DerivedData/itlwm-*/Build/Products/Release/Tahoe/AirportItlwm.kext
plutil -extract CFBundleVersion raw "$K/Contents/Info.plist"          # 2.5.10
plutil -extract CFBundleIdentifier raw "$K/Contents/Info.plist"       # com.zxystd.AirportItlwm
nm "$K/Contents/MacOS/AirportItlwm" | grep -c itlAWDL                 # 有输出说明 AWDL 层编进去了
nm -u "$K/Contents/MacOS/AirportItlwm" | grep -c AWDLProtocol         # 应 > 0，说明确实引用 Apple 的 AWDL 类
```

## 常见构建问题

- **`MacKernelSDK/Headers` 找不到**：SDK 没 checkout 或 commit 不对。
- **`deployment target 10.12` 警告**：上游遗留，无害。
- **改了 `include/Airport/macOS26/*.h` 后 Sequoia 目标报错**：三套私有头按 `__IO80211_TARGET` 选择，改一套不会影响另一套，但如果新增了跨版本符号，需要三处同时补。
