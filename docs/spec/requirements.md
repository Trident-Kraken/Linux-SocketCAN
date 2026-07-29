# vUDS 需求规格

| 项目 | 内容 |
|---|---|
| 版本 | v1.4 |
| 日期 | 2026-08-03 |
| 状态 | 已评审（T010 稳定性验收和 T011 证据均已完成） |
| 修订 | v1.4：补充 FT-01 特权回归结果和最终证据状态 |
| 关联文档 | [AGENTS.md](../../AGENTS.md)、[系统架构](architecture.md)、[详细设计](design.md)、[开发路线图](../roadmap.md)、[ADR-0001](../adr/0001-separate-m2-from-vin-evidence.md) |
| 变更规则 | 见[第 7 节](#7-文档变更规则) |

本文件是项目目标、范围、需求和验收标准的权威来源。协议字段、状态机和接口细节以[详细设计](design.md)为准；模块边界和运行模型以[系统架构](architecture.md)为准。任何规格变更必须同步检查 AGENTS.md 的工程约束和协议事实基准。

## 1. 项目概述

### 1.1 背景与目标

在纯 Linux 环境（无 CAN 硬件）下，从零手写一套车载诊断协议栈：

- 传输层：ISO 15765-2 (ISO-TP)，解决 CAN 单帧 8 字节与诊断报文最长 4095 字节之间的分段/重组问题。
- 应用层：ISO 14229 (UDS) 诊断服务引擎，实现 6 个核心诊断服务。
- 产品形态：C 语言虚拟 ECU（Server，被诊断方）+ Python 自动化诊断仪（Tester，测试与演示）。
- 通信介质：Linux vcan0 虚拟 CAN 总线。

工程目标：一键编译运行、自动化测试全绿、Wireshark 抓包可验证。

### 1.2 范围与约束

必做范围、选做范围和明确不做项以 [AGENTS.md 范围约束](../../AGENTS.md) 为唯一来源。本项目规格承诺实现必做项；选做的 0x14、0x11 和 NRC 0x78 只预留扩展点，不纳入当前验收基线。

必做功能包括：

- ISO-TP SF/FF/CF/FC 双向状态机、序列号校验、BS/STmin 流控和 N_Bs/N_Cr 超时。
- UDS 0x10、0x3E、0x22、0x2E、0x27、0x19 六个服务。
- 至少 15 条 Python 自动化用例、Wireshark 抓包证据、README、架构图和状态机图。

### 1.3 术语

| 术语 | 含义 |
|---|---|
| PCI | Protocol Control Information，ISO-TP 每帧首字节的协议控制信息 |
| SF / FF / CF / FC | 单帧 / 首帧 / 连续帧 / 流控帧 |
| SN | Sequence Number，CF 帧的 4 位序列号 |
| BS | Block Size，收到多少个 CF 后需重新等待 FC |
| STmin | Separation Time minimum，发送方相邻 CF 的最小时间间隔 |
| SID | Service Identifier，UDS 服务 ID |
| NRC | Negative Response Code，否定响应码 |
| DID | Data Identifier，2 字节数据标识符 |
| DTC | Diagnostic Trouble Code，3 字节故障码 + 1 字节状态 |
| S3 | 非默认会话的超时定时器，超时自动回退默认会话 |
| N_Bs / N_Cr | ISO-TP 网络层超时参数，详见[详细设计](design.md) |
| UDS Core | M2 的最小应用层闭环：完整 payload 上交、表驱动分发、基础 0x10、短 DID 0x0100、集中 NRC 和 TX busy 单槽处理 |

## 2. 功能需求

### 2.1 通信与传输

- ECU 默认使用 `vcan0`，支持命令行覆盖接口和 11 位请求/响应 ID。
- 仅处理物理请求 ID `0x7E0`，响应发送到 `0x7E8`；功能寻址 `0x7DF` 直接忽略。
- ISO-TP 必须支持 1 至 4095 字节 payload，严格处理 SF/FF/CF/FC、SN、BS、STmin、N_Bs 和 N_Cr。
- 协议错误、超时和无效长度不得污染后续传输状态；具体行为以[详细设计错误处理汇总](design.md#27-错误处理汇总)为准。

### 2.2 UDS 服务

| SID | 服务 | 必须支持的行为 |
|---|---|---|
| 0x10 | DiagnosticSessionControl | 默认/扩展会话切换、P2/P2* 响应、S3 管理 |
| 0x3E | TesterPresent | 正常响应、抑制肯定响应位、重置 S3 |
| 0x22 | ReadDataByIdentifier | 读取静态 DID 表中的单个 DID |
| 0x2E | WriteDataByIdentifier | 扩展会话且解锁后写入可写 DID |
| 0x27 | SecurityAccess | Seed-Key 挑战响应、顺序校验、失败锁定 |
| 0x19 | ReadDTCInformation | 按状态掩码读取预置 DTC |

会话、安全权限、字段格式和 NRC 触发条件以[详细设计第 3 节](design.md#3-uds-应用层规格iso-14229-1)为准。

### 2.3 测试与交付

- Python Tester 必须使用 pytest、python-can 和 can-isotp；不得使用 udsoncan。
- 自动化测试必须覆盖正常流程、异常流程、ISO-TP 边界、SecurityAccess 锁定、异常注入和稳定性场景。
- 必须提供 can-utils 手动验证、pytest 交叉验证和 Wireshark 人工核对三层证据。
- 最终交付必须包含 README、架构图、状态机图和五类关键抓包截图。

## 3. 非功能需求

| 项 | 目标 |
|---|---|
| 吞吐 | pytest 顺序执行 1000 条诊断请求（soak 用例）无丢失、无状态错乱 |
| 资源观测 | soak 期间记录 CPU/内存占用，作为数据记录，不设硬指标 |
| 内存 | 进程常驻内存 < 8MB；数据路径零动态分配 |
| 可移植性 | SocketCAN 帧类型与 socket 操作仅存在于 can_driver；ISO-TP 调度按本项目约束使用 Linux timerfd，换 can0 接真实硬件不改协议行为 |
| 构建质量 | C11、`-Wall -Wextra -Werror` 零警告；Debug 构建支持 ASan/UBSan |
| 并发模型 | 单进程、单线程、单 epoll 事件循环，禁止线程和阻塞 sleep |
| 依赖 | C 侧除 libc 与 Linux 内核头文件外零第三方依赖 |

## 4. 里程碑与验收标准

| 切片 | 内容 | 出口标准 |
|---|---|---|
| A（D1-3） | SF 垂直切片：vcan + can_driver + 单帧 ISO-TP + 分发器 + 0x10 + 0x22（短 DID） | cansend 发 `10 03` 收到 `50 03`，全链路打通 |
| B（D4-7） | ISO-TP + UDS Core 闭环：双向多帧、流控、超时、分发与 TX busy | IT-01 ~ IT-11 及 UDS Core 自动化闭环通过 |
| C（W2） | UDS 服务按序落地：0x3E+S3 → 0x27 → 0x2E/完整 DID → 0x19 | UT-01 ~ UT-20 通过；VIN 与完整诊断流程在 T008 关闭 |
| D（W3） | 边界/异常注入用例、抓包截图、README 与文档整理 | 验收红线全过 |

进度纪律：pytest harness 随切片 C 第一个服务（0x3E）建立，之后每加一个服务补 2-3 条用例，持续回归。

根据 [ADR-0001](../adr/0001-separate-m2-from-vin-evidence.md)，M2 不包含 VIN DID 0xF190 或完整 candump/vcan/Wireshark 证据链。VIN 和 UT-20 的端到端场景属于 T008；完整证据链属于 T011。项目最终 Must 范围不变。

验收红线：编译零警告；pytest 全绿；Valgrind/ASan 干净；README 能让陌生人 5 分钟跑起来。

## 5. 风险与应对

| 风险 | 等级 | 应对 |
|---|---|---|
| ISO-TP TX 流控调试（BS/STmin + FC.Wait/Overflow + RX 并发） | 中高 | IT-08/09/10 最先写；candump 逐帧对 + can_driver hex 日志 |
| WSL2 内核不支持 vcan | 中 | D1 第一件事验证；失败即转 Ubuntu 虚拟机，当天解决不恋战 |
| pytest 期间残留内核 ISO-TP socket 干扰（双 FC） | 中 | conftest 总线独占检查；candump 只收不发，允许共存 |
| 时序类测试抖动误报 | 低 | 下限断言 + 宽松上限，例如 STmin=10ms 时断言间隔 ≥5ms |

## 6. 完成定义

- 编译零警告 + `pytest tests/ -v` 全绿。
- Debug 构建开启 `-fsanitize=address,undefined` 跑测试无报错；每周至少一次 Valgrind 跑 10 分钟无泄漏。
- 涉及协议行为的改动必须有自动化行为证据；can-utils、candump、vcan 和 Wireshark 的完整证据链在 T011 汇总。
- 所有实现、测试和文档引用新的三份规格文件，不再引用已删除的旧规格入口。

## 7. 文档变更规则

协议字段、状态机、NRC、测试编号等事实只在[详细设计](design.md)维护；模块与运行模型只在[系统架构](architecture.md)维护；目标、范围和验收标准只在本文件维护。其他文档使用交叉链接，不复制规则。
