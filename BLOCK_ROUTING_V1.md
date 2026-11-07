# BRBF v1：基于预留与瓶颈反馈的 KV Block 选路

BRBF：Block Reservation and Bottleneck Feedback。

本文是 BRBF 第一版设计与实现说明。公共 block 子 QP、父共享控制器、交换机预留和真实链路反馈已接入当前 astra-sim/ns-3；默认模式仍是最基本的 ECMP。每请求一个独立 KV SEND/RECV，保留计算 batching。第 13 节记录实际接口、实现限制及验证范围；第 11 节的完整研究验收不能仅以短测试代替。

**阅读顺序（2026-11-06 对照源码补充）：先读第 14–16 节，理解算法为什么不保证性能逐层改善；再读第 17–20 节，沿一次请求掌握实现；最后读第 21–23 节，核对修改清单、实验与未完成事项。**第 1–13 节保留参数、协议和验收规范；其中“必须”“不得”等表述是约束，不等于已经通过全部验证。下文明确区分实现事实、设计动机、待验证假设。

## 1. 部署范围与研究目标

部署目标：当前 astra-sim/ns-3，静态 leaf-spine 网络。允许各端口使用不同的非零带宽与非负链路延迟；100 Gbps、1 us 是默认实验配置而非算法限制。交换机逐包转发，自主决策，不使用全局队列查询、集中调度器或零延迟远端状态。

第一版不承诺商用 ASIC 线速可部署，不处理故障重路由或循环下一跳路由。放开异构链路不等于证明任意拓扑上的精确 ECT。链路故障、控制消息长期丢失和传输异常必须可观测，不能伪造完成。

研究目标是减少整份 KV 完成时间的 mean、P95/P99，并观察 TTFT 的相应变化。优化目标不是让某个 block 的 FCT 小于原来的大流 FCT。

三个确定的改进机制：

1. 以 KV block 为路径固定周期，而非以整份 KV 大流为周期。
2. 首包决策时登记 block 尚未排入队列的工作量，避免只看到已经积累的 Q。
3. 沿反向拓扑传播定量瓶颈工作量，而非只有 LDLB 的拥塞/解除标签。

不预先承诺优于 LDLB。该结论必须由同环境对比及消融得出。

## 2. 第一版固定参数

| 参数 | 固定值 |
|---|---|
| KV block 载荷 M | 2,097,152 B，即 2 MiB |
| 单包 KV 载荷 MTU | 4000 B |
| 数据优先级 | 原有 pg=3 |
| 原有拥塞控制 | CC_MODE=1，DCQCN |
| 拥塞控制归属 | 一个 ParentTransfer 一个控制器 |
| 每 ParentTransfer 活跃 block 上限 | 4 |
| 每源主机活跃 block 总上限 | 32 |
| 周期反馈间隔 | 16 us |
| 反馈最短发送间隔 | 4 us，邻接链路分别限频 |
| 事件触发阈值 | 相比上次发送，任一目的的瓶颈工作量变化 >= M/8 |
| 反馈有效期 | 64 us，按报告生成时间计算 |
| 选路近似平局阈值 epsilon | 5 us |
| 控制队列 | queue 0 |
| 反馈控制流量预算 | 每邻接发送方向 0.5% 链路容量，令牌桶容量 2 MTU |
| 单 ParentTransfer 无进展告警 | 10 ms 仿真时间 |
| 单 ParentTransfer 无进展终止 | 100 ms 仿真时间；记录失败，不记录成功 FCT |
| Block 完成墓碑保留 | 2 ms；ID 不复用 |
| 第一版随机种子 | 42，写入运行清单 |

无进展指该 ParentTransfer 的累计确认载荷（包括填充）未增加，不是“请求尚未完成”。尚未获得源端活跃额度的父传输不计为网络停滞。终止阈值是可见的运行保护，不是提前完成机制。若合法 PFC 暂停超过此值，该实验将以失败结束，必须报告原因。

模型输入或 block 大小改变时，必须建立新实验配置，不在运行中改变上述大小。

## 3. 传输对象、填充与完成语义

### 3.1 三种 ID

RequestID：serving 请求的 ID。

ParentTransferID：一个 `(RequestID, KV 传输轮次)` 的唯一 64 位 ID。不再以 `(prefill batch, decode 目的端)` 聚合不同请求。当前完整 prefill KV 基线，每请求只传输一次，对应轮次 0。

BlockKey：`(ParentTransferID, RequestID, kv_epoch, block_index)`。`kv_epoch` 区分同请求后续的 KV 增量，所有 ID 在同一运行中不得复用。

当前每请求基线使用 `comm_tag = (1<<20) + RequestID` 匹配独立 SEND/RECV。BRBF 扩展到多轮增量时，还需独立 ParentTransferID，不能让同一请求的不同传输轮次共用唯一身份。

### 3.2 真正等大的 block

当前基线先完成请求的全部 prefill，再一次性传输整份 KV；中间 prefill 块不发 KV。BRBF 第一版使用同样的发射条件。以下增量范围字段为后续兼容预留，第一版 kv_epoch=0、kv_range_offset=0。

对于请求应发送的 V 字节有效 KV 范围：

```
N = ceil(V / M)
每个 block 的传输载荷 = M
最后一个 block 的 valid_bytes = V - (N - 1) * M
其余 block 的 valid_bytes = M
```

尾块填充字节不参与模型计算，但占真实链路带宽。每个 block 属于一个请求，不将两个请求的尾部拼成一个跨请求 block。

每个 epoch 的 kv_range_offset 指明该范围在请求 KV 中的有效字节起点。写入地址为 `kv_range_offset + block_index*M + payload_offset`，仅写 valid_bytes 覆盖的部分，填充不写入 KV。若 prefill 分多轮产生 KV，不能假定 block_index 在整请求范围内连续，也不能在一个 epoch 完成时宣布整份 KV 完成。

四种公共扩展比较模式使用相同的填充、数据包头和 ParentTransfer 元数据；基础 ECMP 不包含这些扩展。MEGA_ECMP 大流基线仍将一个父对象固定到一个 ECMP 路径，但承载与 block 模式相同的有效数据和填充。

当前 sps20 在“每请求一次发送全部 KV”的条件下，按请求填充得到 79,046 个 block；有效 KV 为 165,570,936,832 B，填充后载荷为 165,771,476,992 B，增加约 0.121%。若实际存在多个 kv_epoch，必须按真实 epoch 计划重新统计，不能沿用这个估计。填充不是控制/包头开销，两者分别统计。

### 3.3 完成屏障

在三个 block 模式中，每个 block 使用独立、可靠的 RDMA 子 QP，序号从 0 到 M。不同 block 允许乱序完成；同一 block 不允许随包换路。Mega-ECMP 使用父 QP 的累计序号，不把父 ACK seq 错当成某个子 block 的 seq。

设计上的 KV 写入位置由 BlockKey 和 offset 确定。当前网络仿真使用占位载荷模拟字节传输，不保存或写入真实的模型 KV 张量。父对象必须等所有子 block 完成才能触发原来的 AstraSim SEND/RECV 回调，且传给原 MsgEvent 的计数字节为有效 KV 字节，不是含填充/包头的链路字节。

通用多 epoch 设计需要 RequestKVTracker 校验有效范围覆盖原有 D 请求计划中的全部所需 KV 字节，无缺口；仅累计收到的字节总数不够，重叠范围不得重复计数。**当前没有独立的通用 RequestKVTracker 实现。**第一版通过接收子 QP 的可靠累计序号和父对象 completed block 集合判断一次整份传输是否完成，再由原 SEND/RECV 的有效字节匹配完成任务图。接收父描述来自 DATA，后续检查其一致性，不是在接收首包前预建一个通用范围计划。不能将这套实现直接解释为支持多 epoch 范围覆盖。

每个请求使用自己的 KV 完成屏障，不再按网络目的端合并。计算 batching 保留：若一个 decode 计算 batch 包含多个请求，其计算仍依赖这些请求各自的 RECV 节点。第一版不额外拆计算 batch，也不把未完整收到 KV 的请求提前释放。

需要区分两个测量：

- `KV_NET_FCT`：该请求/父对象第一个 KV 数据包真正发出，到 D 主机已收到全部所需 block 载荷的 t_rx_complete。
- `KV_READY_DELAY`：KV 可发送时刻到 t_rx_complete；包括源端排队和 block 窗口等待。
- `KV_COMMIT_DELAY`：KV 可发送时刻到源端最后一个所需 ACK；用于解释保持原 AstraSim 完成屏障后的调度延迟。

主评价同时报告前两个量，第三个作完成协议辅助量。Mega 和 block 模式都用接收端的 BlockKey/offset 覆盖记录计算 t_rx_complete，不以“一个父 ACK”和“多个子 ACK”作为不同的主指标终点。不得通过延后第一个数据包发出、缩短计时窗口来获得虚假的 FCT 改善。

## 4. RDMA 子 QP 不得额外获得带宽

每 ParentTransfer 保持一套 DCQCN 状态：

- rate、targetRate、alpha，以及更新和恢复定时器。
- 聚合的未确认载荷字节窗口 W，计入填充载荷，沿用父对象的原始 BDP/VarWin 规则。
- 聚合 nextAvail，按每个实际发出的数据包占用一次速率预算。

子 QP 保留自己的 seq、ACK、重传、block 完成状态，但不能各自创建一套 DCQCN 定时器或独立放大 W。

任一子 QP 的 CNP 更新父控制器。数据包可发条件为：

```
block 未完成
父传输未确认字节总量 < 父传输 W
父 nextAvail <= now
NIC 当前可发
```

NIC 首先在父对象之间做原有公平轮转，然后在父对象的活跃 block 之间轮转。每主机 32 个子 QP 的总额度也按父对象轮转分配，不让第一个父对象长期占满。

父窗口耗尽时不创建忙等事件。ACK 推进窗口时唤醒 NIC；等待 rate 时只保留一个有效的 nextSend 事件。

这部分必须在 Block-ECMP、Block-Local、BRBF 三种模式中完全相同。与 Mega-ECMP 比较时，Mega QP 的控制器也是同样的父控制器，不能比较“1 个窗口”和“4 个独立窗口”。

## 5. 数据与 ACK 元数据

每个 KV DATA 包和 ACK/NACK 携带固定 72 B 的 KV 路由头，按网络字节序序列化；不能只使用 ns-3 PacketTag 模拟免费元数据。

| 字段 | 字节 |
|---|---:|
| version、flags、header_len | 4 |
| ParentTransferID | 8 |
| RequestID | 8 |
| parent_valid_bytes | 8 |
| kv_range_offset | 8 |
| kv_epoch | 4 |
| block_index | 4 |
| blocks_in_epoch | 4 |
| parent_block_count | 4 |
| payload_offset | 4 |
| valid_bytes | 4 |
| route_cookie | 4 |
| comm_tag | 4 |
| reserved | 4 |
| 合计 | 72 |

DATA 的 offset 是该包在 block 中的载荷偏移。ACK 的进度仍使用原 RDMA ACK seq，元数据提供 BlockKey。

父对象在源端创建并得到唯一 ID。接收端首次看到该父对象的 DATA 时，用 parent_block_count 和 parent_valid_bytes 建立完成屏障；后续所有包的父字段必须一致。以 BlockKey 去重，完成时检查有效字节之和与父字段一致。它不要求 D 的计算 batch 与 P 的发送 batch 一一对应；原有 RECV 仍按 comm_tag/源/目的的有效字节记账。

route_cookie 初始为无效值。源 leaf 首次选路后写入所选 spine ID；同一 block 的后续包均写入缓存的同一个 cookie。

目的主机回显 cookie，目的 leaf 的 ACK/NACK 选择 cookie 指定的 spine。spine 返回源 leaf，再返回源主机。这样 ACK 沿同一交换机路径返回，各交换机都能看到最终 block ACK。

对于 Mega-ECMP，同一父对象的全部 block 使用同一 cookie；Block-ECMP 按 block 的子 QP 五元组选择 cookie；BRBF 使用下述算法。ACK 不参与新的自适应选路。

