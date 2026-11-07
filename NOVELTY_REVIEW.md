# BRBF / KV Block 选路课题：相关工作与创新性初查

检索日期：2026-11-06。比较对象为当前 `BLOCK_ROUTING_V1.md` 及已有实现，不将尚未实现的请求级 ECT、通用 KV 页映射或硬件部署能力算作贡献。

## 1. 结论

**目前没有在已核对的论文中发现与 BRBF 当前机制组合完全相同的方案。但多个核心构件有明确先例，不能据此宣称“首次 Block 分路”或“首次定量瓶颈反馈”。目前课题可以继续推进，创新性论证需要收窄，论文尚不适合仅凭现有消融就宣称已经定型。**

最需要面对的四项工作：

- **Presto**：把流拆成近等大的 flowcell 分路，直接削弱“固定大小分块分路本身是新思想”的表述。[R1]
- **HULA**：逐跳向上游传播最优后缀瓶颈，以最小化路径最大链路利用率，直接削弱“逐跳 min–max 定量反馈本身是新思想”的表述。[R2]
- **ARK**：面向分离式 LLM 的 KV 大流做分布式路径预留，直接覆盖“KV 网络哈希碰撞及预留”的动机。[R3]
- **LDLB**：本地拥塞与上游传播的下游标签组合，覆盖当前本地/远端协同的整体架构动机。[R4]

较值得保留并验证的差异是：

> 在可靠 RDMA 的 KV 分块传输中，将首包已选路径但尚未入队的原始字节登记为 R，与真实 Q/S 分开记账；以包含预留下限 F 的后缀反馈估计承诺工作量；在共享父拥塞控制和每请求完成屏障下评价这种机制对整份 KV 尾部完成时间的作用。

这是**候选贡献定位**，不是已证明全球首创的结论。是否超过“Presto 式分块 + HULA 式反馈 + 预留”的自然组合，仍需要直接比较、独立 R 消融、反馈误差和反例分析。

“课题相关”不等于论文重复；“未找到完全相同论文”也不等于具有足够新颖性。学术审稿可能质疑的是可预期的机制组合，而不只是一篇论文是否逐字采用同一公式。

## 2. 本次检索覆盖与证据等级

搜索方向包括 KV cache transfer / block routing / multipath / reservation、RDMA load balancing、flowcell/flowlet、定量后缀瓶颈、本地与下游拥塞结合，以及 2025–2026 年近期预印本。

来源优先使用论文正文、作者托管 PDF、正式出版机构页面及 arXiv 版本页。检索命中的博客、自动论文摘要和聚合页面只用于发现线索，不能替代正文作强结论。

| 工作 | 本次证据范围 | 判断使用方式 |
|---|---|---|
| ARK | arXiv v1 正文，重点 §2、§3、Algorithm 1、§4 | 可具体比较预留单位、位置及信号 |
| LDLB | 工作区 `LDLB_paper_text.txt` 正文，IEEE 出版页面/DOI | 可具体比较标签、pin、代价及候选过滤 |
| HULA | 作者 PDF，重点 Introduction/设计说明；Princeton 正式记录 | 可确认逐跳最小最大利用率及 flowlet |
| Presto | IBM 正式摘要、作者 PDF 14 页，重点 §2、§3 | 可确认 64 KB flowcell、轮转路径及 GRO 重排处理 |
| Flowcut | arXiv v2 正文，重点 §II-A/B/D | 可具体比较 ACK 在途计数、draining 和有序换路 |
| NetKV | arXiv v1 正文，重点 §III–V，尤其 §V-C/D | 可比较派发计数、网络感知和遥测滞后 |
| MMA | arXiv 正文/当前 v2 摘要 | 可确认单机 Host–GPU 多路径与本课题边界 |
| Mooncake | FAST 2025 正式论文页；PDF 获取返回 403 | 可确认 KV 存储/传输架构，不能仅凭摘要排除传输实现相似 |
| DRILL | University of Illinois 正式摘要 | 可确认本地逐包队列感知；未作全文穷尽比较 |
| CONGA | 作者 PDF 12 页，重点 §3；出版摘要及 HULA 交叉说明 | 可确认本地与远端压力取 max、DRE 及 leaf 间反馈 |
| ConWeave | 论文摘要检索、LDLB 正文和参考文献 | 支持 RDMA 有序性相关背景；完整实现比较仍需原文 |
| HICS、Rate-Aware LB | 见 §10 | 已发现但未充分核对，不能当作已排除 |

