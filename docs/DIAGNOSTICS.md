# 诊断

## 一、先看三个数

```bash
ioreg -rn AirportItlwm | grep -E "AWDLBuildCheck|AWDLActivationBlocked|AWDLNativeUp"
```

| 输出 | 含义 | 下一步 |
| --- | --- | --- |
| `AWDLBuildCheck = verified` | 系统与虚表都通过 | 正常 |
| `AWDLBuildCheck = blocked: unverified macOS build` | 系统不在白名单 | 见下文「系统不匹配」 |
| `AWDLBuildCheck = blocked: IO80211AWDLProtocol table differs` | 虚表指纹对不上（系统已改动 Apple 私有类） | 不要强开，等适配 |
| `AWDLActivationBlocked = <原因>` | 有代码路径主动挡住 | 按原因逐条看 |
| `AWDLNativeUp = Yes` | AWDL 接口已起来 | 正常 |

## 二、遥测属性判读

`ioreg -rn AirportItlwm` 会列出几百个 `r2vhj*` 属性。真正需要的只有下面这些：

| 属性 | 正常值 | 异常含义 |
| --- | --- | --- |
| `r2vhjBuildCheck` | `verified` | 见上表 |
| `r2vhjInterfaceBuild` | `airportitlwm` | 说明跑的是哪个构建 |
| `r2vhjNativeUp` | `Yes` | `No`：接口没起来，看 `AWDLActivationBlocked` |
| `r2vhjMiState` | `on`（验证构建） | `blocked: build not verified`：master indication 拷贝被挡，属预期 |
| `r2vhjFactoryStage` / `r2vhjNetworkStage` | `done` | 停在中间某步说明创建接口失败，值就是失败点 |
| `r2vhjTxAccepted` / `r2vhjTxCompleted` | 两者同步增长 | `Accepted` 涨、`Completed` 不涨 = 固件没回完成 |
| `r2vhjTxHeldStashed` / `HeldReleased` | 前者 ≥ 后者 | 差值长期不减 = 帧被扣住（射频被扫描或 station 占用） |
| `r2vhjDataCopySuccess` / `r2vhjDataDelivered` | 同步增长 | `CopySuccess` 涨、`Delivered` 不涨 = 数据面拷回来了但没交付 |
| `r2vhjPeerTLVs` | 非空 | 空 = 还没收到对端广播 |
| `r2vhjSyncMIBuilt` / `SyncMIWanted` | `Wanted` 为 `Yes` 时 `Built` 应为 `Yes` | `Built=No` = master indication 未生成 |
| `r2vhjManagementSubmissions` / `ManagementLastError` | `LastError = 0` | 非 0 = 管理帧提交失败，值即 IOReturn |
| `ItlwmLinkQuality` | 有值 | 空说明链路还没建立 |

## 三、日志

```bash
log show --last 5m --predicate 'process == "kernel"' | grep -i "AirportItlwm"
```

启动时会打印一行：

```
AirportItlwm: AirDrop verified
```

其余 AWDL 日志按需打开：`-itlawdlpmdebug`（电源/唤醒路径）、`-itlawdlsamples`（原始采样）。

## 四、按症状查

**看不到 AirDrop 图标**
1. `r2vhjNativeUp` 是不是 `Yes`；不是则看 `AWDLActivationBlocked`。
2. 蓝牙有没有起来：AirDrop 先用蓝牙 LE 发现对端。`BlueToolFixup` + `IntelBluetoothFirmware` 缺一不可。
3. 对方设备是否在同一 Apple ID / 允许"所有人"。

**能看到对端但传不动（30 秒超时）**
- 典型症状是"第一次超时、重传就通"。这是关联交接没做对的旧版本特征；2.5.10 已改为 `MODIFY` 重绑，若仍出现，看 `r2vhjTxHeldStashed` 与 `HeldReleased` 的差值是否卡住。
- 也可能是射频被 station 占着：断开 Wi-Fi 再试一次，能通说明是共存仲裁问题。

**传输中速率周期性抖动**
- 20 秒左右一次，持续很短：这是扫描放行的硬上限兜底，属已知限制，不是故障。

**扫描面板空白**
- 说明监听器没有让位给扫描。检查是否在无站态常驻 149；2.5.x 的仲裁双判据（帧速率 / BA 队列）负责这件事，`r2vhjTxAccepted` 在无流量时应停止增长。

**关掉 Wi-Fi 再打开后 AirDrop 失效**
- 代次自愈没生效。看 `r2vhjFactoryStage` 是否回到 `done`；若停在旧阶段，重启可临时恢复，并记录当时的系统构建号。

## 五、可以用的启动参数

正式使用**一个都不需要**。以下仅用于定位问题，除前两个外都不要长期开。

| 参数 | 作用 |
| --- | --- |
| `-itlawdlanybuild` | 跳过系统白名单（**不跳虚表校验**）。仅在确认系统是同一内核训练线时使用 |
| `-itlawdlnomitx` | 关闭 master indication 拷贝，排除该路径导致的问题 |
| `-itlawdlnocaps` | 不上报两流/VHT，退回单流 20 MHz |
| `-itlawdlcapshtonly` | 只上报 HT，不上报 VHT |
| `-itlawdlnoiface` | 不创建 AWDL 接口，验证"纯 Wi-Fi"是否正常 |
| `-itlawdlnonativeup` | 不置 native up 标记 |
| `-itlawdlhideinfra` | AWDL 期间隐藏基础设施接口 |
| `-itlawdlpmdebug` | 电源/唤醒路径详细日志 |
| `-itlawdlmsipoll` | 轮询 master indication 状态 |
| `-itlawdlsamples` | 输出原始采样 |
| `-itlawdldatacopy` / `-itlawdldatarx` | 数据面拷贝 / 接收路径单独打开 |
| `-itlawdlcoexnodata` | 共存栈不接管数据路径 |
| `-itlawdlpoolprobe` | 探测固件包池类型 |
| `-itlawdlnoelicit` | 不主动诱发对端广播 |
| `-itlmccprobe` | 监管域（国家码）探测 |
| `-noht40` / `-novht` | 关 40 MHz / 关 VHT，用于排除速率相关故障 |

## 六、抓一份可用的现场

```bash
{
  sw_vers
  sysctl -n kern.osversion
  uname -a
  ioreg -rn AirportItlwm | grep -E "Itlwm(AWDL|LinkQuality)"
  kmutil showloaded --list-only | grep -iE "airportitlwm|lilu|bluetooth"
  log show --last 10m --predicate 'process == "kernel"' | grep -i AirportItlwm
} > ~/Desktop/airportitlwm-diag.txt
```

这五段能覆盖绝大部分问题定位。