flags 明确区分 MEGA_PARENT 和 CHILD_BLOCK。Mega 的最终 ACK 是 parent 的填充载荷总字节，整个父对象才结束 pin；子 QP 的最终 ACK 是 M。不能对 Mega QP 错套 ACK seq=M 的完成规则。

当前无 INT 的序列化包头为 36 B，再增加 72 B 路由头，因此一个 2 MiB block 的预计单链路字节：

```
packet_count = ceil(M / 4000) = 525
wire_block = M + packet_count * 108 = 2,153,852 B
wire_block / (100 Gbps / 8) = 172.30816 us
```

设计要求是根据实际序列化长度生成 header_bytes。当前 `MakeKvContext()`、`CommitKvPacket()` 和扩展 standalone FCT 的估算使用 `36 + KvBlockHeader::SIZE`，即上述 108 B；实际包生成及 R 扣减使用实际包长。这个估算依赖当前固定 DATA 格式，不是任意协议头的自动适配实现。

## 6. 交换机本地状态与精确记账

每个物理出口维护：

- Q：已经排队、尚未出队的链路字节，包含真实包头、背景数据及 ACK。
- S：正在序列化的包剩余链路字节；端口空闲时为 0。
- R：已决定走这个出口、但尚未成功入队的原始 block 链路字节。
- B：物理出口带宽，单位 B/ns。

Q 以实际队列入/出事件更新；不得把 MMU egress_bytes 再与单独 Q 累加。S 由 tx_start、预计 tx_end 和速率计算，不能把正在发的整包再次保留在 Q 中。

R 只是工作量账本，不申请 2 MiB 的交换机数据缓冲，也不要求收齐 block 才转发。按原始数据包偏移的去重 bitmap 维护，一个 2 MiB block 有 525 个偏移槽。每个槽在首次成功入队时，从 R 减去对应真实链路长度；随后该包增加 Q。

重传/重复包仍增加实际 Q，但不能再次扣减 R。控制和 ACK 包不建立 R，其实际服务计入 Q/S。预留值只覆盖 block 后续原始 DATA 包，不预留未知重传和 ACK 数量。

### 6.1 Block 首次决策的事务顺序

```
解析 BlockKey
如果存在 ACTIVE/DRAINED pin：沿 pin 转发，不重新选路
否则：
    计算候选出口和 cost，暂选 p
    执行已有 ingress/egress admission check
    如果 admission 失败：
        记录 drop，不建立 pin，不增加 R
        交给原可靠传输恢复，不能假完成
    如果 admission 成功：
        建立 pin，预计总预留 wire_block
        当前包偏移槽标为已入队
        R[p] += wire_block - 当前包链路长度
        Q[p] += 当前包链路长度
        原 MMU 记账保持不变
        在首包真正送入出口队列前，原子提交以上状态
```

首次被观察到的包不一定 offset=0：若首包丢失，后续包也能建立 pin，未见的 offset=0 仍保留在 R 中，等待重传。

### 6.2 pin 状态机

- ACTIVE：R > 0，仍有尚未成功入队的原始 DATA。
- DRAINED：本交换机原始 DATA 已全部成功入队，R=0，但仍保留路径等待可靠完成；Q/S 中可以仍有未服务完的数据。
- DONE：看到 ACK seq=M，原 block 已可靠完成。保留同一端口墓碑 2 ms，用于迟到重复包；不再持有 R。

以上是语义状态；代码没有 ACTIVE/DRAINED/DONE 枚举，使用 `KvPin.remaining` 和 `expiry` 表示。Mega 完成目标为整个父载荷，不是 M。

ACTIVE/DRAINED 不因反馈 TTL 或普通 idle TTL 直接释放 R。当前实现遇到父传输超时会刷新日志并终止整个仿真进程，不产生成功回调；没有实现可继续运行的显式 ABORT 清理协议。若后续支持单父传输失败后继续仿真，必须先加入真实 ABORT 消息和逐交换机状态清理。

DONE 墓碑在数据包访问时和周期维护时清理。不得为每个 packet 新建清理定时器。

## 7. 定量瓶颈反馈

目的组 g 定义为最终 decode 主机的出口类。第一版 8 个 D 使用 8 个目的组，不把所有 D 盲目合成一个 leaf 组，因为不同主机出口的排队可能不同。

只在预先计算的最短路径 DAG 中选择候选 next-hop，不修改原拓扑可达性，不在拥塞时删除可达路径。

### 7.1 局部虚拟工作量

```
w_s(p,t) = [Q_s(p,t) + S_s(p,t) + R_s(p,t)] / B_s(p)
```

它是按当前承诺工作量计算的虚拟服务时间，不是对“当前真实队列等待”的精确测量。特别是 R 对应数据可能受源端 pacing 限制而尚未到达，因此不能声称它是严格实际完成时间。

实现统一使用整数 ns 并向上取整：`w_ns = ceil(8 * bytes * 1e9 / link_bps)`。`ServiceNs()` 使用 128 位乘法中间量；Q/S/R 和 cost 使用 uint64。静态传播累加有溢出检查，但当前并未对 Q+S+R、cost 和所有 R 加法逐项实现显式溢出检查，不能将其描述为全链路算术检查已完成。S 根据正在发送的包和 tx_end−now 计算，不能随 packet header 采样得到负值。

### 7.2 后缀瓶颈代理

目的主机后缀压力为 0。

交换机对 g 计算：

```
H_s(g,t) = min over reachable p:
             max(w_s(p,t), aged_H_next(p,g,t))
```

目的 leaf 只有一个通向 g 的主机出口，H 就是这个出口的工作量。spine 综合其通向目的 leaf 的出口与目的 leaf 的反馈。源 leaf 再综合自身出口与 spine 的反馈。

使用 max 而不是简单累加，避免把流水传输中同一个 block 在各交换机的整块预留机械加算多次。该量是瓶颈代理，不是普遍准确的多跳 ECT：串行等待、将来到达和共享资源仍会造成误差，必须记录预测误差。

计算通告 H 的多个出口恰好同值时，选择端口编号最小者，F 必须对应这个同一个出口。通告不使用 BlockKey 的随机平局规则，因为它尚未面对具体 block。

为避免把尚未到达的预留当成已经被链路服务的队列，每条 H 还携带预留下限 F。F 使用本次计算 H 所选的同一个出口：

```
F_s(g,t) = max(R_s(p_selected,t) / B_s(p_selected),
               fresh_F_next(p_selected,g,t))
```

必须有 0 <= F <= H。目的主机的 H/F 都为 0。接收侧用：

```
age = now - generated_ns
aged_H = max(F_reported, H_reported - age)
```

这只允许可排空部分随时间衰减，不让未到达的预留自动消失。F 保持报告值直到新快照更新；旧 F 可能保守高估，最长反馈有效期内由刷新修正。不能认为老报告已经知道新到达的流量。

第一版不对 Q/R 做 EWMA，不将突发的整块预留抹平；稳定性通过限频、版本、有效期和平局规则实现。

### 7.3 真正经过链路的反馈协议

使用仅相邻交换机处理的 KV_LOAD 控制包。自定义 IPv4 protocol 固定为 0xFA，在 CustomHeader 中显式支持序列化/解析。

反馈走反向邻接链路 queue 0，经历真实排队、序列化及传播延迟。不能用 Simulator::Schedule(0) 直接写远端表。

对目的 g，仅向“距 g 更远、且可以把本交换机作为 next-hop”的邻接交换机报告。每跳基于本地状态和缓存重新计算 H/F，不原样转发收到的快照，避免反馈环路。该方向由静态最短路径 DAG 确定，不查询远端实时负载。

每邻接报告包含：

| 固定报告头字段 | 字节 |
|---|---:|
| version、type、header_len | 4 |
| sender ID | 4 |
| boot epoch | 8 |
| report_seq | 8 |
| generated_ns | 8 |
| fragment_index、fragment_count、record_count、reserved | 8 |
| CRC32 | 4 |
| 合计，不含 IPv4/PPP | 44 |

每目的组记录固定为 group ID 4 B、H_ns 8 B、F_ns 8 B、oldest_component_ns 8 B、valid_flags 4 B，共 32 B。CRC32 覆盖固定头（CRC 字段清零）与记录体；同时检查长度、分片编号和记录数，不调用有副作用的 C++ 对象序列化。

目的 leaf 的本地记录标为 FRESH，oldest_component_ns 为本次生成时间。每个中间交换机只有在所有必需后缀记录均有效时才能标为 FRESH，并继承所选后缀的最老组成观测时间。若只能计算 local fallback，则 valid_flags 清除 FRESH，不能用一张刚生成的报告把过期的下游观测洗成“新鲜”。

当前 8 个 D 时，单报告线路字节约为 `44+8*32+20+2=322 B`。16 us 周期对应约 161 Mbps，约占一条 100 Gbps 链路的 0.161%；分片或协议头变化时必须按实际大小重新统计。

报告按当前网络的 D 主机数分包，每包不超过 MTU；一个报告组内的分片必须全部到达，才原子更新缓存。旧 boot epoch、旧 report_seq 或生成时间晚于接收时间的非法记录直接拒绝并计数。

记录大小和所有包头用 GetSerializedSize 计算，控制流量预算使用真实线上字节，不使用对象 sizeof。

设计目标是在状态变化后合并刷新。当前在交换机普通包入队、出队和反馈缓存更新时调用 `MarkKvDirty()`，安排一个合并事件；没有为 S 的每次连续减少建立事件。事件执行时重算向量，H 相比上次提供的向量变化达到 M/8 对应服务时间，或者 FRESH 标志变化时创建待发报告；受 4 us 限频和令牌桶约束。16 us 的周期报告覆盖其他刷新。

当前每个交换机共用一个事件报告定时器、一个周期定时器和一个 pending 发送重试定时器；邻接端口分别维护限频及令牌桶。逐包钩子若发现事件定时器已经存在，不再次 Schedule。完整 H/F 向量在定时器执行时重算，不为每个 packet 扫描所有目的。

### 7.4 反馈过期策略

如果本次候选集合中任一必需的后缀记录缺失、未标为 FRESH、报告 age >64 us 或 `now−oldest_component_ns >64 us`，本次决策对整个候选集合统一退化为本地工作量模式。不能把缺失路径当成“下游 0 等待”，而同时给其他路径保留非零反馈。

恢复完整的新鲜反馈后自动恢复完整模式。统计 fallback 次数、比例、最长持续时间。

第一版不加入跨源 leaf 的远端预预约协议。因此，不承诺在一个反馈传播窗口内完全避免不同源 leaf 的同步追逐；这是必须测试的反馈滞后边界，不得用仿真全局变量隐藏。

## 8. 唯一的选路规则

当前静态拓扑的第一版规则（带宽和传播项按候选端口分别计算）：

```
如果全部所需反馈新鲜：
    pressure[p] = max(w_local[p], aged_H_next[p,g])
否则：
    pressure[p] = w_local[p]

cost[p] = pressure[p] + wire_block / B[p] + static_path_propagation[p,g]
min_cost = min(cost)
ties = { p | cost[p] <= min_cost + 5 us }
p_star = ties[ stable_hash(seed, switch_id, BlockKey) mod len(ties) ]
```

ties 必须按稳定物理端口编号排序，不能按对象指针顺序排序。stable_hash 固定为 FNV-1a-64：初始值 14695981039346656037，逐字节先 XOR 再乘 1099511628211，按 uint64 自然取模。输入依次为 seed:u64、switch_id:u32、ParentTransferID:u64、RequestID:u64、kv_epoch:u32、block_index:u32，字段均为网络字节序。不能使用 Python hash 或 std::hash 的跨运行不稳定行为。

`B[p]` 读取该 QbbNetDevice 的实际速率。`static_path_propagation[p,g]` 在路由表建好后缓存：从该端口 QbbChannel 读取当前链路延迟，再加下一跳到目的端的最小静态路由传播延迟；直连目的主机时仅计当前链路。初始化只读取静态拓扑和路由，不读取远端实时队列，运行时策略只读取缓存。若下游存在多个候选，该最小后缀是估计值，并不保证下游实际选路沿该最短延迟后缀。