检索没有覆盖所有数据库、所有中文学位论文、专利或尚未公开稿件；这是针对机制的学术新颖性初查，不是法律意义的侵权判断。没有访问到的近期论文明确留作缺口，不隐含“无冲突”。

## 3. 对你的课题作精确定义

当前实现的研究对象是：

1. P/D 分离后的每请求整份 KV 传输。
2. 固定 2 MiB 分块，块内 pin、块间可换路。
3. 每父最多 4 个活跃子 QP，父级共享 DCQCN、在途窗口及 pacing。
4. 首次 DATA 成功入队时，对剩余原始字节建立 R；重复包不再次扣 R。
5. 本地压力 `w=(Q+S+R)/B`。
6. 下游报告瓶颈 H，以及不随报告年龄直接排空的预留下限 F。
7. 新块使用 `max(w, aged_H)+本地块服务时间+静态传播` 的代价。
8. 反馈真实过链路，版本/TTL 校验，不完整时全候选统一 local fallback。
9. 所有子块完成后触发父回调，评价整份 KV FCT 与 TTFT。

其中 `cost` 是代理，不是严格请求级 ECT。当前块属于网络传输描述，没有实际保存 GPU 页地址、张量内容或多 epoch 范围覆盖。研究定位必须与这些实现事实一致。

## 4. 最接近的工作：逐项比较

### 4.1 Presto：对“Block 粒度首创”的最大挑战

**Presto: Edge-based Load Balancing for Fast Datacenter Networks**，SIGCOMM 2015。[R1]

论文已提出将流分成 64 KB、近等大的 flowcell，在软件网络边缘对预设路径轮转分路，并修改 GRO 处理 TCP 接收重排（§2、§3）。它不是 LLM 论文，但“不要按整流分路，改按固定大小数据单元分路”的思想已经存在。正文还讨论 flowlet 大小不均匀及等待流间隙的限制，所以“固定大小比自然 flowlet 更适合大流”这一动机也已有明确先例。

当前差异包括：KV 场景、2 MiB 单元、独立可靠子 QP、父共享 CC、交换机 R 及 H/F。可是如果论文的唯一新点是把通用 flowcell 改叫 KV block，并选择另一个大小，审稿人很容易认为这是场景迁移。

**需要证明的事**：KV block 是否带来通用等大小 chunk 不具备的决策信息或正确性约束？例如真实 KV 页边界、生成/就绪时序、后续消费依赖、完整请求的完成屏障。当前只有“整请求拆成等大小、完成后聚合”时，这些语义优势尚不充分。

建议表述：借鉴固定大小子流分路，针对 KV 可靠传输中的未来工作量估计和完成屏障作扩展。避免表述“首次固定大小 Block 分路”。

### 4.2 HULA：对“min–max 后缀反馈首创”的最大挑战

**HULA: Scalable Load Balancing Using Programmable Data Planes**，SOSR 2016。[R2]

正文明确说明：各交换机以邻居为单位追踪最优后缀，更新特殊 probe，向上游传播；最优路径是“minimizes the maximum utilization of all links along a path”，并按 flowlet 选路。

与 BRBF 相同的结构是：

```text
本地链路代价与下游代价组合 → 选择最优后缀 → 向上游传播
```

所以“逐跳反馈”“定量而不是二值”“max 表示瓶颈”不是可单独宣称的新贡献。

可能成立的差异：

- HULA 主要使用链路利用率；BRBF 使用 Q/S/R 所代表的承诺服务时间。
- BRBF 的 R 包含尚未成功入队的原始字节，不能作为已经服务过的工作直接随时间衰减。
- F 的含义是预留服务下限，不是普通利用率快照。
- BRBF 使用固定可靠 block，不依赖自然 flowlet 间隔。

