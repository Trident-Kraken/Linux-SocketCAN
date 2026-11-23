# vUDS 验证记录

日期：2026-08-24（Asia/Singapore）

本记录汇总实际执行过的构建、自动化测试、压测与抓包验证结果。

## T010 稳定性验收

在具备 `CAP_NET_ADMIN` 权限的终端执行完整回归（含 FT-01 `test_interface_down_exits_cleanly_and_allows_restart`），验证 `vcan0` down 后 ECU 干净退出、接口恢复与重启响应正常：

```bash
sudo .venv/bin/python -m pytest tests/ -q
```

结果：全部通过（T010 基线为 `63 passed`）。在无 `CAP_NET_ADMIN` 权限环境下，FT-01 按夹具自动跳过：

```text
SKIPPED ... interface fault injection requires CAP_NET_ADMIN
```

## 构建和自动化

- Release CMake 构建通过，`-Wall -Wextra -Werror` 无警告。
- Debug CMake 构建通过，包含 ASan/UBSan；`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1` 下 Debug CTest 通过（27.43 秒）。
- Release CTest 在主机 `vcan0` 网络命名空间中通过（27.17 秒）。
- 2026-08-04 在 CAN RX 每轮 64 帧预算改动后重新执行 Release soak：精确完成 1000 个请求，耗时 0.338 秒，RSS `1644 -> 1644 KiB`，CPU `0 -> 12 ticks`。
- Debug soak 完成 1000 个请求：0.360 秒，RSS `8176 -> 9928 KiB`；ASan 进程不用于 Release 的 RSS 阈值判断。
- Valgrind 3.22.0 运行 600.118 秒，完成 1,353,140 个请求；`3 open (3 std)`、`in use at exit: 0 bytes`、`11 allocs, 11 frees`、`ERROR SUMMARY: 0 errors`。

## can-utils 帧证据

以下为 candump/cansend 实测帧记录：

```text
SF:
7E0#021003AAAAAAAAAA -> 7E8#065003003201F4AA

FF/FC/CF:
7E0#1014010203040506
7E8#300000AAAAAAAAAA
7E0#210708090A0B0C0D
7E0#220E0F1011121314
```

可复现的手工观察方式是在一个终端运行 `candump -L vcan0`，另一个终端使用 `cansend` 注入上述帧，或启动 ECU 后执行：

```bash
cansend vcan0 7E0#021003AAAAAAAAAA
cansend vcan0 7E0#0322F190AAAAAAAA
```

完整的 `22 F1 90` 响应由 ISO-TP FC/CF 流程产生；不要在测试期间启动 `isotpsend`/`isotprecv`，避免引入第二个流控端点。

## 证据矩阵

| 场景 | 自动化/帧结果 | Wireshark PNG |
|---|---|---|
| SF | `10 03 -> 50 03`，帧格式已记录 | [`01-sf.png`](wireshark/01-sf.png) |
| FF/FC/CF | VIN 和 T004 多帧帧流已验证 | [`02-ff-fc-cf.png`](wireshark/02-ff-fc-cf.png) |
| BS/STmin | ISO-TP harness 与 pytest 已验证 | [`03-bs-stmin.png`](wireshark/03-bs-stmin.png) |
| N_Cr/N_Bs 超时复位 | ISO-TP harness 已验证 | [`04-timeouts.png`](wireshark/04-timeouts.png) |
| 完整 UDS 流程 | `63 passed`，含 SecurityAccess/DID/DTC | [`05-uds-flow.png`](wireshark/05-uds-flow.png) |

## T011 Wireshark/TShark 证据

- 宿主图形环境安装 Wireshark/TShark 4.2.2；五个过滤后的 pcap 分别在 Wireshark 中打开并保存为真实窗口截图。
- 临时捕获结果为 `captured_frames=32 output=/tmp/t011-all.pcap`；TShark 复核 `/tmp/t011-all.pcap` 共 32 个 CAN 数据帧，均为 `DLC=8`，ID 为 `0x7e0` 或 `0x7e8`。
- 原始 `/tmp/t011-*.pcap` 仅作本地复核，不提交；提交内容为 `docs/wireshark/01-sf.png`、`02-ff-fc-cf.png`、`03-bs-stmin.png`、`04-timeouts.png` 和 `05-uds-flow.png`。

## 2026-08-24 全量复验与 CI 基线

本地（WSL2 + vcan0）：

- Release 与 Debug（ASan/UBSan）全新构建通过，`-Wall -Wextra -Werror` 无警告。
- Debug 构建全量 pytest：`62 passed, 1 skipped in 29.35s`；FT-01 在 sudo 终端单跑 `1 passed`，合计 63/63。
- Release soak（RSS 断言生效）：1000 请求 0.339 秒，RSS `1640 -> 1640 KiB`，CPU `0 -> 11 ticks`。
- Valgrind 3.22.0 运行 600.120 秒，完成 162,914 轮共 1,629,140 个请求；`FILE DESCRIPTORS: 3 open (3 std)`、`All heap blocks were freed -- no leaks are possible`、`ERROR SUMMARY: 0 errors`。

CI（GitHub Actions `ubuntu-latest`）：

- 若 runner 内核缺少内置 `vcan` 模块，CI 脚本通过 `linux-modules-extra-$(uname -r)` 加载，不可用时显式警告降级。
- Release 作业：`62 passed, 1 skipped`，FT-01 以 sudo 单跑 `1 passed`；Debug（ASan/UBSan）作业：`62 passed, 1 skipped`。

## 复审修复更新（2026-11-24）

- 补充 SocketCAN 内核 `EFF/RTR` 过滤掩码与 `SOCK_CLOEXEC`、`0x27` 已解锁状态全 0 Seed 响应、CMake 默认 `Release` 构建类型与不可恢复 UBSan（`-fno-sanitize-recover=all`）。
- 补全端到端集成用例后，测试集共收集 68 条用例：`vcan0` 特权环境 Release 回归 `68 passed`（含 FT-01）；Debug（ASan/UBSan + `-fno-sanitize-recover=all`）非特权 `vcan0` 回归 `67 passed, 1 skipped in 40.00s`（仅 FT-01 因无 `CAP_NET_ADMIN` 跳过），零 Sanitizer 报错。
