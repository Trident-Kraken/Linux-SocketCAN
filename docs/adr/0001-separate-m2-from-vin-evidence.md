# ADR-0001: 将 M2 与 VIN/完整证据链解耦

- 状态：已接受
- 日期：2026-08-02
- 决策者：lingfeng-pang

## 背景

原 M2 出口同时要求 ISO-TP 双向传输、UDS busy 处理、VIN DID 0xF190
端到端响应以及完整 candump/vcan 证据。VIN 属于 T008 的 DID 数据与读写服务，
完整抓包证据链属于 T011 的发布验收。把这些要求留在 M2 会让传输层完成状态依赖
尚未开始的 UDS 业务服务和最终交付工作。

## 决策

M2 定义为“ISO-TP + UDS Core 闭环”，由 T004 和 T005 完成：

- ISO-TP 覆盖 SF/FF/CF/FC 双向状态机、SN、BS/STmin、N_Bs/N_Cr、
  1..4095 字节边界以及 RX/TX 正交性。
- UDS Core 覆盖完整 payload 上交、表驱动分发、基础 0x10、短 DID 0x0100、
  集中 NRC 映射以及 TX busy 单槽 NRC 0x21 行为。
- M2 使用公开 API harness 和自动化 pytest 证明闭环，不要求 VIN 0xF190
  或完整 candump/vcan/Wireshark 证据链。
- VIN 0xF190、多帧 VIN 响应和完整诊断流程归 T008；UT-20 的 VIN 端到端
  场景也在 T008 关闭。
- can-utils、candump、vcan 和 Wireshark 的完整交付证据链归 T011。

## 后果

M2 可以独立证明传输层和最小 UDS 分发闭环，不再提前实现 T008 的业务数据。
代价是 M2 结束时尚未通过真实 VIN 响应证明长 UDS payload；该风险由 T005 的
4095 字节公开 API 测试、T008 的 VIN 端到端回归和 T011 的抓包证据共同覆盖。

本决策不缩减项目最终 Must 范围，也不改变 ISO-TP、UDS 或 CAN 的运行时架构。