**必须做的比较**：在相同 block 传输和父 CC 上实现明确标为适配版的 BLOCK_HULA，检查 Q/S/R/F 相对利用率反馈是否还有增量。只拿原 flowlet 版在没有流间隙的 RDMA 中表现差，不能充分隔离你的反馈贡献。

### 4.3 ARK：与你的问题和“预留”最直接重合

**ARK: Avoiding Routing Collisions for KV Cache Transfer in Disaggregated LLM Inference**，arXiv:2610.41160，2026。[R3]

正文页标注 NAIC ’26（SIGCOMM 的 Networks for AI Computing workshop）。arXiv 摘要页给出较宽泛的 SIGCOMM proceedings 标注，正式引用应依据正文与 DOI 核实，不直接称为 SIGCOMM 主会论文。

ARK 的核心流程：

1. 每主机维护复制的路径占用预留表。
2. 新 KV 大流选择未预留 spine。
3. 通过预计算源端口，使普通 ECMP 哈希映射到选定 spine。
4. 广播 RESERVE/RELEASE；路径全被预留时回退 ECMP。
5. 以时间戳/host ID 解决异步预留冲突。

| 维度 | ARK | 当前 BRBF |
|---|---|---|
| 决策位置 | 端主机控制器 | 交换机首包 |
| 预留单位 | 大流路径占用状态 | 出口尚未入队的原始字节工作量 |
| pin 周期 | 整份 KV 大流 | 2 MiB block |
| 协同信号 | 主机间预留/释放广播 | 邻接交换机 H/F 快照 |
| 交换机修改 | 保持普通 ECMP | 新头解析、pin/R/反馈逻辑 |
| 下游压力建模 | 核心机制为占用协调 | 使用后缀定量压力 |
| 对可靠性的处理 | 单路径避免接收重排 | 子 QP 内固定路径、块间独立完成 |

这些差异意味着没有看到 ARK 直接包含 BRBF 的整套机制。但是，“KV 大流已知大小/目的，可以提前承认未来传输”这个机会已经被 ARK 明确提出。

应将论文问题写成“粗粒度占用预留不能充分表达多流共享和下游动态瓶颈，细粒度承诺字节如何改善选择”，并实测，而不是写成“首次利用 KV 信息预留网络”。

### 4.4 LDLB：架构来源必须承认，不能只说换成定量就完成创新

**LDLB: Label-Based Dynamic Load Balance Algorithm for Data Center Network**，IEEE TCC，DOI 10.1109/TCC.2026.3723429。[R4]

本地文本页眉出现 2025，但 IEEE 当前页面及 DOI 为 2026；引用以出版元数据为准。

LDLB 已有本地拥塞表 LCT、从下游上报的 CLT、受拥塞影响的目的 IP、flow pinning，以及结合队列/带宽/故障代价的 WCMP-like 选择。

BRBF 的主要差异：

- 首包 R 在实际排队形成前承认后续工作量。
- block pin 比整流 pin 有更多重新决策机会。
- 报告服务时间及预留下限，替代单纯拥塞/解除标签。
- 不因所有候选被标拥塞就由选路策略删光路径，而是选择可达候选的有限最小 cost。

但“本地 + 全局”“看不到下游所以反馈”“保序 pin”不能作首创贡献。LDLB 原文也讨论性能稳定性与 pin，不能以其最简算法伪代码代表整个实际方案。

### 4.5 Flowcut：有序换路与在途字节计数也已有先例

**Flowcut Switching: High-Performance Adaptive Routing with In-Order Delivery Guarantees**，arXiv:2506.21406，2025，核对 v2。[R5]

Flowcut 在交换机或 NIC 保存 flowcut 与在途字节计数，ACK 减少计数；在途归零后可建立新 flowcut。对持续 RDMA 流，可以暂停源端、drain 后再换路，不依赖自然 flowlet 间隔。

与 BRBF 的重合是细粒度保序适应、状态保持到 ACK、可靠完成后释放。**但它的在途计数不是 BRBF 的 R**：

