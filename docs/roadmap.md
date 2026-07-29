T001 已建立三份规格基线，T002 已完成 CAN 驱动与单线程 epoll 骨架，T003 已打通 ISO-TP SF、UDS 0x10 和短 DID 0x0100，T004/T005 已完成 ISO-TP + UDS Core 闭环，T006 已完成会话/S3，T007 已完成 SecurityAccess 0x27，T008 已完成 DID 读写，T009 已完成 DTC 状态查询，M2 已关闭。T010 已完成异常、资源、1000 请求、Sanitizer、10 分钟 Valgrind 以及具备 `CAP_NET_ADMIN` 环境中的 vcan down/restart 验证；完整抓包证据链属于 T011，当前 T001~T011 已全部完成。

**Milestone**
| Milestone | 时间 | 目标 | 任务 | 出口标准 |
|---|---:|---|---|---|
| M0 规格基线 | D1 | 建立三文档唯一事实源 | T001 | 无占位规格或冲突旧引用 |
| M1 可运行 MVP | D1-D3 | 打通 CAN → ISO-TP SF → UDS | T002-T003 | `10 03` 得到 `50 03` |
| M2 ISO-TP + UDS Core | D4-D7 | 双向多帧、流控、超时、分发、忙处理 | T004-T005 | IT-01～IT-11 及 UDS Core 自动化闭环通过 |
| M3 完整 UDS | W2 | 实现六个必做服务 | T006-T009 | UT-01～UT-20 通过 |
| M4 稳定性验收 | W3 前半 | 异常、资源、soak 验证 | T010 | 1000 请求、Sanitizer、Valgrind 通过 |
| M5 发布交付 | W3 后半 | 文档、抓包、可复现交付 | T011 | 陌生环境 5 分钟运行成功 |

依赖主链：`T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010 → T011`。

`T009` 依赖 `T006`，逻辑上可与 T007/T008 并行，但因共享 UDS 文件，建议顺序提交。

**T001 — 建立规格唯一事实源**
- **目标**：把旧规格基线拆成需求、架构、详细设计三份正式基线。
- **输入依赖 / 修改文件**：AGENTS、旧规格基线；修改 `docs/spec/*.md`、`README.md`、`AGENTS.md`、目录结构文档，迁移完成后移除旧规格文件。
- **实现步骤**：需求与验收写入 requirements；模块和事件模型写入 architecture；协议、接口、状态机和测试矩阵写入 design；统一链接和版本信息。
- **验收标准 / 测试要求 / DoD**：三文档覆盖原 SPEC 全部有效决策；`rg` 无失效旧引用；人工逐节对照无遗漏；独立提交 `docs(spec)`。

**T002 — SocketCAN 与事件循环基础**
- **目标**：建立可长期运行、能接收和记录 CAN 帧的 ECU 进程。
- **输入依赖 / 修改文件**：T001、vcan0；修改 `can_driver.h/.c`、`main.c`、`tests/conftest.py`、必要的 README。
- **实现步骤**：实现参数解析、CAN RAW socket/filter、收发封装、epoll、信号退出、DEBUG 帧日志和基础统计。
- **验收标准 / 测试要求 / DoD**：只接收 0x7E0；`cansend` 后日志与 `candump` 一致；SIGINT 清理正常；零警告、ASan/UBSan 干净；独立提交 `feat(can)`。

**T003 — 单帧诊断 MVP**
- **目标**：打通 SF 接收、UDS 分发、0x10 和短 DID 0x22 响应。
- **输入依赖 / 修改文件**：T002；修改 `isotp.h/.c`、`uds.h`、三个 UDS 源文件、`main.c`、`uds_client.py`、ISO-TP/UDS 测试。
- **实现步骤**：实现 SF 编解码与回调；建立最小服务表和 NRC 映射；实现 0x10、短 DID 读取；连接三层回调。
- **验收标准 / 测试要求 / DoD**：`10 03 → 50 03 00 32 01 F4`；短 DID 正确；非法 SF、未知 SID、错误长度返回规定结果；pytest 覆盖正常和异常；独立提交 `feat(uds)`。

**T004 — ISO-TP 多帧接收（已完成）**
- **目标**：完成 FF/CF 重组、FC 回复、SN 校验和 N_Cr 超时。
- **输入依赖 / 修改文件**：T003；修改 `isotp.h/.c`、`test_isotp.py`、`conftest.py`、`CMakeLists.txt`，新增 `tests/isotp_rx_harness.c`，并同步 `design.md` 状态图。
- **实现步骤**：实现 RX 状态机、静态 4095 字节缓冲、timerfd、SN F→0、新 SF/FF 抢占和异常复位。
- **验收标准 / 测试要求 / DoD**：20/4095 字节重组成功；SN 跳变、超时、`FF_DL<8`、`SF L=0` 和严格 DLC/padding 被正确处理；RX harness 六场景和真实 vcan 回归通过；抓包证据已记录；尚未创建独立 Git 提交。

