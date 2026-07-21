from __future__ import annotations

import argparse
import re
import shutil
import subprocess
from pathlib import Path

REQUIRED_EXPORTS = (
    "hsa_init",
    "hsa_shut_down",
    "hsa_system_get_info",
    "hsa_iterate_agents",
    "hsa_queue_create",
    "hsa_amd_memory_pool_allocate",
    "hsa_amd_profiling_set_profiler_enabled",
    "rocprofiler_register_import_hsa",
)
DISABLED_PC_SAMPLING_EXPORTS = (
    "hsa_ven_amd_pcs_iterate_configuration",
    "hsa_ven_amd_pcs_create",
    "hsa_ven_amd_pcs_create_from_id",
    "hsa_ven_amd_pcs_destroy",
    "hsa_ven_amd_pcs_start",
    "hsa_ven_amd_pcs_stop",
    "hsa_ven_amd_pcs_flush",
)
FORBIDDEN_DYNAMIC_RUNTIMES = ("MSVCP", "VCRUNTIME", "UCRTBASE")
REQUIRED_DEPENDENCIES = ("rocprofiler-register.dll",)


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
        description="Validate a Windows ROCr runtime without loading HSA"
    )
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    runtime = args.runtime.resolve()
    cache = args.cache.resolve()
    if not runtime.is_file() or not cache.is_file():
        raise RuntimeError(f"missing validation input: runtime={runtime} cache={cache}")

    cache_text = cache.read_text(encoding="utf-8", errors="replace")
    require_cache_value(cache_text, "CMAKE_MSVC_RUNTIME_LIBRARY", "MultiThreaded")
    require_cache_value(cache_text, "IMAGE_SUPPORT", "OFF")
    require_cache_value(cache_text, "PC_SAMPLING_SUPPORT", "OFF")
    require_cache_value(cache_text, "HSA_DEP_ROCPROFILER_REGISTER", "ON")

    dumpbin = shutil.which("dumpbin.exe")
    if dumpbin is None:
        raise RuntimeError("dumpbin.exe is required for Windows ROCr validation")

    headers = run_dumpbin(dumpbin, "/headers", runtime)
    if not re.search(r"8664\s+machine\s+\(x64\)", headers, re.IGNORECASE):
        raise RuntimeError("runtime DLL is not an x64 PE image")

    dependencies = parse_dependencies(run_dumpbin(dumpbin, "/dependents", runtime))
    if not dependencies:
        raise RuntimeError("runtime DLL has no reported dependencies")
    for dependency in dependencies:
        if dependency.upper().startswith(FORBIDDEN_DYNAMIC_RUNTIMES):
            raise RuntimeError(
                f"runtime unexpectedly depends on the dynamic MSVC runtime: {dependency}"
            )
    dependency_names = {dependency.lower() for dependency in dependencies}
    missing_dependencies = [
        dependency
        for dependency in REQUIRED_DEPENDENCIES
        if dependency.lower() not in dependency_names
    ]
    if missing_dependencies:
        raise RuntimeError(
            "runtime is missing required dependencies: " + ", ".join(missing_dependencies)
        )

    exports = run_dumpbin(dumpbin, "/exports", runtime)
    expected_exports = REQUIRED_EXPORTS + DISABLED_PC_SAMPLING_EXPORTS
    missing = [
        name for name in expected_exports if not re.search(rf"\b{re.escape(name)}\b", exports)
    ]
    if missing:
        raise RuntimeError("runtime is missing required exports: " + ", ".join(missing))

    rows = [
        "runtime_validation=passed",
        "hsa_loaded=no",
        "gpu_work_executed=no",
        "machine=x64",
        "msvc_runtime=MultiThreaded",
        "image_support=OFF",
        "pc_sampling_support=OFF",
        "rocprofiler_register=ON",
        f"dependencies={','.join(dependencies)}",
        f"exports_checked={','.join(REQUIRED_EXPORTS)}",
        f"disabled_pc_sampling_exports={','.join(DISABLED_PC_SAMPLING_EXPORTS)}",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print("\n".join(rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