```text
Flowcut：已经发出、尚未收到相应确认的字节
BRBF R：已选出口、尚未成功排入该出口队列的原始字节
```

BRBF 不要求不同 block 之间全请求字节有序；不同子 QP 可乱序完成，只在请求完成屏障聚合。因此不能宣称“整份 KV 包按序到达”，只能宣称不因块内换路引入子 QP 内路径乱序。

### 4.6 NetKV：网络感知与自我承诺计数已用于 LLM 调度

**NetKV: Network-Aware Decode Instance Selection for Disaggregated LLM Inference**，arXiv:2606.03910，2026。[R6]

其主要决策是选择 decode 实例，联合计算负载、缓存匹配和网络代价，不是已经给定 P/D 后选择 spine。

特别相关的 §V-C：同源派发请求时增加按 tier 的 in-flight 计数，传输完成后减少，以防贪心连续过度承诺同一近端资源。§V-D 分析网络遥测过期对排序的影响。

这不直接覆盖交换机逐包去重的 R，但说明“把自己刚作出的决定记入负载，防止只读旧观测而重复分配”已有 LLM 场景先例。需要明确你的粒度、资源位置、字节语义与计数型机制不同。

### 4.7 CONGA、DRILL、ConWeave：不能遗漏的网络基线

- **CONGA**（SIGCOMM 2014）：flowlet 加远端拥塞反馈，定量下游感知已有先例。§3 明确将本地 uplink 拥塞与远端路径拥塞取 maximum；这与 BRBF 的 `max(w_local,H_next)` 结构直接相近。区别需建立在 Q/S/R/F 的状态语义上，而不是仅凭 max 公式。[R7]
- **DRILL**（SIGCOMM 2017）：每交换机按本地队列与随机化作逐包选择，本地快速负载感知已有先例。[R8]
- **ConWeave**（SIGCOMM 2023）：面向 RDMA 的细粒度换路与网络内重排，证明 RDMA 不易使用自然 flowlet 的问题并非新发现。[R9]

BRBF 可选择避免大型网络内重排、在独立子 QP 上 pin；但这项优势需要同时计算子 QP 状态、72 B 头、父 CC 和端口复用成本，不能把“无需重排”作为零代价结论。

### 4.8 Mooncake、MMA、光 KV 网络：相关但决策层不同

- **Mooncake**（FAST 2025）：KV 存储/传输与调度的生产系统，应比较传输引擎的多 NIC/分片策略，但不能把其应用缓存路由直接当交换机选路。[R10]
- **MultiPath Memory Access（MMA）**（arXiv:2512.16056，当前 v2 2026）：单机 Host–GPU 直接/中继通路分配，并以队列 backpressure 调节。此前版本标题含 MultiPath Transfer Engine；引用应注明版本。[R11]
- **An Optical KV Cache Network for Disaggregated LLM Serving**（arXiv:2610.18444，2026）：prefill 期间预订光电路以隐藏 OCS 重配置，属于硬件/电路调度路线。[R12]

它们不能直接判为 BRBF 重复，但让“KV 数据移动多路径”“KV 预留”这种宽泛题目更难成为独立贡献。

## 5. 贡献风险矩阵

等级表示对“这一项单独作为首创”的挑战程度，不是对整篇论文重复概率的数值估计。

