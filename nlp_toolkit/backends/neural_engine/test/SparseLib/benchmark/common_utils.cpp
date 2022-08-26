//  Copyright (c) 2022 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include "common_utils.hpp"

float get_exp(float x) {
  unsigned int max = exp_ln_flt_max_f;
  unsigned int min = exp_ln_flt_min_f;
  float fmax = *reinterpret_cast<float*>(&max);
  float fmin = *reinterpret_cast<float*>(&min);
  if (x > fmax) {
    return INFINITY;
  } else if (x < fmin) {
    return 0;
  } else {
    return expf(x);
  }
}

float get_gelu(float x) {
  // an approximate fitting function of GELU(x)
  // GELU(x)≈0.5x(1+tanh[(2/pi)^0.5)*(x+0.044715x^3)]
  // for more details,pls refer this paper:https://arxiv.org/abs/1606.08415
  // printf("gelu\n");
  return 0.5f * x * (1 + tanhf(0.7978845834732056f * (x + 0.044714998453855515f * x * x * x)));
}

int get_quantize(float x, float alpha, float scale) {
  x = scale * (x - alpha);
  int ans = std::round(x);
  ans = ans > 255 ? 255 : ans;
  ans = ans < 0 ? 0 : ans;
  return ans;
}

float get_dequantize(float x, float alpha, float scale) {
  uint8_t tmp = x;
  float ans = tmp;
  ans -= alpha;
  ans *= scale;
  return ans;
}

int get_data_width(jd::data_type dtype) {
  int data_width = 0;
  switch (dtype) {
    case jd::data_type::fp32:
      data_width = 4;
      break;
    case jd::data_type::bf16:
      data_width = 2;
      break;
    case jd::data_type::u8:
    case jd::data_type::s8:
      data_width = 1;
      break;
    default:
      throw std::runtime_error(std::string("sparselib_ut_malloc error:unsupport data type."));
      break;
  }
  return data_width;
}

float apply_postop_list(float value, const std::vector<jd::postop_attr>& attrs) {
  for (auto&& i : attrs) {
    if (i.op_type == jd::postop_type::eltwise) {
      if (i.op_alg == jd::postop_alg::exp) value = get_exp(value);
      if (i.op_alg == jd::postop_alg::gelu) value = get_gelu(value);
      if (i.op_alg == jd::postop_alg::relu) value = get_relu(value, i.alpha);
      if (i.op_alg == jd::postop_alg::quantize) value = get_quantize(value, i.alpha, i.scale);
      if (i.op_alg == jd::postop_alg::dequantize) value = get_dequantize(value, i.alpha, i.scale);
      if (i.op_alg == jd::postop_alg::tanh) value = tanh(value);
    } else {
      std::runtime_error("do not support postop type.");
    }
  }
  return value;
}

void assign_val(void* ptr, jd::data_type dtype, float val, int idx) {
  switch (dtype) {
    case jd::data_type::fp32:
      *(reinterpret_cast<float*>(ptr) + idx) = val;
      break;
    case jd::data_type::bf16:
      *(reinterpret_cast<uint16_t*>(ptr) + idx) = jd::make_bf16(val);
      break;
    case jd::data_type::u8:
      *(reinterpret_cast<uint8_t*>(ptr) + idx) = static_cast<uint8_t>(val);
      break;
    default:
      throw std::runtime_error(std::string("assign_val:unsupport this dtype."));
  }
}

void* memo_op(void* ptr, int num, jd::data_type dtype, memo_mode mode) {
  int data_width = get_data_width(dtype);
  switch (mode) {
    case MALLOC:
      ptr = malloc(num * data_width);
      break;
    case MEMSET:
      std::memset(ptr, 0, num * data_width);
      break;
    default:
      break;
  }
  return ptr;
}

int get_element_num(const jd::operator_desc& op_desc, int idx) {
  auto ts_descs = op_desc.tensor_descs();
  if (idx >= ts_descs.size()) {
    std::cerr << "idx out of range" << std::endl;
    return 0;
  }
  int num = 1;
  for (auto&& i : ts_descs[idx].shape()) num *= i;
  return num;
}

namespace jd {

std::vector<postop_attr> get_postop_attr(const char* postop_str, data_type* dt_ptr) {
  std::istringstream postop_stream(postop_str);
  std::vector<postop_attr> postop_attrs(0);

  while (!postop_stream.eof()) {
    std::string postop_str;

    std::getline(postop_stream, postop_str, '+');
    size_t sep_idx = postop_str.find("_");
    if (sep_idx == postop_str.npos) {  // quantize or dequantize
      if (!strcmp(postop_str.c_str(), "quantize")) {
        postop_attrs.emplace_back(data_type::fp32, postop_type::eltwise, postop_alg::quantize, 0, 0, 0.5);
      } else if (!strcmp(postop_str.c_str(), "dequantize")) {
        postop_attrs.emplace_back(data_type::u8, postop_type::eltwise, postop_alg::dequantize, 0, 0, 2);
      } else {
        std::cerr << postop_str << " is not supported" << std::endl;
      }
    } else {  // other algorithms
      // get data_type
      const char* data_type_str = postop_str.substr(0, sep_idx).c_str();
      if (!strcmp(data_type_str, "fp32")) {
        *dt_ptr = data_type::fp32;
      } else if (!strcmp(data_type_str, "bf16")) {
        *dt_ptr = data_type::bf16;
      } else {
        std::cerr << "Unsupported data type: " << data_type_str << std::endl;
        continue;
      }

      // get algorithm
      postop_alg alg;
      const char* alg_str = postop_str.substr(sep_idx + 1).c_str();
      if (!strcmp(alg_str, "exp")) {
        alg = postop_alg::exp;
      } else if (!strcmp(alg_str, "gelu")) {
        alg = postop_alg::gelu;
      } else if (!strcmp(alg_str, "relu")) {
        alg = postop_alg::relu;
      } else if (!strcmp(alg_str, "tanh")) {
        alg = postop_alg::tanh;
      } else {
        std::cerr << "Unsupported algorithm: " << alg_str << std::endl;
        continue;
      }

      postop_attrs.emplace_back(*dt_ptr, postop_type::eltwise, alg);
    }
  }

  return postop_attrs;
}

}  // namespace jd
