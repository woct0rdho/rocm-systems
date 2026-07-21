#!/usr/bin/env python3

# MIT License
#
# Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.


import sys
import ctypes
import json
import os


UINT8 = ctypes.c_uint8
UINT64 = ctypes.c_uint64
SIZE_T = ctypes.c_size_t


def fatal_error(msg, exit_code=1):
    sys.stderr.write(f"Fatal error: {msg}\n")
    sys.stderr.flush()
    sys.exit(exit_code)


def build_counter_string(obj):
    counter_str = "\n".join(
        ["{:20}:\t{}".format(key, itr) for key, itr in obj.get_as_dict().items()]
    )
    spm_support = "Supported" if obj.spm_support else "Not Supported"
    counter_str += "\n" + "{:20}:\t{}".format("SPM", spm_support)
    counter_str += "\n" + "{:20}:\t".format("Dimensions")
    counter_str += " ".join(dim.__str__() for dim in obj.dimensions)
    return counter_str


class dimension:
    columns = ["Dimension_Id", "Dimension_Name", "Dimension_Instances"]

    def __init__(self, dimension_id, dimension_name, dimension_instances):
        self.id = dimension_id
        self.name = dimension_name
        self.instances = dimension_instances

    def get_as_dict(self):
        return dict(zip((self.columns), [self.id, self.name, self.instances]))

    def __str__(self):
        dimension = "{}[0:{}]".format(
            self.get_as_dict()["Dimension_Name"],
            self.get_as_dict()["Dimension_Instances"] - 1,
        )
        return dimension


class counter:

    columns = ["Counter_Name", "Description"]

    def __init__(
        self,
        counter_handle,
        counter_name,
        counter_description,
        counter_dimensions,
        is_hw_constant,
        spm_support,
    ):
        self.name = counter_name
        self.counter_handle = counter_handle
        self.description = counter_description
        self.dimensions = counter_dimensions
        self.is_hw_constant = is_hw_constant
        self.spm_support = spm_support

    def get_as_dict(self):
        return dict(zip((self.columns), [self.name, self.description]))

    def __str__(self):
        return "\n".join(
            ["{:20}:\t{}".format(key, itr) for key, itr in self.get_as_dict().items()]
        )


class derived_counter(counter):

    columns = ["Counter_Name", "Description", "Expression"]

    def __init__(
        self,
        counter_handle,
        counter_name,
        counter_description,
        counter_expression,
        counter_dimensions,
        is_hw_constant,
        spm_support,
    ):
        super().__init__(
            counter_handle,
            counter_name,
            counter_description,
            counter_dimensions,
            is_hw_constant,
            spm_support,
        )
        self.expression = counter_expression

    def get_as_dict(self):
        return dict(zip((self.columns), [self.name, self.description, self.expression]))

    def __str__(self):
        return build_counter_string(self)


class basic_counter(counter):

    columns = ["Counter_Name", "Description", "Block"]

    def __init__(
        self,
        counter_handle,
        counter_name,
        counter_description,
        counter_block,
        counter_dimensions,
        is_hw_constant,
        spm_support,
    ):
        super().__init__(
            counter_handle,
            counter_name,
            counter_description,
            counter_dimensions,
            is_hw_constant,
            spm_support,
        )
        self.block = counter_block

    def get_as_dict(self):
        return dict(zip((self.columns), [self.name, self.description, self.block]))

    def __str__(self):
        return build_counter_string(self)


class pc_config:

    columns = ["Method", "Unit", "Min_Interval", "Max_Interval", "Flags"]

    def __init__(self, config_method, config_unit, min_interval, max_interval, flags):

        self.method = self.get_method_string(config_method.value)
        self.unit = self.get_unit_string(config_unit.value)
        self.min_interval = min_interval
        self.max_interval = max_interval
        self.flags = flags

    def __str__(self):

        return "\n".join(
            [
                "   {:20}:\t{}".format(
                    key,
                    itr if key == "Method" or key == "Unit" else self.get_value(key, itr),
                )
                for key, itr in self.get_as_dict().items()
            ]
        )

    @staticmethod
    def get_value(key, itr):
        if key == "Min_Interval" or key == "Max_Interval":
            return itr.value
        elif key == "Flags":
            if itr.value == 1:
                return "interval pow2"
            else:
                return "none"
        else:
            fatal_error("Incorrect key")

    @staticmethod
    def get_method_string(key):
        method_map = {1: "stochastic", 2: "host_trap"}
        return method_map[key]

    @staticmethod
    def get_unit_string(key):
        unit_map = {1: "instructions", 2: "cycle", 3: "time"}
        return unit_map[key]

    def get_as_dict(self):

        return dict(
            zip(
                (self.columns),
                [
                    self.method,
                    self.unit,
                    self.min_interval,
                    self.max_interval,
                    self.flags,
                ],
            )
        )