H/F 仍表示拥塞工作量的时间，不混入静态传播项，避免把传播延迟随报告年龄衰减。此改动不扩展原 ECMP 路由表的候选集合，也不改变 ACK cookie 的 leaf-spine 范围；不能据此宣称支持任意深度拓扑或精确预测下游慢链路的空载 block 完成时间。

不在已建立 pin 的 block 中应用 cost，不因拥塞或 feedback 更新改动正在传输的 block。下一 block 独立计算。

所有出口都忙时仍选择有限 cost 最小的可达出口。不因为拥塞标签而丢弃 block。真实故障没有可达路径时，显式失败。

第一版不加入请求优先级、最后一个 block 优先级、SRPT 或额外队列调度，以免把调度收益混入选路收益。

## 9. 代码接入点

| 文件/组件 | 规定改动 |
|---|---|
| `serving/core/trace_generator.py` | 每请求独立有效字节计划；完整 prefill 后发射；保持请求到达时的 P/D 选择；不能只保留 peer 总字节而丢掉 RequestID |
| `serving/core/graph_generator.py` / Chakra 转换器 | 加载仓库转换器；逐请求生成有效 KV 字节的 SEND/RECV，写入请求属性和配对标签；ParentTransferID 在 RDMA 后端创建，不从图传入 |
| `network_frontend/ns3/entry.h` | KV 父对象创建、窗口化子 QP、整份完成屏障、原 MsgEvent 有效字节回调；非 KV 原行为不变 |
| `rdma-hw.*` / `rdma-queue-pair.*` | 子 QP 到 ParentTransfer 的引用；共享 DCQCN/pacing/window；ACK 元数据回显和正确唤醒 |
| 新 `src/network/utils/kv-routing-header.*` | 实际文件名；72 B DATA/ACK 元数据与 KV_LOAD 控制报告真实序列化 |
| `custom-header.*` | 解析 KV 元数据与 KV_LOAD；不能改变 ECN 位或覆盖原 ACK seq |
| `switch-node.*` | 首包 pin、R/去重 bitmap、cost、候选排序、反馈缓存；原 ECMP 保留为可选模式 |
| `qbb-net-device.*` | tx_start/complete 更新 S；KV_LOAD 在普通转发前消费；ACK 按 cookie 返回；queue0 保留真实开销 |
| `switch-mmu.*` | 原 Q/PFC/ECN 机制不替换；新的算法不将 MMU egress_bytes 重复记账 |

算法模式固定为：

```
MEGA_ECMP
BLOCK_ECMP
BLOCK_LOCAL
BRBF_V1
```

LDLB 在同一 KV/传输框架内单独实现，记录其具体版本、flow pinning、阈值、反馈、无可用路径行为，不能用论文描述替代实际实现。

LDLB 对照只保留 MEGA_LDLB（父流 pin，对应原有大流粒度）。以 KV block 为选路单位是本项目作者提出的设计，属于本项目的创新点；LDLB 基线保持请求级，不加入 block 粒度适配。

### 9.1 实施后的配置接口

以下字段已由后端解析。模板位于 `astra-sim/extern/network_backend/ns-3/scratch/config/config.txt`；未填写的字段使用下列默认值。Python 生成独立运行配置，并从后端注册表校验策略名称；C++ 校验参数和传输条件，未知 `KV_` 字段直接失败：

```
KV_ROUTING_MODE BRBF_V1
KV_BLOCK_BYTES 2097152
KV_PARENT_BLOCK_WINDOW 4
KV_HOST_ACTIVE_BLOCKS 32
KV_LOAD_REFRESH_NS 16000
KV_LOAD_MIN_GAP_NS 4000
KV_LOAD_TTL_NS 64000
KV_ROUTE_TIE_NS 5000
KV_CONTROL_BUDGET_PPM 5000
KV_BLOCK_TOMBSTONE_NS 2000000
KV_PARENT_WARN_NS 10000000
KV_PARENT_TIMEOUT_NS 100000000
KV_ROUTE_SEED 42
```

Python 仍生成每请求整份 KV 的 SEND/RECV 计划；C++ 根据同一运行配置将它展开为 ParentTransfer 和子 QP。Chakra 图不为每个 block 增加 SEND/RECV，只有父传输完成才触发原图的完成回调。

沿用 `python -m serving` 的原命令入口；`--kv-routing-policy BRBF_V1` 覆盖本次运行的 `KV_ROUTING_MODE`。不指定时沿用模板，当前模板为 ECMP。`--request-routing-policy RAND` 只负责 P/D 请求分配，不是交换机算法开关。现有 DCQCN、buffer、拓扑字段保持独立，不由 BRBF 模式偷偷修改。

### 9.2 当前已实施的每请求 ECMP 基线

正常 serving 的 KV trace 使用 `peer_npu_offset:request_id:bytes`，保留同批、同目的端的不同请求。Chakra 在最后一个 KV 层生成各自的整份 SEND，在 decode 第一层生成各自的整份 RECV；配对标签为 `(1<<20)+request_id`。

prefill 计算仍可分块、合批，但本轮只实现“完整 prefill 后一次传输”，不实现中间计算块的流式 KV 发射。正常 TP=1、PP=1 的一次完整 KV 请求对应一个 QP；TP/PP 配置可能产生多个 rank 分片，不能承诺所有配置的 QP 数都等于请求数。

在 `ECMP` 模式中，每个 QP 的源/目的 IP 和端口在生命周期中固定，原有 ECMP 哈希将整份请求 KV 固定到同一路径。该模式不使用 block 子 QP、72 B 数据头、填充和反馈。`MEGA_ECMP` 则在公共扩展传输框架中保留整份固定路径，用于和 block 模式作受控对比。

`ns3/output/fct.txt` 保留原八列格式。新增的 `ns3/output/kv_fct.csv` 当前列为：

```
request_id,src,dst,sport,bytes,start_ns,end_ns,fct_ns,standalone_fct_ns
```

ECMP 的 start_ns 是请求 QP 创建时刻；扩展模式的 start_ns 是父传输 ready 时刻。end_ns 均为源端最终确认完成时刻，fct_ns=end_ns−start_ns。该兼容文件不是接收端完成指标；扩展模式的完整时间口径见 `kv_transfers.csv`。请求 ID 解码仅适用于新请求级 trace，旧 peer:bytes 格式的标签仍按层序号生成，不能将其附属 CSV 解释成请求 ID。

图生成器优先加载仓库里的 `llm_converter.py`，其 protobuf schema 等依赖继续使用安装包。修改该转换器后不再需要重装整个 Chakra 包，新的模拟器进程即可读取仓库版本。

## 10. 运行与统计接口

扩展模式在本次运行的 `ns3/output/` 输出如下文件；基础 ECMP 只保留原有统计与 `kv_fct.csv`：

- `block_fct.csv`：request_id、transfer_id、block、有效/填充载荷、父 ready、block first_tx/rx_complete/last_ack。MEGA_ECMP 不输出独立子 block 行。尚无重传次数和逐 block 线上字节列。
- `kv_transfers.csv`：每父传输一行，含有效/填充字节和 ready/first_tx/rx_complete/last_ack；`kv_net_fct_ns=rx_complete−first_tx`，`kv_ready_delay_ns=rx_complete−ready`。`KV_COMMIT_DELAY=last_ack−ready`；额外确认尾延迟为 `last_ack−rx_complete`，两者不要混用。
- `kv_fct.csv`：保持第 9.2 节的兼容格式，不将其终点混称为接收端完成。
- `route_decisions.csv`：记录每个 block 在每个交换机首次成功入队时的全部候选 Q/S/R、H/F/年龄、cost、选择出口、fallback；不逐包记录。目前没有采样开关，大实验应注意日志体积。
- `feedback_stats.csv`：真实发送控制包/字节，以及接收 commit/fragment/reject/old 事件。TTL 降级在 route_decisions 中体现；尚无专门预算等待计数。
- `port_stats.csv`：每 100 us 记录发生变化或有工作量的端口累计发送字节、Q/S/R；利用率需按累计字节差分计算。
- `kv_manifest.json`：后端模式、block、父/主机额度、刷新/TTL、种子、72 B 元数据。
- `ns3/kv_run_manifest.json`：Python 写入策略、可用注册策略、运行配置和拓扑 SHA256。当前未记录完整 workload 哈希及软件版本，正式实验应另外保存负载和版本。

请求 CSV 中原 TTFT、latency、TPOT 继续保留。mean FCT/P95/P99 在相同父对象或请求上统计；block FCT 仅作辅助。

上游 pin 选择到 block 完成的实际路径和重传也要能对应起来，不能只由哈希推算代替真实选路记录。

## 11. 强制验收

### A. 正确性

1. 每个请求的 sum(valid_bytes) 等于原有 KV 计划；每个 block 传输 M 载荷。
2. 所有扩展对比模式的填充、元数据、父控制器和父活跃额度相同；基础 ECMP 是明确标注的兼容基线，不强行加入这些改动。
3. 每个 block 在每个交换机只有一个数据出口；反馈变化不改 pin。
4. R 从不为负；同一原始包重复入队不重复扣 R；完成时残余 R 为 0。
5. Q 出队转 S、S 完成清零；无 Q/S 同时记同一服务字节。
6. 强制两个 block 逆序到达，父对象必须等两者都可靠完成。
7. 父 SEND 和 RECV 回调各一次，回调有效字节一致。
8. 超过 2 GiB 的父传输通过子 QP 正常完成，无符号截断、无零载荷循环。
9. 源端子 QP 端口编号只有在 ACK/QP 清理且 2 ms 墓碑期结束后才能复用；端口池不足时暂停发行 block，禁止 16 位整数自动绕回。BlockKey 不复用。
10. DCQCN 的定时器数量随活跃父对象而非全部历史 block 数增长。

### B. 可见性和降级

1. 在反馈中人为加入真实 32/64/128 us 延迟，观察过期和统一 local fallback。
2. 交换反馈的到达顺序，旧 report 不覆盖新 report。
3. 丢失一片报告，不得部分更新不同目的组的混合版本。
4. 控制包必须在真实接口发送字节计数中出现。
5. 所有候选出口拥塞但仍可达时，算法不得自行丢包。
6. 传输看门狗触发时保留失败清单，不能计入成功 FCT。

### C. 受控性能验收

1. 空载、小流：检查没有显著无负载退化。
2. 同源 leaf 突发四个 block：验证原子 R 更新打破连续错误分配。
3. 多源 leaf 同时突发：测量反馈窗口内残留碰撞，不声称完全消除。
4. 本地无队列而下游某 spine 路径受背景流竞争：比较 BRBF 与 BLOCK_LOCAL。
5. 热点迁移：记录转移后新 block 的响应时间，旧 block 不迁移。
6. 复用 sps20 的相同负载计划和种子；比较相同父/请求的整体 KV 完成时间。
7. 多种子重复；报告均值和置信区间，不只保留有利种子。

第一版是否优于 LDLB，以这些测试实际结果决定，不能只用一条示例碰撞宣布成功。

## 12. 本版不作的承诺

- 不证明全网 max−min <=1 block。该界只在初始工作量相同、等速端口、无下游差异、顺序贪心分配且不混入不同服务过程的简化模型中成立。
- 不把 cost 称为严格 ECT。它是基于承诺工作量和反馈的瓶颈完成时间代理，必须测量误差。
- 不承诺低负载、不可绕开的目的主机瓶颈或所有路径等价时仍有提升。
- 不免费调用邻居状态，也不免费增加 ACK/控制包。
- 不把 block QP 的 FCT 与 Mega QP 的 FCT 直接相比。
- 第一版对比集限定每个父对象的填充载荷 <=4 GiB−1，保持现有 Mega QP 的 32 位序号可表示范围。超限时所有模式统一拒绝该实验配置，不通过让 Mega 基线失败来制造收益。扩大这一范围需要另行实现可公平比较的序号绕回或共同传输分段。
- 不将仿真器实现等同于商用交换机已经可运行。

## 13. 使用与扩展

### 13.1 已注册模式

