# IntelBTPatcher 扫描停放修复 — 版本记录

## 2.8.0 / 2.8.1 已撤回 — 合并准入层破坏固件下载（2026-11-17 03:50）

### 一、症状

从 ESP 引导 2.8.1 后**蓝牙完全不启动**：`system_profiler` 显示
`Address: NULL / State: Off / Chipset: BCM_4350C2 / Vendor 0x004C / Firmware v0 c0`，
Wi-Fi 菜单里没有蓝牙开关可用项。

### 二、证据（`/usr/bin/log show`）

| 时间 | 事件 |
|---|---|
| 03:34:51 / 03:36:35 / 03:43:33 | `AppleUSBIORequest::complete: … endpoint 0x00: status 0xe00002d6 (timeout)` |
| 每次启动 | `enumerated 0x8087/0029` → `IntelBluetoothFirmware selected configuration 1` → 17~20s 后端点 0 超时 |
| 整个 boot | bluetoothd **从未 spawn BlueTool**（正常启动有 `BlueTool finished running boot script`） |

近 12 小时内该端点 0 超时**此前一次都没有**，全部出现在 2.8.x 上线之后。

### 三、结论

- 失败点在**内核态固件下载**（IntelBluetoothFirmware 走控制端点 0 的厂商请求），
  不是用户态准入层 —— BlueToolFixup 有没有装都影响不到这一层，所以"移除 BlueToolFixup"
  不是本次故障的原因。
- 2.8.0（准入层）与 2.8.1（移植 0.9.2）两个改动都在内核侧，**整体撤回**，不再逐个细究：
  25G332 上四条准入 pattern 离线扫描本来就 0 命中，合并收益为 0，风险却是整机蓝牙。
- 暖重启不恢复：Intel 控制器卡在固件半载状态时必须**关机断电冷启**。

### 四、现状

- 源码树 = 2.7.14（准入层与 0.9.2 移植整体摘除，`admissionHits`/`kAdmissionPatches`/
  第 13 组测试/相关注释全部回退），宿主测试全过，可重新构建。
- 2.8.x 完整代码保留在分支 `merge-admission-2.8.x`（HEAD `5931011`），未删除。
- ESP 上的 kext 回滚为 2.7.13（`9a67bca5`）+ BlueToolFixup 2.7.3（`84c1d427`），
  config 回滚到 `config.plist.bak-20261117-before-merge-btlfx`（27 条 Kernel>Add）。
  2.7.13 与 2.7.14 只差建连路径指纹遥测（只读），行为等价。

### 五、下一步（若还要合并）

一次只动一个变量、每次都要冷启验证：先在保持 BlueToolFixup 的前提下单独上准入层 →
再单独上 0.9.2 移植。禁止再把两件事塞进同一次部署。
## 2.7.15 — 只移植上游 0.9.2 的四处修复（不带准入层）（2026-11-17 03:58）

准入层撤回之后重新落一次 0.9.2：**代码与 2.8.1 完全一致，只是不再含任何准入层内容**
（`kAdmissionPatches`、`admissionHits`、页钩子里的准入循环、第 13 组测试都不在树上）。
也就是说 2.8.1 里值得留下的部分被单独拣了出来，出问题的那部分留在
`merge-admission-2.8.x` 分支里不再上车。

改动范围：`BTLEAdvHci.hpp`（DataLength 段重写）、`kern_start.cpp`（事件读包裹/完成副本）、
`test-scan-resume.cpp`（第 14 组 6 项）、`tools/build_btleadv.py`（版本号）。
宿主测试全过；构建产物 `04-构建产物/IntelBTPatcher-2.7.15-*.kext`。

### 一、上游改了什么

上游 `v0.6.0-beta.1`（2026-11-16 18:11 UTC）把 BTLEAdvFixup 从 0.9.1 提到 **0.9.2**，改动只在
LE Data Length Change 这一段（+243/−56），也就是我们继承的那段代码。四件事：

| # | 0.9.1 的缺陷 | 0.9.2 的修法 | 为什么对我们重要 |
|---|---|---|---|
| 1 | 只要 `outstanding` 非 0，**每一次中断读都被包住且无上限**；一轮配对没走完就让控制器事件全部被吞 → USB 设备无响应直到**冷启动**，或 panic（PR #61） | 每轮最多包 `kMaxWrappedReads = 24` 次，条目 **2 s 过期**，读不动就整轮放弃 | 我们这次一并带上 |
| 2 | `prepare()` 被拒仍照 `readBytes`/`writeBytes`，注释假设"Tahoe 已经 prepare 过" → `IOGMD: not wired for the IODMACommand` panic | 只有自己 prepare 成功才碰 buffer，否则放弃并计 `BTLEAdvLengthAborts`；上游实测 Tahoe 上 prepare **会成功** | 与我们 2.7.10 三连崩同一机制（那次在发命令路径） |
| 3 | 直接改调用方的 `IOUSBHostCompletion`（`completion->action/owner`），提交失败就留下指向已释放内存的指针 | 自建 `wrapped` 副本提交；提交返回错误即 `IOFree` 并原样返回 | 契约问题，滥用时是先崩后查 |
| 4 | 单个共享 stage：两条链路同时配对会串事件，fake 事件带错句柄（BT03） | 按连接句柄 8 槽跟踪、2 s 过期；槽满就不发副本 | 新 BLE 鼠标/键盘配对时会踩 |

