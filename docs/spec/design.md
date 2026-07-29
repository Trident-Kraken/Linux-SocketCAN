# vUDS 详细设计

| 项目 | 内容 |
|---|---|
| 版本 | v1.10 |
| 日期 | 2026-08-23 |
| 状态 | 已评审（T004 RX + T005 TX + T006 会话/S3 + T007 SecurityAccess + T008 DID + T009 DTC + T010 验证基线） |
| 修订 | v1.10：新增第 6 节与 ISO 标准的已知偏差与限制对照 |
| 关联文档 | [需求规格](requirements.md)、[系统架构](architecture.md)、[AGENTS.md](../../AGENTS.md) |
| 变更规则 | 见[需求规格第 7 节](requirements.md#7-文档变更规则) |

本文件是 CAN、ISO-TP、UDS、接口、状态机、数据表、错误处理和测试矩阵的权威来源。

## 1. 通信基础规格

| 项 | 规格 |
|---|---|
| CAN 接口 | vcan0，可用命令行参数 `-i` 覆盖 |
| 寻址模式 | Normal Addressing，11 位标准帧（ISO-TP 不要求 29 位扩展帧） |
| 物理请求 ID | 0x7E0（Tester → ECU） |
| 物理响应 ID | 0x7E8（ECU → Tester） |
| 功能寻址 | 不支持（收到 0x7DF 直接忽略） |
| 帧 DLC | ISO-TP 帧恒为 8；发送方将未用字节填为 0xAA，T004 接收方严格要求 DLC=8 且未用字节为 0xAA |
| CAN RAW 过滤器 | 只接收 rx_id = 0x7E0，内核层过滤无关帧 |

### 1.1 CAN 驱动接口

`can_driver.h` 对上层暴露经典 CAN 数据帧，不暴露 Linux `struct can_frame`：

```c
#define CAN_CLASSIC_DLEN 8u

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[CAN_CLASSIC_DLEN];
} can_frame_t;

typedef int (*can_frame_send_fn_t)(void *context,
                                   const can_frame_t *frame);

typedef struct {
    void                *context;
    can_frame_send_fn_t send;
} can_frame_sender_t;

typedef struct {
    uint64_t frames_rx;
    uint64_t frames_tx;
} can_stats_t;

int                can_open(can_driver_t *driver, const char *ifname,
                            uint32_t rx_id, bool verbose);
void               can_close(can_driver_t *driver);
int                can_get_fd(const can_driver_t *driver);
int                can_recv(can_driver_t *driver, can_frame_t *frame);
int                can_send(can_driver_t *driver, const can_frame_t *frame);
can_frame_sender_t can_driver_frame_sender(can_driver_t *driver);
const can_stats_t  *can_get_stats(const can_driver_t *driver);
```

接口契约：

- `can_open()` 校验非空参数和 11 位 ID，创建 nonblocking CAN RAW socket、安装精确过滤器并绑定接口。成功返回 0；失败返回负 errno，且 driver 保持可安全 `can_close()` 的关闭状态。
- `can_get_fd()` 返回借用 fd，仅供 main 注册 epoll；调用方不得关闭。未打开时返回 -1。
- `can_recv()` 成功取得一个 11 位数据帧时返回 0；队列已排空返回 `-EAGAIN`；其他失败返回负 errno。EFF/RTR/error frame 在驱动内丢弃，DLC=0 的标准数据帧仍上交 ISO-TP 做异常输入验证。
- `can_send()` 只接受 11 位数据帧且 `dlc <= 8`，参数错误返回 `-EINVAL`。ISO-TP 发送的帧必须令 DLC=8，并自行填充 0xAA。
- `can_driver_frame_sender()` 返回借用 driver 的 SocketCAN adapter。CAN 帧发送器的 context 和 `send()` operation 都由 ISO-TP 借用；内存 adapter 可在 harness 中替代 SocketCAN。
- RX/TX 十六进制日志和 `frames_rx/frames_tx` 只在成功收发后由 can_driver 更新。统计指针为只读借用，在 driver 对象生命周期内有效。
- `can_close()` 幂等，只关闭自己拥有的 CAN fd 并复位运行状态，不清空最终统计，也不关闭 epoll 或协议层 timerfd；下一次成功 `can_open()` 才重新清零统计。

## 2. ISO-TP 传输层规格（ISO 15765-2）

### 2.1 帧格式（经典 CAN，8 字节）

**SF 单帧**（数据长度 1-7 字节）：

```
Byte0: [0x0L]  高4位=0x0，低4位 L=数据长度(1..7)
Byte1..L: 数据
例: 02 10 03 AA AA AA AA AA   → 携带 2 字节数据 [0x10, 0x03]
```

**FF 首帧**（数据长度 8-4095 字节）：

```
Byte0: [0x1X]  高4位=0x1，低4位=总长度高 4 位
Byte1: 总长度低 8 位（总长度 = (Byte0 & 0x0F) << 8 | Byte1，最大 0xFFF = 4095）
Byte2..7: 前 6 字节数据
例: 10 14 62 F1 90 4C 56 55   → 总长 0x14=20 字节，本帧带 6 字节数据
```

FF 的 8 个字节全部由 PCI、长度字段和首段 payload 使用，因此不存在 FF padding
校验。FF_DL 是 12 位字段；线上可编码的最大值是 4095，不能构造“FF_DL
大于 4095”的经典 CAN FF。

**CF 连续帧**：

```
Byte0: [0x2N]  高4位=0x2，低4位 N=序列号 SN
Byte1..7: 最多 7 字节数据（最后一帧可不足，以剩余长度为准）
SN 规则: FF 之后第一个 CF 的 SN = 1，逐帧 +1，0xF 之后回绕到 0x0（F → 0，不是 F → 1）
```

**FC 流控帧**：

```
Byte0: [0x3S]  高4位=0x3，低4位 S=流控状态 FS
        FS=0 CTS（继续发送）/ FS=1 Wait（等待，发送方重置 N_Bs）/ FS=2 Overflow（中止）
Byte1: BS（0=不再限制，一次性发完；N=每发 N 个 CF 后等待下一个 FC）
Byte2: STmin（0x00-0x7F = 毫秒；0xF1-0xF9 = 100-900 微秒；其他值按 127ms 处理）
例: 30 00 0A ... → CTS, BS=0, STmin=10ms
```

### 2.2 发送状态机（TX，T005 目标）

```
                      isotp_send(len <= 7)
             ┌──────────────────────────────────┐
             │ 发送 SF；恢复 IDLE 后回调完成      │
             ▼                                  │
          ┌──────┐                              │
          │ IDLE │◄─────────────────────────────┘
          └──┬───┘
             │ isotp_send(len > 7)：复制 payload，发送 FF，启动 N_Bs
             ▼
        ┌─────────┐  FC(Wait)：重启 N_Bs
        │ WAIT_FC │──────────────────────┐
        └──┬───┬──┘◄─────────────────────┘
           │   │ FC(Overflow) / N_Bs 超时 / CAN 发送失败
           │   └──→ 清理 TX → IDLE → 回调错误
           │ FC(CTS)：停止 N_Bs，保存 BS/STmin，发送第一个 CF
           ▼
   ┌──────────────────┐  已全部发送：清理 TX → IDLE → 回调成功
   │ 判断发送后状态    │  已发满当前 BS：启动 N_Bs → WAIT_FC
   └────────┬─────────┘  仍可继续：启动 STmin → WAIT_STMIN
            ▼
      ┌────────────┐  STmin 到点：发送下一个 CF
      │ WAIT_STMIN │────────────────────────────→ 回到“判断发送后状态”
      └────────────┘
```

状态枚举固定为 `TX_IDLE / TX_WAIT_FC / TX_WAIT_STMIN`。所有完成路径先停止 TX timer、复位 offset/BS/SN 并进入 IDLE，再恰好调用一次 `on_tx_done(status)`。

收到 CTS 后第一个 CF 立即发送；之后的 CF 由 STmin timerfd 调度。逻辑上的
`STmin=0` 使用 1ns 的最小 timerfd tick 让出当前 epoll 回调，不能把零值直接
传给 `timerfd_settime()`（零值表示停用定时器）。

### 2.3 接收状态机（RX，T004 已实现）

```
     ┌──────┐  路由到 SF：长度和 padding 校验 → 提取数据 → on_message()，留在 IDLE
     │ IDLE │  路由到 FF：长度校验 → 存前 6 字节 → 回 FC(CTS, BS=0, STmin=0)
     └──┬───┘              → 启动 N_Cr → 进入 WAIT_CF
        │  路由到 CF：协议违例，忽略
        ▼
   ┌─────────┐  收到 CF 且 SN == expected_sn 且 padding 正确：
   │ WAIT_CF │    追加数据；若收齐 → on_message() → 回 IDLE
   └──┬──────┘    未收齐 → expected_sn = (expected_sn + 1) & 0xF，重置 N_Cr
      │  收到 CF 但 SN 不符：丢弃整条消息，回 IDLE（ISO-TP 无重传，由上层决定重试）
      │  收到新的 SF/FF：视为新传输，放弃当前重组，按 IDLE 处理该帧
      │  N_Cr 超时：丢弃缓冲，回 IDLE，统计 isotp_timeouts + 1
      ▼
```

状态枚举：`RX_IDLE / RX_WAIT_CF`。`expected_sn` 在收到 FF 后置 1。

接收端的输入策略是严格的：`isotp_on_frame()` 只处理目标 CAN ID 且
`DLC == 8` 的经典 CAN 数据帧。SF 和最后一个 CF 的有效数据之后必须全部是
`0xAA`；padding 不正确的帧被忽略，不改变当前 RX 状态，也不递增 SN 错误统计。
FF 没有未用字节。FF_DL 只接受 8..4095；0..7 是协议违例并被忽略。FC 的
Byte3..7 也必须为 `0xAA`；未知 FS 或非 `TX_WAIT_FC` 状态下的 FC 直接忽略。

### 2.4 帧路由与双向并行

`isotp_on_frame()` 先校验 CAN ID、DLC 和 PCI，再按 PCI 类型分派：

| PCI | 目标状态机 | 对另一状态机的影响 |
|---|---|---|
| FC | TX | 不读取或重置 RX 状态 |
| SF / FF / CF | RX | 不读取或重置 TX 状态 |
| 非法类型 | 无 | 忽略，不污染 RX/TX |

RX 与 TX 状态正交。即使 TX 正在 WAIT_FC/WAIT_STMIN，新请求仍可完成 RX 重组并触发 `on_message()`；若收到 FF，内部发送 FC 不经过 `isotp_send()`，也不占用业务 TX 状态。UDS 是否执行新请求由第 3.4 节的 busy 门禁决定。

### 2.5 流控策略

- 本 ECU 作为接收方（T004）：固定回复 `FC(CTS, BS=0, STmin=0)`，即不对发送方限速（vcan 回环足够快）。内部 FC 直接调用 CAN 帧发送器，不经过 `isotp_send()`，不占用业务 TX 状态。
- 本 ECU 作为发送方（T005）：必须完整实现对端 FC 的处理（CTS/Wait/Overflow、BS 计数、STmin 定时），因为 Python 测试端会用 can-isotp 的真实流控参数（如 BS=4, STmin=10ms）来考核。

### 2.6 超时参数

| 参数 | 含义 | 取值 | 本项目实现 |
|---|---|---|---|
| N_Bs | 发送方等待 FC 的最长时间 | 1000ms | tfd_bs，超时即中止 |
| N_Cr | 接收方等待下一个 CF 的最长时间 | 1000ms | tfd_cr，超时即丢弃 |
| STmin | 相邻 CF 发送间隔 | 由对端 FC 指定 | tfd_stmin |
| N_As / N_Ar | 单帧收发时间 | 1000ms | 用户态由内核保证，不单独实现，文档注明即可 |
| N_Br / N_Cs | 发送 FC/CF 的本地时间约束 | — | 用户态即时发送，不单独实现 |

### 2.7 错误处理汇总

| 场景 | 行为 |
|---|---|
| DLC != 8 | 忽略，不改变 RX/TX 状态 |
| PCI 类型非法 | 忽略，不改变 RX/TX 状态 |
| SF 长度 L = 0 或 L > 7 | 忽略 |
| SF 或最后 CF 的未用字节不是 0xAA | 忽略，不改变当前 RX 状态 |
| FF 声明长度 < 8（含 0..7） | 忽略（应走 SF，协议违例） |
| FF_DL 大于 4095 | 经典 CAN FF 的 12 位字段无法表示；不存在可在线注入的该值 |
| `isotp_send()` 传入 len > 4095 或 len = 0 | 返回 `-EINVAL`，不发送 |
| TX 进行中直接调用 `isotp_send()` | 返回 `-EBUSY`，不复制 payload、不触发完成回调；正常 UDS 路径必须在执行 handler 前通过 busy 门禁处理，而不是事后映射该错误 |
| CF 的 SN 错乱 | `sn_errors + 1`，丢弃整条重组消息，回 IDLE |
| 等待 CF 超时（N_Cr） | `timeouts + 1`，丢弃缓冲，回 IDLE |
| 等待 FC 超时（N_Bs）/ 收到 Overflow | 中止发送，`on_tx_done(ERR)` |
| FC Wait | 保持 `TX_WAIT_FC`，重新启动 N_Bs；本任务不限制 Wait 帧次数 |
| FC padding、FS 或状态无效 | 忽略，不改变 RX/TX 状态 |

### 2.8 接口规格

```c
#define ISOTP_MAX_LEN 4095u

typedef void (*isotp_rx_cb_t)(const uint8_t *data, uint16_t len, void *user);
typedef void (*isotp_tx_done_cb_t)(int status, void *user);

typedef struct {
    can_frame_sender_t frame_sender;
    uint32_t           rx_id;
    uint32_t           tx_id;
    isotp_rx_cb_t      on_message;
    isotp_tx_done_cb_t on_tx_done;
    void              *user;
} isotp_config_t;

typedef enum {
    ISOTP_TIMER_N_BS,
    ISOTP_TIMER_N_CR,
    ISOTP_TIMER_STMIN,
} isotp_timer_t;

typedef struct {
    uint64_t timeouts;
    uint64_t sn_errors;
} isotp_stats_t;

int                  isotp_init(isotp_ctx_t *ctx, const isotp_config_t *config);
void                 isotp_deinit(isotp_ctx_t *ctx);
int                  isotp_send(isotp_ctx_t *ctx, const uint8_t *data,
                                uint16_t len);
bool                 isotp_tx_busy(const isotp_ctx_t *ctx);
int                  isotp_on_frame(isotp_ctx_t *ctx, const can_frame_t *frame);
int                  isotp_timer_fd(const isotp_ctx_t *ctx,
                                    isotp_timer_t timer);
int                  isotp_on_timer(isotp_ctx_t *ctx, isotp_timer_t timer);
const isotp_stats_t *isotp_get_stats(const isotp_ctx_t *ctx);
```

`isotp_ctx_t` 在 `isotp.h` 中是可静态分配的具体类型，但其字段只允许 `isotp.c` 读写。内部 RX/TX 缓冲区均为 4095 字节。RX 字段含义为：`rx_len` 是已经写入的有效字节数，`rx_total_len` 是 FF 声明的完整长度，`rx_expected_sn` 是下一帧必须匹配的 SN。TX 字段含义为：`tx_total_len` 是完整 payload 长度，`tx_offset` 是下一个待发送偏移，`tx_next_sn` 是下一个 CF SN，`tx_block_size/tx_block_count` 表示当前 FC 块进度。发送侧解码后的 STmin 使用 `uint32_t tx_stmin_us` 保存，`0x00-0x7F` 转为 0-127000us，`0xF1-0xF9` 转为 100-900us，保留值转为 127000us。

生命周期与返回值契约：

- `isotp_init()` 借用 `config->frame_sender`，要求其 context 和 `send()` operation 非空，并校验回调和 11 位 ID，创建三个初始未激活的 timerfd。成功返回 0；失败返回负 errno，并回滚本次已创建的全部 timerfd。
- `isotp_timer_fd()` 返回由 ISO-TP 拥有的借用 fd，main 只注册、不读取、不关闭。`isotp_on_timer()` 负责读取并清空 expiration counter，再处理对应事件。
- `isotp_deinit()` 幂等，停止并关闭三个 timerfd、清空状态和缓冲区；不关闭 can_driver，不调用任何回调。
- `isotp_on_frame()` 对已处理或按协议忽略的帧返回 0；CAN 帧发送器等本地操作失败时返回负 errno，并先把受影响状态机恢复到可接收下一次传输的状态。

发送与回调契约：

- `isotp_send()` 仅接受 1-4095 字节。返回 0 表示完整 payload 已复制到内部 TX 缓冲，之后恰好产生一次 `on_tx_done()`；返回负值表示请求未被接受，不产生完成回调。
- SF 可以在 `isotp_send()` 内同步完成，因此 `on_tx_done()` 允许在该函数返回前发生。回调执行前 TX 必须已经 IDLE，回调可以再次调用 `isotp_send()`。
- `on_tx_done()` 的 status 为 0 表示发送完成；CAN I/O 或 TX timer 清理错误使用对应负 errno，N_Bs 超时使用 `-ETIMEDOUT`，FC Overflow 使用 `-EMSGSIZE`。
- `on_message()` 可以由 `isotp_on_frame()` 同步触发。data 指向内部 RX 缓冲，只在回调期间有效；回调不得保存或修改该指针。触发回调前 RX 必须已经停止 N_Cr 并恢复 IDLE。
- `isotp_tx_busy()` 仅反映业务 TX 状态。内部发送 FC 不令其变为 true；已完成同步 SF 后它返回 false。

数据路径禁止 malloc/free。

## 3. UDS 应用层规格（ISO 14229-1）

### 3.1 消息格式

```
请求:     [SID] [Sub-function(可选)] [参数...]
肯定响应: [SID + 0x40] [Sub-function 回显] [数据...]
否定响应: [0x7F] [请求 SID] [NRC]
```

Sub-function 字节的 bit7 为 suppressPosRspMsgIndicationBit：置位时不发肯定响应（否定响应照发）。0x10 与 0x3E 必须支持该位，分发器统一处理。

### 3.2 服务分发表与 NRC 检查顺序

busy 门禁通过后，分发器按以下固定顺序检查。结构长度检查必须先于访问 Sub-function/DID，避免短报文越界：

1. `len >= 1`；空 payload 防御性忽略，因为没有 SID 可用于否定响应。
2. SID 查表；不支持返回 `serviceNotSupported`。
3. 检查表项 `min_len..max_len`；范围外返回 `invalidFormat`，此后才允许读取服务字段。
4. 检查当前会话；不允许返回 `serviceNotSupportedInSession`。
5. 调用只读 validator 检查 Sub-function 和服务相关精确长度；失败返回对应内部结果。
6. 检查安全权限；未解锁返回 `securityDenied`。
7. 调用 handler 执行业务语义；DID 存在性、SecurityAccess 顺序和 Key 等结果由 handler 返回。

在扩展会话中，通过第 5 步的请求定义为“有效诊断请求”：进入第 6 步前重置 S3。因此安全拒绝或 handler 返回业务 NRC 的有效请求仍续期 S3；未知 SID、结构/格式错误和 busy 门禁拦截的请求不续期。suppress positive 不影响 S3 续期。

validator 严禁修改任何状态。handler 不直接发送响应，只写 UDS 肯定响应 payload，并返回内部 `uds_result_t`；`uds_engine` 是唯一把内部结果映射为 NRC 和组装 `7F [SID] [NRC]` 的位置。

请求视图、响应视图和函数指针契约：

```c
typedef struct {
    const uint8_t *data;
    uint16_t       len;
    uint8_t        sid;
    uint8_t        subfunction;
    bool           suppress_positive;
} uds_request_t;

typedef struct {
    uint8_t  *data;
    uint16_t  capacity;
    uint16_t  len;
} uds_response_t;

typedef enum {
    UDS_RESULT_OK = 0,
    UDS_ERROR_GENERAL_REJECT,
    UDS_ERROR_SERVICE_NOT_SUPPORTED,
    UDS_ERROR_SUBFUNCTION_NOT_SUPPORTED,
    UDS_ERROR_INVALID_FORMAT,
    UDS_ERROR_REQUEST_OUT_OF_RANGE,
    UDS_ERROR_SEQUENCE,
    UDS_ERROR_SECURITY_DENIED,
    UDS_ERROR_INVALID_KEY,
    UDS_ERROR_ATTEMPTS_EXCEEDED,
    UDS_ERROR_DELAY_NOT_EXPIRED,
    UDS_ERROR_SERVICE_NOT_SUPPORTED_IN_SESSION,
} uds_result_t;

typedef uds_result_t (*uds_validator_t)(const uds_request_t *request);
typedef uds_result_t (*uds_handler_t)(uds_ctx_t *ctx,
                                      const uds_request_t *request,
                                      uds_response_t *response);

typedef struct {
    uint8_t         sid;
    uint8_t         session_mask;       /* bit0=default, bit1=extended */
    bool            need_security;
    bool            has_subfunction;
    bool            supports_suppress;
    uint16_t        min_len;
    uint16_t        max_len;
    uds_validator_t validate;
    uds_handler_t   handle;
} uds_service_entry_t;
```

只有 `supports_suppress=true` 时 engine 才把 bit7 分离为 `suppress_positive`，并将 `request.subfunction` 规范化为低 7 位。0x27/0x19 虽然具有 Sub-function，但本项目不支持它们的 suppress bit。

服务表长度基线：0x10=`2..2`、0x3E=`2..2`、0x22=`3..3`、0x2E=`4..20`、0x27=`2..6`、0x19=`3..3`。0x27 和 0x2E 的精确长度由其只读 validator 根据 Sub-function 或已知 DID 再检查。

### 3.3 UDS 对外接口与生命周期

```c
typedef struct {
    uint64_t requests;
    uint64_t positive_responses;
    uint64_t negative_responses;
} uds_stats_t;

int                uds_init(uds_ctx_t *ctx, isotp_ctx_t *transport);
void               uds_deinit(uds_ctx_t *ctx);
void               uds_on_isotp_message(const uint8_t *data, uint16_t len,
                                         void *user);
void               uds_on_isotp_tx_done(int status, void *user);
int                uds_s3_timer_fd(const uds_ctx_t *ctx);
int                uds_on_s3_timer(uds_ctx_t *ctx);
const uds_stats_t *uds_get_stats(const uds_ctx_t *ctx);
```

`uds_ctx_t` 在 `uds.h` 中可静态分配，字段仅由 UDS 三个编译单元访问。它拥有会话/安全状态、S3 timerfd、4095 字节静态响应缓冲和一个 busy SID 槽；借用 `isotp_ctx_t *`，不释放传输层。

- `uds_init()` 创建并保持 S3 timerfd 为未激活状态，默认会话为 DEFAULT，安全状态为 LOCKED。失败返回负 errno并回滚自己的资源。
- `uds_s3_timer_fd()` 返回借用 fd；main 只注册。`uds_on_s3_timer()` 读取 expiration counter，执行回默认会话、清 seed 和重新上锁。
- `uds_deinit()` 幂等，停止并关闭 S3 timerfd、清空响应和 busy 槽，不发送响应、不触发 ISO-TP 回调。
- `uds_on_isotp_message()` 与 `uds_on_isotp_tx_done()` 直接作为 ISO-TP 回调。main 必须在进入 epoll 前完成两个 context 的初始化，退出时先 deinit UDS。
- handler 写入完整肯定响应 payload；失败时 response 内容被忽略。handler 可以执行协议明确要求的失败副作用，例如错误 Key 增加计数，但 validator 始终无副作用。
- engine 在 handler 返回后统一处理 suppress、NRC 和 `isotp_send()`。一旦非 busy 请求进入 handler，其协议副作用不因后续 CAN/ISO-TP 发送失败而回滚。

`uds_engine.c` 独占服务表、会话/安全状态、S3 和 busy 槽；`uds_services.c` 只实现 validator/handler。0x10 handler 只构造肯定响应，成功后的会话切换和安全复位由 engine 应用；响应发送失败不回滚这些协议副作用。`uds_data.c` 独占 DID/DTC 静态表并提供查找/读写辅助函数以及纯 `uds_compute_key()`。数据辅助函数不发送响应、不修改会话或安全状态，返回内部结果而不是 NRC 字节。

S3 生命周期固定如下：默认会话不启动定时器；成功的 `10 03` 或任何扩展会话中的有效诊断请求启动/重置 5000ms timerfd；成功的 `10 01` 和 S3 到期都停用定时器并回默认会话、重新上锁。有效请求定义为通过 validator 的请求，因此安全拒绝、业务 NRC 和 `3E 80` 会续期，而未知 SID、长度/子功能错误和 TX busy 请求不会续期。0x2E 端到端的 S3 降级断言属于 T008，T006 不引入临时 0x2E 服务。

```text
DEFAULT -- valid 10 03 --> EXTENDED + arm/rearm S3(5000 ms)
DEFAULT -- other valid --> DEFAULT  (S3 remains disarmed)
EXTENDED -- valid request --> EXTENDED + rearm S3(5000 ms)
EXTENDED -- valid 10 01 --> DEFAULT  + disarm S3 + LOCKED
EXTENDED -- S3 expiry --> DEFAULT  + disarm S3 + LOCKED
invalid / unknown / busy request --> no session or S3 change
```

### 3.4 TX busy 门禁与完成回调

`uds_on_isotp_message()` 收到完整请求后，第一项操作是调用 `isotp_tx_busy()`：

- TX idle：按第 3.2 节继续分发。
- TX busy 且 busy 槽为空：只保存 `request[0]` 原始 SID，立即返回。
- TX busy 且 busy 槽已占用：丢弃新忙请求，保持原槽不变。

busy 请求不查服务表、不调用 validator/handler、不重置 S3，也不改变会话、DID、安全状态或失败计数。它唯一允许的状态变化是占用 busy SID 槽。

`uds_on_isotp_tx_done()` 在当前 TX 成功或失败结束后检查 busy 槽。若存在 SID，必须先清槽，再调用 `isotp_send()` 发送三字节 `7F [SID] 21`。先清槽保证同步 SF 完成回调不会递归重复发送；发送调用失败时记录错误并丢弃，不重试。ISO-TP 保证调用 TX 完成回调前已回 IDLE，因此这里不应得到 `-EBUSY`。

### 3.5 服务清单与会话/安全矩阵

| SID | 服务 | Sub-function | 默认会话 | 扩展会话 | 需解锁 |
|---|---|---|---|---|---|
| 0x10 | DiagnosticSessionControl | 0x01 / 0x03 | ✓ | ✓ | 否 |
| 0x3E | TesterPresent | 0x00 | ✓ | ✓ | 否 |
| 0x22 | ReadDataByIdentifier | —（DID 参数） | ✓ | ✓ | 否 |
| 0x2E | WriteDataByIdentifier | —（DID 参数） | — | ✓ | 是 |
| 0x27 | SecurityAccess | 0x01 / 0x02 | — | ✓ | 否 |
| 0x19 | ReadDTCInformation | 0x02 | ✓ | ✓ | 否 |

### 3.6 0x10 DiagnosticSessionControl

| 项 | 规格 |
|---|---|
| 请求 | `10 01`（默认会话）/ `10 03`（扩展会话）；其他子功能 → NRC 0x12 |
| 肯定响应 | `50 01 00 32 01 F4` / `50 03 00 32 01 F4`（P2=0x0032=50ms，P2*=0x01F4，单位 10ms，即 5000ms） |
| 副作用 | 切换会话时：安全状态重置为 LOCKED，清除未完成的 seed；进入非默认会话时启动/重置 S3（5000ms） |
| S3 超时 | tfd_s3 到点：自动回退默认会话 + 重新上锁。任何有效诊断请求都会重置 S3 |

### 3.7 0x3E TesterPresent

| 项 | 规格 |
|---|---|
| 请求 | `3E 00` → 肯定响应 `7E 00`；`3E 80` → 不响应但重置 S3 |
| 长度 | 必须为 2 字节，否则 NRC 0x13 |

### 3.8 0x22 / 0x2E 与 DID 数据表

请求格式：`22 [DID_H] [DID_L]` / `2E [DID_H] [DID_L] [data...]`
肯定响应：`62 [DID_H] [DID_L] [data...]` / `6E [DID_H] [DID_L]`

| DID | 名称 | 长度 | 初始值 | 读 | 写 |
|---|---|---|---|---|---|
| 0xF190 | VIN | 17 | ASCII `LVUDS2026DEMO0001` | 任意会话 | 扩展会话 + 解锁 |
| 0xF189 | ECU 软件版本 | 10 | ASCII `vUDS-1.0.0` | 任意会话 | 只读 |
| 0x0100 | 冷却液温度 | 2 | 0x0055（85°C，模拟值） | 任意会话 | 只读 |
| 0x0101 | 发动机转速 | 2 | 0x0FA0（4000 RPM，模拟值） | 任意会话 | 只读 |
| 0x0200 | 采样周期配置 (ms) | 2 | 0x0064（100） | 任意会话 | 扩展会话 + 解锁 |

校验规则：

- 0x22 长度必须恰好 3 字节（本实现每次只读 1 个 DID，属声明过的简化），否则 NRC 0x13。
- DID 不存在 → NRC 0x31；写只读 DID → NRC 0x31。
- 0x2E 数据长度必须与 DID 定义长度一致，否则 NRC 0x13。
- 0x2E 在默认会话 → NRC 0x7F；未解锁 → NRC 0x33。
- VIN 响应共 20 字节（3+17），必然走多帧传输，是 ISO-TP TX 的天然测试用例。

T008 实现使用静态 DID 表保存五项数据；0xF190 VIN 和 0x0200 采样周期可写，其他三项只读。写入由 validator 先校验 DID、权限和精确长度，handler 仅在 engine 已通过扩展会话和安全权限检查后修改静态值。

### 3.9 0x27 SecurityAccess

状态机：

```text
LOCKED --27 01--> SEED_SENT --correct key--> UNLOCKED
   ^                  |  ^                     |
   |                  |  +--wrong key (1/2)---+
   |                  +--wrong key (3)--> LOCKED + lock_until(10 s)
   |                                               |
   +---------------- requestSeed after expiry ----+

session switch / S3 expiry --> LOCKED + invalidate seed
new requestSeed             --> SEED_SENT + replace seed
```

| 项 | 规格 |
|---|---|
| 请求 Seed | `27 01` → `67 01 [4 字节随机 Seed]` |
| 发送 Key | `27 02 [4 字节 Key]` → 正确 `67 02`；错误 `7F 27 35` |
| 顺序错误 | 未先请求 Seed 直接 `27 02` → `7F 27 24` |
| Seed 生成 | `/dev/urandom` 取 4 字节，不全为 0x00/0xFF；每次 requestSeed 重新生成并覆盖旧 seed |
| Seed 生命周期 | 解锁成功 / 会话切换 / 新的 requestSeed 时失效 |
| 失败锁定 | 连续 3 次 Key 错误 → 本次返回 `7F 27 36`，锁定 10 秒（`CLOCK_MONOTONIC` 惰性判断）；锁定期间 requestSeed 返回 `7F 27 37`；期满后首个 requestSeed 清零失败计数 |

失败计数在成功解锁或锁定期满后的首个 `requestSeed` 时清零；会话切换和 S3 到期只使当前 seed/解锁态失效，不得通过切换会话绕过尚未到期的锁定截止时间。`requestSeed` 每次都覆盖旧 seed，seed/key 均按大端在线序编码。

```c
uint32_t uds_compute_key(uint32_t seed) {
    uint32_t key = seed ^ 0xA5A5A5A5u;
    key = (key << 3) | (key >> 29);          /* 循环左移 3 位 */
    key = key + 0x3C6EF35Fu;                 /* 取低 32 位 */
    return key;
}
```

实现与 Tester 必须逐字节一致，seed/key 在线序上均为大端。该算法用于演示挑战-响应流程与防爆破机制，不追求密码学强度；真实 ECU 通常使用 AES 或 OEM 私有算法。

### 3.10 0x19 ReadDTCInformation

仅实现子功能 0x02（reportDTCByStatusMask），其他子功能 → NRC 0x12。

请求：`19 02 [statusMask]` → 肯定响应：`59 02 [DTCStatusAvailabilityMask=0x09] { [DTC 3字节] [status] }*`

| DTC | status | 模拟含义 |
|---|---|---|
| 0x010203 | 0x09 | 发动机过热（当前失败 + 已确认） |
| 0x020304 | 0x08 | 变速箱转速传感器故障（已确认） |
| 0x030401 | 0x01 | ABS 轮速传感器间歇故障（当前失败） |

status 位定义：bit0=testFailed，bit3=confirmedDTC。仅返回 `status & statusMask != 0` 的 DTC；无匹配时返回空列表（仍为肯定响应）。

T009 实现使用三条静态 DTC 记录，数据辅助函数只编码匹配的 3 字节 DTC 编号和 1 字节状态；handler 负责写入 `59 02 09` 头部，engine 统一处理长度、会话和 NRC 分发。

### 3.11 NRC 总表

| NRC | 名称 | 本项目触发场景 |
|---|---|---|
| 0x10 | generalReject | 兜底（不应出现） |
| 0x11 | serviceNotSupported | SID 不在服务表 |
| 0x12 | subFunctionNotSupported | 如 `10 02`、`19 05` |
| 0x13 | incorrectMessageLengthOrInvalidFormat | 长度/格式校验失败 |
| 0x21 | busyRepeatRequest | TX 忙时收到新请求；请求不执行，响应经单槽待发区在 TX 完成后补发 |
| 0x24 | requestSequenceError | 未请求 Seed 直接发 Key |
| 0x31 | requestOutOfRange | DID 不存在 / 写只读 DID |
| 0x33 | securityAccessDenied | 未解锁执行 0x2E |
| 0x35 | invalidKey | Key 校验失败 |
| 0x36 | exceededNumberOfAttempts | 连续 3 次 Key 错误 |
| 0x37 | requiredTimeDelayNotExpired | 锁定 10 秒内请求 Seed |
| 0x7E | subFunctionNotSupportedInActiveSession | 预留（扩展用） |
| 0x7F | serviceNotSupportedInActiveSession | 默认会话中请求 0x2E/0x27 |

## 4. 测试矩阵

### 4.1 ISO-TP（`tests/test_isotp.py`）

| 编号 | 用例 | 预期 |
|---|---|---|
| IT-01 | 发送 7 字节 payload | 对端收到单 SF，数据一致 |
| IT-02 | 发送 1 字节 payload | SF，L=1 |
| IT-03 | 发送 8 字节 payload | 走 FF+CF×1 |
| IT-04 | 接收 20 字节多帧（can-isotp 发送） | ECU 回 FC 后正确重组，on_message 数据一致 |
| IT-05 | 收发 4095 字节（协议上限） | 完整传输成功 |
| IT-06 | 注入 SN 跳变的 CF | ECU 丢弃整条消息，不上交 |
| IT-07 | FF 后停发 CF | N_Cr 超时后状态复位，随后新 FF 可正常接收 |
| IT-08 | 对端 FC 设 BS=4, STmin=10ms | ECU 每 4 个 CF 停等下一个 FC，帧间隔 ≥5ms |
| IT-09 | 对端先发 FC(Wait) 再 FC(CTS) | 暂停后正确续传 |
| IT-10 | 对端 FC(Overflow) | 中止发送，TX 错误回调触发 |
| IT-11 | `isotp_send(0)` / `isotp_send(4096)` | 返回错误，不发送 |

### 4.2 UDS（`tests/test_uds.py` / `test_security.py`）

| 编号 | 用例 | 预期 |
|---|---|---|
| UT-01 | `10 03` | `50 03 00 32 01 F4` |
| UT-02 | `10 02` | `7F 10 12` |
| UT-03 | `3E 00` / `3E 80` | `7E 00` / 无响应且 S3 被重置 |
| UT-04 | 扩展会话静置 5.5s 后发 `2E ...` | `7F 2E 7F`（S3 已自动回退默认会话） |
| UT-05 | `22 F1 90` | `62 F1 90` + 17 字节 VIN（多帧响应） |
| UT-06 | `22 12 34` | `7F 22 31` |
| UT-07 | `22 F1`（长度错误） | `7F 22 13` |
| UT-08 | 默认会话发 `2E 02 00 00 C8` | `7F 2E 7F` |
| UT-09 | 扩展会话未解锁发 `2E 02 00 00 C8` | `7F 2E 33` |
| UT-10 | 完整流程：10 03 → 27 解锁 → 2E 写 → 22 读回 | 全部肯定响应，读回值=写入值 |
| UT-11 | 直接 `27 02 ...`（未请求 Seed） | `7F 27 24` |
| UT-12 | 错误 Key | `7F 27 35` |
| UT-13 | 连续 3 次错误 Key 后立刻 `27 01` | 第 3 次 `7F 27 36`；随后 `7F 27 37` |
| UT-14 | `19 02 FF` | `59 02 09` + 3 条 DTC |
| UT-15 | `19 02 08` | 仅返回 status 含 bit3 的 2 条 DTC |
| UT-16 | `19 05 00` | `7F 19 12` |
| UT-17 | `99 00`（未知 SID） | `7F 99 11` |
| UT-18 | 多帧写：构造 >7 字节的 0x2E 请求（扩展 DID） | 走 FF/FC/CF 完整流程，写入成功 |
| UT-19 | `10 83`（suppress bit） | 会话切换成功但无响应报文 |
| UT-20 | `22 F1 90` 后先不发 FC，TX 忙中插入 `10 01`，再补发 FC | VIN 多帧响应完整；TX 完成后补发 `7F 10 21`；插入的请求未被执行 |

### 4.3 异常注入与健壮性（手动 + 脚本）

| 编号 | 用例 | 预期 |
|---|---|---|
| FT-01 | 传输中 `ip link set vcan0 down` | ECU 干净退出且错误日志清晰，接口恢复后重新启动可继续 |
| FT-02 | 发送 DLC=0 / 垃圾帧 | 忽略，无状态污染 |
| FT-03 | Valgrind 跑 10 分钟混合流量 | 无泄漏、无非法访问 |
| FT-04 | ASan/UBSan 构建跑全部 pytest | 零报错 |

## 5. 典型报文交互示例

### 5.1 单帧请求/响应（`10 03` 切扩展会话）

```
Tester → ECU : 7E0# 02 10 03 AA AA AA AA AA      (SF, 2 字节)
ECU → Tester : 7E8# 06 50 03 00 32 01 F4 AA      (SF, 6 字节: P2=50ms, P2*=5000ms)
```

### 5.2 多帧响应（`22 F1 90` 读 VIN，响应 20 字节）

```
Tester → ECU : 7E0# 03 22 F1 90 AA AA AA AA      (SF)
ECU → Tester : 7E8# 10 14 62 F1 90 4C 56 55      (FF: 总长 0x14=20, 带 6 字节 "LVU")
Tester → ECU : 7E0# 30 00 00 AA AA AA AA AA      (FC: CTS, BS=0, STmin=0)
ECU → Tester : 7E8# 21 44 53 32 30 32 36 44      (CF SN=1: "DS2026D")
ECU → Tester : 7E8# 22 45 4D 4F 30 30 30 31      (CF SN=2: "EMO0001")
```

VIN = `LVUDS2026DEMO0001`（17 字节 ASCII），6+7+7=20，末帧恰好无填充。

### 5.3 否定响应（默认会话写 DID）

```
Tester → ECU : 7E0# 05 2E 02 00 00 C8 AA AA      (SF)
ECU → Tester : 7E8# 03 7F 2E 7F AA AA AA AA      (NRC 0x7F: 当前会话不支持该服务)
```

## 6. 已知偏差与限制（与 ISO 标准对照）

以下条目均为**有意取舍**：实现与本文件规格完全一致且有测试锁定，不是缺陷，
但与 ISO 标准的推荐做法或常见实现存在差异。逐条给出标准侧说法、本项目行为
和影响，作为对外讲解与后续评估是否放宽的事实基准。

1. **接收帧验收严格性（ISO 15765-2）**：本项目只接受 DLC=8 且未用字节均为
   0xAA 的帧（第 1、2.7 节），`strict` 场景锁定该行为。标准接收端一般被要求
   同时接受未填充的短 DLC 帧，且填充字节值并不强制为 0xAA（各 OEM 常见
   0x00/0xAA/0xCC）。收紧的收益是畸形帧在传输层即被拒绝、可测边界明确；
   代价是与默认配置的 `cansend`/`isotpsend` 或任意非 0xAA 填充对端不互通。
   配套 Python Tester 固定 `tx_padding=0xAA`，互通不受影响。放宽只需调整
   padding 校验，列为 stretch。

2. **FC Wait 无次数上限（ISO 15765-2）**：每个 FC Wait 都重臂 1000ms N_Bs，
   不限制 Wait 帧次数（第 2.5、2.7 节）。标准未强制上限，但多数实现限制
   Wait 总次数或总时长。影响：异常对端每 <1s 发送一个 Wait 即可无限期保持
   TX busy，并经 UDS busy 门禁阻塞所有响应；单 Tester 的 vcan 场景下风险
   可忽略。

3. **UDS busy 单槽（本项目行为，ISO 14229 未规定）**：TX 忙期间只有第一个
   到达的请求被暂存并在 TX 结束后补发 `7F [SID] 21`，其余忙请求被静默丢弃
   （第 3.4 节）。影响：多请求同时到达时除首个外无任何响应，且这些请求计入
   `uds_requests` 统计但没有对应的正/负响应。单诊断连接顺序处理场景下可接受。

4. **NRC 检查顺序：长度先于会话（ISO 14229-1）**：分发顺序为查表(0x11) →
   长度(0x13) → 会话(0x7F) → 子功能 → 安全（第 3.2 节）。标准的 NRC 报告
   优先级示例将会话支持检查置于格式检查之前，但措辞并非强制。本项目让长度
   检查先行，是为了在读取服务字段前确保报文结构完整、避免越界。可观察差异：
   默认会话发送 1 字节 `27` 返回 `7F 27 13` 而非 `7F 27 7F`。

5. **锁定期内 `27 02` 返回 0x24（ISO 14229-1）**：第三次错误 Key 清除 seed
   并进入锁定期后，`27 02` 因无有效 seed 命中顺序检查返回 0x24（第 3.9 节
   状态机）。常见实现在锁定期内对任何 0x27 子功能统一返回 0x36/0x37。本项目
   语义与"仅 SEED_SENT 状态允许 sendKey"自洽，锁定期内 `27 01` 仍正确返回
   0x37，锁定语义不受影响。

6. **STmin 保留值解释为 127ms（ISO 15765-2）**：STmin 0x80-0xF0 为标准保留
   值，本项目统一按 127ms（最大合法毫秒值）处理（第 2.1、2.8 节）。标准未
   定义该区间行为，常见实现取 0 或 127ms，本项目取后者以保持发送节奏保守。
