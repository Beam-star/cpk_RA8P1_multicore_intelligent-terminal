# Ethernet 驱动说明

基于 FreeRTOS+TCP 的以太网驱动封装，适用于 RA8P1 双核工程 CPU1 端。

硬件链路：RA8P1 内置 RMAC (MAC) + 板载 PHY 芯片 -> RGMII 接口 -> RJ45 网口

**支持的协议：** TCP + UDP（FreeRTOS+TCP 原生支持，UDP 无需额外配置宏）

---

## 文件结构

| 文件 | 说明 |
|------|------|
| `ethernet.h` / `ethernet.c` | 网络驱动层：初始化、状态查询、IP 管理、ping、**UDP socket 辅助函数** |
| `ethernet_test.h` / `ethernet_test.c` | 测试代码：ping + 网速测试 + TCP echo + **UDP echo + UDP 图传接收** |
| `drv_rtl8211.c` | RTL8211F PHY 初始化钩子（LED 配置、链路协商） |
| `ethernet_speed_recv.py` | PC 端 TCP 网速测试接收脚本 |
| `udp_screen_sender_1024x600.py` | PC 端 UDP 图传脚本（1024x600 RGB565，直接送 LCD 显示） |

---

## IP 模式切换

在 `ethernet.h` 中通过宏 `ETH_USE_DHCP` 控制：

**静态 IP 模式（默认）：** 不定义 `ETH_USE_DHCP`，直接使用以下地址：

```c
#define ETH_STATIC_IP          "192.168.1.100"
#define ETH_STATIC_NETMASK     "255.255.255.0"
#define ETH_STATIC_GATEWAY     "192.168.1.1"
#define ETH_STATIC_DNS         "8.8.8.8"
```

修改这些宏即可适配你的网络环境。

**DHCP 模式：** 在 `ethernet.h` 中取消注释或定义：

```c
#define ETH_USE_DHCP
```

启用后设备启动时会自动向 DHCP 服务器请求 IP 地址，上述静态地址将作为 DHCP 失败时的回退地址。

---

## API 使用说明

### 1. `ethernet_init()` - 初始化网络栈

```c
int ethernet_init(void);
```

**功能：** 初始化整个以太网协议栈，调用后会依次完成：
1. `FreeRTOS_IPInit()` -> 注册网络接口
2. `R_RMAC_Open()` -> 初始化 RMAC 硬件
3. `R_LAYER3_SWITCH_Open()` -> 初始化以太网交换机、PHY 芯片
4. 自动创建 RX 接收任务和链路状态监测任务

**返回值：** `0` 成功，负值失败

**使用示例：**

```c
void cpu1main_thread_entry(void *pvParameters) {
    // ... 其他初始化 ...

    if (ethernet_init() != 0) {
        // 初始化失败处理
        return;
    }
}
```

**注意：** 必须在 FreeRTOS 任务中调用，且只能调用一次。调用后硬件开始自协商，但网络尚未就绪，需要等待链路建立。

---

### 2. `ethernet_wait_ready()` - 等待网络就绪

```c
int ethernet_wait_ready(uint32_t timeout_ms);
```

**功能：** 阻塞等待直到以太网物理链路建立且 IP 协议栈就绪（即可以进行 socket 通信）。

**参数：**
- `timeout_ms`：最大等待时间（毫秒）。传 `0` 表示无限等待。

**返回值：** `0` 网络已就绪，负值超时

---

### 3. `ethernet_get_status()` - 查询网络状态

```c
eth_status_t ethernet_get_status(void);
```

**返回值：**

| 值 | 含义 |
|----|------|
| `ETH_STATUS_LINK_DOWN` | 物理链路断开（网线未插或 PHY 异常） |
| `ETH_STATUS_LINK_UP` | 物理链路已建立，但 IP 协议栈尚未就绪 |
| `ETH_STATUS_IP_READY` | 网络完全就绪，可以通信 |

---

### 4. `ethernet_get_ip()` / `ethernet_get_netmask()` / `ethernet_get_gateway()` - 查询网络参数

```c
int ethernet_get_ip(char *buf, int size);
int ethernet_get_netmask(char *buf, int size);
int ethernet_get_gateway(char *buf, int size);
```

**参数：**
- `buf`：输出缓冲区，建议至少 16 字节
- `size`：缓冲区大小

---