附带：广告集移除后不再把地址留给下一个集（P-R07）、控制器对象更替时重置管道与待处理状态（BT02）。

### 二、我们多出的一处暴露（上游没有）

我们的 `patched_pipe_io` 包裹条件是 `lengthState.outstanding || scanResume.suspended` —— 停放打开且
没恢复时，同样是无上限接管事件管道。停放默认关，但 opt-in 就是雷。**本次一并治理**：停放走自己的
预算（最多 256 次读、1 s 窗口），用尽后由 `wantsScanResume` 里已有的 `stallGuard` 看门狗在下一
条主机命令时恢复扫描，不改变停放的语义。

### 三、代码

- `BTLEAdvHci.hpp`：`DataLengthState` 改为 8 槽 `DataLengthLink` 表 + `expired`/`aborts`/`wrapped`；
  新增 `findLink` / `expireLinks` / `abortLinks` / `dataLengthPending` / `wrapRead`；
  `extraRemoteReads` 取 `nowNs` 并按句柄登记，`forgetRemoteRead` 也按句柄
- `kern_start.cpp`：新增 `IOSimpleLock *lengthLock`（只在表本身上持有，绝不跨 USB 调用/分配/写注册表，
  且只在 `hciLock` 之后取）；`rewriteCompletion` 改成 prepare-honest + 失败即 abort；
  `patched_pipe_io` 改为自建 `wrapped` 副本提交；控制器对象更替时清管道与待处理表
- 新遥测：**`BTLEAdvLengthAborts`**（读不动而整轮放弃）、**`BTLEAdvLengthExpired`**（2 s 过期）
- 宿主测试第 14 组：句柄隔离、2 s 过期、包裹预算耗尽即放弃、槽满不发副本、陌生事件原样通过、
  reset 清表但保留计数器

### 四、部署

- ESP `IntelBTPatcher.kext` = **2.8.1**，md5 `9ae46cb4`
- `config.plist` 与 2.8.0 时**逐字节相同**（`cmp` 一致），未加任何 boot-arg
- 备份：`蓝牙/IntelBTPatcher-2.8.0-6074a435.kext`、`蓝牙/IntelBTPatcher-2.7.14-9ab61f3b.kext`

### 五、重启后判读

正常情况这两个新键**都不出现**。出现即说明踩到了原本会 panic 或吞事件管道的路径：
- `BTLEAdvLengthAborts > 0` 且蓝牙仍正常 = 新代码挡住了原来会崩的那条路
- `BTLEAdvLengthExpired > 0` = 有配对轮没等到重复完成事件（控制器只回了 Command Status），2 s 后自愈

## 2.8.0 — 合并准入层：BlueToolFixup 退役（2026-11-17 03:25）

### 一、为什么叫"合并"而不是"摘掉"

2.7.14 那一节已经查清三层分工：固件（IntelBluetoothFirmware）／准入（BlueToolFixup）／
能力+HCI（本仓库）。用户要求"两个 kext 合并掉"，落点只能是**把准入层搬进本仓库**，
然后 BlueToolFixup 才可以退役 —— 单纯摘掉会失去唯一的准入尝试。

### 二、搬了什么、没搬什么

| BlueToolFixup 的补丁 | 是否搬入 | 说明 |
|---|---|---|
| `kVendorCheck`（`cmp edx, 0x0a5c; je` → `jmp`） | ✅ | 跳过"必须是 Broadcom"的厂商检查 |
| `kBadChipsetCheck`（`cmp ecx, 1999; jb` → `jmp`） | ✅ | 让 chipset 0（第三方 dongle）合法 |
| 13.3 变体（`cmp ecx, 3998; ja` → 8×nop） | ✅ | 同一检查的倒置写法 |
| 15.4 变体（`cmp ecx, 5999; ja` → 8×nop） | ✅ | 同上 |
| `/usr/sbin/BlueTool` 的 SkipUpdate 路径改写 | ❌ | 只与 Broadcom 固件升级有关，Intel 控制器不用 BlueTool |
| `-btlfxboardid` / `-btlfxnvramcheck` / `-btlfxallowanyaddr` | ❌ | 上游默认关闭，从未启用 |

四条准入补丁**不带任何 build 相关地址**（只比厂商/芯片号），所以一套覆盖所有 macOS，
每次全试、不适用的自然不命中 —— 与 BlueToolFixup 的做法完全一致。它们**不受 `-btlenoscan` 影响**：
扫描补丁可以关，准入补丁不能，否则一句 boot-arg 会把"蓝牙能不能起来"一起关掉。

### 三、今天的行为变化 = 0（这是合并安全的前提）

静态复核 25G332 的 bluetoothd（x86_64 切片 11.9MB，逐页扫描）：

| pattern | 命中 |
|---|---|
| `81 fa 5c 0a 00 00 74`（vendor） | 0 |
| `81 f9 cf 07 00 00 72`（chipset <1999） | 0 |
| `81 f9 9e 0f 00 00 77 1a`（13.3） | 0 |
| `81 f9 6f 17 00 00 77 30`（15.4） | 0 |

