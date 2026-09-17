# MIT License
#
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import importlib.machinery
import importlib.util
import os
from pathlib import Path


def load_rocprofv3(path):
    path = Path(path)
    loader = importlib.machinery.SourceFileLoader("rocprofv3_launcher", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_args(module):
    return module.dotdict({"rocm_root": None, "att_library_path": None})


def test_att_library_path_single_entry(rocprofv3_script, monkeypatch):
    module = load_rocprofv3(rocprofv3_script)
    monkeypatch.setenv("ROCPROF_ATT_LIBRARY_PATH", "/opt/att-decoder")

    assert module.get_att_paths(make_args(module)) == ["/opt/att-decoder"]


def test_att_library_path_list_and_empty_entries(rocprofv3_script, monkeypatch):
    module = load_rocprofv3(rocprofv3_script)
    value = os.pathsep.join(["/opt/att-one", "", "/opt/att-two", ""])
    monkeypatch.setenv("ROCPROF_ATT_LIBRARY_PATH", value)

    assert module.get_att_paths(make_args(module)) == [
        "/opt/att-one",
        "/opt/att-two",
    ]