### 5. `ethernet_is_dhcp()` - 查询是否使用 DHCP

```c
bool ethernet_is_dhcp(void);
```

**返回值：** `true` 使用 DHCP 获取的地址，`false` 使用静态地址

---

### 6. `ethernet_ping()` - 发送 ICMP Ping 请求

```c
int ethernet_ping(const char *target_ip);
```

**参数：**
- `target_ip`：目标 IP 地址字符串，如 `"192.168.1.10"`

**返回值：** `0` 发送成功，负值发送失败

---

## UDP Socket 辅助函数

FreeRTOS+TCP 原生支持 UDP，通过标准 BSD socket API 即可使用。以下是对常用操作的简单封装。

### 6. `ethernet_udp_create()` - 创建 UDP Socket

```c
Socket_t ethernet_udp_create(void);
```

**功能：** 创建一个 UDP socket（`SOCK_DGRAM`），等价于：

```c
FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP)
```

**返回值：** socket 句柄，失败返回 `FREERTOS_INVALID_SOCKET`

---

### 7. `ethernet_udp_bind()` - 绑定本地端口

```c
int ethernet_udp_bind(Socket_t sock, uint16_t port);
```

**功能：** 将 UDP socket 绑定到指定本地端口，接收该端口上的所有 UDP 数据。

**参数：**

- `sock`：socket 句柄
- `port`：本地端口号（主机字节序）

---

### 8. `ethernet_udp_sendto()` - 发送 UDP 数据

```c
int ethernet_udp_sendto(Socket_t sock, const void *data, uint32_t len,
                        const char *dest_ip, uint16_t dest_port);
```

**功能：** 向指定 IP:port 发送一个 UDP 数据报。

**参数：**

- `data`：发送缓冲区
- `len`：数据长度
- `dest_ip`：目标 IP 字符串，如 `"192.168.1.10"`
- `dest_port`：目标端口（主机字节序）

**返回值：** 实际发送的字节数，负值表示失败

---

### 9. `ethernet_udp_recvfrom()` - 接收 UDP 数据

```c
int ethernet_udp_recvfrom(Socket_t sock, void *buf, uint32_t buf_len,
                          uint32_t timeout_ms,
                          char *src_ip, int src_ip_len, uint16_t *src_port);
```

**功能：** 阻塞接收一个 UDP 数据报，可设置超时。

**参数：**

- `buf` / `buf_len`：接收缓冲区
- `timeout_ms`：超时时间（毫秒），`0` 表示永久等待
- `src_ip` / `src_ip_len`：发送方 IP 输出（可为 NULL）
- `src_port`：发送方端口输出（可为 NULL）

**返回值：** 接收字节数，`0` 超时，负值错误

---

### 10. `ethernet_udp_close()` - 关闭 UDP Socket

```c
void ethernet_udp_close(Socket_t sock);
```

---

## 测试代码 API

### 11. `ethernet_test_ping()` - 批量 Ping 测试

```c
void ethernet_test_ping(void);
```

**功能：** 向 `ETH_TEST_PING_TARGET` 发送 `ETH_TEST_PING_COUNT` 次 ping 请求。

**建议：** 在 TCP echo 服务器启动之前调用，避免缓冲区竞争导致 ping 失败。

**配置宏（在 `ethernet_test.h` 中）：**

```c
#define ETH_TEST_PING_TARGET    "192.168.1.10"   // ping 目标 IP (PC)
#define ETH_TEST_PING_COUNT     10               // ping 次数
#define ETH_TEST_PING_INTERVAL  2000             // 间隔 (ms)
```

---

### 8. `ethernet_test_speed()` - 网速测试

```c
int ethernet_test_speed(void);
```

**功能：** TCP 吞吐量测试。设备主动连接 PC 端接收脚本，发送指定大小的数据块，测量传输时间和吞吐量。

**使用步骤：**

1. 在 PC 端先运行接收脚本：

```bash
python ethernet_speed_recv.py 5001
```

2. 然后在设备端调用 `ethernet_test_speed()`，设备会自动连接 PC 并发送数据。

3. PC 脚本会显示接收结果和吞吐量。

**配置宏（在 `ethernet_test.h` 中）：**

```c
#define ETH_TEST_SPEED_SERVER   "192.168.1.10"   // PC 端 IP
#define ETH_TEST_SPEED_PORT     5001             // PC 端接收端口
#define ETH_TEST_SPEED_SIZE     (64 * 1024)      // 发送数据量 (64KB)
```