| 名称 | 传输粒度 | 选路 |
|---|---|---|
| ECMP | 每请求整份 KV，原传输 | 原五元组 ECMP，不增加反馈/填充 |
| MEGA_ECMP | 公共父传输，单大 QP | 整份固定 ECMP 路径 |
| BLOCK_ECMP | 2 MiB 子 QP，父共享 CC | 每 block 固定 ECMP 路径 |
| BLOCK_LOCAL | 同上 | 本地 Q+S+R，不读取后缀反馈 |
| BRBF_V1 | 同上 | 本地预留和真实瓶颈反馈；过期统一 local fallback |

默认每父最多 4 个活跃 block、每主机最多 32 个；所有扩展模式同时活跃父请求上限为 `32/4=8`，MEGA_ECMP 也遵守这个父额度。这样不会因拆成多个 QP 而获得更多独立 DCQCN 份额，但不意味着并行 block 与单大 QP 的所有行为都相同。

### 13.2 命令

以下命令在已配置依赖的仿真容器、仓库根目录执行。不会自动运行长实验：

```bash
python3 scripts/test_kv_routing.py --skip-build
astra-sim/extern/network_backend/ns-3/build/scratch/ns3.42-AstraSimNetwork-default \
  --list-kv-routing-policies=1
```

不加 `--skip-build` 时测试脚本先增量编译；测试结果保存在容器 `/tmp/kv-routing-tests-*`。正式运行示例：

```bash
mode=BRBF_V1
python3 -m serving \
  --cluster-config configs/cluster/dual_node_8p8d.json \
  --network-backend ns3 --kv-routing-policy "$mode" \
  --dataset workloads/kv_poisson_sps5.jsonl --num-reqs 50 \
  --max-num-batched-tokens 65536 \
  --request-routing-policy RAND --pd-pairing dynamic \
  --keep-inputs --run-id "kv_v1_${mode}_sps5" \
  --output "outputs/kv_v1_${mode}_sps5.csv"
```

分别将 mode 改为 ECMP、MEGA_ECMP、BLOCK_ECMP、BLOCK_LOCAL，使用不同 run-id，保留原 sps20 输出。严谨消融优先比较四种公共扩展模式；基础 ECMP 用来观察相对于原系统的整体变化。

### 13.3 新算法接入

策略接口：`astra-sim/extern/network_backend/ns-3/src/point-to-point/model/kv-routing-policy.h`。

1. 继承 `KvRoutingPolicy`，实现 `Transport()` 和 `Select(const KvRoutingContext&)`。
2. 需要公共瓶颈反馈时覆盖 `UsesFeedback()`；可覆盖 `Advertise()`。自定义 LDLB 标签协议需另加真实控制头/事件，不能把公共 H/F 反馈冒充原 LDLB。
3. 通过 `KvRoutingPolicy::Register("MY_POLICY", factory)` 注册，在 point-to-point 的 CMake SOURCE_FILES 加入实现文件后编译。
4. 后端注册表自动列出新名字，Python 不使用写死的 argparse choices；直接运行 `--kv-routing-policy MY_POLICY`。
5. 使用同一公共传输与统计接口，不复制父完成、填充、CC、端口回收逻辑；新增策略必须先通过自己的正确性和公平性测试。

`KvRoutingContext` 只提供本交换机候选出口视图与收到的反馈，不提供远端实时队列对象。策略返回不可达端口、未知策略名称或未知 KV 配置会失败，不会悄悄退回 ECMP。LDLB 现已增加独立标签协议与策略，版本和适配边界见第 24 节。

### 13.4 实际支持范围

- 静态 leaf-spine、4000 B DATA 载荷、DCQCN；允许异构非零端口速率，不支持链路故障重路由。新增速率必须在 KMIN_MAP、KMAX_MAP、PMAX_MAP 中配置相应 ECN 参数；反馈周期、TTL、watchdog 也应随实验 RTT 和速率检查。
- cost 的传播项按候选出口读取并累加静态路由后缀，不再固定 3000 ns。默认 1 us 的三跳路径仍为 3000 ns；不同后缀延迟可以改变候选排序。它是静态最小后缀估计，而非实际下游选择路径的保证。
- 反馈变化阈值由反馈发送端口的实际速率换算，不再按固定 100 Gbps 换算；令牌桶预算原本即按端口实际速率补充。
- `route_decisions.csv` 末尾新增 `bandwidth_bps` 和 `propagation_ns`，用于核验每个候选的实际带宽与传播项。H/F 反馈格式不变。
- 反馈按所有可路由主机 IP 报告，目前包含 16 台主机，不仅 8 台 D。不同目的 IP 的缓存独立。
- 交换机 boot 版本固定为 1，暂不支持运行中重启。
- 所有扩展模式父填充载荷必须 <=4 GiB−1；基础 ECMP 保持原有无填充语义及原 RDMA 单流限制。
- 接收完成时间的全局关联表只用于输出统计，不参与选路、反馈计算或源端发行控制。
- 当前 serving 任务图会串行释放同一发送 rank 的不同请求 KV SEND；每请求独立不等于同源多个父传输同时在途。本轮未改变这个计算/通信依赖关系。网络传输层支持并发父对象，使用直接 RDMA 测试验证其额度和共享控制机制。
- 当前短测试不等于第 11 节全部通过；反馈延迟/分片丢失注入、超过 2 GiB 的扩展父传输和多种子性能对比仍需专项验证。用户完成的长 sps20 结果另见第 22 节。

### 13.5 本轮验证范围

后续带宽/传播项修改新增：异构速率与每候选延迟的策略单元检查；网络测试增加 400 Gbps 主机口、100 Gbps spine 口以及仅后缀延迟不同的场景。本地 C++/Python 语法检查和独立策略/协议单元测试已通过；完整网络测试尚未重新运行，需在构建容器中执行 `python3 scripts/test_kv_routing.py`。以下记录为此前版本验证结果，不能代替此次修改的端到端验证。

已分别验证：

- 后端增量编译通过；注册表可查询五个内建模式。
- 单元检查：本地 R 影响决策、后缀瓶颈选择、过期统一降级、F 下限不随时间消失、父共享窗口、72 B DATA/ACK 头、CRC 损坏拒绝、自定义策略注册，以及禁止覆盖基础 ECMP。
- 基础 ECMP 的 DATA 不带 KV 元数据；共享 DATA/ACK 序列化长度和关键字段往返一致；相同短负载重复运行的请求级 FCT 完全一致。这是协议兼容与确定性检查，不是与旧二进制逐数值回归。
- 五个模式各 3 请求完整完成；扩展模式每请求 5 个 block，四种扩展模式填充字节一致。block 模式共 15 行，有效字节守恒、每交换机 pin 单出口、时间戳顺序正确。
- BRBF 真实控制包发送和接收 commit 均出现；其他四个模式不发送 KV_LOAD 反馈。
- 原 8P8D 下 96 请求全部完成，每源 12 个请求；接收/父完成记录与请求数一致。
- 独立网络测试在 t=0 同源同时创建 12 个父传输，MEGA_ECMP 和 BRBF_V1 活跃父对象上限均为 8，第 9 个必须等待额度，12 个父完成回调各一次。
- 未知 KV 配置、未知策略和扩展模式误用 CC_MODE=12 均明确退出；空 trace 计数头明确拒绝，不能继续使用未初始化数据或悄悄回退到 ECMP。

测试脚本默认组合运行以上检查。只验证并发网络传输可执行：

```bash
python3 scripts/test_kv_routing.py --skip-build --network-only
```

重复回归暴露并修复了一个既有启动问题：Python 曾生成空的 FLOW_FILE/TRACE_FILE，而网络初始化未检查计数读取是否成功，trace_num 可能未初始化，进而在 trace 节点列表循环中耗时极长。现在这两份辅助文件明确写入 `0` 计数头，后端拒绝非法计数头和 trace 节点 ID。这项修复不改变数据流大小或 ECMP 哈希；它发生在仿真开始之前。

上述“未启动长实验”的描述属于此前短测试阶段。2026-11-06 已核对用户完成的 sps20 全量批次，结果见第 22 节；这不补齐故障注入、多 trace 或 LDLB 对比验收。

## 14. 先理解：机制递进不等于性能必然递进

### 14.1 你的直觉在什么条件下成立

“能看到更多状态，应该作出更好的决策”是合理的算法设计方向。如果新增信息完全准确、包含真正影响目标的所有资源，决策器还能比较所有后续结果，并且新增机制没有开销，那么它可以选择沿用旧决策，理论上不必变差。

当前实现没有满足这些条件：

1. R 是已选路径的未来工作量，不是当前真实排队时间；这些字节何时到达受源端窗口和 pacing 影响。
2. H/F 是已经经过链路传播的快照，不是当前下游状态。
3. cost 只估计一个新 block 的出口服务和后缀瓶颈，没有计算所有请求的完整剩余完成时间。
4. 选路会改变后续队列、ECN/CNP、父 DCQCN 速率、block 完成次序和任务图释放时间，旧策略下的状态轨迹不会原样保留。
5. 反馈占真实链路服务，且块内 pin 使一次不合适的选择无法立刻纠正。

因此，当前算法是**机制上的消融阶梯**，不是一个具有单调性能定理的算法序列。

### 14.2 BLOCK_LOCAL 并不是“先执行 BLOCK_ECMP，只有确认更好才换”

源码 `kv-routing-policy.cc` 中：

```cpp
// BLOCK_ECMP
return {c.ecmpPort, false, 0};

// BLOCK_LOCAL / BRBF_V1
pressure = ServiceNs(Q + S + R, bandwidth);
if (feedback_enabled && all_candidates_fresh)
    pressure = max(pressure, aged_downstream_H);
cost = pressure + ServiceNs(wireBytes, bandwidth) + propagation;
// 在接近最小 cost 的集合中，用另一套稳定哈希选择。
```

BLOCK_LOCAL 直接重新计算候选集合；即使候选全部等价，也使用 FNV-1a 的 BlockKey 哈希，而 BLOCK_ECMP 使用原交换机的五元组 ECMP 哈希。这意味着路径序列本来就可能不同。

BRBF 同样没有“新规则的真实 FCT 不优于 BLOCK_LOCAL 就回滚”的机制。它只在反馈不完整或过期时统一退回本地代价；有效但不准确的反馈仍会影响选择。

### 14.3 对称拓扑不等于所有路径在每个时刻等价

“对称”描述静态带宽和传播延迟。动态状态仍可以不同：

- 多个源请求的启动时间不同，block 会短暂撞到同一出口。
- 不同目的主机对应不同的最后一跳；共享一个目的出口的流量无法靠换 spine 消除。
- 父速率下降后，其预留数据到达更慢，R 在多个端口残留的时间也会变化。
- 反馈在不同端口的排队与到达时间不同。

对称实验仍可以体现负载感知价值，前提是存在足够的、可绕开的动态争用。但“链路都一样快”本身既不保证收益，也不保证新增策略无退化。

### 14.4 当前 cost 是 Block 代理目标，研究指标是请求目标

对于一次整份 KV：

```text
网络接收完成时刻 = max(所有 block 的接收完成时刻)
源端可靠完成时刻 = max(所有 block 的最终 ACK 时刻)
```

还要注意 block 不是全部在 ready 时同时启动，而是最多 4 个活跃、完成一个再补一个。早期 block 变慢，会延迟后续 block 的发行。

当前策略没有请求剩余 block 数、哪个 block 位于最后完成的关键路径、其他父请求的目标 FCT 或目的主机共享瓶颈的完整模型。让眼前一个 block 的估算 cost 更小，不等于让整份 KV 的最大完成时刻更小，更不等于全体请求的 P99 更小。

P95 和 P99 也不必同向：一项策略可以改善大多数请求，同时让少数请求变慢。symmetric 的结果已经表现出这个现象，不能用均值近似重合掩盖尾部退化。

## 15. 五种策略到底分别做了什么

### 15.1 共同框架与策略差异

