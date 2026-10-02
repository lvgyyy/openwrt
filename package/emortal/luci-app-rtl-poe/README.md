# luci-app-rtl-poe — RTL8238B PoE PSE LuCI 应用

OpenWrt LuCI 界面和后台守护进程，用于控制 RTL8238B PoE PSE 控制器。

## 硬件架构

```
Host CPU (MT7987B) → I2C @ 0x20 → 外部MCU (GD32/Nuvoton) → RTL8238B PoE Chip
                              ↕ Host Command Protocol (12-byte packets)
```

RTL8238B 通过 **外部 MCU**（如 GD32F310）接收 Host Command 协议包，而非直接 I2C 寄存器读写。

## 端口映射（基于原厂固件 BXOS/ WNOS DTB 分析）

| RTL8238B 通道 | 物理端口 | PoE | 说明 |
|:---:|----|:---:|------|
| 0 | LAN1 | ✅ | PoE供电 |
| 1 | LAN2 | ✅ | PoE供电 |
| 2 | LAN3 | ✅ | PoE供电 |
| 3 | LAN4 | ✅ | PoE供电 |
| 4 | LAN5 | ✅ | PoE供电 |
| 5 | LAN6 | ✅ | PoE供电 |
| 6 | LAN7 | ✅ | PoE供电 |
| 7 | — | ❌ | 未使用（无物理端口） |

> 设备基于 MT7987B + RTL8373N 交换机架构。WAN 口通过独立 Airoha PHY 连接 GMAC0，不经过 PoE 控制器。

## 通信协议（Host Command Protocol v1.7）

所有通信通过 **12 字节 Host Command 包** 在 I2C 总线上进行：

```
请求（顺序写）: [CMD(1B) | SEQ(1B) | DATA[9B] | CHKSUM(1B)]
响应（顺序读）: [CMD(1B) | SEQ(1B) | DATA[9B] | CHKSUM(1B)]
```

- **I2C 地址**: 7-bit 0x20（固定）
- **最大速率**: 1MHz
- **请求→响应延迟**: ≥25ms（CMD 0x00/0x05 需 ≥1s）
- **校验**: 累加和（Byte0~Byte10 相加 mod 256）
- **RSVD 字节**: 填充 0xFF

### 使用的 Host Commands

| 命令 | 名称 | 功能 |
|:---:|------|------|
| 0x00 | Global Enable Set | 全局使能/禁用所有端口 |
| 0x01 | Port Enable Set | 单端口使能/禁用 |
| 0x04 | Global Power Source Set | 设置电源预算 |
| 0x10 | Global PM Mode Set | 设置功率管理模式 |
| 0x40 | Global Status Get | 获取系统状态（设备ID、固件版本等） |
| 0x41 | Global Power Status Get | 获取功率状态（预算/消耗） |
| 0x42 | Port Status Get | 获取端口详细状态 |
| 0x44 | Port Measurement Get | 获取端口电气测量值 |

### 端口状态字段 (CMD 0x42 响应)

- **STS1**: 电源状态（0=Disabled, 1=Searching, 2=Delivering, 4=Fault, 6=Requesting）
- **STS2**: 检测/分级结果；故障时为故障码
- **STS3**: PD 分级结果（0-8 对应 Class 0-8）

### 测量值单位 (CMD 0x44 响应)

| 参数 | 单位 | 范围 |
|------|------|------|
| 端口电压 | 64.45mV/LSB | 0-255 → 0-16.4V |
| 端口电流 | 1mA/LSB | 0-255 → 0-255mA |
| 温度 | 公式: °C = (raw - 120)×(-1.25) + 125 | ~-25°C ~ +275°C |
| 功耗 | 0.1W/LSB | 0-65535 → 0-6553.5W |

## 文件结构

```
luci-app-rtl-poe/
├── Makefile                          OpenWrt 包构建
├── README.md                         本文档
├── htdocs/luci-static/resources/
│   ├── rtl-poe/overview.css          LuCI 样式
│   └── view/rtl-poe/overview.js      LuCI JS 页面
├── po/zh_Hans/rtl-poe.po            简体中文翻译
├── root/etc/
│   ├── config/rtl-poe                UCI 默认配置
│   └── init.d/rtl-poe               procd init 脚本
└── src/
    ├── Makefile                      编译规则
    └── rtl-poe.c                     核心 C 守护进程
```

## 安装

```bash
# 复制到 OpenWrt 源码树
cp -r luci-app-rtl-poe package/feeds/luci/

# 编译
make package/luci-app-rtl-poe/compile V=s

# 安装
opkg install luci-app-rtl-poe*.ipk
```

## 配置（/etc/config/rtl-poe）

```ini
config poe 'main'
    option enabled '1'        # 1=全局使能, 0=禁用
    option budget_mw '60000'  # 电源预算 (mW), 最大 120000
    option debug '0'          # 调试日志
    option port_lan1 '1'      # 端口使能 (1=使能, 0=禁用)
    option port_lan2 '1'
    # ... port_lan3 ~ port_lan8
```

## 命令行用法

```bash
rtl-poe run              # 守护进程模式
rtl-poe status           # JSON 状态输出
rtl-poe status --debug   # 含调试信息的 JSON
rtl-poe clear-log        # 清空日志
rtl-poe log              # 查看最近 200 行日志
rtl-poe version          # 版本
```

## JSON 状态格式

```json
{
  "controller": "RTL8238B",
  "device_id": "0x0138",
  "fw_version": "1.0",
  "mcu_type": 0,
  "config_saved": true,
  "max_ports": 8,
  "poe_ports": 7,
  "budget_mw": 120000,
  "allocated_mw": 35000,
  "available_mw": 85000,
  "consumed_mw": 28700,
  "ports": [
    {
      "port": 1,
      "label": "LAN1",
      "requested": true,
      "connected": true,
      "powered": true,
      "power_good": true,
      "state": "Delivering",
      "class": 4,
      "protocol": "802.3at (30W)",
      "voltage_mv": 53400,
      "current_ma": 120,
      "power_mw": 6408,
      "temperature_c": 45,
      "mode": "auto"
    }
  ]
}
```

## 依赖

- OpenWrt 24.10+ (或 beta 版本)
- LuCI (ucode template)
- I2C 内核驱动 (`CONFIG_I2C=y`, `CONFIG_I2C_CHARDEV=y`)
- 外部 MCU 已加载 App 固件

## 参考资料

- RTL8238B Host Command Guide Rev. 1.7 (2023-03-16), Realtek Semiconductor Corp.
- WNOS.XS.1.1.7 / BXOS.H.1.0.23 内核模块逆向分析
- MT7987B + RTL8373N 参考设计