---

### 9. `ethernet_test_tcp_echo_start()` - 启动 TCP Echo 服务器

```c
int ethernet_test_tcp_echo_start(int priority, int stack_size);
```

**功能：** 创建一个 FreeRTOS 任务，运行 TCP echo 服务器。服务器监听指定端口，接受客户端连接，将收到的所有数据原样回传。

**参数：**

- `priority`：FreeRTOS 任务优先级，如 `3`
- `stack_size`：任务栈大小（word），如 `1024`

**PC 端连接方式：**

```bash
telnet 192.168.1.100 5000
```

连接后输入任何内容，设备会原样回传。服务器持续运行，支持多客户端先后连接（同一时间只处理一个连接）。

---

### 10. `ethernet_test_udp_echo_start()` - 启动 UDP Echo 服务器

```c
int ethernet_test_udp_echo_start(int priority, int stack_size);
```

**功能：** 创建一个 FreeRTOS 任务，运行 UDP echo 服务器。监听 `ETH_TEST_UDP_PORT`（默认 5000）端口，将收到的 UDP 数据报原样回传。

**参数：**

- `priority`：FreeRTOS 任务优先级，如 `3`
- `stack_size`：任务栈大小（word），如 `1024`

**PC 端测试方式：**

```bash
# Python 快速测试
python -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.sendto(b'hello UDP',('192.168.1.100',5000)); print(s.recvfrom(1024))"
```

---

### 11. `ethernet_test_udp_screen_start()` - 启动 UDP 图传接收（直接送 LCD 显示）

```c
int ethernet_test_udp_screen_start(int priority, int stack_size);
```

**功能：** 创建一个 FreeRTOS 任务，监听 `ETH_UDP_SCREEN_PORT`（默认 5001）端口，接收来自 PC 的 1024x600 RGB565 图传数据。帧数据**直接写入 LCD 的 SDRAM framebuffer**（`fb_background[0]`），无需额外缓冲区。

**数据协议：**

每个 UDP 包由 12 字节包头 + 有效载荷组成（所有多字节字段为**网络字节序 / 大端**）：

| 偏移 | 长度 | 类型   | 说明                          |
| ---- | ---- | ------ | ----------------------------- |
| 0    | 2    | uint16 | 魔数 `0x5556`                 |
| 2    | 4    | uint32 | 帧序号 (frame_id)             |
| 6    | 2    | uint16 | 包序号 (packet_id, 0-based)   |
| 8    | 2    | uint16 | 本帧总包数                    |
| 10   | 2    | uint16 | 有效载荷长度                  |
| 12   | 1024 | bytes  | RGB565 像素数据               |

每帧 = `1024 * 600 * 2 / 1024 = 1200` 个包。

**像素处理：**

PC 端发送 RGB565 **大端**字节序像素数据（高字节在前）。接收端逐像素读取为 `uint16_t`，完成**字节序转换**（大端→小端）后写入 framebuffer。颜色空间映射（RGB→BGR）由 GLCDC 硬件自动完成（`color_order=BGR` + `endian=LITTLE`），CPU 不参与颜色转换。

**参数：**

- `priority`：FreeRTOS 任务优先级，建议 `3`
- `stack_size`：任务栈大小（word），建议 `>= 2048`

**PC 端发送脚本（已提供）：**

```bash
pip install mss pillow

# 图传模式
python udp_screen_sender_1024x600.py --target-ip 192.168.1.100 --local-ip 192.168.1.10

# 测试模式（纯色条纹，不截屏）
python udp_screen_sender_1024x600.py --target-ip 192.168.1.100 --local-ip 192.168.1.10 --test
```

**帧缓冲区：**

直接使用 GLCDC 的 framebuffer `fb_background[0]`（SDRAM，1024×600×2 = 1,228,800 字节），无需额外分配。每收完一帧数据即可在 LCD 上实时显示。

---

### 12. `ethernet_test_run()` - 运行全部测试

```c
void ethernet_test_run(void);
```

**功能：** 一键运行所有以太网测试。执行流程：

1. 打印当前网络配置信息
2. 运行网速测试（需要 PC 端先运行接收脚本）
3. 运行 ping 测试
4. 启动 TCP echo 服务器（后台持续运行）
5. 启动 UDP echo 服务器（后台持续运行）
6. 启动 UDP 图传接收器（后台持续运行，直接送 LCD 显示）