| 模式 | RDMA 传输 | 首次 DATA 选路 | pin 周期 | 读取 R | 读取下游反馈 | 发 KV_LOAD |
|---|---|---|---|---|---|---|
| ECMP | 原每请求 QP，无填充/扩展头 | 原五元组哈希 | 原 QP 固定五元组 | 否 | 否 | 否 |
| MEGA_ECMP | 扩展父框架，一个大数据 QP | 原五元组哈希 | 整份父载荷 | 否 | 否 | 否 |
| BLOCK_ECMP | 扩展父框架，窗口化子 QP | 每子 QP 五元组哈希 | 单 block | 否 | 否 | 否 |
| BLOCK_LOCAL | 同 BLOCK_ECMP | 本地 Q/S/R + 本地服务 + 静态传播 | 单 block | 是 | 否 | 否 |
| BRBF_V1 | 同 BLOCK_ECMP | 本地代价与收到的后缀反馈组合 | 单 block | 是 | 是，有效时 | 是 |

实现细节：所有扩展模式都会在交换机建立 pin 和 R 账本，包括 MEGA_ECMP、BLOCK_ECMP。后两者的 `EcmpPolicy::Select()` 不读取这些代价。因此，“记录了 R”不等于“使用 R 选路”。

BLOCK_LOCAL 同时使用 Q、S、R；当前没有独立的“只用 Q/S、不用 R”内建策略。用 BLOCK_ECMP→BLOCK_LOCAL 的结果，只能评价本地感知组合的收益，不能单独量化 R 的净收益。

### 15.2 三个有意义的消融问题

1. MEGA_ECMP→BLOCK_ECMP：保持扩展包头、填充、父额度与父 CC，考察粒度/子 QP 传输组织的影响。仍有子序号、ACK 完成和发行方式的差异，不能称为只改了一个哈希字段。
2. BLOCK_ECMP→BLOCK_LOCAL：保持 block 传输，考察本地 Q/S/R 贪心选路与不同平局哈希的组合影响。
3. BLOCK_LOCAL→BRBF_V1：保持本地预留及 block 传输，考察真实 H/F 反馈、其开销和有效性判断的净影响。

ECMP→MEGA_ECMP 是原系统与公共扩展框架的校准比较。它们的元数据、填充和完成实现不同，不应跳过校准后把所有变化都归因于选路。

### 15.3 为什么分路不能让一个请求凭空获得 400G

当前 symmetric 的源主机口和目的主机口都是 100G。一个请求即使使用四个 spine，也受源 NIC、目的 NIC 及父控制器约束。四条路径主要提供避开其他请求争用的机会，并不把一个 100G 主机变成 400G。

从路由角度，block pin 是避免一个可靠子 QP 随包换路；从传输角度，子 QP 让不同 block 可以独立可靠完成。两者合作实现 block 分路，但不能把子 QP 当作四个独立拥塞控制器。

## 16. 代价函数逐项推导与数值例子

### 16.1 单位与本地代价

代码中的带宽存储为 bit/s，字节到服务时间的换算为：

```text
ServiceNs(bytes, bps) = ceil(bytes × 8 × 10^9 / bps)
w_local[p] = ServiceNs(Q[p] + S[p] + R[p], bps[p])
local_cost[p] = w_local[p] + ServiceNs(wire_block, bps[p]) + D[p,g]
```

`ServiceNs()` 使用 `unsigned __int128` 做乘法中间量并向上取整。新父对象字节数、R、时间、代价等使用 uint64；单包长度、block 索引、部分原 ns-3 队列接口和线上序号仍是较窄类型。当前没有把整个原 ns-3 所有计数类型全部改为 uint64，仍需遵守单包边界及父载荷上限。

Q 是设备所有队列的实际排队字节，S 是当前包剩余序列化字节，R 是这个端口所有已 pin 传输尚未成功入队的原始 DATA 字节。Q/S/R 是物理端口级共享工作量；不是只统计当前目的 g 的工作量。

当前 block 的预计线路字节为 2,153,852 B：

| 带宽 | 当前 block 本地服务时间（向上取整） |
|---|---:|
| 40G | 430,771 ns ≈ 430.77 us |
| 100G | 172,309 ns ≈ 172.31 us |
| 400G | 43,078 ns ≈ 43.08 us |

因此 local40g 的慢口即使 Q/S/R 都为 0，本地服务项也比 100G 口大约多 258.46 us。**本地瓶颈实验同时验证了速率感知与工作量感知，不能把全部收益归为排队检测或 R。**

### 16.2 首包预留为什么有意义

假设四个等速出口都没有 Q/S，四个 block 的首包紧接着到达：

- 若只看真实队列，每次可能只看到一个几 KB 的包，未意识到它后面还跟着约 2 MiB 的数据。
- 第一个 block pin 到 p9 后登记其剩余 R，第二个 block 决策就会看到 p9 约一个 block 的承诺工作量。
- 本地贪心因而更倾向 p10/p11/p12。

这是一种在首包阶段提前承认后续工作量的方法。它没有在交换机存储整块，也不保证从现在起链路能连续服务整块。

### 16.3 BRBF 的下游项为什么用 max

有效反馈下：

```text
H_aged[p,g] = max(F_reported[p,g], H_reported[p,g] − age, 0)
pressure[p,g] = max(w_local[p], H_aged[p,g])
cost[p,g] = pressure[p,g] + local_block_service[p] + D[p,g]
```

本地服务和下游服务会流水重叠；同一 block 在多个交换机建立的整块 R 若简单相加，会重复放大相同流量的负担。所以第一版使用瓶颈最大值做代理。

但 max 也会漏掉真正需要累加的串行等待。当前新 block 服务项仅使用本地出口速率，没有再计算该 block 经过全部下游链路的空载瓶颈服务项。下游 40G 空闲时 H 可以为 0，源叶四个 100G 出口的代价便仍相同；慢口开始有工作量后，H 才提供区别。

这解释了当前算法为什么能缓解下游慢口，却不必按静态带宽比例完全分配流量。它是定量压力反馈算法，尚不是完整的异构路径 ECT。

### 16.4 F 下限解决的问题与代价

如果一个报告 H=180 us，其中有 F=160 us 的未到达预留，过了 40 us：

```text
简单年龄扣减：180 − 40 = 140 us
当前规则：max(160, 180 − 40) = 160 us
```

未到达字节不能假定已经被链路服务完，所以 F 阻止这部分自动排空。代价是旧 F 在本地 R 已经减少后，仍可能保持过高，直到刷新或过期。

F 解决的是一种估算偏差，同时引入了保守滞后的可能；不能认为增加 F 后估计必然更准确。

### 16.5 5 us 平局意味着什么

不是只选绝对最小代价，而是：

```text
ties = {p: cost[p] − min_cost <= 5000 ns}
chosen = sorted(ties)[FNV1a(BlockKey, seed, switch_id) % len(ties)]
```

目的：不要让很小的状态差异把所有新 block 同时吸向一个端口，并保证结果可复现。

代价：允许选择比最小估计值大最多 5 us 的出口。候选集合的数量变化还会改变取模结果，使相邻时刻的选择发生变化。它不是保持 BLOCK_ECMP 原路径的规则。

### 16.6 一个“多了反馈反而选错”的示意例子

两个 100G 出口，服务项和传播项相同。本地压力分别为 0、20 us。最新收到的有效报告给出下游压力分别为 200、0 us；假设真实当前下游已经变为 0、150 us。

```text
BLOCK_LOCAL：选择出口 0
BRBF 估计：max(0,200)=200；max(20,0)=20，选择出口 1
真实状态：出口 0 更空，出口 1 更忙
```

这是说明机制边界的构造例子，不是已经定位出的 symmetric 某条请求根因。即使报告还在 TTL 内，也不能保证排序仍然正确。

## 17. 一次请求从 Python 到 RDMA 的完整执行过程

### 17.1 serving 保留请求身份

`scheduler.py` 在最后一轮 prefill 时，为完成输入的请求建立：

```text
pd_kv_send_tokens[(decode_instance_id, request_id)] = original_input
```

第一次 decode 建立对应接收 token 计划。`request.py::Batch` 保存这些字段，计算仍按原 batch 执行。

`trace_generator.py::_pd_kv_peers()` 根据模型 KV head、head dimension、KV dtype 和 TP 分片换算逐层字节，输出 `peer_npu_offset:request_id:bytes`。`_coalesce_pd_kv_sends()` 把同请求逐层 KV 汇到完整发送位置，P 在最后载体层发送，D 在第一载体层等待，不合并不同请求。

`graph_generator.py::_get_llm_converter()` 优先加载：

```text
astra-sim/extern/graph_frontend/chakra/src/converter/llm_converter.py
```

转换器为每个请求生成 SEND/RECV，写入 `kv_request_id:uint64` 辅助属性，并使用 `comm_tag=(1<<20)+request_id` 配对。当前 RDMA 父对象实际从 tag 恢复 RequestID；并不是直接读取这个辅助属性创建 ParentTransferID。

当前多个 KV SEND 在同一发送 rank 的任务图中仍有串行依赖。网络层支持并发父传输，但 serving 图不保证同源请求一起发射。这会限制实际网络压力，sps20 到达率也不能直接当作网络传输到达率。

### 17.2 创建控制父对象

`RdmaHw::AddQueuePair()` 检查：

```text
tag >= 1<<20 且策略 Transport != LEGACY
    → AddKvParent(qp)
否则
    → 原 QP 流程
```

传入的 qp 保存原应用回调、有效字节、窗口和 RTT，作为 `parent->control`。它自身不进入 NIC 发包队列。

`AddKvParent()`：

1. 检查 DCQCN 模式。
2. 计算 blocks、填充后的 payloadBytes，并检查共同 32 位线上序号限制。
3. 从 tag 恢复 request，分配 `id=(src<<48)|本源递增计数器`。
4. 记录 ready、进展时刻、初始 NIC 速率和父 pacing。
5. 根据策略判断 mega 或 block，加入 `m_kvParents`，安排 watchdog。
6. 调用 `FillKvWindow()`。

transfer ID 当前不直接编码 RequestID 或 kv_epoch；源编号与本源单调计数器负责运行内唯一性。epoch/rangeOffset 当前保持 0。

### 17.3 子 QP 窗口不是把全部 block 同时生成

`FillKvWindow()` 在父对象之间轮转发行。默认：

- 每个 block 父对象最多 4 个活跃子 QP。
- 每主机最多 32 个活跃数据 QP。
- 活跃父对象最多 32/4=8；Mega 也限制为 8 个活跃父对象。

Mega 每父只发行一个大 QP，block 每父逐步发行子 QP。子 QP 初始化相同父引用及路由元数据，块大小固定为 M。

Block 子源端口从 49152–65535 的端口池分配，完成后保留 2 ms 复用保护；找不到可用端口时等待，不让 uint16 自动绕回。目的端口固定 49500，用于识别扩展 DATA。

父控制对象不是“第五个发数据的 QP”。第一个子 QP 完成只释放一个活跃额度，不意味着请求完成；接着补发下一个 block。

### 17.4 实际发包前的两级轮转与共享预算

`RdmaEgressQueue::GetNextQindex()` 的扩展分支先选择可发父对象，再在该父的子 block 间轮转。不会因为一个父有四个子 QP，就在主机公平轮转中获得四份父份额。

子 QP 的关键方法被接到父状态：

```text
GetOnTheFly()         → parent.inFlight
GetWin()              → parent.control.GetWin()
GetEffectiveNextAvail → parent.nextAvail
```

`GetNxtPacket()` 用 uint64 剩余字节先限制到 MTU，再转为单包长度。Mega 也在 block 边界截断包，保持与 block 模式相同的填充及每块包数。生成 DATA 时增加父 inFlight，记录首次实际发送时刻，并附加真实元数据。

`UpdateNextAvail()` 按实际包长和父 rate 更新父 nextAvail。默认启用 rate bound 时，所有子包共同消耗父速率预算；不是分别以父 rate 发四份流量。

### 17.5 ACK、NACK 与 DCQCN 如何归到父对象

接收 ACK 时先匹配传输身份和 block 身份，防止旧端口上的迟到 ACK 推进新传输。

```text
子 QP ACK 新增确认 delta
    → parent.inFlight -= delta
    → parent.lastProgress = now

任一子 QP 的 ACK 携带 CNP
    → cnp_received_mlx(parent.control)

NACK 触发 RecoverQueue
    → 从父 inFlight 撤销该子尚待恢复的发送量
    → 子 snd_nxt 回退到 snd_una
```

