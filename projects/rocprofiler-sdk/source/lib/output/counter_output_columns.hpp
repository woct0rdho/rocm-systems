// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <array>
#include <string_view>

namespace rocprofiler
{
namespace tool
{
namespace csv
{
inline constexpr auto agent_info_columns = std::array<std::string_view, 53>{
    "Node_Id",
    "Logical_Node_Id",
    "Agent_Type",
    "Cpu_Cores_Count",
    "Simd_Count",
    "Cpu_Core_Id_Base",
    "Simd_Id_Base",
    "Max_Waves_Per_Simd",
    "Lds_Size_In_Kb",
    "Gds_Size_In_Kb",
    "Num_Gws",
    "Wave_Front_Size",
    "Num_Xcc",
    "Cu_Count",
    "Array_Count",
    "Num_Shader_Banks",
    "Simd_Arrays_Per_Engine",
    "Cu_Per_Simd_Array",
    "Simd_Per_Cu",
    "Max_Slots_Scratch_Cu",
    "Gfx_Target_Version",
    "Vendor_Id",
    "Device_Id",
    "Location_Id",
    "Domain",
    "Drm_Render_Minor",
    "Num_Sdma_Engines",
    "Num_Sdma_Xgmi_Engines",
    "Num_Sdma_Queues_Per_Engine",
    "Num_Cp_Queues",
    "Max_Engine_Clk_Ccompute",
    "Max_Engine_Clk_Fcompute",
    "Sdma_Fw_Version",
    "Fw_Version",
    "Capability",
    "Cu_Per_Engine",
    "Max_Waves_Per_Cu",
    "Family_Id",
    "Workgroup_Max_Size",
    "Grid_Max_Size",
    "Local_Mem_Size",
    "Hive_Id",
    "Gpu_Id",
    "Workgroup_Max_Dim_X",
    "Workgroup_Max_Dim_Y",
    "Workgroup_Max_Dim_Z",
    "Grid_Max_Dim_X",
    "Grid_Max_Dim_Y",
    "Grid_Max_Dim_Z",
    "Name",
    "Vendor_Name",
    "Product_Name",
    "Model_Name"};

inline constexpr auto counter_collection_columns = std::array<std::string_view, 19>{
    "Correlation_Id",
    "Dispatch_Id",
    "Agent_Id",
    "Queue_Id",
    "Process_Id",
    "Thread_Id",
    "Grid_Size",
    "Kernel_Id",
    "Kernel_Name",
    "Workgroup_Size",
    "LDS_Block_Size",
    "Scratch_Size",
    "VGPR_Count",
    "Accum_VGPR_Count",
    "SGPR_Count",
    "Counter_Name",
    "Counter_Value",
    "Start_Timestamp",
    "End_Timestamp"};
}  // namespace csv
}  // namespace tool
}  // namespace rocprofiler