---

## TCP / UDP 通信实战指南

### TCP 通信

FreeRTOS+TCP 提供 BSD socket 风格的 TCP API，与标准 socket 编程一致。

**设备端 TCP 服务器示例：**

```c
/* 1. 创建 TCP socket */
Socket_t sock = FreeRTOS_socket(FREERTOS_AF_INET,
                                FREERTOS_SOCK_STREAM,
                                FREERTOS_IPPROTO_TCP);

/* 2. 绑定端口 */
struct freertos_sockaddr bind_addr = {0};
bind_addr.sin_port = FreeRTOS_htons(8080);
bind_addr.sin_addr = 0;  /* INADDR_ANY */
FreeRTOS_bind(sock, &bind_addr, sizeof(bind_addr));

/* 3. 监听 */
FreeRTOS_listen(sock, 1);  /* backlog = 1 */

/* 4. 接受连接 */
struct freertos_sockaddr client_addr;
socklen_t client_len = sizeof(client_addr);
Socket_t client = FreeRTOS_accept(sock, &client_addr, &client_len);

/* 5. 收发数据 */
char buf[128];
int32_t n = FreeRTOS_recv(client, buf, sizeof(buf), 0);  /* 接收 */
FreeRTOS_send(client, buf, n, 0);                         /* 回传 */

/* 6. 关闭 */
FreeRTOS_closesocket(client);
FreeRTOS_closesocket(sock);
```

**设备端 TCP 客户端示例：**

```c
Socket_t sock = FreeRTOS_socket(FREERTOS_AF_INET,
                                FREERTOS_SOCK_STREAM,
                                FREERTOS_IPPROTO_TCP);

struct freertos_sockaddr server_addr = {0};
server_addr.sin_port = FreeRTOS_htons(8080);
server_addr.sin_addr = FreeRTOS_inet_addr("192.168.1.10");

FreeRTOS_connect(sock, &server_addr, sizeof(server_addr));
FreeRTOS_send(sock, "Hello", 5, 0);

char buf[128];
int32_t n = FreeRTOS_recv(sock, buf, sizeof(buf), 0);  /* 等待回复 */

FreeRTOS_closesocket(sock);
```

**PC 端配合（Python）：**

```python
import socket

# TCP 服务器（等待设备连接）
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.bind(("0.0.0.0", 8080))
srv.listen(1)
conn, addr = srv.accept()
data = conn.recv(1024)
conn.send(data)  # echo
conn.close()

# TCP 客户端（主动连接设备）
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("192.168.1.100", 8080))
sock.send(b"Hello")
reply = sock.recv(1024)
sock.close()
```

---

### UDP 通信

UDP 无需建立连接，直接发送/接收数据报。适合实时性要求高、允许少量丢包的场景（如图传、传感器数据）。

**设备端 UDP 收发示例（使用辅助函数）：**

```c
/* 1. 创建并绑定 */
Socket_t sock = ethernet_udp_create();
ethernet_udp_bind(sock, 5001);

/* 2. 接收数据 */
char buf[512];
char src_ip[16];
uint16_t src_port;
int n = ethernet_udp_recvfrom(sock, buf, sizeof(buf), 1000,
                              src_ip, sizeof(src_ip), &src_port);
if (n > 0) {
    /* 收到来自 src_ip:src_port 的 n 字节数据 */
}

/* 3. 发送数据 */
ethernet_udp_sendto(sock, "ACK", 3, "192.168.1.10", 6000);

/* 4. 关闭 */
ethernet_udp_close(sock);
```

**设备端 UDP 收发示例（直接使用 FreeRTOS+TCP API）：**