DCQCN 的 alpha、降速和恢复定时器运行在父 control 上。一个路径上的拥塞可以使整个父速率下降，包括走其他路径的子 block；这保持控制份额公平，但也使路由与传输强耦合。

### 17.6 接收完整性与父完成回调

`ReceiveUdp()` 沿用 `ReceiverCheckSeq()` 检查可靠序号，再调用 `ObserveKvReceive()`：

- Block 子 QP：累计序号达到 M，标记这个 block 完成。
- Mega：累计序号每跨过一个 M，标记相应逻辑 block 完成。
- 父 completed 集合达到 parent.blocks，记录一次父接收完成。
- 重复 DATA 会重发 ACK，不重复计完成 block。

`CompleteKvBlock()` 在源端最终 ACK 后释放子 QP，记录 block 完成，补充发行额度。所有子完成后才记录父行、取消父定时器、调用原 `qp_finish()` 和应用完成回调。

回调代理的 `m_size` 保持有效 KV 字节，`m_init_size` 保存填充载荷。`entry.h::qp_finish()` 用有效字节完成原 SEND/RECV 匹配，避免把填充当模型数据。接收完整时刻是统计观察点；当前任务图仍在源最终确认回调时释放。

## 18. 交换机怎样实现“首包选路、后续固定、预留不重计”

### 18.1 状态与函数对应

| 状态/函数 | 作用 |
|---|---|
| `m_kvPins[(transfer,block)]` | 数据路径、剩余 R、已入队 offset bitmap、墓碑到期 |
| `m_kvReserved[port]` | 端口全部 pin 的剩余预留之和 |
| `m_kvCache[(port,destination)]` | 从这个下一跳端口收到的目的反馈 |
| `m_kvPropagation[(destination,port)]` | 初始化计算的静态传播后缀 |
| `MakeKvContext()` | 打包本地 Q/S/R、速率、传播、已收到 H/F |
| `SelectKvPort()` | ACK 反向路由；DATA 查询 pin 或调用策略 |
| `CommitKvPacket()` | admission 后创建 pin、登记/扣 R、回写 cookie |
| `MarkKvDirty()` | 合并状态变化，最多保留一个事件报告定时器 |
| `MaintainKvState()` | 每 100 us 清理墓碑及采样端口 |

Mega 的 pin key 使用 `(transfer,UINT32_MAX)`，覆盖整个父载荷；block 使用 `(transfer,block)`。RequestID/epoch 不直接加入 pin key，当前依赖 transfer 的唯一性。

### 18.2 为什么选路与提交分开

执行顺序是：

```text
GetOutDev → SelectKvPort → 原 MMU admission 检查
    admission 成功 → CommitKvPacket → SwitchSend 入队
    admission 失败 → 原丢包恢复，不登记本包新 pin/R
```

`SelectKvPort()` 没有先修改 R。否则一个最终被丢掉的首包会产生虚假的承诺。

ns-3 的单个事件回调串行执行，`CommitKvPacket()` 在实际入队前完成。后一首包决策可以看到前一首包登记的 R，不需要仿真全局锁或集中调度。

### 18.3 R、Q、S 的字节流转

首个成功入队的包建立整个 block 的预计 R，然后立即扣除该包实际线上长度。普通后续包第一次成功入队时，bitmap 对应位从 0 变 1，并扣相应 R。

```text
尚未成功入队：R
成功入队：    R 减少，设备 Q 增加
出队发送：    Q 减少，S 表示剩余序列化
发送结束：    S 归零
```

相同 offset 重传：实际包仍占 Q/S，但 bitmap 已为 1，不再扣 R。R 不预留未知重传和 ACK。

当前 Q 直接读 `GetQueue()->GetNBytesTotal()`，没有额外实现一份 Q 累计计数，也没有与 MMU egress_bytes 相加。S 由 QbbNetDevice 记录的 `m_kvTxEnd-now` 和实际速率换算，并限制不超过当前包长。

首包 offset 不必为 0；若 offset=0 丢失，后面首次 admission 成功的包仍能创建 pin，丢失偏移对应字节继续留在 R 中等待可靠恢复。

### 18.4 pin 生命周期与 ACK cookie

原始包全部入队时，remaining 变为 0，但 pin 不能删除。否则迟到重传或尚在发送的后续包可能重新选路。

最终 ACK 在每个交换机经过 `SelectKvPort()` 时，若达到完成目标，检查残余 R 为 0，然后设置 2 ms expiry。周期维护负责删除到期 pin。

源 leaf 在 DATA 真正向 spine 转发时，把所选 spine ID 写入 72 B 头中的 cookie。接收主机回显；目的 leaf 有多个反向出口时，按 cookie 找到该 spine。ACK 不重新做压力选择。

这是一条 leaf-spine 反向路径标识，不是任意多层网络完整路径编码；更深拓扑不能直接复用当前 cookie 语义。

## 19. 反馈怎样逐跳产生、传输与使用

### 19.1 以一个 decode 主机 g 为例

```mermaid
flowchart LR
    P[源主机] --> SL[源 leaf 16]
    SL --> SP[候选 spine 18–21]
    SP --> DL[目的 leaf 17]
    DL --> D[目的主机 g]
    DL -. "目的出口 H/F" .-> SP
    SP -. "本地出口与后缀组合 H/F" .-> SL
```

源主机不运行交换机反馈策略。源 leaf 为新 block 比较四个 spine 出口；spine 到目的 leaf 通常只有一个最短下一跳；目的 leaf 到 g 只有一个主机出口。

目的 leaf 报告其到 g 的出口工作量。Spine 收到后，与自己到目的 leaf 的出口工作量取 max，再向源 leaf 报告。源 leaf 收到的 H 因而能够包含自己直接看不到的 spine→目的 leaf 压力。

### 19.2 Advertise 与 Select 是两个不同目标

`Advertise()` 对每个目的 g 计算：

```text
若全部候选后缀有效：
    H[p] = max(w_local[p], H_next_aged[p])
    F[p] = max(ServiceNs(R[p]), F_next[p])
否则：
    H[p] = w_local[p]
    F[p] = ServiceNs(R[p])
    整条报告清除 FRESH

选 H 最小的出口，通告其 H/F/oldest。
```

通告不加“新 block 的服务时间”和静态传播；否则传播项会被年龄衰减，且拥塞信号与新流服务项会混在一起。通告只携带一个后缀瓶颈代理，不传所有下游逐链路状态。

`MakeKvContext()` 会排序端口；`Advertise()` 仅在 `h < minimum` 时更新，因此 H 相等时选编号最小者，F/oldest 取自这个同一候选。它不是 Select 的 BlockKey 哈希规则。

### 19.3 实际目的集合和发送方向

当前 `EmitKvFeedback()` 遍历 `m_rtTable` 的所有可路由主机 IP，不是只遍历 8 个 decode 主机，也没有单独的 leaf 级 g 编号。

对一个目的，向不在本地该目的 next-hop 集合中的交换机邻接端口提供报告。在当前最短路 leaf-spine 拓扑上，这实现向上游反向报告；接收侧还要求 inPort 确实属于自己到该目的的下一跳。

这依赖当前无环最短路候选结构，不是已经实现并验证的任意拓扑控制树构造算法。

### 19.4 时序、阈值与令牌桶的实际实现

初始化时，在 1 ns 后安排第一次周期报告。此后周期重算间隔为 16 us。

入队、出队、收到新反馈时调用 `MarkKvDirty()`。如果事件定时器已存在，就合并这些变化；否则 4 us 后调用 `EmitKvFeedback(false)`。它是交换机级事件/周期定时器，邻接发送限频和令牌桶则按端口保存，不是每个邻居各建一套周期定时器。

事件报告判断：H 相比上次提供的向量变化达到 `ServiceNs(M/8,发送端口速率)`，或 FRESH 标志变化，才产生新 pending 报告。**当前没有单独的 F 变化触发条件，也没有只靠 oldest 变化就立即发送的条件；这些变化由周期刷新覆盖。**

`DrainKvFeedback()` 使用 0.5% 端口容量补充令牌，桶上限 8000 B，同时检查距本端口上次发送至少 4 us。不足时保留 pending，4 us 后重试。

generated 时间在创建 pending 时确定；若令牌等待或出队延迟很长，收到时快照已经变老。已有 pending 时不会把它不断替换成最新快照。限频/预算提供开销边界，也会延长反馈滞后。

### 19.5 控制包不是免费事件

`KvLoadHeader` 固定头 44 B，每条目的记录 32 B，IPv4/PPP 再加 22 B：

```text
8 条记录：322 B
16 条记录：578 B
```

当前实际记录数按方向/目的可达性决定，不是每条链路都固定 8 或 16 条。分片最多 122 条记录/包。

`device->SwitchSend(0,packet,ch)` 进入真实控制队列，经历序列化、链路延迟和与其他包的竞争。协议号为 0xFA，`QbbNetDevice::Receive()` 将其交给本地交换机消费，不作为 DATA 继续普通转发。

包内包含 sender、boot=1、sequence、generated、分片信息和 CRC32。这里的 CRC 用于验证报告编码/内容，不替代底层真实丢包或可靠传输协议；反馈本身没有逐报告 ACK 重传协议，周期刷新承担更新。

### 19.6 接收与新鲜度校验

`ReceiveKvFeedback()` 检查相邻 sender、boot、报告版本/CRC/长度、生成时间、目的可达关系、序号和分片一致性。分片完整后才原子更新 `(inPort,destination)` 缓存，旧报告不覆盖新缓存。

`MakeKvContext()` 判定每候选：

```text
直连主机：fresh=true，后缀压力为 0
交换机下一跳：需存在缓存，且
    record.FRESH = true
    now − generated <= 64 us
    now − record.oldest <= 64 us
```

`oldest` 用于保留所选反馈后缀中最老的组成观测时间，防止中间交换机重打 generated 就把旧下游信息洗成新鲜。

`PressurePolicy::Select()` 只有所有候选 fresh 才使用 H/F；任一失败，整组用本地 cost，并把 fallback 写入日志。fallback 不表示丢包，也不取消已 pin block。

## 20. 为何当前实现仍可能让 symmetric 的少数请求变慢

下面是从源码可以确定的机制边界，**不是已经完成的尾部根因定位**。

| 机制边界 | 可能怎样影响真实完成时间 | 应如何验证 |
|---|---|---|
| 整块 R 忽略未来到达时序 | 已承诺但慢慢到达的数据，使实际上可服务的出口显得忙 | 对齐 R、实际 Q/S、父速率和后续到包时间 |
| 本地平局哈希不同于 ECMP | 即使没有有效压力差，生成的路径序列也不同 | 比较同请求各 block 的实际 chosen_port |
| H/F 滞后且 F 保守 | 已经清空的路径仍被高估，或新拥塞尚未上报 | 对齐报告生成/到达、首包选择与下游实际状态 |
| max 是流水瓶颈代理 | 不能精确累计串行等待、未来服务及共享资源竞争 | 比较估算与实际 block 完成趋势，不能直接把 cost 当 block FCT |
| 目的主机口共享 | 四条 spine 最后汇到同一端口，换路无法消除该瓶颈 | 检查目的 leaf 的主机出口，而不只看源上行 |
| 一个子 block 的拥塞降低父速率 | 其他路径的 block 也受限，收益不只取决于自身选路 | 记录父 CNP/速率、窗口等待与 block 完成顺序 |
| 4-block ACK 完成后补窗 | 个别慢块影响后续 block 的启动与整份完成 | 重建每个 block 的 first_tx/rx_complete/last_ack |
| 控制包占服务 | 有效信息很少时仍产生周期开销 | 用真实发送字节与控制队列服务量核对 |
| serving 图和调度反馈 | 网络变化改变后续 SEND 释放、batch 和等待 | 逐请求对齐 ready，另做固定网络注入 trace 隔离选路 |

特别要区分两个观察：

1. BLOCK_LOCAL 和 BRBF 在 symmetric 的 P99 都比 BLOCK_ECMP 差，所以不能先把共同尾部退化归罪于 BRBF 反馈。它们共有的本地选择/R/路径序列及调度轨迹应先检查。
2. BRBF 与 BLOCK_LOCAL 的 mean 接近，只能说反馈在这批负载下增量很小，不能证明它完全不影响路径或完全零开销。

