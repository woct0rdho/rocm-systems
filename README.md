# Fork of [rocm-systems](https://github.com/ROCm/rocm-systems) for PC sampling on gfx1151 (Strix Halo)

This is supposed to work with my forked amdgpu driver https://github.com/woct0rdho/linux-amdgpu-driver

## Installation

1. Install the latest `rocm-sdk-devel` from TheRock, and export `ROCM_PATH` to be the path to `_rocm_sdk_devel`
2. Clone this repo (use `--filter=blob:none` to save time) and checkout `pc_sampling_gfx1151` branch
3. `bash build_rocr.sh` (It installs to `ROCM_PATH` and overwrites the existing version)
4. `bash build_aqlprofile.sh`
5. `bash build_rocprofiler.sh`
6. Run `bash build_pc_sampling_test.sh` to build the test binary, and run `python3 test_pc_sampling.py` to test it