class spm_config:

    columns = ["Type", "Minimum_Interval", "Maximum_Interval"]

    def __init__(self, config_type, sample_interval_min, sample_interval_max):

        self.type = self.get_type_string(config_type.value)
        self.sample_interval_min = sample_interval_min
        self.sample_interval_max = sample_interval_max

    def __str__(self):

        return "\n".join(
            [
                "   {:20}:\t{}".format(
                    key,
                    itr if key == "Type" else self.get_value(key, itr),
                )
                for key, itr in self.get_as_dict().items()
            ]
        )

    @staticmethod
    def get_value(key, itr):
        if key == "Minimum_Interval" or key == "Maximum_Interval":
            return itr.value
        else:
            fatal_error("Incorrect key")

    @staticmethod
    def get_type_string(key):
        type_map = {0: "None", 1: "SAMPLE_INTERVAL_SCLK_CYCLES"}
        return type_map[key]

    def get_as_dict(self):

        return dict(
            zip(
                (self.columns),
                [
                    self.type,
                    self.sample_interval_min,
                    self.sample_interval_max,
                ],
            )
        )


class loadLibrary:
    libname = None
    c_lib = None
    dll_directories = []


def get_library():
    if loadLibrary.c_lib is not None:
        return loadLibrary.c_lib
    if not loadLibrary.libname:
        fatal_error("ROCPROF_LIST_AVAIL_TOOL_LIBRARY did not resolve to a library")

    library_path = os.path.abspath(loadLibrary.libname)
    if os.name == "nt" and hasattr(os, "add_dll_directory"):
        search_directories = [
            os.path.dirname(library_path),
            os.path.dirname(os.path.dirname(library_path)),
        ]
        search_directories.extend(
            path
            for path in os.environ.get("ROCPROFILER_WINDOWS_DLL_DIRS", "").split(
                os.pathsep
            )
            if path
        )
        for directory in dict.fromkeys(search_directories):
            if os.path.isdir(directory):
                loadLibrary.dll_directories.append(os.add_dll_directory(directory))

    try:
        loadLibrary.c_lib = ctypes.CDLL(library_path)
    except OSError as error:
        fatal_error(f"Failed to load availability library '{library_path}': {error}")

    status_function = getattr(loadLibrary.c_lib, "availability_status", None)
    if status_function is not None:
        status_function.argtypes = [ctypes.POINTER(ctypes.c_char_p)]
        status_function.restype = ctypes.c_int
        message = ctypes.c_char_p()
        status = status_function(ctypes.byref(message))
        if status != 0:
            detail = (
                message.value.decode("utf-8", errors="replace")
                if message.value
                else "no failure detail was provided"
            )
            loadLibrary.c_lib = None
            fatal_error(f"Availability initialization failed ({status}): {detail}")
    return loadLibrary.c_lib


def get_string_value(str_ptr):
    return ctypes.cast(str_ptr, ctypes.c_char_p).value.decode("utf-8")


def get_agent_info(agent_handle):
    lib = get_library()
    lib.agent_info.argtypes = [UINT64, ctypes.POINTER(ctypes.c_char_p)]
    lib.agent_info.restype = None
    agent_info_str = ctypes.c_char_p()
    lib.agent_info(agent_handle, ctypes.byref(agent_info_str))
    return json.loads(agent_info_str.value.decode("utf-8"))


def get_number_of_counters(agent_handle):
    return get_number_of_agent_counters(agent_handle)


def get_number_agents():
    lib = get_library()
    lib.get_number_of_agents.restype = SIZE_T
    return lib.get_number_of_agents()


def get_agent_handles():
    lib = get_library()
    num_agents = get_number_agents()
    lib.agent_handles.argtypes = [ctypes.POINTER(UINT64), SIZE_T]
    lib.agent_handles.restype = None
    agent_handles_arr = (UINT64 * num_agents)()
    lib.agent_handles(agent_handles_arr, num_agents)
    return list(agent_handles_arr)