如果研究目标要求“对称环境下也尽量不退化”，后续可以讨论 ECMP 锚定平局、压力优势阈值、到达时序感知预留或请求尾部目标。它们是新的算法设计，需要分别验证；本次文档整理没有加入这些机制，也不以拍脑袋调参解释现有 P99。

## 21. 具体改了什么：按功能查源码

以下列的是当前 KV 功能实现涉及的文件及职责。它不是“这次文档更新修改了全部这些文件”；本次只更新本文。现有工作区中无关的 examples 删除或其他改动，不属于 BRBF 算法机制。

### 21.1 新增模块

| 相对仓库根目录的文件 | 内容与关键入口 |
|---|---|
| `astra-sim/extern/network_backend/ns-3/src/network/utils/kv-routing-header.h/.cc` | `KvBlockHeader` 72 B DATA/ACK；`KvLoadHeader` 44+32n B、CRC、网络字节序 |
| `astra-sim/extern/network_backend/ns-3/src/point-to-point/model/kv-routing-policy.h/.cc` | 配置、注册表、`KvRoutingContext`、ECMP/PressurePolicy、ServiceNs/Hash、H/F 通告、CSV |
| `astra-sim/extern/network_backend/ns-3/src/point-to-point/model/kv-block-transport.cc` | `AddKvParent`、`FillKvWindow`、端口池、接收完成、父回调、watchdog |
| `astra-sim/extern/network_backend/ns-3/src/point-to-point/model/kv-switch-routing.cc` | 首包 pin/R、ACK cookie、静态传播、反馈发送/接收/缓存、采样 |
| `astra-sim/extern/network_backend/ns-3/src/point-to-point/test/kv-routing-test.cc` | 策略、共享窗口、包头、CRC、注册与异构带宽/传播单元检查 |
| `astra-sim/extern/network_backend/ns-3/src/point-to-point/test/kv-transport-smoke.cc` | 直接创建并发 RDMA 父传输的网络驱动 |
| `scripts/test_kv_routing.py` | 构建/短 serving/直接网络回归及无效配置检查 |
| `astra-sim/extern/network_backend/ns-3/scratch/topology/pd_kv_16gpu_local40g.txt` | 独立本地慢链路拓扑，16–18 为 40G |
| `astra-sim/extern/network_backend/ns-3/scratch/topology/pd_kv_16gpu_downstream40g.txt` | 独立下游慢链路拓扑，17–18 为 40G |

### 21.2 原模块接入改动

| 文件 | 从原行为到当前行为 |
|---|---|
| `serving/core/scheduler.py`、`request.py` | KV token 计划保留请求键，Batch 携带发送/接收计划 |
| `serving/core/trace_generator.py` | peer 总流量改为逐请求身份；完整 prefill 后整份发送 |
| `serving/core/graph_generator.py` | 优先加载仓库 Chakra 转换器，使本地修改生效 |
| `astra-sim/extern/graph_frontend/chakra/src/converter/llm_converter.py` | 拆出逐请求 SEND/RECV，配对标签与 uint64 请求属性；保留旧格式兼容 |
| `serving/__main__.py` | 策略 CLI、查询后端注册表、自定义拓扑/运行快照、配置 manifest、合法辅助文件计数头 |
| `serving/core/controller.py` | 后端 stdout 提前关闭时明确报错，避免空读忙等 |
| `serving/run.sh` | SPS 列表、默认全量请求、三场景策略组合、短批次目录、拒绝覆盖结果 |
| `astra-sim/astra-sim/network_frontend/ns3/AstraSimNetwork.cc` | `--list-kv-routing-policies=1` 查询接口 |
| `astra-sim/astra-sim/network_frontend/ns3/entry.h` | 请求级 KV FCT、扩展父有效字节回调、填充后的 standalone 估算 |
| `astra-sim/extern/network_backend/ns-3/scratch/common.h` | KV 配置解析/校验、统计打开、路由建立后初始化交换机、辅助计数头检查 |
| `astra-sim/extern/network_backend/ns-3/scratch/config/config.txt` | 模式模板开关，当前 ECMP；其他 KV 参数可用 C++ 默认值 |
| `.../src/network/utils/custom-header.h/.cc` | 按扩展端口/ACK 识别 DATA 元数据，识别 KV_LOAD |
| `.../src/point-to-point/model/rdma-queue-pair.h/.cc` | 父对象状态、子引用、共享 inFlight/window/nextAvail、接收身份 |
| `.../src/point-to-point/model/rdma-hw.h/.cc` | 扩展父入口、包生成、ACK/NACK/CNP 父记账、父 pacing、接收完成 |
| `.../src/point-to-point/model/qbb-net-device.h/.cc` | 两级公平轮转、父可发时刻、序列化剩余 S、控制包本地接收 |
| `.../src/point-to-point/model/switch-node.h/.cc` | KV 状态字段、选路入口、admission 后提交 R、入/出队反馈触发 |
| ns-3 `src/network/CMakeLists.txt`、`src/point-to-point/CMakeLists.txt` | 编译新增协议/传输/选路/测试源文件 |

表中 `...` 为 `astra-sim/extern/network_backend/ns-3`。`switch-mmu.*` 没有被 BRBF 替换；原 admission/PFC/ECN 仍在，算法从设备真实队列读取 Q。

### 21.3 算法边界与仿真框架边界

交换机策略负责：对新 pin 提供出口；基于本地与收到的后缀计算报告。RDMA 负责：可靠性、窗口、速率、发行和完成。Python 负责：请求调度、计算图、实验配置。

`KvRoutingStats` 的全局接收完成关联仅用于记录 t_rx_complete；选路和父发行不读取它。静态传播初始化读取邻居拓扑是静态配置计算，不是运行时远端队列查询。

算法的新策略应复用这套公共传输层，以免一个策略额外获得 QP/CC 份额或提前触发完成。MEGA_LDLB 复用这套传输层，并使用独立标签协议；具体版本见第 24 节。

## 22. sps20 已完成实验：能说明什么，不能说明什么

### 22.1 数据与时间口径

批次：`kv261105_1503_s20_nall`。请求结果在 `outputs/`，网络结果在 `astra-sim/inputs/runs/` 对应批次下。

2026-11-06 核对：10 个 run 均完成 200 个请求；各请求的到达时间、输入输出长度和有效 KV 字节一致，总有效字节为 165,570,936,832 B。相同外部负载不表示相同 ready、batch 或网络注入轨迹，因为 serving 会响应完成时间变化。

以下 FCT 使用 `kv_fct.csv::fct_ns`，即 ready→源最终 ACK；扩展 `kv_transfers.csv::kv_net_fct_ns` 使用 first_tx→目的接收完整。该批两种口径的均值仅差约几 us，显示在下表两位小数时几乎一致，但正式报告仍须明确分开。

分位数方法为排序后零基索引 `round(p*(n-1))`，n=200 的 P95 取索引 189，P99 取索引 197（第三大）。这不是所有统计库默认的线性插值定义。P99 由极少量请求主导；应同时报告样本量、分位数定义与 max，不能直接把观测差异称为随机噪声。

| 场景 | 策略 | Mean FCT ms | P95 ms | P99 ms | Mean TTFT ms |
|---|---|---:|---:|---:|---:|
| symmetric | MEGA_ECMP | 87.61 | 198.80 | 291.59 | 2557.6 |
| symmetric | BLOCK_ECMP | 78.79 | 183.00 | 225.08 | 2178.8 |
| symmetric | BLOCK_LOCAL | 79.29 | 176.59 | 302.95 | 2193.8 |
| symmetric | BRBF_V1 | 79.40 | 176.87 | 298.30 | 2194.8 |
| local40g | BLOCK_ECMP | 111.99 | 288.27 | 364.35 | 3067.8 |
| local40g | BLOCK_LOCAL | 79.76 | 177.94 | 284.60 | 2197.5 |
| local40g | BRBF_V1 | 79.92 | 185.40 | 272.74 | 2198.7 |
| downstream40g | BLOCK_ECMP | 112.71 | 292.13 | 336.40 | 3070.6 |
| downstream40g | BLOCK_LOCAL | 90.36 | 214.35 | 266.48 | 2632.3 |
| downstream40g | BRBF_V1 | 80.63 | 180.23 | 225.72 | 2368.0 |

### 22.2 三个可支持的结论

1. symmetric 的 BLOCK_ECMP 相对 MEGA_ECMP：mean −10.1%、P99 −22.8%、TTFT −14.8%。支持公共框架下 Block 分路/传输组织有效。
2. local40g 的 BLOCK_LOCAL 相对 BLOCK_ECMP：mean −28.8%、P95 −38.3%、TTFT −28.4%。源慢路径端口累计发送从 42.50 GB 降到 4.84 GB，支持本地带宽/工作量感知避开慢口。
3. downstream40g 的 BRBF 相对 BLOCK_LOCAL：mean −10.8%、P95 −15.9%、P99 −15.3%、TTFT −10.0%。对应源路径累计发送从 41.24 GB 降到 33.34 GB，支持反馈进一步减少流量经过下游慢口。

端口累计发送包括线上协议及控制字节，不等于纯 KV payload；BRBF 总端口字节略增，不能把这些数字直接当有效 KV 分配的精确比例。下游慢口实际数据方向为 spine18→leaf17；源 leaf16 的 p9 是通向它的路径入口，leaf17 的 p9 出方向主要是反向流量，不能误当正向慢口负载。

下游场景 BLOCK_LOCAL 也改善了 FCT，但长期分配几乎没变。这提示瞬时分配/时序及调度反馈也在起作用，不能只凭累计字节解释全部性能差异。

### 22.3 symmetric 必须保留的负面结果

BRBF 相对 BLOCK_ECMP：

```text
Mean FCT：78.79 → 79.40 ms，约 +0.8%
P95：183.00 → 176.87 ms，约 −3.4%
P99：225.08 → 298.30 ms，约 +32.5%
Max：275.22 → 380.54 ms
```

相对 MEGA_ECMP，BRBF mean 仍改善约 9.4%，但 P99 略差约 2.3%。不能写成“各层指标递增改善”，也不能写成“对称环境零代价”。

该批平均有效流量速率约 91–93 Gbps，相对四条源上行 400G 约 23%；按请求 first_tx→rx_complete 区间计算，平均在途 KV 请求约 1.1，峰值 4–5。这是整段时间平均，不能证明没有瞬时拥塞，也不是并发 block 数。

低平均压力说明持续上行拥塞可能不是主导因素；它没有解释具体尾部请求为什么变慢。第 20 节列出的机制需结合实际记录定位。

### 22.4 为什么 TTFT 的收益比网络均值差更大

下游 BRBF 与 BLOCK_LOCAL 的 Mean FCT 仅差约 9.7 ms，Mean TTFT 却差约 264 ms。TTFT 包括到达后等待、prefill、网络屏障以及首个 decode 的调度；网络提前完成可能改变后续请求和 batch 的排队。

因此 TTFT 是有效的 serving 结果，但不能把全部 TTFT 差值视为单请求 KV 传输直接节省。需要固定网络注入计划的实验来隔离网络算法收益，再用 serving 实验评价调度放大效应。

### 22.5 当前结果的适用范围

当前 symmetric 没跑基础 ECMP，40G 两场景没跑 MEGA_ECMP；这是不完整的全策略矩阵，但现有局部消融仍可使用。当前没有多 trace/多种子置信区间，也没有 LDLB 对比。

结果支持“这批受控瓶颈负载上的机制有效性”，尚不能支持所有对称负载、拓扑规模、热点变化或极端尾部上的普遍优势。

## 23. 如何沿代码和日志深入掌握，以及仍需补上的验证

### 23.1 推荐源码阅读顺序