而且 26 这一代已经没有"厂商必须是 Broadcom"的比较了（`cmp ?, 0xa5c` 各种寄存器编码全 0 命中），
chipset 检查改成了 `41 81 fe d0 07 00 00 72 3d`（`cmp r14d, 2000; jb`，0 天然落进第三方分支）。
**准入门槛在这代已经不存在**，所以 BlueToolFixup 今天同样一条都没打上 —— 合并前后行为逐条相同。

### 四、代码改动

- `BTLEAdvPatches.hpp`：新增 `struct AdmissionPatch` / `kAdmissionPatches`（4 条），与原有
  `kPatches`（四个能力站点）并列且互不重叠
- `kern_start.cpp`：页面钩子里第二段循环应用准入层；命中位图遥测
  **`BTLEAdvAdmissionHits`**（bit0 vendor / bit1 cs1999 / bit2 cs3998 / bit3 cs5999）
- 宿主测试第 13 组：四条 pattern 与 BrcmPatchRAM 源码逐字节对齐、id 不重复、
  `kPatches` 与 `kAdmissionPatches` 的 pattern 互不包含（否则一层会抹掉另一层的站点）、
  模拟落位后字节精确且不越界

### 五、部署

- ESP `IntelBTPatcher.kext` = **2.8.0**，md5 `6074a435`（`04-构建产物/IntelBTPatcher-2.8.0-合并准入层.kext`）
- ESP `config.plist`：`Kernel>Add` 27 → 26，移除 `BlueToolFixup.kext` 条目；
  boot-args 与其余所有段经 plistlib 逐键比对**完全一致**
- `BlueToolFixup.kext` 移出 ESP → `~/Downloads/ESP-Kexts-Backup/蓝牙/BlueToolFixup-2.7.3-advsets-84c1d427-已从ESP移除.kext`
- 回滚点：`~/Downloads/ESP-Kexts-Backup/config-backups/config.plist.bak-20261117-before-merge-btlfx`
  + `蓝牙/IntelBTPatcher-2.7.14-9ab61f3b.kext`

### 六、重启后判读

`BTLEAdvAdmissionHits` **不出现**（预期）= 26 上准入层不需要补丁，与 BlueToolFixup 结果一致；
若出现某个 bit，说明这代 bluetoothd 还留有该检查，且已被本仓库接住 —— 两种情况都不影响蓝牙本身。

2.8.0 同时包含 2.7.14 的 `BTLEAdvLeOps` / `BTLEAdvLeFailOp` 建连指纹（2.7.14 未单独验证即合并）。

## 2.7.14 — 建连路径指纹 + 三个蓝牙 kext 的层次核查（2026-11-17 03:15）

### 一、日志解锁：`log` 这个名字被占，必须用 `/usr/bin/log`

此前多轮"日志被屏蔽"是误判：zsh 里 `log` 解析到了别的东西（`too many arguments`），
`/usr/bin/log show` 一切正常。**今后取证优先用绝对路径**，不必再靠自建遥测硬猜。

### 二、热点断点的第一手证据（2026-11-17 03:09 实测，当前 U 盘 EFI 环境）

```
bluetoothd  MUC - Adding device "EF819D94-…" to connection filter accept list
bluetoothd  Address "Public 20:3C:AE:8E:35:08" Added to Filter Accept List with status:0
bluetoothd  Device connecting - {adv-addr: 20:3C:AE:8E:35:08-Public, resolved-addr: same}
bluetoothd  cancelling … (10.1s 后)
rapportd    ### Connect failed: kNetworkErr (BLE client connect timeout)
```

- 本机 BT 地址 = `50:E0:85:BB:A5:57`（不是目标地址），目标 `20:3C:AE:8E:35:08` 是手机的
  **配对身份地址**（`reconciled paired device`、`Loading keys from keychain`）。
- 建连方式是 **Filter Accept List + 身份地址**（不是把地址直接交给控制器），控制器必须把
  手机在空中发的 RPA 解析回这个身份地址才可能匹配。
- 每次 attempt 精确 **10.0–10.1s** 超时，与 rapport 的 10s BLE client connect timeout 完全吻合。
- 旁证：`Removing temporary IRK for device EF819D94…`、`MUC - enhancedFilterAcceptListSupported? 0`。
- 结论（待遥测确认）：**断点最可能在"控制器拿到/使用手机 IRK 做 RPA 解析"这一步**，
  而不是广播参数。widen/停放都不解决这个问题。

### 三、本版新增遥测（纯只读，不加任何 boot-arg）

| 键 | 含义 |
|---|---|
| `BTLEAdvLeOps` | 建连路径指纹位图：legacy/ext create、accept list 增删清、resolving list 增删清、ext adv/scan、随机地址、conn update |
| `BTLEAdvLeFailOp` / `BTLEAdvLeFailStatus` / `BTLEAdvLeFails` | 控制器拒绝的第一条 LE 命令及其状态码（用户态看不到） |

用途：一次 reboot 就能分清"地址直连 / accept list 匹配 / resolving list 解析"三种路径，
并看到控制器是否直接拒绝了 `0x2027`（LE Add Device To Resolving List）。
宿主测试新增第 12 组 34 项（覆盖 0x2038 vs 0x200b 混淆、位图唯一性）。

### 四、三个蓝牙 kext 是否冲突（用户质疑，已核实）

