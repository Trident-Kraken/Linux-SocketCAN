# vUDS

从零实现 ISO 15765-2 (ISO-TP) + ISO 14229 (UDS) 车载诊断协议栈，基于 C11 + Linux SocketCAN 构建，包含协议状态机、诊断服务、自动化测试体系，零第三方协议库依赖。

[![CI](https://github.com/lingfeng-pang/Linux-SocketCAN/actions/workflows/ci.yml/badge.svg)](https://github.com/lingfeng-pang/Linux-SocketCAN/actions/workflows/ci.yml)

形态为 **C 语言虚拟 ECU(Server)+ Python 自动化诊断仪(Tester)**，经 `vcan0` 虚拟总线通信，全程无需真实硬件。

## 亮点

- **ISO-TP 传输层**：SF/FF/CF/FC 双向收发状态机、SN 校验(F→0 回绕)、BS/STmin 流控(含 0xF1-0xF9 微秒档)、N_Bs/N_Cr 超时管理，4095 字节协议上限双向覆盖
- **UDS 应用层**：0x10 会话控制(含 S3 5s 超时自动降级)、0x3E TesterPresent(含肯定响应抑制)、0x22/0x2E DID 读写、0x27 Seed-Key 安全访问(3 次失败锁定 10 秒)、0x19 DTC 状态掩码查询
- **运行时设计**：单线程 epoll 事件循环，CAN socket 与全部 timerfd 统一 fd 化派发；数据路径零 malloc/free,缓冲区静态预分配；C 侧除 libc 与内核头文件外零依赖
- **验证体系**：pytest 68 条用例，以 python-can + can-isotp 参考实现与本协议栈交叉验证；Debug 构建 ASan/UBSan 全绿；Valgrind 10 分钟 163 万请求 0 错误 0 泄漏
- **文档**：需求/架构/设计三层规格文档、架构决策记录（ADR）、五类 Wireshark 抓包证据、与 ISO 标准的已知偏差清单

## 架构

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

分层依赖严格单向 `uds → isotp → can_driver`:CAN 驱动只懂 CAN 帧，ISO-TP 只懂 PCI 与分段重组，UDS 只收发完整 payload。详见[系统架构](docs/spec/architecture.md)。

## 快速开始

```bash
./scripts/setup_vcan.sh                      # 创建 vcan0（需要 sudo）
python3 -m venv .venv && . .venv/bin/activate
python -m pip install -r requirements.txt    # 仅测试侧需要
cmake -S . -B build && cmake --build build --parallel
./build/vuds_ecu -i vcan0                    # 终端 1：启动虚拟 ECU
```

终端 2 用 can-utils 直接对话(以下命令均实测)：

```console
$ candump -tz vcan0 &                        # 观察总线

$ cansend vcan0 7E0#021001AAAAAAAAAA       # 10 01 切默认会话
  vcan0  7E8   [8]  06 50 01 00 32 01 F4 AA

$ cansend vcan0 7E0#03220100AAAAAAAA      # 22 01 00 读冷却温度 DID
  vcan0  7E8   [8]  05 62 01 00 00 55 AA AA

$ cansend vcan0 7E0#0322F190AAAAAAAA      # 22 F1 90 读 VIN → 多帧响应
  vcan0  7E8   [8]  10 14 62 F1 90 4C 56 55   # FF：总长 0x14=20 字节
$ cansend vcan0 7E0#300000AAAAAAAAAA        # 手动发 FC(CTS, BS=0, STmin=0)
  vcan0  7E8   [8]  21 44 53 32 30 32 36 44   # CF
  vcan0  7E8   [8]  22 45 4D 4F 30 30 30 31   # CF → "LVUDS2026DEMO0001"
```

注：本 ECU 接收端严格要求经典 CAN `DLC=8` 且未用字节填充 `0xAA`(有意取舍，理由见[设计文档第 6 节](docs/spec/design.md#6-已知偏差与限制与-iso-标准对照))。

## UDS 服务矩阵

| SID | 服务 | 支持 | 关键行为 |
|---|---|---|---|
| 0x10 | DiagnosticSessionControl | 0x01 / 0x03 | 扩展会话 5s(S3)无活动自动降级 |
| 0x3E | TesterPresent | 0x00 / 0x80 | 抑制位生效；续期 S3 |
| 0x22 | ReadDataByIdentifier | 5 项 DID | 含 17 字节 VIN(多帧响应) |
| 0x2E | WriteDataByIdentifier | 2 项可写 DID | 需扩展会话 + 安全解锁 |
| 0x27 | SecurityAccess | 0x01 / 0x02 | Seed-Key;3 次错误锁定 10s,锁定不可被切换会话绕过 |
| 0x19 | ReadDTCInformation | 0x02 | 3 条静态 DTC 按状态掩码过滤 |

完整协议规格、状态机图与 NRC 映射见[详细设计](docs/spec/design.md)。

## 验证体系

```bash
.venv/bin/python -m pytest tests/ -v \
  --ecu-binary=build/vuds_ecu --isotp-harness=build/isotp_rx_harness
```

- **pytest 68 条**：正常/异常流程、ISO-TP 边界(7 字节 SF、8 字节 FF、4095 上限、SN 错乱、双超时)、0x27 全锁定生命周期、vcan down/restart 故障注入。Tester 侧以 can-isotp 作为参考实现交叉验证；无 vcan0 的环境下集成用例自动 skip,协议 harness 用例照常运行
- **接口 down/restart 用例**需要 `CAP_NET_ADMIN`:

  ```bash
  sudo .venv/bin/python -m pytest \
    "tests/test_robustness.py::test_interface_down_exits_cleanly_and_allows_restart" \
    -q -p no:cacheprovider --ecu-binary=build/vuds_ecu
  ```

  用例名含 `::`,须整体加引号;`-p no:cacheprovider` 避免 root 写入 `.pytest_cache` 留下属主问题
- **Sanitizer**:Debug 构建默认启用 ASan/UBSan,全套测试无报错
- **Valgrind**:`tests/valgrind_soak.py` 10 分钟混合请求压测，0 错误、0 泄漏、0 fd 泄漏
- **Wireshark 证据**:[SF 单帧](docs/wireshark/01-sf.png) · [FF/FC/CF 多帧](docs/wireshark/02-ff-fc-cf.png) · [BS/STmin 流控](docs/wireshark/03-bs-stmin.png) · [超时行为](docs/wireshark/04-timeouts.png) · [完整诊断流程](docs/wireshark/05-uds-flow.png)
- **验证记录**:[docs/verification.md](docs/verification.md)

CI(GitHub Actions)在 ubuntu runner 上拉起 vcan0,以 Release 与 Debug(ASan/UBSan)双构建跑上述全部测试。

## 构建细节

```bash
cmake -S . -B build                            # Release(默认)
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug   # 启用 ASan/UBSan
ctest --test-dir build --output-on-failure     # 经 CTest 调用 pytest
```

环境要求：Linux(SocketCAN/vcan)、CMake ≥ 3.16、C11 编译器、Python 3 + venv、`iproute2`/`kmod`/`can-utils`。

ECU 启动参数:`./vuds_ecu [-i vcan0] [-r 0x7E0] [-t 0x7E8] [-v]`(`-v` 输出帧级调试日志)。

## 目录结构

```text
include/              C 模块公开接口
src/                  虚拟 ECU 的 6 个 C 编译单元
scripts/              vcan0 环境初始化脚本
tests/                Python Tester、pytest 用例与 C 协议 harness
docs/                 技术规格、任务记录、验证记录与抓包证据
```

## 文档索引

- [需求规格](docs/spec/requirements.md) · [系统架构](docs/spec/architecture.md) · [详细设计](docs/spec/design.md)(含[已知偏差与限制](docs/spec/design.md#6-已知偏差与限制与-iso-标准对照))
- [验证记录](docs/verification.md) · [开发路线图](docs/roadmap.md)
- [`AGENTS.md`](AGENTS.md)：工程规范（范围约束、分层红线、协议事实基准、测试与提交规范）
