#!/usr/bin/env python3
import argparse
import ctypes
from pathlib import Path

import torch


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("plugin", type=Path)
    parser.add_argument("--use-plugin", action="store_true")
    args = parser.parse_args()

    # Materialize HIP device, RNG, stream, allocator, and code-object state before dlopen.
    torch.randn(64, device="cuda")

    plugin = ctypes.CDLL(str(args.plugin.resolve()))
    if args.use_plugin:
        value = torch.zeros(1, dtype=torch.int32, device="cuda")
        launch = plugin.hip_late_dso_launch
        launch.argtypes = [ctypes.c_void_p]
        launch.restype = ctypes.c_int
        status = launch(ctypes.c_void_p(value.data_ptr()))
        if status != 0:
            raise RuntimeError(f"hip_late_dso_launch failed with HIP status {status}")
        torch.cuda.synchronize()
        if value.item() != 1:
            raise RuntimeError(f"unexpected plugin result: {value.item()}")

    print("Python case completed before process finalization", flush=True)


if __name__ == "__main__":
    main()