| kext | 角色 | hook 点 | 依赖 |
|---|---|---|---|
| IntelBluetoothFirmware 2.5.1 | **IOKit 驱动**，13 个 personality 绑 `IOUSBHostDevice`，上传固件 | 不 FunctionCast | IOUSBHostFamily |
| IntelBTPatcher 2.7.14（本仓库） | Lilu 插件 | `IOUSBHostDevice::deviceRequest`、`IOUSBHostPipe::io`、`_cs_validate_page`（改 bluetoothd 4 处） | Lilu |
| BlueToolFixup 2.7.3（advsets fork） | Lilu 插件 | `_cs_validate_page`（同一批 bluetoothd 站点） | Lilu |

**结论：三者不冲突**，且已实证——本次 boot 里 BlueToolFixup 2.7.3 与 ForbiddenItlwm
BTLEAdvFixup 0.8.2 **同时加载**（kextstat 68/69），蓝牙正常、`extended advertisingsets : enabled`。
两个 bluetoothd 补丁器只会"先到先得"，后到的 find 不再匹配，静默跳过。

**⚠️ 更正（03:20，用户质疑后查 acidanthera 源码）**：BlueToolFixup 的主业**不是** advsets。
`BrcmPatchRAM/BlueToolFixup.cpp` 无条件打的是**准入类补丁**，与我们的"能力类补丁"是两回事：

| 补丁 | 作用 | 本仓库是否也有 |
|---|---|---|
| `kVendorCheck`（cmp BRCM; je→jmp） | 跳过厂商检查 | ❌ |
| `kBadChipsetCheck`（cmp 1999; jb→jmp）+ 13.3/15.4 变体 | 让 chipset 0（THIRD_PARTY_DONGLE）被当成合法值，否则下次开机蓝牙起不来 | ❌ |
| `-btlfxboardid` / `-btlfxnvramcheck` / `-btlfxallowanyaddr` | 可选，默认关 | ❌ |

我们的 `BTLEAdvPatches.hpp` 只有四个**能力**站点（广告集/扩展扫描/随机地址/扩展建连），
不含任何准入补丁。**所以 BlueToolFixup 不可摘**：它是唯一在尝试"让 bluetoothd 接受非 Apple
控制器"的组件；一旦系统更新把检查恢复成老形态，缺它蓝牙会直接起不来。
它唯一与我们重叠的是 advsets 那部分，而那部分由 `-btlfxadvsets` 控制、当前**没开** → 今天零双打。

**静态复核（25G332）**：acidanthera 那四条准入 pattern 在当前 bluetoothd 里也**0 命中**
（寄存器与立即数都变了，真实站点形如 `41 81 fe d0 07 00 00 72 3d`）。"今天没命中"不等于
"可以删"——它仍是唯一的准入层保险。

**离线复核（用当前系统 25G332 的 bluetoothd x86_64 切片）**：4 个站点里 **2 个命中**
（adv-set 随机地址 0x51123e、扩展建连 0x510f60），广告集能力/扩展扫描两个站点 **0 命中**。
但 bluetoothd 日志显示这两项能力**不做补丁也是 enabled**（控制器本身支持），
所以那两处未命中不构成缺陷，属于"补丁没打，也不需要打"。

**IntelBluetoothFirmware 不可合并**：它是绑 USB 设备的 IOKit 驱动，必须早于蓝牙传输层 probe
且不依赖 Lilu；并进 Lilu 插件会让固件上传排队等 Lilu，破坏加载顺序。

### 五、部署

ESP `IntelBTPatcher.kext` = 2.7.14，md5 `9ab61f3b`；config 未改动（`cmp` 与备份一致），
本轮**不新增任何 boot-arg**。


## 2.7.13 — 自研热点第一步：Continuity 广播目击（只读）（2026-11-17 03:00）

**用户实测把范围钉死了**：手动输密码连接热点后**能连上并保持**。也就是说 Wi-Fi 侧
（关联、4-way、保持连接）全部正常，itlwm 无病；唯一断点在 **Mac ↔ iPhone 的 BLE 信息交换**。

**机制澄清（有公开逆向证据，推翻此前含糊说法）**：Instant Hotspot 的发现是**双向广播**，
而且 **Mac 是先手**：

| 帧 | 谁发 | 内容 | 时机 |
|---|---|---|---|
| `0x0D` Wi-Fi Settings / Tethering Target | **Mac** | 4 字节 iCloud ID（DSID 派生，24h 轮换） | 仅 Wi-Fi 菜单/设置页打开期间持续发 |
| `0x0E` Instant Hotspot / Tethering Source | **iPhone** | 电量、蜂窝制式、信号格（明文） | 扫到 0x0D 且 iCloud ID 匹配后才发 |

依据：PoPETs 2019《Handoff All Your Privacy》§4.1.7/4.1.8 —— 0x0D 是 “broadcast in order to
elicit a response from a device to be used as a hotspot”；0x0E 是 “will automatically begin
transmitting … when another device on the same iCloud account is nearby and is transmitting
Wi-Fi Settings messages containing the correct iCloud-derived ID”。

**这解释了 2.7.9 的现场**：停放关掉 Mac 的广播 → iPhone 收不到 0x0D → 不发 0x0E →
Wi-Fi 列表里的同 ID 热点入口**直接消失**。当时归因为"手机看不到电脑广播"，现在有了准确的
帧级解释。

