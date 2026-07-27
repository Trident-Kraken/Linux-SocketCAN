# AGENTS.md — vUDS 工程规范

本文件定义 vUDS（ISO 15765-2 / ISO 14229 车载诊断协议栈）的项目级工程规范与协作约束。

## 1. 项目概述与范围

基于 Linux SocketCAN 从零实现车载诊断协议栈：C11 虚拟 ECU（Server）+ Python 自动化诊断仪（Tester），经 `vcan0` 虚拟总线通信。

- **必做范围（Must）**：
  - ISO-TP：SF/FF/CF/FC 双向收发状态机、SN 校验、BS/STmin 流控、N_Bs/N_Cr 超时管理
  - UDS 服务：0x10 会话控制、0x3E TesterPresent（含 S3 超时自动降级）、0x22/0x2E DID 读写、0x27 Seed-Key 安全访问（含失败锁定）、0x19 读 DTC
  - 验证交付：pytest 自动化套件、Wireshark 抓包证据、规格与验证文档
- **扩展预留（Stretch，须在 Must 全部验收通过后启动）**：0x14 清 DTC、0x11 ECUReset、NRC 0x78 ResponsePending
- **明确不做（Non-Goals）**：
  - 0x31 RoutineControl、CAN FD、多诊断仪并发、JSON 配置与 cJSON 依赖、完整 AUTOSAR 诊断栈
  - 调用 Linux 内核 `CAN_ISOTP` 或任何第三方 ISO-TP/UDS C 库代替自研状态机

## 2. 技术栈与工程结构

- **技术选型**：C11（`-std=c11 -Wall -Wextra -Werror`）、CMake ≥ 3.16、Linux SocketCAN（`PF_CAN / SOCK_RAW / CAN_RAW`）、单线程 `epoll` + `timerfd` 事件循环（禁止引入多线程）；C 侧除 libc 与 Linux 内核头文件外零第三方依赖
- **CAN 寻址**：11 位标准帧常规寻址，请求 `0x7E0`、响应 `0x7E8`，默认接口 `vcan0`（支持命令行参数覆盖）
- **测试工具**：`pytest` + `python-can` + `can-isotp`（`requirements.txt` 锁版本）、`can-utils`、Wireshark、ASan/UBSan、Valgrind

```text
├── CMakeLists.txt
├── README.md
├── AGENTS.md
├── docs/
│   ├── spec/             # requirements.md / architecture.md / design.md
│   ├── adr/              # 架构决策记录
│   ├── roadmap.md        # 里程碑与路线图
│   ├── verification.md   # 验证记录
│   └── wireshark/        # 抓包截图证据
├── include/              # can_driver.h / isotp.h / uds.h
├── src/                  # main.c / can_driver.c / isotp.c / uds_engine.c / uds_services.c / uds_data.c
├── scripts/              # setup_vcan.sh
└── tests/                # conftest.py / uds_client.py / isotp_rx_harness.c / test_*.py / valgrind_soak.py
```

单文件超过约 800 行才考虑拆分，不做预防性重构。

## 3. 代码与分层规范

- **编译与风格**：C11；`-Wall -Wextra -Werror` 零警告（禁止 pragma 压制）；函数/变量 `snake_case`，宏/常量 `UPPER_SNAKE`，typedef 以 `_t` 结尾，模块前缀 `can_` / `isotp_` / `uds_`；4 空格缩进，K&R 花括号，指针 `*` 靠变量名
- **单向分层纪律**：严格遵守 `uds → isotp → can_driver`，禁止反向 `#include`；`can_driver` 只懂 CAN 帧，`isotp` 只懂 PCI 与分段重组（绝不解析 SID），`uds` 只收发完整 payload（绝不触碰 `struct can_frame`）；层间通过接口函数与回调交互
- **内存与错误处理**：4095 字节静态缓冲区预分配，数据路径禁止 `malloc`/`free`；函数返回 0 或负 `errno`，UDS 错误统一由 `uds_engine` 映射为 NRC
- **状态机注释**：状态机定义处必须附带 ASCII 状态转移图；修改状态转移须同步更新注释与 `docs/spec/design.md`

## 4. 协议事实基准

- **CF 序列号**：FF 后首个 CF 的 SN = 1，逐帧 +1，回绕方向为 `F → 0`（不是 `F → 1`）
- **NRC 映射**：顺序错误 = `0x24`（`requestSequenceError`）；越界/不支持 DID = `0x31`（`requestOutOfRange`）；会话不支持 = `0x7E`/`0x7F`；安全未解锁 = `0x33`；Key 错误 = `0x35`；超过尝试次数 = `0x36`；锁定计时未到 = `0x37`
- **响应格式**：肯定响应 `SID + 0x40`；否定响应 `0x7F + 请求 SID + NRC`
- **寻址与定时**：固定 11 位常规寻址 `0x7E0`/`0x7E8`；S3 = 5000ms；N_Bs / N_Cr = 1000ms；STmin `0x00-0x7F` 为 ms，`0xF1-0xF9` 为 100-900us
- **重传策略**：ISO-TP 无重传机制，SN 错误或超时即丢弃当前传输并复位回 IDLE

## 5. 测试与提交规范

- **三层验证**：`can-utils` 手工帧调试、`pytest`（结合 `python-can` / `can-isotp` 交叉验证）以及 Wireshark 抓包核对
- **覆盖底线**：每个 UDS 服务至少 1 条正常 + 2 条异常（校验具体 NRC）；覆盖 ISO-TP 1/7/8/4095 边界、SN 错乱、N_Bs/N_Cr 超时、0x27 锁定生命周期与畸形帧/接口 down 故障注入
- **完成定义（DoD）**：Release 与 Debug（`-fsanitize=address,undefined -fno-sanitize-recover=all`）零警告编译且测试全通；Valgrind 长稳压测无内存与 fd 泄漏
- **Git 提交**：Conventional Commits（`<type>(<scope>): <subject>`），`type` 取 `feat|fix|test|docs|refactor|chore|build|ci`，`scope` 取 `can|isotp|uds|cmake|docs|build`；一次提交只做一件事，不提交构建产物与原始 `.pcap`/`.pcapng`

## 6. AI 辅助开发约束

1. **先读规格再改动**：修改代码或测试前先阅读 `docs/spec/` 对应规格与既有状态机定义，确保与系统设计一致。
2. **严守范围与分层红线**：不引入第三方依赖，不新增 Non-Goals 列表外的功能，不破坏 `uds → isotp → can_driver` 单向依赖。
3. **外科手术式改动**：只改动任务相关代码与文档，不顺手重构无关逻辑；协议歧义先依据标准与 `docs/spec/design.md` 确认，不静默猜改。
4. **验证驱动交付**：每次改动均须附实际执行的构建、测试或抓包验证输出，不作未验证的推定结论。
