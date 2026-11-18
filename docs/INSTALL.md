# 安装

## 前提

- OpenCore 引导，能编辑 `EFI/OC/config.plist`
- 系统是 macOS 26.7.x（Tahoe）/ 15.8.x（Sequoia）/ 14.8.9（Sonoma），仅 x86_64
- 已删除旧的 `AirportItlwm.kext`（上游原生版）或 `itlwm.kext` + HeliPort

三者互斥：同一个 `IOClass` 只能被一个 kext 占用。旧的没删干净时，AirDrop 不会出现，`ioreg` 里也看不到 `r2vhj*` 属性。

## 一、放 kext

```bash
cp -R AirportItlwm.kext /Volumes/EFI/EFI/OC/Kexts/
```

挂载 ESP 后确认路径是 `EFI/OC/Kexts/AirportItlwm.kext/Contents/MacOS/AirportItlwm`。

## 二、改 config.plist

在 `Kernel > Add` 追加一条（顺序上放在 Lilu 之后、同族网卡 kext 之中）：

| 字段 | 值 |
| --- | --- |
| `BundlePath` | `AirportItlwm.kext` |
| `ExecutablePath` | `Contents/MacOS/AirportItlwm` |
| `PlistPath` | `Contents/Info.plist` |
| `MinKernel` | Tahoe `25.0.0` / Sequoia `24.0.0` / Sonoma `23.0.0` |
| `MaxKernel` | Tahoe `25.99.99` / Sequoia `24.99.99` / Sonoma `23.99.99` |
| `Enabled` | `true` |
| `Arch` | `x86_64` |

同时删除 `Kernel > Add` 里旧的 `AirportItlwm.kext` / `itlwm.kext` 条目，并相应清理 `Kernel > Block`（如果曾用 Block 屏蔽原厂卡）。

改完用 `plutil -lint config.plist` 校验，再重启。

## 三、不要做的事

- **不要加任何 boot-arg**。正式配置为零参数：共存、多槽、扫描让位、能力上报全部是代码默认值。
- **不要把 Tahoe 的 kext 挂到 Sequoia 上**（反之亦然）。三条系统线各有自己的 Apple 私有头与虚表布局，混用会被运行时校验挡住，或直接崩。
- **不要同时装上游原生 AirportItlwm**。要保留原生 Wi-Fi 又想试 AirDrop，只能二选一。

## 四、确认加载

```bash
kmutil showloaded --list-only | grep -i airportitlwm
ioreg -rn AirportItlwm | grep -E "AWDLBuildCheck|AWDLNativeUp|AWDLInterfaceBuild"
```

期望：`CFBundleVersion = 2.5.10`，`r2vhjBuildCheck = verified`，`r2vhjNativeUp = Yes`。

若 `r2vhjActivationBlocked` 有值，按 [DIAGNOSTICS.md](DIAGNOSTICS.md) 判读原因。

## 五、卸载与回滚

```bash
# 停用：把 config 里那条 Enabled 改为 false，或整条删除，然后重启
# 彻底移除：删掉 EFI/OC/Kexts/AirportItlwm.kext，并清掉 Kernel > Add 条目
```

回滚到上游原生 AirportItlwm：把原 `AirportItlwm.kext` 放回 `Kexts/`，恢复它的 `Kernel > Add` 条目，重启。

## 六、AirDrop 还需要蓝牙

AirDrop 先用蓝牙 LE 找到对端，再用 AWDL 传输。蓝牙侧不在本仓库内，至少需要：

- `BlueToolFixup.kext`（准入层，把第三方控制器伪装成 Apple 认可的形态）
- `IntelBluetoothFirmware.kext`（固件上传）

两者都由 Lilu 支撑，加载顺序：`Lilu` → `IntelBluetoothFirmware` → `BlueToolFixup`。

蓝牙出向 LE 建连在部分机型上会 `status=702`（控制器缺 resolving list），表现为 iPhone 热点凭据传不过去、部分接力配对失败；入向连接与广播不受影响。