**密码交换路径（原生栈，全程静默无 UI）**：点条目 → `sharingd` → `rapportd` 作 GATT Central
发起 LE Create Connection → `RPPairing` 用 iCloud Keychain 共享身份互验 → 加密通道内
`com.apple.hotspot.request` → iPhone 的 `TetheringAgent` 回 **SSID + PSK** → Mac 用该 PSK 走标准
802.11。**PSK 只在 BLE 加密通道里出现，Wi-Fi 层只负责消费它。**
而"输密码界面弹共享框"是另一条路（iOS 的 Wi-Fi 密码共享 UI，需人工确认），两条路不要混。

**自研边界因此收窄**：PSK 只能从手机拿、也不需要我们重写（`sharingd`/`rapportd` 是系统用户态），
驱动层唯一要保证的是 **`0x0D` 广播出得去 + LE 连接建得起来**。

**本版改动（纯只读，不改任何命令/事件字节）**：
- `BTLEAdvHci.hpp` 新增纯逻辑解析（可被宿主测试驱动）：
  `continuityTypes()`（AD 长度前缀遍历，识别 Apple 0x004C 的 Continuity type）、
  `advPayload()`（legacy `0x2008` 取偏移 3 / extended `0x2037` 取偏移 7）、
  `advReportItems()`（LE Meta `0x02` 逐条报告：event_type / 地址 / RSSI / Apple type）
- `kern_start.cpp` 出方向调 `noteAdvData()`、事件方向调 `noteAdvReport()`

**新增遥测键**：
- `BTLEAdvAdvDataTypes` — Mac 发出的广播里出现过的 Continuity type 位掩码（**bit13 = 0x0D 邀请帧**）
- `BTLEAdvAdvDataSets` — 带 Apple 数据的广播载荷次数
- `BTLEAdvInviteFrame` — 抓一份真实 0x0D 载荷原始字节（OSData）
- `BTLEAdvAdvReportAppleTypes` — 收到的广播里出现过的 Continuity type 位掩码（**bit14 = 0x0E**）
- `BTLEAdvAdvReportEventTypes` — 收到的广播 event_type 位掩码（bit0=ADV_IND 可连接，
  bit3=ADV_NONCONN_IND 不可连接）→ **直接回答"对方的 0x0E 到底可不可连"**
- `BTLEAdvTetherSources` — 收到 0x0E 的次数（**>0 才说明手机真的回了热点存在帧**）
- `BTLEAdvTetherRssi` / `BTLEAdvTetherAddr` — 最后一次 0x0E 的 RSSI 与地址
- `BTLEAdvAdvReports` — 广播报告总数（每 16 条更新一次，避免 IORegistry 抖动）

**宿主测试**：第 11 组，7 项断言全过（legacy 0x0D、extended 0x0E、非 Apple 公司 ID、
长度越界的截断载荷、非广播 opcode、双报告事件、错误子事件/半截缓冲）。

**部署**：ESP `IntelBTPatcher.kext` 2.7.13，md5 前 8 `9a67bca5`
（`04-构建产物/IntelBTPatcher-2.7.13-广播目击.kext`）。**config 未动**（见下条更正）。

**更正（03:02，用户指出）**：曾误把当前 NVRAM 里那串 `-itlawdlcoex*` 搬进 ESP config，理由是
"怕丢 AWDL 定稿参数"。**这是错的**：`ItlIwx.cpp:21403` 注释明确写着 *"The AWDL coex stack is on by
default (2026-11-15) … Each stage now has an opt-out flag instead … which made the EFI boot-args
unusably long"* —— coex 各阶段（含 barefill）**默认全开**，boot-arg 只保留 `-itlawdlcoexno*` 这类
**关闭**开关，旧的正向参数（`-itlawdlcoexprobe/-...rx/-...tx/-...multi/-...data/-...batx/
-...barefill`）**已不再被解析**。所以 ESP 的精简 boot-args 就是正确状态，往里塞一串失效参数纯属
噪声，也违反"别碰无关启动项"的原则。已从备份**逐字节恢复**（`cmp` 一致）。

本次 reboot **不加任何新 boot-arg**：2.7.13 的遥测默认即开，不需要开关。`-btleadvwiden` 留到
基线数据（Mac 到底发不发 0x0D、iPhone 回不回 0x0E、对方广播可不可连）出来之后再做单变量实验。

## 2.7.12 — 两项 HCI 干预全部改为 opt-in（默认 = 上游等价行为）（2026-11-17 02:55）

**定性更正（决定性，来自上游）**：上游 ForbiddenItlwm 的 `docs/ROADMAP.md` 把
**Instant Hotspot 明确列在 "Later"（尚未实现）**，与 Continuity Camera、统一蓝牙 kext 并列：

> **Later** — *Instant Hotspot: joining the iPhone's hotspot from the Wi-Fi menu without typing
> the password.*

BTLEAdvFixup 的存在目的是 AirDrop（AWDL），HCI hook 只是"照 IntelBTPatcher 的做法"。
**这意味着我们在 2.7.3→2.7.11 里追的热点故障，本身就是未实现功能的预期表现，不是我们引入的
回归**（唯一的真实回归是 2.7.10 的 panic，已在 2.7.11 修掉）。