def get_agent_info_map():
    agent_info_map = {}
    agents = get_agent_handles()
    for agent in agents:
        agent_info_map[agent] = get_agent_info(agent)

    return agent_info_map


def get_number_of_agent_counters(agent_handle):
    lib = get_library()
    lib.get_number_of_agent_counters.argtypes = [UINT64]
    lib.get_number_of_agent_counters.restype = SIZE_T
    return lib.get_number_of_agent_counters(agent_handle)


def get_agent_counter_handles(agent_handle):
    lib = get_library()
    num_counters = get_number_of_agent_counters(agent_handle)
    lib.agent_counter_handles.argtypes = [ctypes.POINTER(UINT64), UINT64, SIZE_T]
    lib.agent_counter_handles.restype = None
    counter_handles = (UINT64 * num_counters)()
    lib.agent_counter_handles(counter_handles, agent_handle, num_counters)
    return list(counter_handles)


def get_dimensions(counter_handle):
    lib = get_library()
    lib.get_number_of_dimensions.argtypes = [UINT64]
    lib.get_number_of_dimensions.restype = SIZE_T
    num_dims = lib.get_number_of_dimensions(counter_handle)

    lib.counter_dimension_ids.argtypes = [UINT64, ctypes.POINTER(UINT64), SIZE_T]
    lib.counter_dimension_ids.restype = None
    dims_ids = (UINT64 * num_dims)()
    lib.counter_dimension.argtypes = [
        UINT64,
        UINT64,
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(UINT64),
    ]
    lib.counter_dimension.restype = None
    lib.counter_dimension_ids(counter_handle, dims_ids, num_dims)
    dimensions = []
    for dim_id in list(dims_ids):
        dimension_name = ctypes.c_char_p()
        dimension_instance = UINT64()
        lib.counter_dimension(
            counter_handle,
            dim_id,
            ctypes.byref(dimension_name),
            ctypes.byref(dimension_instance),
        )
        dim = dimension(
            dim_id, get_string_value(dimension_name), dimension_instance.value
        )
        dimensions.append(dim)
    return dimensions


def get_counters_helper(is_spm=False):
    agent_counters = {}
    agents = get_agent_handles()
    agent_counters = {}
    agent_info_map = get_agent_info_map()
    for agent in agents:
        if agent_info_map[agent]["type"] != 2:
            continue
        agent_counters.setdefault(agent, [])
        counters = get_agent_counter_handles(agent)
        if counters:
            for counter_id in list(counters):
                counter_info = get_counter_info(counter_id)
                if is_spm:
                    if counter_info.spm_support.value:
                        agent_counters[agent].append(counter_info)
                else:
                    agent_counters[agent].append(counter_info)
        agent_counters[agent].sort(key=lambda counter_info: counter_info.name)
    return agent_counters


def get_counters():
    return get_counters_helper(False)


def get_spm_counters():
    return get_counters_helper(True)


def get_spm_configs():
    agent_spm_config = {}
    agents = get_agent_handles()

    for agent in agents:
        configs = get_spm_config(agent)
        if len(configs) > 0:
            agent_spm_config[agent] = configs

    return agent_spm_config


def get_pc_sample_configs():
    agent_pc_sample_config = {}
    agents = get_agent_handles()

    for agent in agents:
        configs = get_pc_sample_config(agent)
        if len(configs) > 0:
            agent_pc_sample_config[agent] = configs

    return agent_pc_sample_config


def get_counter_info(counter_handle):
    lib = get_library()
    lib.counter_info.argtypes = [
        UINT64,
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(UINT8),
        ctypes.POINTER(UINT8),
        ctypes.POINTER(UINT8),
    ]
    lib.counter_info.restype = None
    counter_name = ctypes.c_char_p()
    counter_description = ctypes.c_char_p()
    is_derived = UINT8()
    is_hw_constant = UINT8()
    spm_support = UINT8()
    lib.counter_info(
        counter_handle,
        ctypes.byref(counter_name),
        ctypes.byref(counter_description),
        ctypes.byref(is_derived),
        ctypes.byref(is_hw_constant),
        ctypes.byref(spm_support),
    )

    if is_derived.value == 1:
        lib.counter_expression.argtypes = [
            UINT64,
            ctypes.POINTER(ctypes.c_char_p),
        ]
        lib.counter_expression.restype = None
        expression = ctypes.c_char_p()
        lib.counter_expression(counter_handle, ctypes.byref(expression))
        dimensions = get_dimensions(counter_handle)
        return derived_counter(
            counter_handle,
            get_string_value(counter_name),
            get_string_value(counter_description),
            get_string_value(expression),
            dimensions,
            is_hw_constant,
            spm_support,
        )

    elif not is_hw_constant.value:
        lib.counter_block.argtypes = [UINT64, ctypes.POINTER(ctypes.c_char_p)]
        lib.counter_block.restype = None
        block = ctypes.c_char_p()
        lib.counter_block(counter_handle, ctypes.byref(block))
        dimensions = get_dimensions(counter_handle)
        return basic_counter(
            counter_handle,
            get_string_value(counter_name),
            get_string_value(counter_description),
            get_string_value(block),
            dimensions,
            is_hw_constant,
            spm_support,
        )
    else:
        return counter(
            counter_handle,
            get_string_value(counter_name),
            get_string_value(counter_description),
            [],
            is_hw_constant.value,
            spm_support,
        )


