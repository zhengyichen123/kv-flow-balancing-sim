# ASTRA-sim 2.0
[ASTRA-sim](https://astra-sim.github.io/) is a distributed machine learning system simulator developed by Intel, Meta, and Georgia Tech. It enables the systematic study of challenges in modern deep learning systems, allowing for the exploration of bottlenecks and the development of efficient methodologies for large DNN models across diverse future platforms.

The previous version, ASTRA-sim 1.0, is available in the `ASTRA-sim-1.0` [branch](https://github.com/astra-sim/astra-sim/tree/ASTRA-sim-1.0).

Here is a concise visual summary of our simulator:
![alt text](https://github.com/astra-sim/astra-sim/blob/master/docs/images/astrasim_overview_codesign.png)

For a comprehensive understanding of the tool, and to gain insights into its capabilities, please visit our [website](https://astra-sim.github.io/).

For information on how to use ASTRA-sim, please visit our [Wiki](https://astra-sim.github.io/astra-sim-docs/index.html).

ASTRA-sim accepts Chakra Execution Traces as workload-layer inputs. For details, please visit [Chakra Github](https://github.com/mlcommons/chakra).

We appreciate your interest and support in ASTRA-sim!

## LLMServingSim collective scopes

In this fork, collectives described by Chakra's `involved_dim` attribute use
independent stream-tag counters for each dimension scope. A TP-only operation
does not advance an EP operation's sequence, so an idle DP member can omit its
logits gather without breaking the next shared EP wave. Counters persist across
batch graphs, overlapping scopes have distinct tags, and namespace exhaustion
fails explicitly rather than wrapping. Source/destination ranks distinguish
disjoint groups. Explicit communicator handling and timing models are unchanged.

Rebuild this backend when updating LLMServingSim's idle-head execution contract;
an older binary's global counter is not compatible with that omission.

## Logical collective links

The congestion-unaware analytical frontend accepts an optional
`collective_networks` mapping in its network YAML. Supported keys are
`all_reduce`, `all_gather` and `reduce_scatter`; each value names another network
YAML, resolved relative to the containing file unless absolute. Override files
must retain the base topology and rank dimensions, and use finite positive
bandwidths and finite nonnegative latencies. The selected operation must use
`ring` or `oneRing` in the system configuration.

Each stream retains its original collective type so AllReduce's internal
scatter/gather phases use the AllReduce link. Only the network delay function
is selected: message sizes, rank groups and local reduction costs are unchanged.
Missing operations and point-to-point traffic use the common network. This is
an effective analytical link model, not NCCL channel or grouped-launch emulation.

## Expert-region trace dependencies

The bundled Chakra converter preserves the dependency chain across every
collective on an expert marker. LLMServingSim's native MoE components rely on
that ordering between routing, dispatch, experts, combine and finalization.
Update and reinstall the converter together with the frontend; the logical
collectives remain analytical tasks, not a model of NCCL grouped kernel launches.

## Contact Us

For any questions about using ASTRA-sim, you can email the ASTRA-sim User Mailing List: astrasim-users@googlegroups.com

To join the mailing list, please fill out the following form: https://forms.gle/18KVS99SG3k9CGXm6