**据此把默认行为收回上游等价**：
- 停放：早已 opt-in（`-btleadvpark`，默认关）
- **加宽：由默认开改为 opt-in（`-btleadvwiden`）** —— `widenCreateScan()` 会改写 create 命令里
  每个 PHY 的 Scan_Window。它从未在真机验证过（2.7.10 一上机就 panic，2.7.11 未及验证），
  而它作用于**所有** LE 连接（AirDrop BLE、Handoff、接力等），不该躺在默认路径上。
- 结果：2.7.12 默认 = 上游 0.9.1 行为（不改广播、不改扫描、不改 create），
  只保留我们真正需要的部分：改名后的身份 + 事件长度改写补丁 + **只读的目击遥测**。
- 遥测 `BTLEAdvParkEnabled` / `BTLEAdvWidenEnabled` 可直接确认两项干预是否被打开（默认应都为 0）。

**若将来要继续自研热点**（上游未做，属长期 R&D）：先 `-btleadvwiden` 验证加宽是否有任何效果，
判据是 `BTLEAdvConnOk` 首次 >0（从 2.7.3 至今从未出现过）。

## 2.7.11 — 修掉 2.7.10 的 panic（`writeBytes` 打在未布线的 USB 描述符上）（2026-11-17 02:50）

**2.7.10 不可用：开机约 1 分钟内连环三次内核 panic。** 用户被迫用 U 盘 EFI 启动。
三份 panic 报告（02:37:25 / 02:38:32 / 02:39:56）完全一致：

```
panic(cpu N caller ...): IOGMD: not wired for the IODMACommand @IOMemoryDescriptor.cpp:3537
Panicked task ...: pid 161: bluetoothd
Kernel Extensions in backtrace:
  com.apple.iokit.IOUSBHostFamily
  com.zxystd.IntelBTPatcher(2.7.10)
```

**根因**：2.7.10 为了把加宽后的扫描窗口送回控制器，对全路径做了写回：

```cpp
if (data) lilu_os_memcpy(data, hci, hciLength);
else if (descriptor) descriptor->writeBytes(0, hci, hciLength);   // ← panic
```

`IOMemoryDescriptor::writeBytes` 要求描述符先 `prepare()` 完成 DMA 布线；外发命令这条
路（`data == nullptr`，只有 descriptor）我们只在读取时 prepare/complete 过，写回时早已
`complete()`，于是 "not wired" 直接 kernel panic。触发者是 bluetoothd 的 HCI 命令线程。

> 对照：同一文件里事件改写路径（约 403 行）也调用 `writeBytes`，但它周围有
> `prepare(kIODirectionInOut)` / `complete()` 包裹，所以从早期版本起一直稳定。
> 区别不在 writeBytes 本身，而在**调用前有没有 prepare**。

**2.7.11 修复**：
1. **彻底删除 descriptor 写回**——任何情况下都不再对 USB 描述符调用 `writeBytes`。
2. 加宽后只写回调用者的**裸 `data` 指针**（我们本来就在读它，写回同样的 `hciLength`
   字节；不经过任何 IOKit 机制，无 panic 面）。
3. 当命令只带 descriptor（`data == nullptr`）时**放弃加宽**并计数
   `BTLEAdvWidenSkipped`——用遥测判断"这条路是否常态"，再决定要不要换方案，
   绝不再用会崩的写法硬上。
4. 遥测口径：`BTLEAdvWidened` 现在只统计**真正写回成功**的加宽，
   `BTLEAdvWidenSkipped` 统计因无 data 指针而放弃的次数。

**部署过程备注**：先回滚 ESP 到 2.7.9（`df18ed5c`）止血，再覆盖 2.7.11（`29bce615`）。
宿主测试 10 组全过（纯逻辑，无 IOKit 依赖，测不出 writeBytes 这类运行时问题——
这是本轮的教训：**凡是碰 IOKit 对象的改动，宿主测试给不出任何保证**）。

## 2.7.10 — 加宽 create 的发起扫描窗口（对准 10% 占空比）（2026-11-17 02:35）

**2.7.9 实测**：`BTLEAdvParkEnabled=0`、`Mirrors=0x01000100`（扩展扫描+扩展广播都亮）、
`ScanSuspends/Resumes` 键不再出现 → 我们确实不再干预无线电，**同 ID 热点入口恢复显示**
（用户确认）。但点它仍然转圈失败。

**这次抓到的真断点（sharingd 侧，不是 Wi-Fi）**：
```
02:27:31  SendRequestID 'com.sharing.hotspot.request' → rapport:rdid:DirectPeer
02:27:41  error: kNetworkErr (Connect failed) -> kNetworkErr (BLE client connect timeout)  (10s)
02:27:41  TetheringAgent: Enabling Hotspot device: iPhone 15 Pro - SkyOne
02:27:41  再发 → 02:27:54 又是 BLE client connect timeout
```
即：热点请求必须走一条 **rapport BLE 链路**，而 Mac 作为 Central 连不上手机。窗口内连
你的 15 Pro 试 4 次、连另一台 iPhone 试 6 次，**16 秒内全部 status 702 失败**，
本 boot **一次 LE Connection Complete 事件都没有**（遥测里 ConnCompletes 键不存在）。