**T005 — ISO-TP 多帧发送与流控（已完成）**
- **目标**：完成 FF/CF 发送、FC、BS/STmin、N_Bs 和 TX 忙处理。
- **输入依赖 / 修改文件**：T004；修改 `isotp.h/.c`、`uds_engine.c`、`main.c`、`test_isotp.py`、必要的测试专用 C harness。
- **实现步骤**：实现 TX 状态机；处理 CTS/Wait/Overflow；按 BS 暂停；按 STmin 调度；实现单槽 NRC 0x21 补发。
- **验收标准 / 测试要求 / DoD**：1/7/8/4095 边界、FC Wait/Overflow、N_Bs、BS=4、STmin=10ms、RX/TX 正交和 UDS busy 单槽行为通过；IT-01～IT-11 全绿；独立提交 `feat(isotp)`。
- **当前记录**：公开 API harness、pytest、Release/Debug CTest 已验证 ISO-TP + UDS Core 闭环。VIN 0xF190 和 UT-20 端到端场景属于 T008，完整抓包证据链属于 T011。

**T006 — 会话控制、TesterPresent 与 S3**
- **目标**：完整实现 0x10、0x3E、suppress bit 和 S3 自动降级。
- **输入依赖 / 修改文件**：T005；修改 `uds.h`、`uds_engine.c`、`uds_services.c`、`main.c`、`test_uds.py`。
- **实现步骤**：建立默认/扩展会话；添加 S3 timerfd；有效请求重置 S3；切换或超时后重新上锁；统一 suppress 处理。
- **验收标准 / 测试要求 / DoD**：0x10/0x3E 各至少 1 正常、2 异常；`3E 80` 无肯定响应但重置 S3；5.5 秒后回默认会话；UT-01～UT-04、UT-19 通过；独立提交 `feat(uds)`。

**T007 — SecurityAccess 0x27**
- **目标**：实现 Seed-Key、顺序控制、失败计数和 10 秒锁定。
- **输入依赖 / 修改文件**：T006；修改 `uds.h`、`uds_engine.c`、`uds_services.c`、`uds_data.c`、`test_security.py`、`uds_client.py`。
- **实现步骤**：实现 LOCKED/SEED_SENT/UNLOCKED；安全生成 seed；大端 key 校验；三次失败锁定；CLOCK_MONOTONIC 惰性恢复。
- **验收标准 / 测试要求 / DoD**：覆盖正确解锁及 NRC 0x24/0x35/0x36/0x37；会话切换使 seed 和解锁失效；无可预测固定 seed；UT-11～UT-13 及恢复用例通过；独立提交 `feat(uds)`。

**T008 — DID 数据与读写服务（已完成）**
- **目标**：完成 0x22/0x2E、静态 DID 表以及会话/安全权限。
- **输入依赖 / 修改文件**：T007；修改 `uds.h`、`uds_engine.c`、`uds_services.c`、`uds_data.c`、`test_uds.py`。
- **实现步骤**：建立五项 DID 表；实现单 DID 读写；校验存在性、只读、长度、会话和解锁状态。
- **验收标准 / 测试要求 / DoD**：VIN 多帧读取正确；完整 `10→27→2E→22` 流程读回一致；覆盖 NRC 0x13/0x31/0x33/0x7F；每个服务满足 1 正常+2 异常；UT-05～UT-10、UT-18 和 VIN 端到端场景通过。

**T009 — DTC 读取服务（已完成）**
- **目标**：完成 0x19 reportDTCByStatusMask 和剩余通用 NRC 行为。
- **输入依赖 / 修改文件**：T006；修改 `uds.h`、`uds_services.c`、`uds_data.c`、`test_uds.py`。
- **实现步骤**：建立三项静态 DTC 表；按 mask 过滤；返回 availability mask；拒绝未知子功能和错误长度。
- **验收标准 / 测试要求 / DoD**：FF 返回 3 条、08 返回 2 条、无匹配返回肯定空列表；覆盖 NRC 0x12/0x13；UT-14～UT-17 通过。

**T010 — 健壮性、资源与回归验收（已完成）**
- **目标**：验证完整协议栈在异常输入和持续运行下不会崩溃或污染状态。
- **输入依赖 / 修改文件**：T008、T009；新增或修改 robustness/soak 测试，并仅按失败证据修正对应 C 模块。
- **实现步骤**：注入 vcan down、DLC=0、非法 PCI、超长报文；执行 1000 请求 soak；核对统计、内存和恢复行为。
- **验收标准 / 测试要求 / DoD**：FT-01～FT-04 通过；全量 pytest 绿；Debug ASan/UBSan 无报错；Valgrind 10 分钟无泄漏；RSS<8MB；独立提交 `test(uds)` 或针对性 `fix(scope)`。
- **验证记录**：在具备 `CAP_NET_ADMIN` 权限的环境中运行特权回归（含 FT-01），全部通过。

**T011 — 文档、抓包与发布验收（已完成）**
- **目标**：形成可复现、可演示的最终交付。
- **输入依赖 / 修改文件**：T010；修改 README、三份 spec，新增 `docs/verification.md`、状态机图和 `docs/wireshark/*.png`。
- **实现步骤**：同步最终行为；记录构建/测试/Valgrind 结果；抓取 SF、多帧、流控、超时、完整诊断流程；执行全新环境快速开始。
- **验收标准 / 测试要求 / DoD**：陌生人按 README 5 分钟运行；全部规格有实现或测试映射；五类截图可核对；清洁构建、pytest、Sanitizer、Valgrind 全通过；独立提交 `docs(docs)`。
- **验证记录**：Wireshark 4.2.2 已生成并核验五类 PNG；TShark 复核 32 帧 pcap；构建、自动化和稳定性证据见 `docs/verification.md`。
