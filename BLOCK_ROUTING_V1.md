# BRBF v1：基于预留与瓶颈反馈的 KV Block 选路

BRBF：Block Reservation and Bottleneck Feedback。

本文是 BRBF 第一版完整实施规格，不是已经实现的 BRBF 程序。基线改为每请求一个独立 KV SEND/RECV，保留计算 batching；本文的 block 子 QP、共享控制器及交换机反馈尚未接入。本文固定第一版的规则，实施时不得以其他行为悄悄替代。

## 1. 部署范围与研究目标

部署目标：当前 astra-sim/ns-3，静态、等带宽的两层 leaf-spine 网络。数据链路为 100 Gbps，默认链路延迟 1 us，交换机逐包转发。每个交换机自主决策，不使用全局队列查询、集中调度器或零延迟远端状态。

第一版不承诺商用 ASIC 线速可部署，不处理故障重路由、异构链路带宽或任意循环拓扑。链路故障、控制消息长期丢失和传输异常必须可观测，不能伪造完成。

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

无进展指该 ParentTransfer 的累计确认有效载荷未增加，不是“请求尚未完成”。终止阈值是可见的运行保护，不是提前完成机制。若合法 PFC 暂停超过此值，该实验将以失败结束，必须报告原因。

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

所有比较模式使用相同的填充、数据包头和 ParentTransfer 元数据。大流基线仍将一个父对象固定到一个 ECMP 路径，但承载相同的有效数据和填充。

当前 sps20 在“每请求一次发送全部 KV”的条件下，按请求填充得到 79,046 个 block；有效 KV 为 165,570,936,832 B，填充后载荷为 165,771,476,992 B，增加约 0.121%。若实际存在多个 kv_epoch，必须按真实 epoch 计划重新统计，不能沿用这个估计。填充不是控制/包头开销，两者分别统计。

### 3.3 完成屏障

在三个 block 模式中，每个 block 使用独立、可靠的 RDMA 子 QP，序号从 0 到 M。不同 block 允许乱序完成；同一 block 不允许随包换路。Mega-ECMP 使用父 QP 的累计序号，不把父 ACK seq 错当成某个子 block 的 seq。

接收端只按 BlockKey 和 offset 写入对应 KV 位置。父对象必须等所有子 block 完成才能触发原来的 AstraSim SEND/RECV 回调，且传给原 MsgEvent 的计数字节为有效 KV 字节，不是含填充/包头的链路字节。

接收端 RequestKVTracker 的整份完成条件为：有效范围覆盖原有 D 请求计划中的全部所需 KV 字节，无缺口；仅累计收到的字节总数不够，重叠范围不得重复计数。它使用到达时已建立的 D 请求计划，不能信任某个新父对象自称“请求已结束”。

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

实施时根据实际序列化长度生成 header_bytes，不将上述 108 B 写死用于其他模式。

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

- ACTIVE：R > 0，或仍有未完成的原始数据服务。
- DRAINED：本交换机原始 DATA 已全部服务，R=0，但仍保留路径等待可靠完成。
- DONE：看到 ACK seq=M，原 block 已可靠完成。保留同一端口墓碑 2 ms，用于迟到重复包；不再持有 R。

ACTIVE/DRAINED 不因反馈 TTL 或普通 idle TTL 直接释放 R。源端确认父传输失败后，使用显式 ABORT 消息清理它的 block 状态；ABORT 不能触发成功完成回调。

DONE 墓碑在数据包访问时和周期维护时清理。不得为每个 packet 新建清理定时器。

## 7. 定量瓶颈反馈

目的组 g 定义为最终 decode 主机的出口类。第一版 8 个 D 使用 8 个目的组，不把所有 D 盲目合成一个 leaf 组，因为不同主机出口的排队可能不同。

只在预先计算的最短路径 DAG 中选择候选 next-hop，不修改原拓扑可达性，不在拥塞时删除可达路径。

### 7.1 局部虚拟工作量