**根因假设（有量化支撑）**：bluetoothd 自己的日志把发起扫描参数写得很清楚——
`Cancelling connections due to connection scan params: window - 0x30, interval - 0x1e0`
= 窗口 30ms / 间隔 300ms，**占空比 10%**。iPhone 的 Continuity 广播间隔量级为 1–2 秒，
6 秒的尝试窗口里大约只有 3 次广播机会，每次被 30ms 窗口盖住的概率 10%
→ 理论上限约 25%，与实测（一天 440 次失败 / 21 次成功 ≈ 5%）同量级。

**修复（只碰连接尝试本身，绝不碰主机的扫描/广播）**：
- 新增 `widenCreateScan(hci, length, &observed)`：把 create 命令里**每个 PHY 的
  Scan_Window 改成等于 Scan_Interval**（连续监听），连接参数（interval/latency/timeout）
  一个字节不动。支持扩展 create（0x2043，每 PHY 14 字节块）与 legacy（0x200d，载荷头部两字段）。
- kern_start 在命令发出前改写并**写回调用者的 data/descriptor**（原命令从这个缓冲区发出，
  我们的 `hci[32]` 只是副本）；日志侧新增遥测 `BTLEAdvCreateScanParams`
  （`interval<<16|window`，主机原始值）与 `BTLEAdvWidened`（改写次数）。
- 默认开启；`-btleadvnarrow` 可退回主机原值做 A/B。停放仍默认关闭（`-btleadvpark` 才开）。

**宿主测试**：新增第 10 组（扩展 create 两 PHY 都被加宽且连接参数/间隔不变、已是连续时不动、
legacy 形态、非 create 命令不动），连同前 9 组全部通过。

**部署**：md5 `18c8645c`（2.7.9 备份至 `蓝牙/IntelBTPatcher-2.7.9-停放默认关闭-df18ed5c.kext`）。

**重启后判读**：`BTLEAdvWidenEnabled=1`、`BTLEAdvCreateScanParams` 应显示原始
`0x01e0/0x0030`、`BTLEAdvWidened>0`；若连上，`BTLEAdvConnOk` 首次 >0
（此前它从未出现过）。若仍失败但 `Widened>0`，说明瓶颈不在占空比，下一步查
接受列表/RPA 解析（create 是 filter policy 1 + 对端地址全 0 的形态）。

## 2.7.9 — 停放默认关闭（停车机制是热点故障的原因，不是解药）（2026-11-17 02:25）

**用户实测（2.7.8 运行时，本条最重要）**：
1. 「你把广播停掉之后，同 ID 的热点已经看不到了」—— Wi-Fi 列表里同 Apple ID 的
   Instant Hotspot 入口消失；
2. 手机上手动开启个人热点后，SSID 出现在扫描列表，点击连接，**手机端不弹共享密码框**。

结合 2.7.8 遥测（`Mirrors=0x01000100`、`LastParkCommands=1`、`Suspends=17/Resumes=11`
即净泄漏 6 条使能命令），结论：**停放（尤其是把扩展广播停掉）直接摧毁 Instant Hotspot**。
手机只有在能看到电脑的 BLE 广播（"我要加入热点"意图）时才会回凭据/弹共享框；广播被我们
停掉且泄漏不恢复 → 入口消失、共享框不弹；手动输密码能连（不走 BLE）恰好反证 Wi-Fi 侧无病。

**同时确认停车机制从未有过正面效果**：2.7.3 opcode 错写（打空）→ 2.7.4 修对后遥测证明
触发过但连接照样失败 → 2.7.5 加广播停放 → 2.7.6 目击 → 2.7.7 全无停放（回归）→ 2.7.8
恢复停放，仍失败且新增"热点入口消失"。历史上没有任何一次数据表明停放让某个 create 成功。

**决策：摘下停放，回到干净基线。**
- `wantsPreScanOff(..., bool allowPark = true)`：`allowPark=false` 时该函数**退化为纯镜像**
  （照旧记录主机所有扫描/广播使能命令，但绝不合成任何禁用命令）。
- kern_start 新增 `parkScanning`，由 boot-arg **`-btleadvpark`** 开启，**默认关闭**
  （config.plist 中无此参数 → 关闭；无需改 config）。
- 新增遥测 `BTLEAdvParkEnabled`（0/1），重启后可直接确认当前是否在停放。
- 目击遥测（`LeCreates` / `CreateStatus` / `ConnStatus` / `CancelEvents` / `ConnOk`）
  全部保留且不依赖停放 → 关闭停放后仍能观测 create 的真实命运。

**宿主测试**：新增第 9 组（`allowPark=false` 不停放但镜像照旧跟随；同一 create 在
`allowPark=true` 时仍停放 2 条），连同原 8 组全部通过。

**部署**：md5 `df18ed5c`（2.7.8 备份至 `蓝牙/IntelBTPatcher-2.7.8-停放回归修复-023a6f6d.kext`）。

**重启后要验证的**：①同 ID 热点入口是否恢复出现在 Wi-Fi 列表；②点热点是否弹共享密码；
③`BTLEAdvParkEnabled` 应为 0、`ScanSuspends/Resumes` 应完全不再增长（键可能不再出现）。

