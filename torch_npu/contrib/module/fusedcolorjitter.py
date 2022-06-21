# Copyright (c) 2022 Huawei Technologies Co., Ltd
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

import random
from math import sin, cos, pi
import numpy as np
from PIL import Image
from torchvision.transforms import ColorJitter


class FusedColorJitterApply(object):
    def __init__(self,
                 hue=0.0,
                 saturation=1.0,
                 contrast=0.0,
                 brightness=0.0,
                 is_normalized=False,
                 force_return_array=False):
        self.hue = hue
        self.saturation = saturation
        self.contrast = contrast
        self.brightness = brightness
        self.is_normalized = is_normalized
        self.force_return_array = force_return_array
        self.half_range = 127.5 if not is_normalized else 0.5

    def hue_saturation_matrix(self, hue, saturation):
        """
        Single matrix transform for both hue and saturation change.
        Matrix taken from https://beesbuzz.biz/code/16-hsv-color-transforms.
        Derived by transforming first to YIQ, then do the modification, and transform back to RGB.
        """
        const_mat = np.array([[0.299, 0.299, 0.299],
                              [0.587, 0.587, 0.587],
                              [0.114, 0.114, 0.114],
                              ], dtype=np.float32)
        sch_mat = np.array([[0.701, -0.299, -0.300],
                            [-0.587, 0.413, -0.588],
                            [-0.114, -0.114, 0.886],
                            ], dtype=np.float32)
        ssh_mat = np.array([[0.168, -0.328, 1.250],
                            [0.330, 0.035, -1.050],
                            [-0.497, 0.292, -0.203],
                            ], dtype=np.float32)
        sch = saturation * cos(hue * 255. * pi / 180.0)
        ssh = saturation * sin(hue * 255. * pi / 180.0)
        m = const_mat + sch * sch_mat + ssh * ssh_mat
        return m

    def get_random_transform_matrix(self, hue=0.05, saturation=0.5, contrast=0.5, brightness=0.125):
        hue = random.uniform(-hue, hue)
        saturation = random.uniform(max(0, 1. - saturation), 1 + saturation)
        contrast = random.uniform(max(0, 1. - contrast), 1 + contrast)
        brightness = random.uniform(max(0, 1. - brightness), 1 + brightness)

        transform_matrix = self.hue_saturation_matrix(hue, saturation)
        transform_matrix = transform_matrix * brightness * contrast
        transform_offset = (1. - contrast) * brightness * self.half_range
        return transform_matrix, transform_offset

    def apply_image_transform(self, img, transform_matrix, transform_offset):
        H, W, C = img.shape
        img = np.matmul(img.reshape(-1, 3), transform_matrix) + transform_offset
        return img.reshape(H, W, C)

    def __call__(self, img):
        transform_matrix, transform_offset = self.get_random_transform_matrix(
            self.hue, self.saturation, self.contrast, self.brightness
        )

        if isinstance(img, Image.Image):
            img = np.asarray(img, dtype=np.float32)
            return_img = True
            self.raw_type = np.uint8
        else:
            self.raw_type = img.dtype
            return_img = False
        img = self.apply_image_transform(img, transform_matrix, transform_offset)

        img = img.clip(0., 1. if self.is_normalized else 255.).astype(self.raw_type)

        if return_img and not self.force_return_array:
            return Image.fromarray(img, mode='RGB')

        return img


class FusedColorJitter(ColorJitter):
    """Randomly change the brightness, contrast, saturation and hue of an image.

    Unlike the native torchvision.transforms.ColorJitter,
    FusedColorJitter completes the adjustment of the image's brightness, contrast, saturation, and hue,
    through a matmul and an add operation, approximately 20% performance acceleration can be achieved.

    Args:
        brightness (float or tuple of float (min, max)): How much to jitter brightness.
            brightness_factor is chosen uniformly from [max(0, 1 - brightness), 1 + brightness]
            or the given [min, max]. Should be non negative numbers.
        contrast (float or tuple of float (min, max)): How much to jitter contrast.
            contrast_factor is chosen uniformly from [max(0, 1 - contrast), 1 + contrast]
            or the given [min, max]. Should be non negative numbers.
        saturation (float or tuple of float (min, max)): How much to jitter saturation.
            saturation_factor is chosen uniformly from [max(0, 1 - saturation), 1 + saturation]
            or the given [min, max]. Should be non negative numbers.
        hue (float or tuple of float (min, max)): How much to jitter hue.
            hue_factor is chosen uniformly from [-hue, hue] or the given [min, max].
            Should have 0<= hue <= 0.5 or -0.5 <= min <= max <= 0.5.

    .. Reference 1: Affine HSV color manipulation
        https://beesbuzz.biz/code/16-hsv-color-transforms
    .. Reference 2: Affine HSV color manipulation
        https://github.com/NVIDIA/DALI/blob/release_v1.15/dali/operators/image/color/color_twist.h#L155

    """

    def __init__(self, brightness=0., contrast=0., saturation=0., hue=0.):
        super().__init__(brightness, contrast, saturation, hue)
        self.transformer = FusedColorJitterApply(brightness, contrast, saturation, hue)

    def __call__(self, img):
        return self.transformer(img)
