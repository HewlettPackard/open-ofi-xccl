# Open OFI XCCL

Open OFI XCCL is a plug-in which enables developers to use [libfabric](https://github.com/ofiwg/libfabric) as a network provider while running collective communication applications based on either [NVIDIA's NCCL](https://github.com/NVIDIA/nccl) or [AMD's RCCL (ROCm Communication Collectives Library)](https://github.com/ROCmSoftwarePlatform/rccl).

## Overview

Machine learning frameworks running on top of NVIDIA or AMD GPUs use libraries such as [NCCL](https://developer.nvidia.com/nccl) and [RCCL](https://github.com/ROCmSoftwarePlatform/rccl) to provide standard collective communication routines for multiple GPUs across single or multiple nodes.

This project implements a plug-in that maps NCCL and RCCL's connection-oriented transport APIs to [libfabric's](https://ofiwg.github.io/libfabric/) connection-less reliable interface. This allows NCCL and RCCL applications to benefit from libfabric's transport layer services such as reliable message support and OS bypass.

## Getting Started

Release version numbers that end in `-aws` or `-xccl` indicate the platform or vendor focus. While earlier versions were tested primarily on AWS EC2 instances and EFA, the current maintained version is tested and supported by HPE Slingshot.

It is recommended to check out a release tag that ends in "-xccl" rather than using a development build.

## Basic Requirements

The plugin is regularly tested on Cray EX Slingshot systems.

Most Libfabric providers should work with the plugin, possibly through a utility provider. The plugin generally requires Reliable datagram endpoints (`FI_EP_RDM`) with tagged messaging (`FI_TAGGED`, `FI_MSG`). This is similar to the requirements of most MPI implementations and a generally tested path in Libfabric. For GPUDirect RDMA support, the plugin also requires `FI_HMEM` support, as well as RDMA support.

## Getting Help

If you have any issues building or using the package or if you think you may have found a bug, please open an [issue](https://github.com/HewlettPackard/open-ofi-xccl/issues) in this repository. For additional help or support, please file a bug in the issue tracker.

## Contributing

Reporting issues and sending pull requests are always welcome.

## License

This library is licensed under the [Apache 2.0 License](LICENSE).

## Ownership

This project is maintained and supported by HPE Slingshot.