## 2.7.8 — 停放回归修复 + 目击解除停放门控（2026-11-17 02:10）

**2.7.7 实测结果：完全没停放**（`LeCreates=76` 但 `BTLEAdvScanSuspends/Resumes` 键连影子都没有）。
根因：2.7.7 重构 kern_start 命令路径时，把「非 create 命令也调 `wantsPreScanOff` 以镜像主机
使能状态」的分支丢了 —— 而镜像更新只发生在这个函数里，于是 hciState 的扫描/广播镜像永远
是 0，create 时 `wantsPreScanOff` 认为"没有东西可停"→ 一次都不停放 → Intel 并发 bug 直接命中。

**旁证**：`rapportd` 日志显示手机能被发现（`Hotspot 0x15D`，RSSI -45），bluetoothd 每次
`Connecting(1) → Cancelling(2)` 精确 5.1 秒 —— 就是 create 起不来被 6s 定时器取消的形态。

**修复**：
1. 恢复非 create 命令路径的 `wantsPreScanOff` 调用（镜像更新 + 主机重申时清 parked 标志）；
   若非 create 命令意外产生停放，计入新遥测 `BTLEAdvUnexpectedParks`（正常应恒 0）。
2. 目击遥测解除 `scanResume.suspended` 门控 —— 停没停放都能看到控制器对 create 的裁决
   （2.7.7 就是因为在未停放时完全无目击而"盲飞"）。
3. 新增 `BTLEAdvMirrors`（bit0 legacy scan / bit8 ext scan / bit16 legacy adv / bit24 ext adv）
   与 `BTLEAdvLastParkCommands`：镜像与无线电是否一致可直接看，不用再推断。

md5 `023a6f6d`，已部署 ESP（2026-11-17 02:12），需重启生效。

## 2.7.7 — 停放泄漏三重修复（2026-11-16）

**实证根因**（本机遥测 + 用户复现）：
- 遥测 `BTLEAdvScanSuspends=5 / BTLEAdvScanResumes=3`：有停放从未恢复。
- 用户复现：忘记网络后由手机共享热点密码 → 手机端永久转圈（无失败弹窗）；
  点一下其他 SSID 触发新的加入动作 → 共享立刻成功。
- 期间 `BTLEAdvLeCreates` 保持 8 不变：Mac 侧全程没有发起 LE 连接——
  共享方向是手机当 Central 连电脑，电脑只靠 BLE 广播被"看见"。
- 结论：停放把电脑的 BLE 广播/扫描留在关闭状态 → 手机够不着电脑 → 转圈；
  点其他 SSID 使 bluetoothd 重发广播/扫描使能命令（透传）→ 无线电复活 → 成功。

**修复三处**：
1. **恢复触发面补全**（BTLEAdvHci.hpp `onScanResumeEvent`）：
   - Command Status (0x0f)：opcode==停放中的 create 且 status≠0 → 尝试结束；
   - Command Complete (0x0e)：同上且 status≠0 → 结束；
   - LE Create Connection Cancel (0x200e) 的 0x0f/0x0e（任意 status）→ 结束
     （bluetoothd 6 秒超时后必发 Cancel，这是 eaten-create 的常规出路）；
   - 原有 LE meta 0x01/0x0a/0x0b、Disconnection 0x05 不变。
2. **看门狗兜底**（`kResumeStallCommands=128`）：停放期间每过一条主机命令计一次数，
   到 128 强制恢复（`BTLEAdvResumeWatchdog` 遥测）。正常路径 6 秒内由 Cancel 先恢复，
   看门狗只兜"控制器吞掉一切事件"的极端情况。
3. **镜像回写 bug（深层，v2.7.4 起就存在）**：kern_start.cpp 在 `wantsScanResume`
   清空 parked 标志**之后**才读标志回写 hciState 镜像 → 回写永不生效 → 每次恢复后
   kext 以为扫描/广播都关着（实际无线电在跑）→ 下一个 create 不再停放 → 固件
   并发 bug 直接命中 → 状态永久脱同步。现改为调用前先捕获标志。

**新遥测键**：`BTLEAdvCreateStatus`（0x0f 对 create 的拒绝码）、
`BTLEAdvCreateStatusEvents`、`BTLEAdvCancelEvents`、`BTLEAdvResumeWatchdog`。

**宿主测试**：`BTLEAdvFixup/test-scan-resume.cpp`（clang++ 纯逻辑，8 组用例全过，
含 0x0f/0x0e/取消/LE-meta/看门狗/主机重申/legacy 回归）。

**部署**：ESP `IntelBTPatcher.kext` 2.7.7，md5 前8 `00111bf7`
（构建产物 `04-构建产物/IntelBTPatcher-2.7.7-扫描停放修复.kext`）。

## 2.7.6 — HCI 目击诊断（未上车即被 2.7.7 取代诊断角色，修复逻辑并入）
KMOD 版本/目击遥测保留（BTLEAdvCreateOpcode/FilterPolicy/PeerAddr/CmdStatus/ConnStatus）。

## 2.7.4 / 2.7.5 — 双扫描停放 / 广播停放
opcode 0x2043 修正（此前 0x203A 打空）、ext 广播逐字回放。遗留：恢复只认 LE meta
事件 + 镜像回写 bug → 本版修复。