| 拟宣称贡献 | 最近先例 | 风险判断 | 可收窄的表述 |
|---|---|---|---|
| 首次 KV 网络负载均衡 | ARK 等 | 高 | 给定 P/D 后的可靠分块承诺工作量选路 |
| 首次固定大小 block 分路 | Presto/flowcell | 高 | 在公共父 CC 与 KV 完成屏障下的分块传输适配 |
| 首次本地 + 远端感知 | LDLB/CONGA/HULA | 高 | 字节预留与带保留下限的后缀服务时间反馈 |
| 首次定量 min–max 反馈 | HULA | 高 | 将利用率瓶颈替换为承诺服务工作量，并分析误差 |
| 首次预留 KV 路径 | ARK/光 KV 网络 | 高 | 非独占的出口未来字节账本，不是整路径占用预订 |
| 首包 R、成功入队去重扣减 | 本次未找到完全同义机制 | 候选差异，待查证 | 明确 R/Q/S 守恒与预测收益，独立 R 消融 |
| F 阻止未到达 R 随年龄排空 | 本次未找到同一语义组合 | 候选差异，待验证 | 专门讨论保守高估、TTL 与热点切换 |
| 父共享 CC、子 QP 完成屏障 | 多路径传输已有广泛先例 | 主要是公平性/正确性支撑 | 不以 4 份 CC 放大带宽；证明实现必要性 |
| 请求级 FCT/TTFT 评价 | ARK/NetKV 等 | 评价要求，不是算法首创 | 证明代理选择对整份 KV 尾部的实际作用 |

本次没有发现与 R/F 完全同义并相同部署的方案，只能作为继续检索及实验验证的线索。一般的虚拟队列、资源预留、在途计数都有长期先例，不能因为符号 R/F 新就判为新算法。

## 6. 目前最现实的审稿质疑

可能的质疑是：

> “这是否只是将 Presto 的 flowcell 换成较大 KV chunk，再将 HULA 的利用率代价换成队列/预留服务时间？”

回答必须是机制和证据：

1. 通用 chunk 为什么不能正确估计 paced KV 的后续承诺？
2. Q/S-only 或利用率反馈在什么受控场景产生什么明确错误？
3. R 的收益是否来自信息提前，而不是仅改变哈希或多拿拥塞窗口？
4. F 是否纠正旧报告的错误衰减，还是只让代价更保守？在哪些条件下反而恶化？
5. 这些差异是否改善请求完成关键路径，而不只是平均 block 分布？
6. 为得到这些收益，增加了多少状态、控制字节及路径选择开销？

如果只能回答“我们在 LLM 场景测到了改善”，算法论文的创新论证偏弱；若能说明新的信息缺口、准确的记账语义、反馈失真与解决边界，论文定位会更有说服力。

## 7. 当前实验距离“论文定型”还差什么

sps20 已有三组有效消融，但主要对比是自家扩展 ECMP、本地版和完整版。downstream40g 的 BRBF 相对 BLOCK_LOCAL mean 改善约 10.8%，是反馈有效的证据，尚不能说明它优于已有定量反馈。

建议优先补足以下证据，而不是先扩写“首创”叙述：

1. **独立 R 消融**：同 block 传输、同平局哈希，比较 Q/S 与 Q/S/R。否则本地收益混有速率项和哈希变化。
2. **与 HULA 的同框架适配比较**：明确标注 BLOCK_HULA；再分别加 R/F，以区分指标改动与保序粒度改动。
3. **LDLB 原始/Block 适配比较**：说明原 flow pin 与 block pin 的差别，保持 CC/填充/完成口径公平。
4. **ARK 对照**：即使部署成本不同，也解释其主机预留与交换机承诺字节在高并发/下游瓶颈下的边界。
5. **F 专项实验**：报告延迟、丢失、过期、热点迁移，观察排序反转及保守高估。
6. **请求尾部与预测误差**：当前 symmetric P99 相对 BLOCK_ECMP 恶化约 32.5%，应定位而不是称为噪声。
7. **多源/多目的与规模**：两 leaf 单向 KV 场景不足以揭示跨源反馈同步追逐和状态规模。
8. **状态/开销**：每 pin bitmap、QP、目的缓存、控制流量预算，真实协议头和包数。

这些是研究建议，本次没有实现新基线、修改算法或重跑长实验。

## 8. 建议的论文定位

较稳妥的题目方向：

> 面向分离式 LLM 推理的承诺工作量感知 KV 分块选路。

候选英文定位：

> Committed-Work-Aware Routing for Fixed-Size KV Transfers in Disaggregated LLM Inference.

最终标题可再修改。若尚未接入真实运行时 KV page 语义，应称 fixed-size KV transfer blocks，不声称直接对 vLLM 物理页操作。

候选贡献段：