```c
/* 创建 UDP socket */
Socket_t sock = FreeRTOS_socket(FREERTOS_AF_INET,
                                FREERTOS_SOCK_DGRAM,
                                FREERTOS_IPPROTO_UDP);

/* 绑定 */
struct freertos_sockaddr bind_addr = {0};
bind_addr.sin_port = FreeRTOS_htons(5001);
FreeRTOS_bind(sock, &bind_addr, sizeof(bind_addr));

/* 设置超时 */
TickType_t timeout = pdMS_TO_TICKS(1000);
FreeRTOS_setsockopt(sock, 0, FREERTOS_SO_RCVTIMEO, &timeout, sizeof(timeout));

/* 接收 */
struct freertos_sockaddr sender;
socklen_t sender_len = sizeof(sender);
uint8_t buf[512];
int32_t n = FreeRTOS_recvfrom(sock, buf, sizeof(buf), 0, &sender, &sender_len);

/* 发送 */
struct freertos_sockaddr dest = {0};
dest.sin_port = FreeRTOS_htons(6000);
dest.sin_addr = FreeRTOS_inet_addr("192.168.1.10");
FreeRTOS_sendto(sock, "ACK", 3, 0, &dest, sizeof(dest));

FreeRTOS_closesocket(sock);
```

**PC 端配合（Python）：**

```python
import socket

# UDP 接收端
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 6000))
data, addr = sock.recvfrom(1024)  # 等待设备发来数据

# UDP 发送端（向设备发数据）
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.sendto(b"Hello UDP", ("192.168.1.100", 5001))
reply, addr = sock.recvfrom(1024)  # 等待 echo 回复
```

---

### UDP 图传示例（屏幕投射 -> LCD 直显）

将 PC 桌面画面通过 UDP 传输到开发板，直接在 1024x600 RGBLCD 上显示。

**原理：**

```
PC 桌面 -> mss 截屏 -> 缩放 1024x600 -> RGB565 大端编码 -> UDP 分包发送 (1200包/帧)
                                                                  |
RA8P1 LCD <- GLCDC 自动做 RGB→BGR <- fb_background[0] (SDRAM) <- 字节序转换 (大端→小端)
```

**处理分工：** CPU 只做字节序转换（大端→小端，逐像素 uint16_t 读取），颜色空间映射（RGB→BGR）由 GLCDC 硬件自动完成。

**PC 端操作：**

```bash
pip install mss pillow

# 图传模式（默认 30μs 包间延迟，约 27 FPS）
python udp_screen_sender_1024x600.py --target-ip 192.168.1.100 --local-ip 192.168.1.10

# 测试模式（纯色条纹，不截屏，用于验证链路）
python udp_screen_sender_1024x600.py --target-ip 192.168.1.100 --local-ip 192.168.1.10 --test
```

**设备端操作：**

```c
// ethernet_test_run() 已自动启动 UDP 图传接收器
// 或手动启动：
ethernet_test_udp_screen_start(3, 2048);
```

**关键配置宏（`ethernet_test.h`）：**

```c
#define ETH_UDP_SCREEN_PORT     5001    // 监听端口
#define ETH_UDP_SCREEN_WIDTH    1024    // 帧宽度 = LCD 宽度
#define ETH_UDP_SCREEN_HEIGHT   600     // 帧高度 = LCD 高度
#define ETH_UDP_SCREEN_PAYLOAD  1024    // 每包有效载荷
```

---

## FreeRTOS+TCP 关键配置

以下配置在 `ra_cfg/aws/FreeRTOSIPConfig.h` 中设置，也可在 e2studio 图形化配置界面中修改。

### TCP 窗口机制（必须启用）

```c
#define ipconfigUSE_TCP_WIN    (1)     // 启用 TCP 窗口
#define ipconfigTCP_WIN_SEG_COUNT  240 // 窗口段数量
```

**作用：** 允许同时发送多个 TCP 数据段再等待 ACK，而非逐个发送逐个确认（停等模式）。

**不启用的影响：** 吞吐量极低（约 0.27 Mbit/s），因为每发一个数据段（1460 字节）就要等一个网络往返。

**启用后：** 吞吐量可提升数十倍，取决于窗口大小和网络延迟。

### 其他重要配置

| 配置项 | 当前值 | 说明 |
|--------|--------|------|
| `ipconfigUSE_DHCP` | `1` | DHCP 启用（应用层通过 `ETH_USE_DHCP` 宏控制实际行为） |
| `ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM` | `1` | 硬件校验和卸载（提升性能） |
| `ipconfigREPLY_TO_INCOMING_PINGS` | `1` | 响应 ping 请求 |
| `ipconfigSUPPORT_OUTGOING_PINGS` | `1` | 支持主动发送 ping |
| `ipconfigTCP_RX_BUFFER_LENGTH` | `3000` | TCP 接收缓冲区大小（字节） |
| `ipconfigTCP_TX_BUFFER_LENGTH` | `3000` | TCP 发送缓冲区大小（字节） |
| `ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS` | `16` | 网络缓冲区描述符数量 |
| `ipconfigIPv4_BACKWARD_COMPATIBLE` | `1` | 使用旧版 `FreeRTOS_IPInit()` 初始化路径 |

