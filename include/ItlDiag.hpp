// 诊断属性闸门。
//
// 本项目在调优期往 IORegistry 写了大量 Aw*/Itlwm*/Ibtp* 属性（AWDL 共存、队列、
// 信道迁移、接收统计等）。这些属性只写不读，纯属取证用；发行版不应携带。
//
// ITL_TELEMETRY 默认 0：任何未显式打开它的构建（含 xcodebuild -configuration Release）
// 都不再写任何诊断属性。需要取证时在命令行加 ITL_TELEMETRY=1，或用 Debug 配置。
#ifndef ITL_TELEMETRY
#define ITL_TELEMETRY 0
#endif

#if ITL_TELEMETRY
// entry 是任意 IORegistryEntry（root、this、controller 均可）。
#define ITL_DIAG_SET(entry, ...) ((entry)->setProperty(__VA_ARGS__))
#else
// 编译成空语句：参数表达式不求值，因此不会产生 OSData/OSNumber 分配。
#define ITL_DIAG_SET(entry, ...) ((void)0)
#endif