def get_number_of_pc_sample_configs(agent_handle):
    lib = get_library()
    lib.get_number_of_pc_sample_configs.argtypes = [UINT64]
    lib.get_number_of_pc_sample_configs.restype = SIZE_T
    return lib.get_number_of_pc_sample_configs(agent_handle)


def get_number_of_spm_configs(agent_handle):
    lib = get_library()
    lib.get_number_of_spm_configs.argtypes = [UINT64]
    lib.get_number_of_spm_configs.restype = SIZE_T
    return lib.get_number_of_spm_configs(agent_handle)


def get_pc_sample_config(agent_handle):
    lib = get_library()
    num_configs = get_number_of_pc_sample_configs(agent_handle)
    lib.pc_sample_config.argtypes = [
        UINT64,
        UINT64,
        ctypes.POINTER(UINT64),
        ctypes.POINTER(UINT64),
        ctypes.POINTER(UINT64),
        ctypes.POINTER(UINT64),
        ctypes.POINTER(UINT64),
    ]
    lib.pc_sample_config.restype = None
    pc_configs = []

    for config in range(0, num_configs):
        method = UINT64()
        unit = UINT64()
        max_interval = UINT64()
        min_interval = UINT64()
        flags = UINT64()
        lib.pc_sample_config(
            agent_handle,
            config,
            ctypes.byref(method),
            ctypes.byref(unit),
            ctypes.byref(min_interval),
            ctypes.byref(max_interval),
            ctypes.byref(flags),
        )
        pc_configs.append(
            pc_config(
                method,
                unit,
                min_interval,
                max_interval,
                flags,
            )
        )
    return pc_configs


def get_spm_config(agent_handle):
    lib = get_library()
    num_configs = get_number_of_spm_configs(agent_handle)
    lib.spm_sample_interval_config.argtypes = [
        UINT64,
        UINT64,
        ctypes.POINTER(UINT64),
        ctypes.POINTER(UINT64),
        ctypes.POINTER(UINT64),
    ]
    lib.spm_sample_interval_config.restype = None
    spm_configs = []

    for config in range(0, num_configs):
        type_ = UINT64()
        max_interval = UINT64()
        min_interval = UINT64()
        lib.spm_sample_interval_config(
            agent_handle,
            config,
            ctypes.byref(type_),
            ctypes.byref(min_interval),
            ctypes.byref(max_interval),
        )
        spm_configs.append(
            spm_config(
                type_,
                min_interval,
                max_interval,
            )
        )
    return spm_configs


def check_pmc(agent_counter):
    lib = get_library()

    def get_counter_names(counter_ids):

        counter_names = []
        for counter_id in counter_ids:
            counter = get_counter_info(counter_id)
            if counter.counter_handle == counter_id:
                counter_names.append(counter.name)
        return counter_names

    def get_agent_name(agent_id):
        agent_info_map = get_agent_info_map()
        for agent, info in agent_info_map.items():
            if agent == agent_id:
                return info["name"]

    for agent, counter_ids in agent_counter.items():
        num_counters = len(counter_ids)
        counters = (UINT64 * num_counters)(*counter_ids)
        lib.is_counter_set.argtypes = [ctypes.POINTER(UINT64), UINT64, SIZE_T]
        lib.is_counter_set.restype = ctypes.c_bool
        if lib.is_counter_set(counters, agent, num_counters) is False:
            fatal_error(
                "{} not collected on agent {}".format(
                    " ".join(get_counter_names(counter_ids)), get_agent_name(agent)
                )
            )
    return True