```
w_s(p,t) = [Q_s(p,t) + S_s(p,t) + R_s(p,t)] / B_s(p)
```

它是按当前承诺工作量计算的虚拟服务时间，不是对“当前真实队列等待”的精确测量。特别是 R 对应数据可能受源端 pacing 限制而尚未到达，因此不能声称它是严格实际完成时间。

实现统一使用整数 ns 并向上取整：`w_ns = ceil(8 * bytes * 1e9 / link_bps)`。乘法使用足够宽的中间类型，检查加法溢出，不再经过 int32。S 根据正在发送的包和 tx_end−now 计算，不能随 packet header 采样得到负值。

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

在任何数据队列/R/S 或后缀缓存状态改变后标记 dirty。相比上次发送变化达到 M/8 对应服务时间，或者 FRESH 标志变化时安排事件报告；受 4 us 限频和令牌桶约束。16 us 的周期报告覆盖非 dirty 时的刷新。

每邻接链路最多有一个事件报告定时器和一个周期定时器。逐包钩子只更新本地计数和 dirty 标志；若事件定时器已经存在，不再次 Schedule。完整 H/F 向量在定时器执行时重算，不能为每个 packet 创建反馈事件或扫描所有目的。

### 7.4 反馈过期策略

如果本次候选集合中任一必需的后缀记录缺失、未标为 FRESH、报告 age >64 us 或 `now−oldest_component_ns >64 us`，本次决策对整个候选集合统一退化为本地工作量模式。不能把缺失路径当成“下游 0 等待”，而同时给其他路径保留非零反馈。

恢复完整的新鲜反馈后自动恢复完整模式。统计 fallback 次数、比例、最长持续时间。

第一版不加入跨源 leaf 的远端预预约协议。因此，不承诺在一个反馈传播窗口内完全避免不同源 leaf 的同步追逐；这是必须测试的反馈滞后边界，不得用仿真全局变量隐藏。

## 8. 唯一的选路规则

当前等速、等深度拓扑的第一版规则：

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

不在已建立 pin 的 block 中应用 cost，不因拥塞或 feedback 更新改动正在传输的 block。下一 block 独立计算。

所有出口都忙时仍选择有限 cost 最小的可达出口。不因为拥塞标签而丢弃 block。真实故障没有可达路径时，显式失败。

第一版不加入请求优先级、最后一个 block 优先级、SRPT 或额外队列调度，以免把调度收益混入选路收益。

## 9. 代码接入点

| 文件/组件 | 规定改动 |
|---|---|
| `serving/core/trace_generator.py` | 每请求独立有效字节计划；完整 prefill 后发射；保持请求到达时的 P/D 选择；不能只保留 peer 总字节而丢掉 RequestID |
| `serving/core/graph_generator.py` / Chakra 读取 | 传递 ParentTransferID、请求列表、有效 KV 字节；对新增属性建立一致类型读取 |
| `network_frontend/ns3/entry.h` | KV 父对象创建、窗口化子 QP、整份完成屏障、原 MsgEvent 有效字节回调；非 KV 原行为不变 |
| `rdma-hw.*` / `rdma-queue-pair.*` | 子 QP 到 ParentTransfer 的引用；共享 DCQCN/pacing/window；ACK 元数据回显和正确唤醒 |
| 新 `kv-block-header.*` | 72 B 元数据真实序列化，范围/offset/valid_bytes/ID/父完成屏障校验 |
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

比较 LDLB 时必须有两种标注清楚的基线：MEGA_LDLB（父流 pin，对应原有大流粒度）与 BLOCK_LDLB（相同子 QP/父 CC 框架，在 block 粒度应用 LDLB 标签规则）。后者不是论文原始工作负载的复现，但用于隔离“粒度”与“预留/反馈”收益。

### 9.1 实施后的配置接口

以下为待实现的配置字段，不是当前未接入的新算法已经支持的参数。Python 和 C++ 必须从同一份配置读取并进行一致性校验：

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