1. `kv-routing-policy.h/.cc`：先理解输入视图、Select、Advertise，避免把传输实现误当选路算法。
2. `kv-block-transport.cc` 与 `rdma-queue-pair.h/.cc`：理解一个请求为什么有多个子 QP，却只有一份控制预算。
3. `rdma-hw.cc` 与 `qbb-net-device.cc`：沿 DATA 发包、ACK/CNP、父窗口及补窗追踪一次请求。
4. `kv-switch-routing.cc` 与 `switch-node.cc`：按 Select→admission→Commit→queue 顺序理解 R。
5. `kv-routing-header.*`：核对 DATA/ACK/cookie 和 H/F 报告实际字节。
6. Python trace/Chakra/entry：理解 RequestID 到回调、网络 FCT 到 TTFT 的连接。

### 23.2 用一条真实记录手算决策

该批 `sym_BRBF_V1/route_decisions.csv` 中请求 1、block 0 在源 leaf16 的首次决策：

```text
time_ns=221055467，候选 p9/p10/p11/p12
Q=S=R=H=F=0，fresh=1
bandwidth=100000000000 bps，propagation=3000 ns
cost=172309+3000=175309 ns
chosen_port=12
```

四个候选等价，选择 p12 来自稳定哈希，而不是 p12 更快。每条决策按候选输出多行；这四行是一次选择，不是四次 block 发射。

日志 `cost_ns` 是被选候选的代价，重复在各候选行；不是该行 `candidate_port` 的独立 cost。手算每个候选时，应使用该行 Q/S/R/H/F/age/带宽/传播重建。

`route_decisions.csv` 没有保存 record.oldest 和 window/父速率，也不记录每次逐包状态变化，所以已有日志不能完整重建所有故障或瞬时行为。端口统计每 100 us 一次也可能漏掉更短的队列峰值。

### 23.3 对 symmetric 尾部的正确定位步骤

1. 在 `kv_fct.csv` 按 RequestID 配对，找 BRBF/BLOCK_LOCAL 相对 BLOCK_ECMP 变慢最多的请求，不只比较各自 top-3 排名。
2. 比较这些请求的 src/dst、valid bytes、ready、first_tx、rx_complete、last_ack；相同 workload 不保证相同 src/dst 或 ready，动态 P/D 配对也应核对。
3. 用 `block_fct.csv` 找延迟集中在哪些 block、是否有长补窗等待；不要把所有 block 的 FCT 相加当父 FCT。
4. 用实际 route 决策检查压力差、近似平局集合、fallback、慢 block 所走 spine。
5. 对齐源上行、spine 下行和目的主机出口的瞬时压力；关注不可绕开的共享瓶颈。
6. 现有日志不足时，再增加针对这些请求的父 rate/CNP/窗口与反馈组成观测，不先重跑全部长实验。

这些是下一步调查方法，本次尚未执行逐请求尾部根因定位。

### 23.4 已有测试与尚未实现的内容要分开

已有测试覆盖策略合同、协议序列化、共享窗口、短请求完成、并发父额度、异构传播/带宽等检查；第 13.5 节记载历史执行范围。本次文档更新只做源码和实验口径核对，没有重新运行网络测试或完整仿真。

尚未具备或未完成专项验证的事项包括：

- 通用多 epoch RequestKVTracker 与真实 KV 张量范围写入。
- 可继续仿真的单父 ABORT 清理协议、运行中 switch reboot、故障重路由。
- 任意深度拓扑的完整反向路径编码与反馈方向验证。
- 完整异构路径空载服务时间与严格请求级 ECT。
- 每决策预测误差的直接统计：现有 CSV 记录 chosen cost，但没有自动对应实际完成误差。
- 专门的 fallback 比例/最长时段、预算等待等汇总输出；当前需要从日志推导或另加记录。
- 更大父载荷的共同线上序号机制、全套丢失/延迟注入、多 trace 尾部验证。
- 独立 R 消融与 LDLB 实际对照。

掌握第一版应以这三个判断为准：**能看见哪些资源、用什么近似把资源转为代价、一次选择怎样通过可靠传输影响整份请求完成。**当前 BRBF 已经解决一部分“源端看不到下游压力”的问题，但它没有承诺代理代价与真实请求 FCT 的排序在所有时刻一致。

## 24. LDLB 作者代码版本集成（2026-11-07）

### 24.1 来源与适配范围

参考仓库为 https://github.com/xuruiiiii/LDLB-NS3 ，固定提交
`39bcd806a476f37db2667ac894fd084a96c69ed6`。重点核对
`simulation/src/point-to-point/model/switch-node.cc` 的 `GetOutDev()`、
`SendToDev()`，`switch-mmu.cc` 的 `SetSwitchLabel()`，以及
`qbb-net-device.cc` 的 `BuildCongestionLabelPayload()` 和接收逻辑。
上游仿真目录许可证为 GPL-2.0；本次是这些算法逻辑在现有 ns-3 接口上的适配。

LDLB 仅提供 `MEGA_LDLB`（整份请求固定路径）。
它共享现有 KV 线上描述符、可靠传输、父 CC、父窗口、主机额度和完成回调。
以 KV block 为选路单位是本项目作者提出的设计；按用户要求已移除 LDLB 的 block 粒度扩展。
MEGA_LDLB 保留本项目的 CC/PFC/准入环境，
没有复制作者为了无速率控制实验而跳过准入检查的代码。
本文此前“尚无 LDLB”的描述属于历史实验状态，并不表示已有 LDLB 对照实验结果。

### 24.2 本地检测、标签与选路

- 本地检测只在开启 ECN 的非零优先级出队时执行，采样位置在 MMU 出队扣减之后。
  样本是该出口该优先级的 MMU `egress_bytes`，阈值复用该出口现有 ECN 配置（`ConfigEcn` 转为字节）。
- 默认 EWMA 从零开始，alpha=0.25。未拥塞时，高于 kmax 连续两次进入；
  kmin < EWMA <= kmax 以 `pmax*(EWMA-kmin)/(kmax-kmin)` 触发，并要求连续两次成功。
  已拥塞时，连续三次 EWMA <= kmin 才解除，中间区保持标签。
  EWMA/计数按 `(port, priority)` 存储，接口拥塞标签跨优先级共享，沿用作者实现。
- 每次找到出口的正常转发包（包括 ACK）在准入之前检查本地标签集合。
  新集合连续稳定两轮、与上次广播不同、距上次广播至少 50 us 才发全量快照。
  首次广播不受间隔限制。目的集合是“至少一个下一跳出口拥塞”的目的地址并集。
  向所有相邻交换机发送，包括空快照解除；只包含本地检测标签，不再传播收到的 CLT。
- 标签占用真实链路和优先级 0 队列，格式为大端
  `[sender:u32][prefixCount:u32]`，每个 /24 前缀包含
  `[prefix:u32][rangeCount:u32]` 和若干 `[start:u8,end:u8]`。
  保留作者始终压缩的实际发送行为。使用协议号 `0xf9` 避免当前 `0xff` CNP 冲突，IPv4 TTL=1。
  接收时确认 sender 是该端口的直接邻居，再完整替换该邻居的目的集合；空集合清除该邻居标签。
  额外拒绝截断、非规范区间和伪造邻居，避免坏包导致部分 CLT 提交。
- 新 KV 单位先排除本地拥塞端口与 CLT 标记该目的地址的相邻出口；
  若过滤为空，回退到原候选集合。候选间比较
  `max(MMU egress_bytes[out][dataPriority], device queue total bytes)`，只选在线端口。
  精确平局用作者 `EcmpHash` 和 `m_ecmpSeed ^ 0x7f4a7c15`。
  平局候选顺序沿用原路由表顺序，不额外按接口号排序。
  保留作者 12 字节地址/端口键。
  实际 pin 在准入成功后建立，此后不会因为标签改变而改变同一单位的路径。
- LDLB **不使用** R、序列化剩余字节、链路速率、传播时延或 BRBF 的 H/F/TTL 代价。
  公共传输层仍维护 R，但不把它提供的压力优势混入 LDLB 决策。

这份作者代码没有论文的 10 ms 标签老化、两跳转发、周期刷新或全部候选拥塞时丢包规则。
也没有定时重试广播：流量停止后，新的本地检测/解除与广播可能不再发生，CLT 会持续保留到收到后续快照。
本项目没有给它额外补上这些机制，应以“作者公开代码版本 LDLB”标注结果。
当前公共传输层没有运行中故障重路由协议；不要用这些策略宣称完成链路故障复现。

### 24.3 参数与使用

| 配置项 | 默认值 |
| --- | --- |
| `KV_LDLB_STABILITY_GUARD` | 1 |
| `KV_LDLB_EWMA_ALPHA` | 0.25 |
| `KV_LDLB_ENTER_SAMPLES` | 2 |
| `KV_LDLB_EXIT_SAMPLES` | 3 |
| `KV_LDLB_BROADCAST_DAMPENING` | 1 |
| `KV_LDLB_BROADCAST_STABLE_ROUNDS` | 2 |
| `KV_LDLB_MIN_BROADCAST_NS` | 50000 |

布尔项只能是 0/1，alpha 必须有限且在 `(0,1]`，样本数与稳定轮数必须正数。
关掉 guard 后采用作者原始概率阈值更新，关掉 dampening 后取消稳定轮数与间隔约束。
随机检测使用 ns-3 `UniformRandomVariable`，流编号由 `KV_ROUTE_SEED` 与 switch id 指定，
可重复，但不保证与作者旧版 `UniformVariable` 的逐次 RNG 序列相同。

CLI 使用 `--kv-routing-policy MEGA_LDLB`。
`serving/run.sh` 在现有各场景策略列表末尾追加 MEGA_LDLB；保留当前 P=LOAD、D=RAND 设置。
`route_decisions.csv` 增加 `local_congested`、`remote_congested`；LDLB 的 Q 列是上述比较用的字节数，
`cost_ns=0`（没有纳秒代价），fallback 表示过滤为空或无在线过滤候选后的回退。
`feedback_stats.csv` 的 `ldlb_send`、`ldlb_receive`、`ldlb_reject` 记录真实字节数和目的集合大小；
该协议没有序列号，对应 sequence 列为 0。`kv_manifest.json` 保存来源提交及检测/广播参数。

### 24.4 验证范围

契约测试加入作者格式的精确字节比较、每个截断位置、空快照解除、
EWMA 零初始化、连续进入/退出与概率失败、广播稳定轮数/最小间隔、
邻居和目的地址隔离、LCT/CLT 并集过滤、全部过滤后的最小队列回退和在线端口选择。
网络完成与 pin 验证复用短并发父传输 smoke，不启动 serving 长实验。
容器内必须重新 `./ns3 build` 后才能用于正式实验；宿主临时编译不替代这个要求。

本次宿主临时库编译与契约测试已通过。直接抽取上游未改动的 `SetSwitchLabel()`，
在两种 guard、三种 alpha、两种进入与退出门限，以及跨接口/优先级的随机采样中
与集成实现做差分比较，共 480,000 次采样，所有接口状态一致。
MEGA_LDLB/BLOCK_ECMP/BRBF_V1 在 fabric40g/local25g/downstream25g
各做一组短并发测试，每组 12 个父传输各完成一次；MEGA_LDLB 源 leaf 有 12 个请求 pin。
LDLB 检查了线上标签避让、空快照撤销和伪造 sender 拒绝。
移除前曾对 LDLB 的分块扩展降低 ECN 阈值做 detector 专项测试，验证了实际拥塞广播与解除，
其结果属于历史扩展测试，不作为当前请求级 LDLB 的测试结果。这些是正确性测试，不是策略性能排名。
下游 25G 场景在首次解除后出现了新的拥塞标签；解除检查以“到下一次有效拥塞快照之前”为窗口，
不能把收到空快照理解为该出口此后永远不会再拥塞。

当前宿主 Docker socket 无访问权限，`sudo -n docker ps` 也无法执行，因此容器内构建尚未完成。
可在已启动的 `servingsim_docker` 中执行：

```bash
sudo docker exec \
  -w /app/LLMServingSim/astra-sim/extern/network_backend/ns-3 \
  servingsim_docker env -u USER ./ns3 build
```

该容器通常以 root 运行，`env -u USER` 用于避开 ns-3 启动器对 USER=root 的拒绝。
若容器名称不同，使用当前 simulator 容器名称。正式运行前还应确认后端
`--list-kv-routing-policies=1` 列出 `MEGA_LDLB`。
