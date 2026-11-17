# 架构

## 一句话

AirDrop 走的是 AWDL：一条**只在 5 GHz 上、按 16 个时隙轮转**的点对点链路。Intel 网卡没有 Apple 那样的双射频，所以这条链路必须和普通 Wi-Fi **抢同一颗射频**。整个 AWDL 层就是把这场抢占规则化。

## 分层

```
macOS 原生 AWDL 栈
      │  请求：setAWDL_*、master indication、数据面 tx/rx
      ▼
ItlAWDLInterface : IO80211AWDLProtocol      ← AirportItlwm/AirportAWDLInterface.hpp
      │  帧组装：同步前缀、动作帧、数据帧、TLV
      ▼
include/AWDL*.hpp                            ← 纯逻辑：调度、探测、帧编解码、监管域
      │  发送：多槽并发 + 每对端 Block Ack
      ▼
itlwm/hal_iwx/ItlIwx.cpp                     ← 固件队列、信道切换、共存仲裁
      │
      ▼
Intel 网卡（802.11ax，x86_64）
```

`include/AWDL*.hpp` 是不碰内核对象的纯逻辑层（帧布局、时隙表、探测策略、TLV 解析），因此可以脱离内核单独测；`ItlAWDLInterface` 负责把这一层接进 Apple 的私有接口族。

## 控制面

Apple 的 AWDL 栈会通过 `IO80211AWDLProtocol` 下发请求：开启/关闭、设置 peer、报能力、要一份 master indication 拷贝。`ItlAWDLInterface` 的做法是：

- **只确认，不代劳**：多数请求直接回 `kIOReturnSuccess`，栈自己维持状态机。
- **能力上报要自己说真话**：不打开 `-itlawdlcaps` 时，Apple 栈会按单流 20 MHz 处理，对端也只按这个速率发。打开后驱动按 AX211 的真实能力报两流 + VHT，TLV 7 与之一致，吞吐才上得去。
- **master indication 需要额外确认**：它带一个固定偏移的裸指针，只在**经过校验**的构建上才允许拷贝，否则保持关闭。

## 数据面：为什么是"多槽"

AWDL 的一轮约 16 ms，被切成 **16 个 legacy 时隙**（`include/AWDLLocalSchedule.hpp`）。单个时隙只够发一帧，如果一帧一个时隙地发，速率被时隙开销吃光。

所以发送侧的做法是：**同一轮内对多个 peer、多个时隙并发提交**，每个 peer 一个固件 station 上下文、一套 Block Ack 窗口，队列非空就继续补位（逐槽 refill）。接收侧对应地把一个聚合窗口内的帧一次取回，再交给数据面。

数据帧承载 IPv6（AirDrop 的传输层），初始 MTU 1500 字节、不支持 jumbogram；载荷超过就分片。

## 射频仲裁：三条规则

这一层全部的复杂度来自一件事：**AWDL 想常驻私有信道 149，而系统随时可能要求全频段扫描或关联到某个 AP**。

1. **监听器只在无站态常驻**。没连 Wi-Fi 时，监听器守在 149，随到随收；一旦关联，射频必须跟着 AP 走。
2. **扫描优先，但要给回执**。系统扫描期间监听器让位，扫描完成后由定时器把被跳过的周期补回来。同步补会空转（每秒几百次），所以补 completion 走 500 ms 的节奏。
3. **关联是"交接"不是"重建"**。关联时监听器的 PHY 上下文要用 `MODIFY` 重绑到 station PHY，而不是 `remove + add`——后者在固件忙时返回 `EAGAIN`，表现为首次传输 30 秒超时、重传才通。断开 Wi-Fi 后自动回到私有信道。

另外还有一层代次（generation）校验：关闭再打开 Wi-Fi 会让固件侧上下文全部换新，旧代次的队列与监听器引用必须失效并自愈，否则 AirDrop 会一直"在线但传不动"。

## 构建守卫

AWDL 层直接子类化 Apple 私有类，并按虚表槽位调用。系统一升级，槽位一动就是内核崩溃。因此每一次启动都先做两道检查（`AirportItlwm/AWDLBuildGuard.cpp`）：

1. **系统构建在白名单里**：`kern.osversion` + 内核版本串双匹配。
2. **虚表指纹对得上**：按锚点槽位核对函数地址（Tahoe 568 槽、Sequoia 563、Sonoma 547），并检查纯虚函数位置与表尾。

两道都过才 `verified`；Sequoia 的 15.8.x 其他小版本允许 `expected`（同内核训练线，虚表仍要过）。任何一道不过，驱动退化成普通 AirportItlwm：Wi-Fi 正常，AWDL 对象一个都不创建。

`-itlawdlanybuild` 可以跳过第一道，**但永远不跳过第二道**。

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `AirportItlwm/AirportAWDLInterface.hpp` | `ItlAWDLInterface` 的接口与状态 |
| `AirportItlwm/AirportItlwmV2.cpp` | AWDL 接口的注册、请求应答、数据面接线 |
| `AirportItlwm/AirportItlwmSkywalkInterface.cpp` | 能力上报、包视图 |
| `AirportItlwm/AWDLBuildGuard.cpp` | 构建与虚表校验 |
| `include/AWDLLocalSchedule.hpp` | 16 时隙的本地调度表 |
| `include/AWDLPeerSchedule.hpp` | 对端时隙序列（TLV 18）解析与预测 |
| `include/AWDLDataFrame.hpp` / `AWDLDataPayload.hpp` | 数据帧封装与 IPv6 载荷 |
| `include/AWDLProbePolicy.hpp` | 探测帧的速率阶梯 |
| `itlwm/hal_iwx/ItlIwx.cpp` | 固件队列、信道切换、与 station 的共存 |