Python 将这份配置传给 KV 计划生成器，C++ 用生成的同一运行配置建立 ParentTransfer 和交换机状态。参数解析失败或版本不一致必须退出，不能将未知字段忽略后实际仍跑 ECMP。

实施、编译及第 11 节验收通过后，沿用 `python -m serving` 的原命令入口；`--request-routing-policy RAND` 仍只负责 P/D 请求分配，不能把它误当作交换机 BRBF 开关。新算法由 KV_ROUTING_MODE 控制。现有 DCQCN、buffer、拓扑字段保持独立，不由 BRBF 模式偷偷修改。

### 9.2 当前已实施的每请求 ECMP 基线

正常 serving 的 KV trace 使用 `peer_npu_offset:request_id:bytes`，保留同批、同目的端的不同请求。Chakra 在最后一个 KV 层生成各自的整份 SEND，在 decode 第一层生成各自的整份 RECV；配对标签为 `(1<<20)+request_id`。

prefill 计算仍可分块、合批，但本轮只实现“完整 prefill 后一次传输”，不实现中间计算块的流式 KV 发射。正常 TP=1、PP=1 的一次完整 KV 请求对应一个 QP；TP/PP 配置可能产生多个 rank 分片，不能承诺所有配置的 QP 数都等于请求数。

每个 QP 的源/目的 IP 和端口在生命周期中固定，因此原有 ECMP 哈希将整份请求 KV 固定到同一路径。没有接入 BRBF 的 block 路由、72 B 数据头或填充策略，不能把当前基线当成已实现的 BRBF。

`ns3/output/fct.txt` 保留原八列格式。新增的 `ns3/output/kv_fct.csv` 当前列为：

```
request_id,src,dst,sport,bytes,start_ns,end_ns,fct_ns,standalone_fct_ns
```

start_ns 是子 QP 创建时的网络开始时刻，end_ns 是源端确认 QP 完成时刻，fct_ns=end_ns−start_ns；该文件不是第 10 节尚未实现的接收端 t_rx_complete/ready-delay 全套指标。请求 ID 解码仅适用于新请求级 trace，旧 peer:bytes 格式的标签仍按层序号生成，不能将其附属 CSV 解释成请求 ID。

图生成器优先加载仓库里的 `llm_converter.py`，其 protobuf schema 等依赖继续使用安装包。修改该转换器后不再需要重装整个 Chakra 包，新的模拟器进程即可读取仓库版本。

## 10. 运行与统计接口

实施后每次运行输出如下独立文件：

- `block_fct.csv`：BlockKey、valid_bytes、载荷/线上字节、ready/first_tx/rx_complete/last_ack、源 leaf 选路、重传次数。
- `kv_fct.csv`：RequestID/ParentTransferID、有效/填充字节、全部 block 数、ready/first_tx/rx_complete/last_ack、KV_NET_FCT、KV_READY_DELAY、KV_COMMIT_DELAY。
- `route_decisions.csv`：采样记录候选 Q/S/R、反馈值/年龄、cost、选择出口、fallback；不逐包记录。
- `feedback_stats.csv`：发送包/字节、分片、过期/乱序拒绝、预算限制、更新延迟。
- `port_stats.csv`：实际发送字节、利用率、100 us 时间窗口的负载；队列统计维持原口径。
- `run_manifest.json`：模式、种子、block/MTU/header、CC/window/控制预算、拓扑和 workload 哈希、软件版本。

请求 CSV 中原 TTFT、latency、TPOT 继续保留。mean FCT/P95/P99 在相同父对象或请求上统计；block FCT 仅作辅助。

上游 pin 选择到 block 完成的实际路径和重传也要能对应起来，不能只由哈希推算代替真实选路记录。

## 11. 强制验收

### A. 正确性

1. 每个请求的 sum(valid_bytes) 等于原有 KV 计划；每个 block 传输 M 载荷。
2. 所有模式的填充、元数据、发射条件完全相同。
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
- 不在设计交付时声称代码已实现或商用交换机已经可运行。
