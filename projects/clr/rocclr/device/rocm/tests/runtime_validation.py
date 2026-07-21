from __future__ import annotations

import argparse
import re
import shutil
import subprocess
from pathlib import Path

REQUIRED_EXPORTS = (
    "hipGetDeviceProperties",
    "hipLaunchKernel",
    "hipMalloc",
    "hipMemcpy",
    "hsa_init",
    "hsa_queue_create",
    "rocprofiler_register_import_hsa",
    "rocprofiler_register_import_hip",
    "rocprofiler_register_import_hip_compiler",
    "rocprofiler_register_import_hip_tools",
)
FORBIDDEN_DYNAMIC_RUNTIMES = ("MSVCP", "VCRUNTIME", "UCRTBASE")
FORBIDDEN_DIRECT_DEPENDENCIES = ("hsa-runtime64.dll",)
REQUIRED_DEPENDENCIES = ("rocprofiler-register.dll", "rocm_kpack.dll")
REQUIRED_EMBEDDED_STRINGS = (
    b"amd_comgr.dll",
    b"aql_profile_pm4",
    b"reject AQL Profile vendor packet",
    b"hsa_init",
)
FORBIDDEN_EMBEDDED_STRINGS = (
    b"hsa-runtime64.dll",
    b"aqlprofile_pmc_create_packets",
    b"ROCPROFILER_WINDOWS_QUEUE_MODE",
    b"ROCPROFILER_WINDOWS_PMC_AQLPROFILE_DLL",
    b"ROCPROFILER_WINDOWS_PMC_OUTPUT",
    b"ROCPROFILER_WINDOWS_PMC_COUNTERS",
    b"ROCPROFILER_WINDOWS_PMC_DISPATCH",
    b"ROCR_USE_PM4",
    b"WSLKMT_VENDOR_PACKET",
)


