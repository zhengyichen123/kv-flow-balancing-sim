#!/bin/bash
# 消融：每个 SPS × 三种环境各三个策略，串行运行；失败即停止。
# 默认跑 sps20/50/100；请求数默认 = 负载文件的全部行（即不传 --num-reqs）。
# 负载口径：workloads/kv_poisson_sps<N>.jsonl 是"N 个请求在约 1 秒内到达"，
# 即 num_reqs = N、到达率 = N req/s。N 越大瞬时负载越高，总请求数仍很小。
# 想限流时设 NUM_REQS，例如先小规模验证：NUM_REQS=12 bash serving/run.sh
# SPS 可给多个（空格分隔），如 SPS="20 50 100"；单个如 SPS=20。
# 取值 20/30/40/50/70/100。
# 只跑反馈组：SCENARIO=downstream25g bash serving/run.sh
# 只打印命令：PLAN_ONLY=1 bash serving/run.sh
# SCENARIO 可选 all / symmetric / local25g / downstream25g。
# symmetric 使用接入 100G、fabric 全 40G；只跑这组：SCENARIO=symmetric bash serving/run.sh
# local25g / downstream25g 在 40G fabric 基础上把一条 fabric 链路降到 25G
# （local25g 降 16–18，downstream25g 降 17–18），接入口保持 100G。
# 请求分配：P 侧 RR，D 侧 RAND，使用动态配对。
# 目录命名：inputs/runs/<批次>/<场景>_<策略>，批次形如 kv261105_1403_s20_n200
# （跑完整份负载时 n 部分为 nall）；场景代码 sym / loc25 / ds25，输出在 outputs/<批次>/。
# 每个 SPS 各自一个批次目录；BATCH_ID 仅单 SPS 时可用（复用同一目录、拒绝覆盖）。
# 后面的旧功能示例保持注释，不参与这轮实验。
set -euo pipefail

repo_root="$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}")")")"
cd "$repo_root"
sps_list="${SPS:-20 50 100}"
# 空 = 跑完负载文件的全部行（不传 --num-reqs，serving 默认全量）。
num_reqs="${NUM_REQS:-}"
scenario_filter="${SCENARIO:-all}"
plan_only="${PLAN_ONLY:-0}"
python_bin="${PYTHON_BIN:-python}"
topology_dir="astra-sim/extern/network_backend/ns-3/scratch/topology"
# 同一次调用里多个 SPS 共用同一个时间戳，批次名形如 kv261105_1403_s20_n200。
batch_stamp="$(date +%y%m%d_%H%M)"

# SPS 可空格分隔多个；逐个校验取值。
read -ra sps_values <<< "$sps_list"
[[ "${#sps_values[@]}" -ge 1 ]] || { echo "SPS 不能为空" >&2; exit 1; }
for sps in "${sps_values[@]}"; do
  [[ "$sps" =~ ^(20|30|40|50|70|100)$ ]] || { echo "SPS 必须是 20/30/40/50/70/100，空格分隔多个，如 SPS='20 50 100'" >&2; exit 1; }
done
if [[ -n "$num_reqs" ]]; then
  [[ "$num_reqs" =~ ^[1-9][0-9]*$ ]] || { echo "NUM_REQS 必须是正整数；不设置则跑完负载文件的全部行" >&2; exit 1; }
fi
case "$scenario_filter" in
  all|symmetric|local25g|downstream25g) ;;
  *) echo "未知 SCENARIO：${scenario_filter}" >&2; exit 1 ;;
esac
[[ "$plan_only" == 0 || "$plan_only" == 1 ]] || { echo "PLAN_ONLY 必须是 0/1" >&2; exit 1; }
if [[ -n "${BATCH_ID:-}" && "${#sps_values[@]}" -gt 1 ]]; then
  echo "BATCH_ID 仅支持单个 SPS；多 SPS 请去掉 BATCH_ID（脚本会按 SPS 各自命名批次目录）" >&2
  exit 1
fi

# 不设 NUM_REQS 时不传该参数，serving 会跑完负载文件的所有行。
num_req_args=()
if [[ -n "$num_reqs" ]]; then
  num_req_args=(--num-reqs "$num_reqs")
