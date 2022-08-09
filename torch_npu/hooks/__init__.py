# Copyright (c) 2020 Huawei Technologies Co., Ltd
# All rights reserved.
#
# Licensed under the BSD 3-Clause License  (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# https://opensource.org/licenses/BSD-3-Clause
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


__all__ = ["register_acc_cmp_hook", "set_dump_path", "seed_all"]


import os
import random
import torch
import numpy as np

from . import wrap_tensor, wrap_torch
from .module import register_acc_cmp_hook
from .hooks import set_dump_path


for attr_name in dir(wrap_tensor.HOOKTensor):
    if attr_name.startswith("wrap_"):
        setattr(torch.Tensor, attr_name[5:], getattr(wrap_tensor.HOOKTensor, attr_name))


for attr_name in dir(wrap_torch):
    if attr_name.startswith("wrap_"):
        setattr(torch, attr_name[5:], getattr(wrap_torch, attr_name))


def seed_all(seed=1234):
    random.seed(seed)
    os.environ['PYTHONHASHSEED'] = str(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)