> 我们研究固定大小 KV 子传输在可靠 RDMA 网络中的路径选择。不同于只观察已排队字节或链路利用率的方法，我们显式维护已承诺但尚未入队的原始字节工作量，并将其与实际排队及序列化状态分离。我们设计携带预留下限的后缀反馈，使未到达工作不因报告年龄而被误认为已经排空。在共享父拥塞控制与每请求完成屏障下，我们评价其对本地和下游瓶颈的作用，并分析反馈滞后和请求尾部退化的边界。

这段避免“首次”与“严格 ECT”。最后一句需要后续真正完成分析，不是已经获得结论的声明。

如要主打“请求完成时间优化”，还需增加明确面向剩余 block/关键路径的决策，而不能仅把 block cost 更名为请求 ECT。那将是新的设计阶段，不属于当前实现。

## 9. 如何在 related work 中承认与区分

可以按三条线组织：

1. **分路粒度与有序性**：Presto、LetFlow、Flowcut、ConWeave。说明固定 block 不依赖流间隙，代价是子 QP/完成屏障。
2. **网络状态与反馈**：DRILL、CONGA、HULA、LDLB。说明承诺字节与利用率/拥塞标签不同，F 解决的是尚未到达预留的老化问题。
3. **KV 数据移动与预留**：ARK、Mooncake、NetKV、MMA、光 KV 网络。区分端点选择、存储路径、单机传输、光电路调度与包交换出口选择。

实验引用他人的配置不是冲突；应说明借鉴来源。与 ARK 使用类似 2-leaf/4-spine、Zipf/Poisson 模型时，不能以拓扑/负载一致作为新贡献，应解释控制变量与新增瓶颈场景。

## 10. 尚未充分核对的近期线索

这些线索阻止本次调查得出“已排除所有直接重合”的结论：

- **Breaking the Interconnect Bottleneck in LLM Serving via Congestion-Aware Scheduling over Heterogeneous Interconnects / HICS**：检索命中 IEEE HPEC 2026 PDF；摘要片段提到为 KV 传输联合选择 fabric、transport、placement、path。本次官方 PDF 返回 403，未取得全文，无法判断是否存在相似分块预留或代价模型。[R13]
- **Enabling Rate-Aware Load Balancing for Datacenter Networks**：Kai Chen 作者列表标为 ICNP 2026，作者 Jin Wang、Yang Li、Jinbin Hu、Kai Chen；当前列表未提供论文 PDF，本次未取得方法正文。标题与源端 pacing/R 到达时序问题相关，但不能按标题推断其算法。[R14]
- **ConWeave 原文细节**：应进一步核对部署、重排队列和换路信号；当前仅据摘要与已读论文的引用作背景比较。[R9]
- 固定字节预留、虚拟服务时间和硬件 flowcell 的更广泛文献：需要围绕明确的 R/F 定义继续引用链追踪，而不是只搜索“KV Block”关键词。

## 11. 来源与引用入口

### [R1] Presto

Keqiang He et al. *Presto: Edge-based Load Balancing for Fast Datacenter Networks*. SIGCOMM 2015. DOI: 10.1145/2785956.2787507.

