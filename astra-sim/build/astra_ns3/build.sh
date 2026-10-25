#!/bin/bash
set -e
# Absolue path to this script
SCRIPT_DIR=$(dirname "$(realpath $0)")
# Absolute paths to useful directories
ASTRA_SIM_DIR="${SCRIPT_DIR:?}"/../../astra-sim
NS3_DIR="${SCRIPT_DIR:?}"/../../extern/network_backend/ns-3
# Inputs - change as necessary.
# WORKLOAD is a chakra workload prefix: <prefix>.<rank>.et must exist for every
# rank. The Python frontend never calls this script: it generates the workload
# for each run under inputs/runs/<run_id>/workload/event_handler/llm and passes
# that path to the ns-3 binary itself. Only the standalone -r/-d/--special_debug
# entry points need WORKLOAD, so it falls back to the newest generated run.
WORKLOAD="${WORKLOAD:-}"
SYSTEM="${SCRIPT_DIR:?}"/../../inputs/system/system.json
MEMORY="${SCRIPT_DIR:?}"/../../inputs/memory/no_memory_expansion.json
LOGICAL_TOPOLOGY="${SCRIPT_DIR:?}"/../../inputs/logical_topology/logical_8nodes_1D.json
# Note that ONLY this file is relative to NS3_DIR/simulation
NETWORK="../../../ns-3/scratch/config/config.txt"
# Functions
function setup {
    protoc et_def.proto\
        --proto_path ${SCRIPT_DIR}/../../extern/graph_frontend/chakra/schema/protobuf/\
        --cpp_out ${SCRIPT_DIR}/../../extern/graph_frontend/chakra/schema/protobuf/
}
function compile {
    cd "${NS3_DIR}"
    ./ns3 configure --enable-mpi
    ./ns3 build AstraSimNetwork -j 12
    cd "${SCRIPT_DIR:?}"
}
function resolve_workload {
    # Compile/clean modes do not need a workload, so this check lives here
    # instead of at the top of the script where it would break `build.sh -c`.
    if [ -z "${WORKLOAD}" ]; then
        WORKLOAD=$(ls -1d "${SCRIPT_DIR:?}"/../../inputs/runs/*/workload/event_handler/llm 2>/dev/null | sort | tail -n 1)
    fi
    : "${WORKLOAD:?no generated workload found; run the Python frontend once or set WORKLOAD=<prefix>}"
}
function run {
    resolve_workload
    mkdir -p "${NS3_DIR}/scratch/output"
    cd "${NS3_DIR}/build/scratch"
    ./ns3.42-AstraSimNetwork-default \
        --workload-configuration=${WORKLOAD} \
        --system-configuration=${SYSTEM} \
        --network-configuration=${NETWORK} \
        --memory-configuration=${MEMORY} \
        --logical-topology-configuration=${LOGICAL_TOPOLOGY} \
        --comm-group-configuration=\"empty\"
    cd "${SCRIPT_DIR:?}"
}
function cleanup {
    cd "${NS3_DIR}"
    ./ns3 distclean
    cd "${SCRIPT_DIR:?}"
}
function cleanup_result {
    echo '0'
}
function debug {
    resolve_workload
    cd "${NS3_DIR}"
    ./ns3 configure --enable-mpi --build-profile debug
    ./ns3 build AstraSimNetwork -j 12 -v
    cd "${NS3_DIR}/build/scratch"
    gdb --args "${NS3_DIR}/build/scratch/ns3.42-AstraSimNetwork-debug" \
        --workload-configuration=${WORKLOAD} \
        --system-configuration=${SYSTEM} \
        --network-configuration=${NETWORK} \
        --memory-configuration=${MEMORY} \
        --logical-topology-configuration=${LOGICAL_TOPOLOGY} \
        --comm-group-configuration=\"empty\"
}
function special_debug {
    resolve_workload
    cd "${NS3_DIR}/build/scratch"
    valgrind --leak-check=yes "${NS3_DIR}/build/scratch/ns3.42-AstraSimNetwork-default" \
        --workload-configuration=${WORKLOAD} \
        --system-configuration=${SYSTEM} \
        --network-configuration=${NETWORK} \
        --memory-configuration=${MEMORY} \
        --logical-topology-configuration=${LOGICAL_TOPOLOGY} \
        --comm-group-configuration=\"empty\"
}
# Main Script
case "$1" in
-l|--clean)
    cleanup;;
-lr|--clean-result)
    cleanup
    cleanup_result;;
-d|--debug)
    setup
    debug;;
-c|--compile|"")
    setup
    compile;;
-r|--run)
    # setup
    # compile
    run;;
-h|--help|*)
    printf "Prints help message";;
esac