fi

for sps in "${sps_values[@]}"; do
  # 批次目录承载时间与规模，运行目录只留“场景_策略”，避免单个名字过长。
  # 限制请求数时标签用具体数字，否则用 all 表示跑完整份负载。
  num_req_tag="${num_reqs:-all}"
  batch_id="${BATCH_ID:-kv${batch_stamp}_s${sps}_n${num_req_tag}}"
  [[ "$batch_id" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "BATCH_ID 只能包含字母、数字、下划线或横线" >&2; exit 1; }
  inputs_root="${repo_root}/astra-sim/inputs/runs/${batch_id}"
  output_dir="outputs/${batch_id}"

  for scenario in symmetric local25g downstream25g; do
    [[ "$scenario_filter" == all || "$scenario_filter" == "$scenario" ]] || continue
    case "$scenario" in
      symmetric)
        scen_code=sym
        topology="${topology_dir}/pd_kv_16gpu_fabric40g.txt"
        policies=(ECMP MEGA_ECMP BLOCK_ECMP BLOCK_LOCAL BRBF_V1 MEGA_LDLB)
        ;;
      local25g)
        scen_code=loc25
        topology="${topology_dir}/pd_kv_16gpu_local25g.txt"
        policies=(BLOCK_ECMP BLOCK_LOCAL BRBF_V1 MEGA_LDLB)
        ;;
      downstream25g)
        scen_code=ds25
        topology="${topology_dir}/pd_kv_16gpu_downstream25g.txt"
        policies=(BLOCK_ECMP BLOCK_LOCAL BRBF_V1 MEGA_LDLB)
        ;;
    esac
    for policy in "${policies[@]}"; do
      run_id="${scen_code}_${policy}"
      # 检查已有输入和输出，指定同一个 BATCH_ID 重跑时也不覆盖结果。
      if [[ -e "${inputs_root}/${run_id}" || -e "${output_dir}/${run_id}.csv" || -e "${output_dir}/${run_id}.log" ]]; then
        echo "结果已存在，拒绝覆盖：${batch_id}/${run_id}" >&2
        exit 1
      fi
      cmd=("$python_bin" -m serving
        --cluster-config configs/cluster/dual_node_8p8d.json
        --network-backend ns3 --ns3-topology "$topology"
        --kv-routing-policy "$policy"
        --dataset "workloads/kv_poisson_sps${sps}.jsonl" ${num_req_args[@]+"${num_req_args[@]}"}
        --request-routing-policy LOAD --decode-routing-policy RAND --pd-pairing dynamic
        --log-interval 1.0 --keep-inputs --run-id "$run_id"
        --inputs-root "${inputs_root}/${run_id}"
        --output "${output_dir}/${run_id}.csv")
      printf '\n[sps%s / %s / %s] %s\n' "$sps" "$scenario" "$policy" "${batch_id}/${run_id}"
      printf '%q ' "${cmd[@]}"
      printf '\n'
      if [[ "$plan_only" == 0 ]]; then
        mkdir -p "$output_dir"
        "${cmd[@]}" 2>&1 | tee "${output_dir}/${run_id}.log"
      fi
    done
  done
done
exit 0

# Single instance. Prefix caching in xPU memory is on by default.
# python -m serving --cluster-config 'configs/cluster/single_node_single_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_single_run.csv' \
#     --num-reqs 10

# Multiple instances on one node, requests routed across them
# python -m serving --cluster-config 'configs/cluster/single_node_multi_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_multi_run.csv' \
#     --num-reqs 10

# Prefix cache spilling to a shared CPU pool
# python -m serving --cluster-config 'configs/cluster/single_node_multi_instance.json' \
#     --block-size 16 \
#     --enable-prefix-caching --enable-prefix-sharing --prefix-storage CPU \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_prefix_cpu_mem_pool_run.csv' \
#     --num-reqs 10

# Prefill/decode disaggregation
# python -m serving --cluster-config 'configs/cluster/single_node_pd_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_pd_run.csv' \
#     --num-reqs 10

