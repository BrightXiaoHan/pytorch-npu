import math
import torch
import numpy as np
import torch_npu
import torch_npu.npu.utils as utils

from torch_npu.testing.testcase import TestCase, run_tests


class TestIncreFlashAttention(TestCase):
    def baseline(self, query_states1, past_key, past_value, head_dim, hidden_size):
        attn_weights1 = torch.matmul(query_states1, past_key.transpose(2, 3)) / 0.0078125
        attn_weights1 = torch.max(attn_weights1, torch.full(
            (1, 1), torch.finfo(attn_weights1.dtype).min, device=attn_weights1.device))
        attn_weights1 = torch.nn.functional.softmax(attn_weights1, dim=-1, dtype=torch.float32).to(query_states1.dtype)
        attn_output1 = torch.matmul(attn_weights1, past_value)
        attn_output1 = attn_output1.transpose(1, 2)
        attn_output1 = attn_output1.reshape(1, 1, hidden_size)  # IFA (1, 1, 4096)
        return attn_output1

    def trans_BNSD2BSH(self, tensor: torch.Tensor):
        tensor = torch.transpose(tensor, 1, 2)
        tensor = torch.reshape(tensor, (tensor.shape[0], tensor.shape[1], -1))
        return tensor

    def incre_flash_attention_npu(self, q, k, v, head_dim):
        scale = 1 / 0.0078125
        return torch_npu.npu_incre_flash_attention(q, k, v, num_heads=32, input_layout="BSH", scale_value=scale)

    def test_op_exec(self):
        q = torch.randn(1, 32, 1, 128, dtype=torch.float16).npu()
        k = torch.randn(1, 32, 2048, 128, dtype=torch.float16).npu()
        v = torch.randn(1, 32, 2048, 128, dtype=torch.float16).npu()

        q_FA = self.trans_BNSD2BSH(q)
        k_FA = self.trans_BNSD2BSH(k)
        v_FA = self.trans_BNSD2BSH(v)

        print("input tensor size: q k v")
        print(q_FA.size(), k_FA.size(), v_FA.size())

        head_dim = 128
        hidden_size = 4096

        ifa_out = self.incre_flash_attention_npu(q_FA, k_FA, v_FA, head_dim)
        print("IFA output", ifa_out, ifa_out.shape)

        baseline_out = self.baseline(q, k, v, head_dim, hidden_size)
        print("baseline output", baseline_out, baseline_out.shape)

        self.assertRtolEqual(ifa_out, baseline_out)


if __name__ == "__main__":
    run_tests()