def run_dumpbin(dumpbin: str, option: str, path: Path) -> str:
    return subprocess.run(
        [dumpbin, option, str(path)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    ).stdout


def parse_dependencies(output: str) -> list[str]:
    marker = "Image has the following dependencies:"
    if marker not in output:
        raise RuntimeError("dumpbin did not report the dependency section")
    section = output.split(marker, 1)[1].split("Summary", 1)[0]
    return [line.strip() for line in section.splitlines() if line.strip().lower().endswith(".dll")]


def require_cache_value(cache_text: str, name: str, value: str) -> None:
    if not re.search(
        rf"^{re.escape(name)}:[^=]+={re.escape(value)}$",
        cache_text,
        re.MULTILINE | re.IGNORECASE,
    ):
        raise RuntimeError(f"CMake cache does not contain {name}={value}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate a Windows HIP/CLR/ROCr binary without loading HSA"
    )
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    runtime = args.runtime.resolve()
    build_directory = args.build_directory.resolve()
    cache = args.cache.resolve()
    for path in (runtime, build_directory, cache):
        if not path.exists():
            raise RuntimeError(f"missing validation input: {path}")

    cache_text = cache.read_text(encoding="utf-8", errors="replace")
    for name, value in (
        ("CMAKE_MSVC_RUNTIME_LIBRARY", "MultiThreaded"),
        ("CLR_BUILD_HIP", "ON"),
        ("ROCCLR_ENABLE_HSA", "ON"),
        ("ROCCLR_ENABLE_PAL", "OFF"),
        ("ROCCLR_BUILD_WINDOWS_TESTS", "ON"),
        ("ROCM_KPACK_ENABLED", "ON"),
        ("USE_PROF_API", "OFF"),
        ("HIP_ENABLE_ROCPROFILER_REGISTER", "ON"),
        ("HSA_DEP_ROCPROFILER_REGISTER", "ON"),
        ("IMAGE_SUPPORT", "OFF"),
        ("PC_SAMPLING_SUPPORT", "OFF"),
    ):
        require_cache_value(cache_text, name, value)

    dumpbin = shutil.which("dumpbin.exe")
    if dumpbin is None:
        raise RuntimeError("dumpbin.exe is required for Windows HIP runtime validation")

    headers = run_dumpbin(dumpbin, "/headers", runtime)
    if not re.search(r"8664\s+machine\s+\(x64\)", headers, re.IGNORECASE):
        raise RuntimeError("HIP runtime DLL is not an x64 PE image")

    dependencies = parse_dependencies(run_dumpbin(dumpbin, "/dependents", runtime))
    if not dependencies:
        raise RuntimeError("HIP runtime DLL has no reported dependencies")
    forbidden_direct = {value.lower() for value in FORBIDDEN_DIRECT_DEPENDENCIES}
    for dependency in dependencies:
        if dependency.upper().startswith(FORBIDDEN_DYNAMIC_RUNTIMES):
            raise RuntimeError(
                f"HIP runtime unexpectedly depends on the dynamic MSVC runtime: {dependency}"
            )
        if dependency.lower() in forbidden_direct:
            raise RuntimeError(f"HIP runtime unexpectedly depends on {dependency}")
    dependency_names = {dependency.lower() for dependency in dependencies}
    missing_dependencies = [
        dependency
        for dependency in REQUIRED_DEPENDENCIES
        if dependency.lower() not in dependency_names
    ]
    if missing_dependencies:
        raise RuntimeError(
            "HIP runtime is missing required dependencies: "
            + ", ".join(missing_dependencies)
        )

    exports = run_dumpbin(dumpbin, "/exports", runtime)
    missing_exports = [
        name for name in REQUIRED_EXPORTS if not re.search(rf"\b{re.escape(name)}\b", exports)
    ]
    if missing_exports:
        raise RuntimeError("HIP runtime is missing required exports: " + ", ".join(missing_exports))

    image = runtime.read_bytes()
    missing_strings = [
        value.decode("ascii") for value in REQUIRED_EMBEDDED_STRINGS if value not in image
    ]
    if missing_strings:
        raise RuntimeError(
            "HIP runtime is missing embedded profiling markers: " + ", ".join(missing_strings)
        )
    forbidden_strings = [
        value.decode("ascii") for value in FORBIDDEN_EMBEDDED_STRINGS if value in image
    ]
    if forbidden_strings:
        raise RuntimeError(
            "HIP runtime unexpectedly contains retired activation or raw PMC names: "
            + ", ".join(forbidden_strings)
        )

    required_objects = (
        build_directory
        / "rocclr/hsa-runtime64/libhsakmt/CMakeFiles/hsakmt-staticdrm.dir/src/dxg/wddm/queue.cpp.obj",
        build_directory
        / "rocclr/hsa-runtime64/libhsakmt/CMakeFiles/hsakmt-staticdrm.dir/src/dxg/wddm/device.cpp.obj",
        build_directory
        / "rocclr/hsa-runtime64/runtime/hsa-runtime/CMakeFiles/hsa-runtime64_static.dir/core/runtime/amd_aql_queue.cpp.obj",
    )
    missing_objects = [path for path in required_objects if not path.is_file()]
    if missing_objects:
        raise RuntimeError(
            "HIP runtime build is missing repaired ROCr/libhsakmt objects: "
            + ", ".join(str(path) for path in missing_objects)
        )

    rows = [
        "hip_runtime_validation=passed",
        "hsa_loaded=no",
        "gpu_work_executed=no",
        "machine=x64",
        "msvc_runtime=MultiThreaded",
        "rocr_linkage=static",
        "hsa_backend=ON",
        "pal_backend=OFF",
        "image_support=OFF",
        "pc_sampling_support=OFF",
        "rocm_kpack=ON",
        "rocprofiler_register_hip=ON",
        "rocprofiler_register_hsa=ON",
        "wddm_aql_profile_activation=capability",
        "retired_pm4_environment_inputs=absent",
        f"dependencies={','.join(dependencies)}",
        f"exports_checked={','.join(REQUIRED_EXPORTS)}",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print("\n".join(rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
