# vUDS 系统架构

| 项目 | 内容 |
|---|---|
| 版本 | v1.3 |
| 日期 | 2026-08-03 |
| 状态 | 已评审（实现与 T010 验证基线） |
| 修订 | v1.3：同步最终模块目录和资源生命周期验证状态 |
| 关联文档 | [需求规格](requirements.md)、[详细设计](design.md)、[AGENTS.md](../../AGENTS.md) |
| 变更规则 | 见[需求规格第 7 节](requirements.md#7-文档变更规则) |

本文件是系统上下文、模块边界、依赖方向、进程模型、事件模型和工程目录的权威来源。协议字段与状态机以[详细设计](design.md)为准，目标和验收以[需求规格](requirements.md)为准。

## 1. 系统上下文

```
┌──────────────────────────┐        ┌──────────────────────────┐
│   Python Tester (诊断仪)  │        │    C 虚拟 ECU (本项目主体) │
│                          │        │  ┌────────────────────┐  │
│  pytest + python-can     │        │  │ UDS 应用层          │  │
│  + can-isotp (参考实现)   │        │  │ 服务分发表/会话/安全  │  │
│                          │        │  ├────────────────────┤  │
│  - 原始帧注入（边界测试）  │        │  │ ISO-TP 传输层       │  │
│  - 完整诊断流程（验收）    │        │  │ TX/RX 状态机+定时器  │  │
└────────────┬─────────────┘        │  ├────────────────────┤  │
             │                      │  │ CAN 驱动层          │  │
             │   vcan0 (SocketCAN)  │  │ SocketCAN 封装      │  │
             └──────────────────────│  └────────────────────┘  │
                                    └──────────────────────────┘
```

Python Tester 通过 vcan0 与 C 虚拟 ECU 通信：原始 SocketCAN 通道负责边界和异常注入，can-isotp 通道作为标准参考实现验证完整诊断流程。上图与当前六编译单元实现一致；验证命令和结果见 [`docs/verification.md`](../verification.md)。

## 2. 模块职责与分层

| 模块 | 文件 | 职责 | 不知道什么 |
|---|---|---|---|
| CAN 驱动层 | `src/can_driver.c` | SocketCAN 打开/关闭/收发、CAN RAW 过滤器 | 不懂 PCI、不懂 UDS |
| ISO-TP 传输层 | `src/isotp.c` | SF/FF/CF/FC 组包解包、SN 校验、流控、N_Bs/N_Cr 超时 | 不懂 SID 与诊断语义 |
| UDS 应用层 | `src/uds_engine.c`、`src/uds_services.c`、`src/uds_data.c` | 服务分发、会话/安全状态机、DID/DTC 数据、NRC | 不碰 `struct can_frame` |
| 主程序 | `src/main.c` | 参数解析、epoll 事件循环、信号处理、统计 | 不含协议逻辑 |

### 2.1 依赖与接口边界

编译期依赖方向严格单向：`uds → isotp → can_driver`，禁止反向 include。`main` 是组合根，可以 include 三层公开头文件并连接对象，但不得包含协议判断。

- `can_driver.h` 定义平台无关的 `can_frame_t` 与 CAN 帧发送器 interface。Linux `struct can_frame`、PF_CAN 和 socket 操作只出现在 `can_driver.c`。
- ISO-TP 借用配置提供的 CAN 帧发送器，所有 CAN 发送都经其 `send()` operation，不直接读写 CAN socket。`can_driver_frame_sender()` 提供 SocketCAN adapter；harness 使用内存 adapter。
- ISO-TP 通过通用函数指针上交完整 payload 和 TX 完成事件；它不 include `uds.h`，因此回调不会形成反向编译依赖。
- UDS 持有借用的 `isotp_ctx_t *`，通过 `isotp_send()` 发送完整响应，并在分发 handler 前通过 `isotp_tx_busy()` 执行忙门禁。

### 2.2 数据流与帧路由

正常请求/响应数据流为：

```text
CAN socket
  → can_recv(can_frame_t)
  → isotp_on_frame()
  → on_message(payload)
  → uds_on_isotp_message()
  → service handler
  → isotp_send(payload)
  → CAN 帧发送器.send(can_frame_t)
  → CAN socket
```

ISO-TP RX 与 TX 是两个独立状态机，可以在同一个事件循环中同时处于非 IDLE 状态。`isotp_on_frame()` 按 PCI 类型路由，而不是按“当前是否正在发送”路由：

- FC 只交给 TX 状态机，不改变 RX 状态。
- SF/FF/CF 只交给 RX 状态机，不改变 TX 状态。
- TX 等待 FC 或 STmin 时收到新 SF/FF/CF，仍继续接收和重组；完整请求照常上交 UDS。
- UDS 发现 TX busy 时必须在查表和执行 handler 之前停止分发，只保存原请求 SID，待当前 TX 结束后发送 `7F [SID] 21`。

因此，TX busy 不会阻断 RX，也不会让忙请求产生会话切换、DID 写入、安全计数或 S3 重置等副作用。

### 2.3 所有权与生命周期

| 资源 | 所有者 | 借用者 | 释放者 |
|---|---|---|---|
| CAN socket fd | `can_driver_t` | main/epoll | `can_close()` |
| N_Bs/N_Cr/STmin timerfd | `isotp_ctx_t` | main/epoll | `isotp_deinit()` |
| S3 timerfd | `uds_ctx_t` | main/epoll | `uds_deinit()` |
| ISO-TP RX/TX 缓冲区 | `isotp_ctx_t` | 回调期间临时借用 RX 数据 | `isotp_deinit()` 重置 |
| UDS 响应缓冲区和 busy 槽 | `uds_ctx_t` | UDS engine/services | `uds_deinit()` 重置 |
| DID/DTC 静态数据 | `uds_data.c` | UDS services | 进程退出，无动态释放 |

初始化由 main 按 `can_open → isotp_init → uds_init → epoll 注册` 执行。在所有模块初始化成功前不进入事件循环，因此 ISO-TP 配置中的 UDS 回调不会指向未就绪对象。任一步失败都按相反顺序回滚已成功的步骤。

正常退出按 `停止事件分派 → 读取/打印统计 → uds_deinit → isotp_deinit → can_close → close(epoll_fd)` 执行。各 deinit/close 必须允许对象处于未初始化或已释放状态，不触发上层回调，也不释放借用资源。

## 3. 进程与事件模型

单进程、单线程、单 epoll 事件循环。所有事件源都是文件描述符，统一由 epoll 派发：

每次 CAN fd 就绪最多处理 64 帧，然后返回当前 epoll 批次继续处理 timerfd；若 CAN 队列仍有数据，level-triggered epoll 会在下一轮再次报告，避免持续总线流量饿死 N_Bs、N_Cr、STmin 或 S3。

| fd | 事件 | 处理 |
|---|---|---|
| `signalfd` | `SIGINT` / `SIGTERM` 到达 | 读取信号并置退出标志 |
| `can_get_fd()` | CAN 帧到达 | `can_recv` → `isotp_on_frame`（瞬态发送拥塞 `-EAGAIN`/`-EWOULDBLOCK`/`-ENOBUFS` 打 WARN 不崩溃） |
| `isotp_timer_fd(N_BS)` | TX 等待 FC 超时 | `isotp_on_timer(N_BS)` |
| `isotp_timer_fd(STMIN)` | TX 可发送下一帧 CF | `isotp_on_timer(STMIN)` |
| `isotp_timer_fd(N_CR)` | RX 等待 CF 超时 | `isotp_on_timer(N_CR)` |
| `uds_s3_timer_fd()` | S3 会话超时 | `uds_on_s3_timer()` |

0x27 的 10 秒锁定期不占用定时器：用 `CLOCK_MONOTONIC` 记录 `lock_until`，`requestSeed` 到达时惰性判断；过期则清零失败计数。除惰性判断外，所有需要主动触发的等待与信号均以 fd 接入 epoll。

## 4. 主程序运行约定

| 项 | 规格 |
|---|---|
| 启动参数 | `./vuds_ecu [-i vcan0] [-r 0x7E0] [-t 0x7E8] [-v]`（`-v` 开启 DEBUG 日志，拒绝多余位置参数与相同收发 ID） |
| 事件循环 | `epoll_wait` 主循环，按 fd 派发 |
| 信号 | `sigprocmask(SIG_BLOCK)` + `signalfd` 接入 epoll → 置退出标志，循环退出后关闭 fd、打印统计 |
| 日志 | `[HH:MM:SS.mmm] [LEVEL] [module] msg`，模块名 can/isotp/uds/main；级别 DEBUG/INFO/WARN/ERROR |
| 帧日志 | can_driver 在 DEBUG 级输出方向、ID 和数据 hex，是 W1 调试主工具 |
| 统计 | `can_frames_rx` / `can_frames_tx` / `uds_requests` / `uds_positive` / `uds_negative` / `isotp_timeouts` / `isotp_sn_errors` |

## 5. Python Tester 架构

| 项 | 规格 |
|---|---|
| 依赖 | python-can、can-isotp、pytest，`requirements.txt` 逐包锁版本（`==`），不使用 udsoncan |
| 双通道 | 原始 SocketCAN 通道：手工组帧做边界/异常注入；can-isotp 通道：作为标准参考实现跑完整诊断流程 |
| 夹具 | `conftest.py` 检查 vcan0 存在，否则提示 `setup_vcan.sh` 并 skip；以 subprocess 启动 `./vuds_ecu`，测试结束自动回收 |
| 总线独占 | 测试期间 0x7E0/0x7E8 只允许 ECU 一个监听者；禁止残留 isotpsend/isotprecv 等内核 ISO-TP socket；candump 只收不发，允许共存 |
| 时序断言 | 一律使用下限断言 + 宽松上限，例如 STmin=10ms 时断言间隔 ≥5ms，避免 CI 抖动误报 |

## 6. 工程目录

```
├── CMakeLists.txt
├── README.md
├── AGENTS.md
├── pytest.ini
├── requirements.txt
├── docs/
│   ├── adr/
│   ├── spec/
│   │   ├── requirements.md
│   │   ├── architecture.md
│   │   └── design.md
│   ├── roadmap.md
│   ├── verification.md
│   └── wireshark/
├── include/
│   ├── can_driver.h
│   ├── isotp.h                 # 接口 + PCI 宏 + 超时常量
│   └── uds.h                   # 接口 + SID/NRC/DID/DTC 常量
├── src/
│   ├── main.c                  # 参数 + epoll 主循环 + 信号 + 统计（~150 行）
│   ├── can_driver.c            # SocketCAN 封装 + 帧 hex 日志（~100 行）
│   ├── isotp.c                 # PCI + RX/TX 状态机 + 定时器（~400 行）
│   ├── uds_engine.c            # 分发表 + 会话/S3 + NRC 映射（~200 行）
│   ├── uds_services.c          # 6 个服务 handler（~250 行）
│   └── uds_data.c              # DID 表 + DTC 表 + seed-key 算法（~80 行）
├── scripts/
│   └── setup_vcan.sh           # vcan0 一键初始化
└── tests/
    ├── conftest.py             # ECU 进程夹具 + vcan0 检查
    ├── uds_client.py           # send/recv 封装 + seed-key 计算
    ├── isotp_rx_harness.c      # 无总线 ISO-TP / UDS C API harness
    ├── test_isotp.py
    ├── test_uds.py
    ├── test_security.py
    ├── test_robustness.py
    ├── test_soak.py
    └── valgrind_soak.py
```

文件粒度规则：单文件超过约 800 行才允许拆分；拆分必须由证据驱动，不做预防性设计。
