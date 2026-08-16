# SDRAM Driver

## W9812G2KB-6I 接入 RA8P1

- 基地址: `0x68000000`
- 大小: 64MB (`0x04000000`)
- CPU0 分配: `0x68000000 - 0x6BFFFFFF` (64MB)
- CPU1 分配: `0x6C000000 - 0x6FFFFFFF` (64MB)

## e2studio 配置步骤

1. **BSP → SDRAM Support**: 设为 `Enabled`
2. **SDRAM 时序参数**: 匹配 W9812G2KB-6I 数据手册
3. **链接脚本**: 确保 CPU0 的 `.sdram` section 指向 `0x68000000`

## 调用方式

```c
extern int sdram_test_run(void);
int result = sdram_test_run();  // 0=成功, -1=失败
```

## CPU1 能否访问 SDRAM？

**能。** RA8P1 的 SDRAM 控制器挂载在系统总线上，两个核心都可以访问。CPU0 负责初始化 SDRAM 控制器，初始化完成后 CPU1 也可以通过 `0x68000000` 地址直接读写 SDRAM。

这意味着 SDRAM 可以作为双核之间的**大容量共享内存**（128MB），相比内部 SRAM（64KB）大幅提升数据传输能力。

## 测试步骤

1. 先用 CPU0 完成 SDRAM 读写测试
2. 验证通过后，测试 CPU1 是否能读写同一 SDRAM 区域
3. 最终目标：利用 SDRAM 作为双核共享内存 + RPMsg 通知机制