- [IBM 正式论文摘要](https://research.ibm.com/publications/presto-edge-based-load-balancing-for-fast-datacenter-networks)
- [作者发表列表](https://www.cs.utexas.edu/~akella/publications.html)
- [作者 PDF，14 页，本次成功读取](https://pages.cs.wisc.edu/~akella/papers/presto-sigcomm15.pdf)

### [R2] HULA

Naga Katta, Mukesh Hira, Changhoon Kim, Anirudh Sivaraman, Jennifer Rexford. SOSR 2016. DOI: 10.1145/2890955.2890968.

- [作者 PDF，12 页](https://www.cs.princeton.edu/~jrex/papers/hula16.pdf)
- [Princeton 论文记录](https://collaborate.princeton.edu/en/publications/hula-scalable-load-balancing-using-programmable-data-planes)

### [R3] ARK

Hung-Chun Lin, Ting-Wei Hsu, Chung-En Ho, Ahmed Saeed. 2026. DOI: 10.1145/3789240.3828750.

- [arXiv 元数据](https://arxiv.org/abs/2610.41160)
- [v1 正文与 Algorithm 1](https://arxiv.org/html/2610.41160v1#S3)

### [R4] LDLB

Xiaoyan Qian et al. IEEE Transactions on Cloud Computing. DOI: 10.1109/TCC.2026.3723429.

- [IEEE Computer Society 出版页面](https://www.computer.org/csdl/journal/cc/5555/01/11653543/2iTyt595XW0)
- 本地全文：`LDLB_paper_text.txt`；本地 PDF：`LDLB.pdf`。

### [R5] Flowcut

Tommaso Bonato et al. *Flowcut Switching: High-Performance Adaptive Routing with In-Order Delivery Guarantees*. arXiv:2506.21406；v1 2025-06-26，v2 2025-11-19。

- [元数据](https://arxiv.org/abs/2506.21406)
- [v2 正文，§II](https://arxiv.org/html/2506.21406v2/#S2)

### [R6] NetKV

Mubarak Adetunji Ojewale. arXiv:2606.03910，2026-06-02。

- [元数据](https://arxiv.org/abs/2606.03910)
- [正文，自我争用计数与滞后分析](https://arxiv.org/html/2606.03910#S5.SS3)

### [R7] CONGA

Mohammad Alizadeh et al. *CONGA: Distributed Congestion-Aware Load Balancing for Datacenters*. SIGCOMM 2014. DOI: 10.1145/2619239.2626316.

- [DOI](https://doi.org/10.1145/2619239.2626316)
- [作者 PDF，12 页，本次成功读取](https://people.csail.mit.edu/alizadeh/papers/conga-sigcomm14.pdf)
- HULA 正文对 CONGA 的利用率/反馈与状态结构有直接说明，见 [R2]。

### [R8] DRILL

Soudeh Ghorbani et al. *DRILL: Micro Load Balancing for Low-latency Data Center Networks*. SIGCOMM 2017.

- [University of Illinois 正式记录与摘要](https://experts.illinois.edu/en/publications/drill-micro-load-balancing-for-low-latency-data-center-networks)

### [R9] ConWeave

Cha Hwan Song et al. *Network Load Balancing with In-network Reordering Support for RDMA*. SIGCOMM 2023，pp. 816–831。

- 题名及发表信息由 LDLB 参考文献 [59] 交叉核对。
- 摘要检索支持 RDMA 细粒度换路与网络内重排；原文的详细机制尚需补读。

### [R10] Mooncake

Ruoyu Qin et al. *Mooncake: Trading More Storage for Less Computation — A KVCache-centric Architecture for Serving LLM Chatbot*. FAST 2025.

- [USENIX 正式论文页](https://www.usenix.org/conference/fast25/presentation/qin)
- [正式 PDF](https://www.usenix.org/system/files/fast25-qin.pdf)

### [R11] MMA

Lingfeng Tang et al. *MultiPath Memory Access: Breaking Host-GPU Bandwidth Bottlenecks in LLM Services*. arXiv:2512.16056，当前 v2 2026-05-13。

- [元数据与当前摘要](https://arxiv.org/abs/2512.16056)
- [当前 v2 正文](https://arxiv.org/html/2512.16056v2)

### [R12] 光 KV 网络

Seongwon Yoon, Shimeng Yu. *An Optical KV Cache Network for Disaggregated LLM Serving*. arXiv:2610.18444，v2 2026-10-18。

- [元数据](https://arxiv.org/abs/2610.18444)
- [当前正文](https://arxiv.org/html/2610.18444v2)

### [R13] HICS 线索

- [官方 HPEC PDF 入口，本次返回 403](https://ieee-hpec.org/wp-content/uploads/2026/09/145.pdf)
- 证据级别仅检索片段，不使用其性能数值或推断具体算法。

### [R14] Rate-Aware LB 线索

- [Kai Chen 作者论文列表，2026 条目](https://cse.hkust.edu.hk/~kaichen/publication.html)
- 本次未取得方法正文，未作“与 BRBF 无重合”的判断。