# CXL memory expansion
# python -m serving --cluster-config 'configs/cluster/single_node_cxl_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_cxl_run.csv' \
#     --num-reqs 10

# PIM attention offloading
# python -m serving --cluster-config 'configs/cluster/single_node_pim_instance.json' \
#     --block-size 16 --enable-attn-offloading \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_pim_run.csv' \
#     --num-reqs 10

# Power and energy modelling
# python -m serving --cluster-config 'configs/cluster/single_node_power_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_power_run.csv' \
#     --num-reqs 10 --log-interval 0.1

# Tensor + pipeline parallelism (tp=2 x pp=2 -> 4 GPUs)
# python -m serving --cluster-config 'configs/cluster/single_node_tp_pp_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_tp_pp_run.csv' \
#     --num-reqs 10

# MoE with expert parallelism (tp=2, ep=2)
# python -m serving --cluster-config 'configs/cluster/single_node_moe_single_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_moe_run.csv' \
#     --num-reqs 10

# Data parallelism (dp=2 x ep=2 -> 2 GPUs, wave-synchronized)
# python -m serving --cluster-config 'configs/cluster/single_node_moe_dp_ep_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_moe_dp_ep_run.csv' \
#     --num-reqs 10

# Agentic sessions: LLM calls chained through tool calls (SWE-bench)
# python -m serving --cluster-config 'configs/cluster/single_node_moe_dp_ep_instance.json' \
#     --block-size 16 \
#     --dataset 'workloads/swe-bench-qwen3-30b-a3b-50-sps0.2.jsonl' \
#     --output 'outputs/example_agentic_run.csv' \
#     --num-reqs 1   # session count, not request count

# Speculative decoding. --num-speculative-tokens is vLLM's own flag name, and
# the acceptance rate defaults to the model's own published measurement
# (configs/spec_decode.json). Llama-3.1-8B has no published figure, so it has
# to be given one -- and since it declares no MTP modules it drafts with a
# separate model or n-gram, which this simulator has nothing to charge for, so
# the run warns that draft *time* is not counted and the speedup is an upper
# bound. On a model with MTP modules (DeepSeek-V3.2, GLM-5, MiniMax-M3,
# Qwen3.8) the run instead **refuses** until its catalog names both
# `mtp.prologue` and `mtp.decoder_block` and the bundle has an `mtp.csv`.
# python -m serving --cluster-config 'configs/cluster/single_node_single_instance.json' \
#     --block-size 16 --num-speculative-tokens 4 --spec-acceptance-rate 0.6 \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_spec_run.csv' \
#     --num-reqs 10

# Hybrid linear-attention model (gated DeltaNet). Needs a profiled bundle for
# the model, which the repo does not ship yet -- profile it first with
# MODEL="Qwen/Qwen3.8-27B" ./profiler/profile.sh. Omitting --block-size is
# deliberate here: vLLM raises the block size until one attention page covers
# one mamba state page (784 on this model), and the bundle records what it
# settled on.
# python -m serving --cluster-config 'configs/cluster/single_node_single_instance.json' \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_hybrid_run.csv' \
#     --num-reqs 10

# Inspect what was emitted: --save-trace-text writes the human-readable trace
# next to the .et files and implies --keep-inputs, so the run's ASTRA-Sim
# inputs survive under astra-sim/inputs/runs/<run-id>/. Both can leave
# gigabytes behind.
# python -m serving --cluster-config 'configs/cluster/single_node_single_instance.json' \
#     --block-size 16 --save-trace-text --run-id 'inspect_me' \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_trace_run.csv' \
#     --num-reqs 4

# -----------------------------------------------------------------------------------------------

# NS-3 network backend. Work in progress: needs the ns-3 submodule built, and
# the placeholder below may not run until that integration lands.
# python -m serving --cluster-config 'configs/cluster/single_node_single_instance.json' \
#     --block-size 16 --network-backend 'ns3' \
#     --dataset 'workloads/example_trace.jsonl' --output 'outputs/example_ns3_run.csv' \
#     --num-reqs 10
