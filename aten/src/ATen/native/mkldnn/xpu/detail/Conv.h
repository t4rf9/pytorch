#pragma once

#include <ATen/ATen.h>
#include <ATen/core/grad_mode.h>
#include <ATen/record_function.h>
#include <c10/core/MemoryFormat.h>

#include "Attr.h"
#include "Utils.h"

#include <oneapi/dnnl/dnnl.hpp>

namespace at{
namespace native::xpu {
namespace onednn {

constexpr int src_batch_size_dim = 0;
constexpr int wgh_dst_channels_dim = 0;

static inline dnnl::memory::dims conv_dst_tz(
    int64_t ndim,
    IntArrayRef src_tz,
    IntArrayRef wgh_tz,
    IntArrayRef padding_front_top_left,
    IntArrayRef padding_back_bottom_right,
    IntArrayRef stride,
    IntArrayRef dilation) {
  bool has_dilation = dilation.size() > 0;
 dnnl::memory::dims dst_tz(ndim);
  dst_tz[0] = src_tz[src_batch_size_dim];
  dst_tz[1] = wgh_tz[wgh_dst_channels_dim];
  for (int d = 2; d < ndim; ++d) {
    auto dilate = has_dilation ? dilation[d - 2] : 1;
    auto kernel = dilate * (wgh_tz[d] - 1) + 1;
    dst_tz[d] =
        (src_tz[d] +
         (padding_front_top_left[d - 2] + padding_back_bottom_right[d - 2]) -
         kernel) /
            stride[d - 2] +
        1;
  }
  return dst_tz;
}

static inline dnnl::memory::dims compatible_dilation(IntArrayRef& dilation) {
 dnnl::memory::dims ret = dilation.vec();
  for (auto it = ret.begin(); it != ret.end(); it++) {
    *it -= 1;
  }
  return ret;
}

static inline dnnl::memory::format_tag conv_src_fmt(
    const int64_t ndim,
    const bool is_channels_last = false) {
  if (!is_channels_last) {
    return (ndim == 3)
        ? dnnl::memory::format_tag::ncw
        : ((ndim == 4) ? dnnl::memory::format_tag::nchw
                       : ((ndim == 5) ? dnnl::memory::format_tag::ncdhw
                                      : dnnl::memory::format_tag::undef));
  } else {
    return (ndim == 3)
        ? dnnl::memory::format_tag::nwc
        : ((ndim == 4) ? dnnl::memory::format_tag::nhwc
                       : ((ndim == 5) ? dnnl::memory::format_tag::ndhwc
                                      : dnnl::memory::format_tag::undef));
  }
}

static inline dnnl::memory::format_tag conv_wgh_fmt(
    const int64_t ndim,
    const bool grouped = false,
    const bool is_channels_last = false) {
  if (!is_channels_last) {
    return (ndim == 3)
        ? (grouped ? dnnl::memory::format_tag::goiw : dnnl::memory::format_tag::oiw)
        : (ndim == 4)
        ? (grouped ? dnnl::memory::format_tag::goihw : dnnl::memory::format_tag::oihw)
        : ((ndim == 5) ? (grouped ? dnnl::memory::format_tag::goidhw
                                  : dnnl::memory::format_tag::oidhw)
                       : dnnl::memory::format_tag::undef);
  } else {
    return (ndim == 3)
        ? (grouped ? dnnl::memory::format_tag::gowi : dnnl::memory::format_tag::owi)
        : (ndim == 4)
        ? (grouped ? dnnl::memory::format_tag::gohwi : dnnl::memory::format_tag::ohwi)
        : ((ndim == 5) ? (grouped ? dnnl::memory::format_tag::godhwi
                                  : dnnl::memory::format_tag::odhwi)
                       : dnnl::memory::format_tag::undef);
  }
}

static inline dnnl::memory::dims compatible_wgh_dims(
    const int64_t ndim,
    const int64_t groups,
    const int64_t oc,
    const int64_t ic,
    const IntArrayRef wsizes) {
  if (ndim == 3) {
    auto kw = wsizes[2];
    return (groups != 1) ? dnnl::memory::dims({groups, oc / groups, ic / groups, kw})
                         : dnnl::memory::dims({oc, ic, kw});
  } else if (ndim == 4) {
    auto kh = wsizes[2];
    auto kw = wsizes[3];
    return (groups != 1)
        ? dnnl::memory::dims({groups, oc / groups, ic / groups, kh, kw})
        : dnnl::memory::dims({oc, ic, kh, kw});
  } else if (ndim == 5) {
    auto kd = wsizes[2];
    auto kh = wsizes[3];
    auto kw = wsizes[4];
    return (groups != 1)
        ? dnnl::memory::dims({groups, oc / groups, ic / groups, kd, kh, kw})
        : dnnl::memory::dims({oc, ic, kd, kh, kw});
  }

  return {};
}

static std::tuple<
    dnnl::memory::desc,
    dnnl::memory::desc,
    dnnl::memory::desc>
 conv_get_md(
    const at::Tensor& src,
    const at::Tensor& wgh,
    const at::Tensor& dst,
    int64_t groups,
    bool is_channels_last) {
  // create memory desc from the src/wgh/dst tensors
  dnnl::memory::desc src_usr_md, wgh_usr_md, dst_usr_md;
  auto ndim = src.ndimension();
  auto fmt_src =
      conv_src_fmt(ndim, is_channels_last);

  auto src_tz = src.sizes().vec();
  auto src_data_t = get_onednn_dtype_include_double(src);
  src_usr_md = dnnl::memory::desc(src_tz, src_data_t, fmt_src);

  auto dst_tz = dst.sizes().vec();
  auto dst_data_t = get_onednn_dtype_include_double(dst);
  dst_usr_md = dnnl::memory::desc(dst_tz, dst_data_t, fmt_src);

  auto ic = src.size(1);
  auto oc = dst.size(1);
  auto wei_data_t = get_onednn_dtype_include_double(wgh);
  dnnl::memory::dims wgh_tz =
      compatible_wgh_dims(ndim, groups, oc, ic, wgh.sizes());
  auto fmt_wgh = conv_wgh_fmt(
      ndim,
      groups != 1,
      is_channels_last);
  wgh_usr_md = dnnl::memory::desc(wgh_tz, wei_data_t, fmt_wgh);

  return {src_usr_md, wgh_usr_md, dst_usr_md};
}

static at::Tensor convolution(
    at::Tensor& dst,
    const at::Tensor& src,
    const at::Tensor& wgh,
    const at::Tensor& bia,
    IntArrayRef padding_front_top_left,
    IntArrayRef padding_back_bottom_right,
    IntArrayRef stride,
    IntArrayRef dilation,
    int64_t groups,
    Attr& attr) {
  auto engine =
      GpuEngineManager::Instance().get_engine({c10::kXPU, c10::xpu::current_device()});
  auto stream = GpuStreamManager::Instance().get_stream();

  bool is_channels_last = use_channels_last_for_conv(src, wgh, false);

  // create usr_md for tensors, and md for conv primitive
  dnnl::memory::desc src_md, wgh_md, dst_md;
  std::tie(src_md, wgh_md, dst_md) = conv_get_md(src, wgh, dst, groups, is_channels_last);

  auto bia_fmt = dnnl::memory::format_tag::x;
  auto bia_md = bia.defined()
      ? dnnl::memory::desc(
            {dst.size(1)}, get_onednn_dtype_include_double(bia), bia_fmt)
      : dnnl::memory::desc();

  // create conv primitive descriptor
  dnnl::memory::dims _stride = stride.vec();
  dnnl::memory::dims _padding_front_top_left = padding_front_top_left.vec();
  dnnl::memory::dims _padding_back_bottom_right = padding_back_bottom_right.vec();
  dnnl::memory::dims _dilation = compatible_dilation(dilation);

  // extract post ops
  dnnl::primitive_attr pattr;
  dnnl::post_ops po = attr.extract_post_ops(dst);
  pattr.set_post_ops(po);

  pattr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

  auto conv_fwd_pd = dnnl::convolution_forward::primitive_desc(
      engine,
      dnnl::prop_kind::forward,
      dnnl::algorithm::convolution_direct,
      src_md,
      wgh_md,
      bia_md,
      dst_md,
      _stride,
      _dilation,
      _padding_front_top_left,
      _padding_back_bottom_right,
      pattr);

  dnnl::memory src_m, wgh_m, dst_m, bia_m;
  at::Tensor src_blocked, wgh_blocked, dst_blocked = dst;

  src_m = xpu_onednn_memory(src_md, engine, src.data_ptr());
  wgh_m = xpu_onednn_memory(wgh_md, engine, wgh.data_ptr());
  dst_m = xpu_onednn_memory(dst_md, engine, dst.data_ptr());


  std::unordered_map<int, dnnl::memory> args;
  if (bia.defined()) {
    bia_m = xpu_onednn_memory(bia_md, engine, bia.data_ptr());
    args.insert({DNNL_ARG_BIAS, bia_m});
  }
  auto expected_dst_md = conv_fwd_pd.dst_desc();
  if (attr.with_binary())
    attr.construct_post_binary(conv_fwd_pd, args);

  args.insert({DNNL_ARG_SRC, src_m});
  args.insert({DNNL_ARG_WEIGHTS, wgh_m});
  args.insert({DNNL_ARG_DST, dst_m});

  size_t scratchpad_size = conv_fwd_pd.scratchpad_desc().get_size();
  at::Tensor scratchpad_tensor = at::empty(
      {static_cast<int64_t>(scratchpad_size)}, src.options().dtype(at::kByte), c10::nullopt);
  auto scratchpad_m = xpu_onednn_memory(
      conv_fwd_pd.scratchpad_desc(), engine, scratchpad_tensor.data_ptr());
  args.insert({DNNL_ARG_SCRATCHPAD, scratchpad_m});

  auto conv_forward = dnnl::convolution_forward(conv_fwd_pd);
  XPU_ONEDNN_EXEC(conv_forward, stream, args);

  return dst;
}

static void convolution_backward_weights(
    at::Tensor& diff_wgh,
    at::Tensor& diff_bia,
    const at::Tensor& diff_dst,
    const at::Tensor& src,
    IntArrayRef diff_wgh_aten_tz,
    IntArrayRef padding_front_top_left,
    IntArrayRef padding_back_bottom_right,
    IntArrayRef stride,
    IntArrayRef dilation,
    int64_t groups) {
  auto engine =
      GpuEngineManager::Instance().get_engine({c10::kXPU, c10::xpu::current_device()});
  auto stream = GpuStreamManager::Instance().get_stream();

  bool is_channels_last = use_channels_last_for_conv(src, diff_dst, /*is_transposed=*/false);

  // create dnnl::memory desc
  dnnl::memory::desc src_md, wgh_md, dst_md;
  std::tie(src_md, wgh_md, dst_md) =
      conv_get_md(src, diff_wgh, diff_dst, groups, is_channels_last);
  dnnl::memory::format_tag bia_fmt = dnnl::memory::format_tag::x;
  auto bia_md = diff_bia.defined()
      ? dnnl::memory::desc({diff_dst.size(1)}, src_md.get_data_type(), bia_fmt)
      : dnnl::memory::desc();

  // create fwd primitive hint
  dnnl::memory::dims _stride = stride.vec();
  dnnl::memory::dims _padding_front_top_left = padding_front_top_left.vec();
  dnnl::memory::dims _padding_back_bottom_right = padding_back_bottom_right.vec();
  dnnl::memory::dims _dilation = compatible_dilation(dilation);
  dnnl::primitive_attr pattr;

  pattr.set_scratchpad_mode(dnnl::scratchpad_mode::user);
  auto conv_fwd_pd = dnnl::convolution_forward::primitive_desc(
      engine,
      dnnl::prop_kind::forward,
      dnnl::algorithm::convolution_direct,
      src_md,
      wgh_md,
      bia_md,
      dst_md,
      _stride,
      _dilation,
      _padding_front_top_left,
      _padding_back_bottom_right,
      pattr);

  // create bwd weight primitive
  auto conv_bwd_w_pd = dnnl::convolution_backward_weights::primitive_desc(
      engine,
      dnnl::algorithm::convolution_direct,
      src_md,
      wgh_md,
      bia_md,
      dst_md,
      _stride,
      _dilation,
      _padding_front_top_left,
      _padding_back_bottom_right,
      conv_fwd_pd,
      pattr);

  // create bwd memory
  at::Tensor expected_src, expected_diff_dst, expected_diff_wgh;
  dnnl::memory src_m, diff_dst_m, diff_wgh_m;

  src_m = xpu_onednn_memory(src_md, engine, src.data_ptr());
  diff_dst_m = xpu_onednn_memory(dst_md, engine, diff_dst.data_ptr());
  diff_wgh_m = xpu_onednn_memory(wgh_md, engine, diff_wgh.data_ptr());

  // insert args
  std::unordered_map<int, dnnl::memory> args;
  args.insert({DNNL_ARG_DIFF_DST, diff_dst_m});
  args.insert({DNNL_ARG_SRC, src_m});
  args.insert({DNNL_ARG_DIFF_WEIGHTS, diff_wgh_m});
  if (diff_bia.defined()) {
    dnnl::memory diff_bia_m =
        xpu_onednn_memory(bia_md, engine, diff_bia.data_ptr());
    args.insert({DNNL_ARG_DIFF_BIAS, diff_bia_m});
  }

  size_t scratchpad_size = conv_bwd_w_pd.scratchpad_desc().get_size();
  at::Tensor scratchpad_tensor = at::empty(
      {static_cast<int64_t>(scratchpad_size)}, src.options().dtype(at::kByte), c10::nullopt);
  auto scratchpad_m = xpu_onednn_memory(
      conv_bwd_w_pd.scratchpad_desc(), engine, scratchpad_tensor.data_ptr());
  args.insert({DNNL_ARG_SCRATCHPAD, scratchpad_m});

  // execute primitive
  auto conv_bwd_w = dnnl::convolution_backward_weights(conv_bwd_w_pd);
  XPU_ONEDNN_EXEC(conv_bwd_w, stream, args);

}

static void convolution_backward_data(
    at::Tensor& diff_src,
    const at::Tensor& diff_dst,
    const at::Tensor& weight,
    IntArrayRef padding_front_top_left,
    IntArrayRef padding_back_bottom_right,
    IntArrayRef stride,
    IntArrayRef dilation,
    int64_t groups,
    bool bias_defined) {
  auto engine =
      GpuEngineManager::Instance().get_engine({c10::kXPU, c10::xpu::current_device()});
  auto stream = GpuStreamManager::Instance().get_stream();

  bool is_channels_last = use_channels_last_for_conv(diff_dst, weight, /*is_transposed=*/false);

  // create memory desc
  dnnl::memory::desc src_md, wgh_md, dst_md;
  std::tie(src_md, wgh_md, dst_md) =
      conv_get_md(diff_src, weight, diff_dst, groups, is_channels_last);
  dnnl::memory::format_tag bia_fmt = dnnl::memory::format_tag::x;
  auto bia_md = bias_defined
      ? dnnl::memory::desc({diff_dst.size(1)}, wgh_md.get_data_type(), bia_fmt)
      : dnnl::memory::desc();

  // create fwd primitive desc hint
  dnnl::primitive_attr pattr;

  pattr.set_scratchpad_mode(dnnl::scratchpad_mode::user);
  dnnl::memory::dims _stride = stride.vec();
  dnnl::memory::dims _padding_front_top_left = padding_front_top_left.vec();
  dnnl::memory::dims _padding_back_bottom_right = padding_back_bottom_right.vec();
  dnnl::memory::dims _dilation = compatible_dilation(dilation);
  auto conv_forward_pd = dnnl::convolution_forward::primitive_desc(
      engine,
      dnnl::prop_kind::forward,
      dnnl::algorithm::convolution_direct,
      src_md,
      wgh_md,
      bia_md,
      dst_md,
      _stride,
      _dilation,
      _padding_front_top_left,
      _padding_back_bottom_right,
      pattr);

  auto conv_backward_data_pd = dnnl::convolution_backward_data::primitive_desc(
      engine,
      dnnl::algorithm::convolution_direct,
      src_md,
      wgh_md,
      dst_md,
      _stride,
      _dilation,
      _padding_front_top_left,
      _padding_back_bottom_right,
      conv_forward_pd,
      pattr);

  // create memory
  at::Tensor expected_src, expected_wei, expected_dst;
  dnnl::memory diff_dst_m, wei_m, diff_src_m;

  diff_src_m = xpu_onednn_memory(src_md, engine, diff_src.data_ptr());
  wei_m = xpu_onednn_memory(wgh_md, engine, weight.data_ptr());
  diff_dst_m = xpu_onednn_memory(dst_md, engine, diff_dst.data_ptr());


  // insert args
  std::unordered_map<int, dnnl::memory> args;
  size_t scratchpad_size = conv_backward_data_pd.scratchpad_desc().get_size();
  at::Tensor scratchpad_tensor = at::empty(
      {static_cast<int64_t>(scratchpad_size)}, diff_dst.options().dtype(at::kByte), c10::nullopt);
  auto scratchpad_memory = xpu_onednn_memory(
      conv_backward_data_pd.scratchpad_desc(),
      engine,
      scratchpad_tensor.data_ptr());
  args.insert({DNNL_ARG_SCRATCHPAD, scratchpad_memory});
  args.insert({DNNL_ARG_DIFF_DST, diff_dst_m});
  args.insert({DNNL_ARG_WEIGHTS, wei_m});
  args.insert({DNNL_ARG_DIFF_SRC, diff_src_m});

  // execute primitive
  auto conv_backward_data =
      dnnl::convolution_backward_data(conv_backward_data_pd);
  XPU_ONEDNN_EXEC(conv_backward_data, stream, args);

}

} // namespace onednn
} // namespace native::xpu
} // namespace at
