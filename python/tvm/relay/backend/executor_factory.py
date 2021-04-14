# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""Executor factory modules."""
from abc import abstractmethod

from tvm import tir

from ..._ffi.base import string_types
from ..._ffi.registry import get_global_func
from ...runtime import ndarray


class ExecutorFactoryModule:
    """Common interface for executor factory modules
    This class describes the common API of different
    factory modules
    """

    @abstractmethod
    def get_internal_repr(self):
        """Common function to return the internal representation
        the executor relies upon to execute the network
        """
        raise NotImplementedError

    @abstractmethod
    def get_params(self):
        """
        Sometimes we want to get params explicitly.
        For example, we want to save its params value to
        an independent file.
        """
        raise NotImplementedError

    @abstractmethod
    def get_lib(self):
        """ Return the generated library"""
        raise NotImplementedError


class AOTExecutorFactoryModule(ExecutorFactoryModule):
    """AOT executor factory module.

    Parameters
    ----------
    runner_function : the PrimFunc containing of the TIR main executor function.
    target : tvm.Target
        The Target used to build this module.
    libmod : tvm.Module
        The module of the corresponding function
    libmod_name: str
        The name of module
    params : dict of str to NDArray
        The parameters of module
    """

    def __init__(self, ir_mod, target, runner_function, libmod, libmod_name, params):
        assert isinstance(runner_function, tir.PrimFunc)
        args = []
        for k, v in params.items():
            args.append(k)
            args.append(ndarray.array(v))

        self.ir_mod = ir_mod
        self.target = target
        self.runner_func = runner_function
        self.lib = libmod
        self.libmod_name = libmod_name
        self.params = params
        self.iter_cnt = 0

    # Sometimes we want to get params explicitly.
    # For example, we want to save its params value to
    # an independent file.
    def get_params(self):
        return self.params

    def get_internal_repr(self):
        return self.runner_func

    def get_lib(self):
        return self.lib


class GraphExecutorFactoryModule(ExecutorFactoryModule):
    """Graph executor factory module.
    This is a module of graph executor factory

    Parameters
    ----------
    graph_str : the json graph to be deployed in json format output by graph compiler.
        The graph can contain operator(tvm_op) that points to the name of
        PackedFunc in the libmod.
    target : tvm.Target
        The Target used to build this module.
    libmod : tvm.Module
        The module of the corresponding function
    libmod_name: str
        The name of module
    params : dict of str to NDArray
        The parameters of module
    """

    def __init__(self, ir_mod, target, graph_str, libmod, libmod_name, params):
        assert isinstance(graph_str, string_types)
        fcreate = get_global_func("tvm.graph_executor_factory.create")
        args = []
        for k, v in params.items():
            args.append(k)
            args.append(ndarray.array(v))

        self.ir_mod = ir_mod
        self.target = target
        self.module = fcreate(graph_str, libmod, libmod_name, *args)
        self.graph = graph_str
        self.lib = libmod
        self.libmod_name = libmod_name
        self.params = params
        self.iter_cnt = 0

    def export_library(self, file_name, fcompile=None, addons=None, **kwargs):
        return self.module.export_library(file_name, fcompile, addons, **kwargs)

    def save_executor_config(self):
        return self.graph

    def get_params(self):
        return self.params

    def get_graph_json(self):
        return self.internal_repr

    def get_internal_repr(self):
        return self.graph

    def get_lib(self):
        return self.lib

    def __getitem__(self, item):
        return self.module.__getitem__(item)