---

## 吞吐量分析

实测 TCP 发送吞吐量约 **0.17 ~ 0.27 Mbit/s**，远低于千兆网口的物理速率。原因如下：

### 核心瓶颈：TCP 发送缓冲区太小

当前 `ipconfigTCP_TX_BUFFER_LENGTH = 3000` 字节，仅能容纳约 2 个 TCP 数据段（MSS = 1460 字节）。`FreeRTOS_send()` 将数据写入缓冲区后，必须等待对端 ACK 返回、释放缓冲区空间，才能继续写入下一批数据。

实际行为等同于**停等模式**：

```
发送 2920 字节 -> 等待 ACK (RTT) -> 发送 2920 字节 -> 等待 ACK -> ...
```

理论吞吐量 = 窗口大小 / 往返延迟(RTT)：
- 窗口 = 2920 字节
- RTT ≈ 10 ms（局域网典型值）
- 理论 = 2920 / 0.01 = 292 KB/s = **0.23 Mbit/s**

实测 0.17~0.27 Mbit/s 低于理论值，额外开销来自：
- FreeRTOS 任务调度延迟
- TCP 协议头开销（每段 20~40 字节）
- ACK 处理和定时器开销
- RMAC 驱动中断处理

### 为什么不大缓冲区？

增大 `ipconfigTCP_TX_BUFFER_LENGTH` 可以提升吞吐量，但每个 TCP socket 都会分配独立的 TX/RX 缓冲区。在内存有限的 MCU 上，需要在速度和内存之间权衡：

| TX 缓冲区 | 每 socket 内存 | 预期吞吐量 |
|-----------|---------------|-----------|
| 3000 字节（当前） | ~6 KB | ~0.2 Mbit/s |
| 8192 字节 | ~16 KB | ~0.5 Mbit/s |
| 16384 字节 | ~32 KB | ~1 Mbit/s |

### 优化方向（如需更高吞吐量）

1. **增大缓冲区** - 最直接的方法，代价是占用更多 RAM
2. **使用零拷贝模式** - `ipconfigZERO_COPY_TX_DRIVER = 1`，减少数据拷贝开销
3. **使用 RAW API** - 绕过 socket 层，直接操作 FreeRTOS+TCP 核心 API
4. **换用 LwIP** - LwIP 在高吞吐场景下经过更多优化
5. **DMA 加速** - RA8P1 的 RMAC 支持 DMA，FSP 驱动已启用

当前 0.27 Mbit/s 的速度对于控制指令传输、状态上报等场景已经足够。如果需要传输大量数据（如固件升级、日志导出），建议增大缓冲区或使用 UDP。

---

## 典型使用流程

```
ethernet_init()           初始化协议栈和硬件
       |
       v
ethernet_wait_ready()     等待网线连接 + IP 就绪
       |
       v
ethernet_get_ip()         查询分配到的 IP
       |
       v
ethernet_test_speed()     网速测试 (需 PC 先运行接收脚本)
       |
       v
ethernet_test_ping()      ping 测试
       |
       v
ethernet_test_tcp_echo_start()   启动 TCP echo 服务器
       |
       v
ethernet_test_udp_echo_start()   启动 UDP echo 服务器
       |
       v
ethernet_test_udp_screen_start() 启动 UDP 图传接收器
       |
       v
FreeRTOS_socket()         自定义 socket 通信 (TCP 或 UDP)
FreeRTOS_bind()
FreeRTOS_listen() / FreeRTOS_recvfrom()
FreeRTOS_accept() / FreeRTOS_sendto()
FreeRTOS_recv()  / FreeRTOS_send()
```

---

## 日志输出

所有日志通过 `rpmsg_log_cpu1_printf()` 输出，经 RPMsg 通道发送到 CPU0 端显示。日志前缀标识来源：

- `[ETH]` - 网络初始化和状态
- `[PING]` - ping 测试
- `[SPEED]` - 网速测试
- `[TCP]` - TCP echo 服务器
- `[UDP]` - UDP echo 服务器
- `[SCR]` - UDP 图传接收器（直接送 LCD 显示）
- `[TEST]` - 测试总控
