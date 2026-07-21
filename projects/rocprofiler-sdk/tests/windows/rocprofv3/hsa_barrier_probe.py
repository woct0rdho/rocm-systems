from __future__ import annotations

import argparse
import ctypes
import os
import time
from pathlib import Path


class HsaAgent(ctypes.Structure):
    _fields_ = [("handle", ctypes.c_uint64)]


class HsaSignal(ctypes.Structure):
    _fields_ = [("handle", ctypes.c_uint64)]


class HsaQueue(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("features", ctypes.c_uint32),
        ("base_address", ctypes.c_void_p),
        ("doorbell_signal", HsaSignal),
        ("size", ctypes.c_uint32),
        ("reserved1", ctypes.c_uint32),
        ("id", ctypes.c_uint64),
    ]


class HsaBarrierAndPacket(ctypes.Structure):
    _fields_ = [
        ("header", ctypes.c_uint16),
        ("reserved0", ctypes.c_uint16),
        ("reserved1", ctypes.c_uint32),
        ("dep_signal", HsaSignal * 5),
        ("reserved2", ctypes.c_uint64),
        ("completion_signal", HsaSignal),
    ]


def configure_runtime(runtime):
    runtime.hsa_init.argtypes = []
    runtime.hsa_init.restype = ctypes.c_uint
    runtime.hsa_shut_down.argtypes = []
    runtime.hsa_shut_down.restype = ctypes.c_uint

    agent_callback_type = ctypes.CFUNCTYPE(
        ctypes.c_uint, HsaAgent, ctypes.c_void_p
    )
    runtime.hsa_iterate_agents.argtypes = [agent_callback_type, ctypes.c_void_p]
    runtime.hsa_iterate_agents.restype = ctypes.c_uint
    runtime.hsa_agent_get_info.argtypes = [HsaAgent, ctypes.c_uint, ctypes.c_void_p]
    runtime.hsa_agent_get_info.restype = ctypes.c_uint
    runtime.hsa_queue_create.argtypes = [
        HsaAgent,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    runtime.hsa_queue_create.restype = ctypes.c_uint
    runtime.hsa_signal_create.argtypes = [
        ctypes.c_int64,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.POINTER(HsaSignal),
    ]
    runtime.hsa_signal_create.restype = ctypes.c_uint
    runtime.hsa_signal_destroy.argtypes = [HsaSignal]
    runtime.hsa_signal_destroy.restype = ctypes.c_uint
    runtime.hsa_queue_load_write_index_relaxed.argtypes = [ctypes.POINTER(HsaQueue)]
    runtime.hsa_queue_load_write_index_relaxed.restype = ctypes.c_uint64
    runtime.hsa_queue_store_write_index_relaxed.argtypes = [
        ctypes.POINTER(HsaQueue),
        ctypes.c_uint64,
    ]
    runtime.hsa_queue_store_write_index_relaxed.restype = None
    runtime.hsa_signal_store_relaxed.argtypes = [HsaSignal, ctypes.c_int64]
    runtime.hsa_signal_store_relaxed.restype = None
    runtime.hsa_signal_wait_scacquire.argtypes = [
        HsaSignal,
        ctypes.c_uint32,
        ctypes.c_int64,
        ctypes.c_uint64,
        ctypes.c_uint32,
    ]
    runtime.hsa_signal_wait_scacquire.restype = ctypes.c_int64
    return agent_callback_type


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--dll-directory", type=Path, action="append", default=[])
    args = parser.parse_args()

    if os.name != "nt":
        raise RuntimeError("the HSA barrier probe requires native Windows")
    runtime_path = args.runtime.resolve()
    if not runtime_path.is_file():
        raise FileNotFoundError(runtime_path)
    if ctypes.sizeof(HsaBarrierAndPacket) != 64:
        raise RuntimeError("unexpected HSA barrier packet size")

    dll_handles = []
    for directory in args.dll_directory:
        directory = directory.resolve()
        if not directory.is_dir():
            raise FileNotFoundError(directory)
        dll_handles.append(os.add_dll_directory(directory))

    runtime = ctypes.WinDLL(str(runtime_path))
    agent_callback_type = configure_runtime(runtime)

    init_status = runtime.hsa_init()
    print(f"runtime={runtime_path} hsa_init=0x{init_status:x}")
    if init_status != 0:
        return int(init_status)

    gpu_agents = []

    @agent_callback_type
    def collect_gpu(agent, _):
        device_type = ctypes.c_uint32()
        status = runtime.hsa_agent_get_info(agent, 17, ctypes.byref(device_type))
        if status == 0 and device_type.value == 1:
            gpu_agents.append(agent.handle)
        return 0

    iterate_status = runtime.hsa_iterate_agents(collect_gpu, None)
    print(f"hsa_iterate_agents=0x{iterate_status:x} gpu_agents={len(gpu_agents)}")
    if iterate_status != 0 or not gpu_agents:
        return int(iterate_status or 1)

    queue = ctypes.c_void_p()
    queue_create_status = runtime.hsa_queue_create(
        HsaAgent(gpu_agents[0]),
        64,
        1,
        None,
        None,
        0xFFFFFFFF,
        0xFFFFFFFF,
        ctypes.byref(queue),
    )
    print(f"hsa_queue_create=0x{queue_create_status:x} queue=0x{queue.value or 0:x}")
    if queue_create_status != 0 or not queue.value:
        return int(queue_create_status or 1)

    queue_pointer = ctypes.cast(queue, ctypes.POINTER(HsaQueue))
    queue_info = queue_pointer.contents
    completion_signal = HsaSignal()
    signal_create_status = runtime.hsa_signal_create(
        1, 0, None, ctypes.byref(completion_signal)
    )
    print(
        f"hsa_signal_create=0x{signal_create_status:x} "
        f"signal=0x{completion_signal.handle:x}"
    )
    if signal_create_status != 0 or completion_signal.handle == 0:
        return int(signal_create_status or 1)

    write_index = runtime.hsa_queue_load_write_index_relaxed(queue_pointer)
    packet = HsaBarrierAndPacket()
    packet.header = 3 | (2 << 9) | (2 << 11)
    packet.completion_signal = completion_signal
    slot_address = queue_info.base_address + (
        (write_index & (queue_info.size - 1)) * ctypes.sizeof(packet)
    )
    ctypes.memmove(slot_address, ctypes.byref(packet), ctypes.sizeof(packet))
    runtime.hsa_queue_store_write_index_relaxed(queue_pointer, write_index + 1)
    runtime.hsa_signal_store_relaxed(queue_info.doorbell_signal, write_index)

    deadline = time.monotonic() + 10.0
    observed = 1
    while observed >= 1 and time.monotonic() < deadline:
        observed = runtime.hsa_signal_wait_scacquire(
            completion_signal, 2, 1, 10_000_000, 0
        )
    print(
        f"barrier_packet queue_id={queue_info.id} packet_id={write_index} "
        f"completion={observed}"
    )
    if observed != 0:
        return 1

    signal_destroy_status = runtime.hsa_signal_destroy(completion_signal)
    print(f"hsa_signal_destroy=0x{signal_destroy_status:x}")
    if signal_destroy_status != 0:
        return int(signal_destroy_status)

    shutdown_status = runtime.hsa_shut_down()
    print(f"hsa_shut_down=0x{shutdown_status:x}")
    return int(shutdown_status)


if __name__ == "__main__":
    raise SystemExit(main())
