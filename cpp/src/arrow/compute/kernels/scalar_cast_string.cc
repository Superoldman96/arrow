// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <limits>
#include <optional>

#include "arrow/array/array_base.h"
#include "arrow/array/builder_binary.h"
#include "arrow/compute/kernels/base_arithmetic_internal.h"
#include "arrow/compute/kernels/codegen_internal.h"
#include "arrow/compute/kernels/common_internal.h"
#include "arrow/compute/kernels/scalar_cast_internal.h"
#include "arrow/compute/kernels/temporal_internal.h"
#include "arrow/result.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/formatting.h"
#include "arrow/util/int_util.h"
#include "arrow/util/logging_internal.h"
#include "arrow/util/utf8_internal.h"
#include "arrow/visit_data_inline.h"

namespace arrow {

using internal::StringFormatter;
using internal::VisitSetBitRunsVoid;
using util::InitializeUTF8;
using util::ValidateUTF8Inline;

namespace compute {
namespace internal {

namespace {

// ----------------------------------------------------------------------
// Number / Boolean to String

template <typename O, typename I>
struct NumericToStringCastFunctor {
  using value_type = typename TypeTraits<I>::CType;
  using BuilderType = typename TypeTraits<O>::BuilderType;
  using FormatterType = StringFormatter<I>;

  static Status Exec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
    const ArraySpan& input = batch[0].array;
    FormatterType formatter(input.type);
    BuilderType builder(input.type->GetSharedPtr(), ctx->memory_pool());
    RETURN_NOT_OK(VisitArraySpanInline<I>(
        input,
        [&](value_type v) {
          return formatter(v, [&](std::string_view v) { return builder.Append(v); });
        },
        [&]() { return builder.AppendNull(); }));

    std::shared_ptr<Array> output_array;
    RETURN_NOT_OK(builder.Finish(&output_array));
    out->value = std::move(output_array->data());
    return Status::OK();
  }
};

template <typename O, typename I>
struct DecimalToStringCastFunctor {
  using value_type = typename TypeTraits<I>::CType;
  using BuilderType = typename TypeTraits<O>::BuilderType;
  using FormatterType = StringFormatter<I>;

  static Status Exec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
    const ArraySpan& input = batch[0].array;
    FormatterType formatter(input.type);
    BuilderType builder(input.type->GetSharedPtr(), ctx->memory_pool());
    RETURN_NOT_OK(VisitArraySpanInline<I>(
        input,
        [&](std::string_view bytes) {
          value_type value(reinterpret_cast<const uint8_t*>(bytes.data()));
          return formatter(value, [&](std::string_view v) { return builder.Append(v); });
        },
        [&]() { return builder.AppendNull(); }));

    std::shared_ptr<Array> output_array;
    RETURN_NOT_OK(builder.Finish(&output_array));
    out->value = std::move(output_array->data());
    return Status::OK();
  }
};

// ----------------------------------------------------------------------
// Temporal to String

template <typename O, typename I>
struct TemporalToStringCastFunctor {
  using value_type = typename TypeTraits<I>::CType;
  using BuilderType = typename TypeTraits<O>::BuilderType;
  using FormatterType = StringFormatter<I>;

  static Status Exec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
    const ArraySpan& input = batch[0].array;
    FormatterType formatter(input.type);
    BuilderType builder(input.type->GetSharedPtr(), ctx->memory_pool());
    RETURN_NOT_OK(VisitArraySpanInline<I>(
        input,
        [&](value_type v) {
          return formatter(v, [&](std::string_view v) { return builder.Append(v); });
        },
        [&]() { return builder.AppendNull(); }));

    std::shared_ptr<Array> output_array;
    RETURN_NOT_OK(builder.Finish(&output_array));
    out->value = std::move(output_array->data());
    return Status::OK();
  }
};

template <typename O>
struct TemporalToStringCastFunctor<O, TimestampType> {
  using value_type = typename TypeTraits<TimestampType>::CType;
  using BuilderType = typename TypeTraits<O>::BuilderType;
  using FormatterType = StringFormatter<TimestampType>;

  static Status Exec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
    const ArraySpan& input = batch[0].array;
    const auto& timezone = GetInputTimezone(*input.type);
    const auto& ty = checked_cast<const TimestampType&>(*input.type);
    BuilderType builder(input.type->GetSharedPtr(), ctx->memory_pool());

    // Preallocate
    int64_t string_length = 19;  // YYYY-MM-DD HH:MM:SS
    if (ty.unit() == TimeUnit::MILLI) {
      string_length += 4;  // .SSS
    } else if (ty.unit() == TimeUnit::MICRO) {
      string_length += 7;  // .SSSSSS
    } else if (ty.unit() == TimeUnit::NANO) {
      string_length += 10;  // .SSSSSSSSS
    }
    if (!timezone.empty()) string_length += 5;  // +0000
    RETURN_NOT_OK(builder.Reserve(input.length));
    RETURN_NOT_OK(
        builder.ReserveData((input.length - input.GetNullCount()) * string_length));

    if (timezone.empty()) {
      FormatterType formatter(input.type);
      RETURN_NOT_OK(VisitArraySpanInline<TimestampType>(
          input,
          [&](value_type v) {
            return formatter(v, [&](std::string_view v) { return builder.Append(v); });
          },
          [&]() {
            builder.UnsafeAppendNull();
            return Status::OK();
          }));
    } else {
      switch (ty.unit()) {
        case TimeUnit::SECOND:
          RETURN_NOT_OK(ConvertZoned<std::chrono::seconds>(input, timezone, &builder));
          break;
        case TimeUnit::MILLI:
          RETURN_NOT_OK(
              ConvertZoned<std::chrono::milliseconds>(input, timezone, &builder));
          break;
        case TimeUnit::MICRO:
          RETURN_NOT_OK(
              ConvertZoned<std::chrono::microseconds>(input, timezone, &builder));
          break;
        case TimeUnit::NANO:
          RETURN_NOT_OK(
              ConvertZoned<std::chrono::nanoseconds>(input, timezone, &builder));
          break;
        default:
          DCHECK(false);
          return Status::NotImplemented("Unimplemented time unit");
      }
    }
    std::shared_ptr<Array> output_array;
    RETURN_NOT_OK(builder.Finish(&output_array));
    out->value = std::move(output_array->data());
    return Status::OK();
  }

  template <typename Duration>
  static Status ConvertZoned(const ArraySpan& input, const std::string& timezone,
                             BuilderType* builder) {
    static const std::string kFormatString = "%Y-%m-%d %H:%M:%S%z";
    static const std::string kUtcFormatString = "%Y-%m-%d %H:%M:%SZ";
    DCHECK(!timezone.empty());
    ARROW_ASSIGN_OR_RAISE(const time_zone* tz, LocateZone(timezone));
    ARROW_ASSIGN_OR_RAISE(std::locale locale, GetLocale("C"));
    TimestampFormatter<Duration> formatter{
        timezone == "UTC" ? kUtcFormatString : kFormatString, tz, locale};
    return VisitArraySpanInline<TimestampType>(
        input,
        [&](value_type v) {
          ARROW_ASSIGN_OR_RAISE(auto formatted, formatter(v));
          return builder->Append(std::move(formatted));
        },
        [&]() {
          builder->UnsafeAppendNull();
          return Status::OK();
        });
  }
};

// ----------------------------------------------------------------------
// Binary-like to binary-like
//

#if defined(_MSC_VER)
// Silence warning: """'visitor': unreferenced local variable"""
#  pragma warning(push)
#  pragma warning(disable : 4101)
#endif

struct Utf8Validator {
  Status VisitNull() { return Status::OK(); }

  Status VisitValue(std::string_view str) {
    if (ARROW_PREDICT_FALSE(!ValidateUTF8Inline(str))) {
      return Status::Invalid("Invalid UTF8 payload");
    }
    return Status::OK();
  }
};

template <typename I, typename O>
Status CastBinaryToBinaryOffsets(KernelContext* ctx, const ArraySpan& input,
                                 ArrayData* output) {
  static_assert(std::is_same<I, O>::value, "Cast same-width offsets (no-op)");
  return Status::OK();
}

// Upcast offsets
template <>
Status CastBinaryToBinaryOffsets<int32_t, int64_t>(KernelContext* ctx,
                                                   const ArraySpan& input,
                                                   ArrayData* output) {
  using input_offset_type = int32_t;
  using output_offset_type = int64_t;
  ARROW_ASSIGN_OR_RAISE(
      output->buffers[1],
      ctx->Allocate((output->length + output->offset + 1) * sizeof(output_offset_type)));
  memset(output->buffers[1]->mutable_data(), 0,
         output->offset * sizeof(output_offset_type));
  ::arrow::internal::CastInts(input.GetValues<input_offset_type>(1),
                              output->GetMutableValues<output_offset_type>(1),
                              output->length + 1);
  return Status::OK();
}

// Downcast offsets
template <>
Status CastBinaryToBinaryOffsets<int64_t, int32_t>(KernelContext* ctx,
                                                   const ArraySpan& input,
                                                   ArrayData* output) {
  using input_offset_type = int64_t;
  using output_offset_type = int32_t;

  constexpr input_offset_type kMaxOffset = std::numeric_limits<output_offset_type>::max();

  auto input_offsets = input.GetValues<input_offset_type>(1);

  // Binary offsets are ascending, so it's enough to check the last one for overflow.
  if (input_offsets[input.length] > kMaxOffset) {
    return Status::Invalid("Failed casting from ", input.type->ToString(), " to ",
                           output->type->ToString(), ": input array too large");
  } else {
    ARROW_ASSIGN_OR_RAISE(output->buffers[1],
                          ctx->Allocate((output->length + output->offset + 1) *
                                        sizeof(output_offset_type)));
    memset(output->buffers[1]->mutable_data(), 0,
           output->offset * sizeof(output_offset_type));
    ::arrow::internal::CastInts(input_offsets,
                                output->GetMutableValues<output_offset_type>(1),
                                output->length + 1);
    return Status::OK();
  }
}

// Offset String -> Offset String
template <typename O, typename I>
enable_if_t<is_base_binary_type<I>::value && is_base_binary_type<O>::value, Status>
BinaryToBinaryCastExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
  const ArraySpan& input = batch[0].array;

  if constexpr (!I::is_utf8 && O::is_utf8) {
    if (!options.allow_invalid_utf8) {
      InitializeUTF8();
      ArraySpanVisitor<I> visitor;
      Utf8Validator validator;
      RETURN_NOT_OK(visitor.Visit(input, &validator));
    }
  }

  // Start with a zero-copy cast, but change indices to expected size
  RETURN_NOT_OK(ZeroCopyCastExec(ctx, batch, out));
  return CastBinaryToBinaryOffsets<typename I::offset_type, typename O::offset_type>(
      ctx, input, out->array_data().get());
}

// String View -> Offset String
template <typename O, typename I>
enable_if_t<is_binary_view_like_type<I>::value && is_base_binary_type<O>::value, Status>
BinaryToBinaryCastExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  using offset_type = typename O::offset_type;
  using DataBuilder = TypedBufferBuilder<uint8_t>;
  using OffsetBuilder = TypedBufferBuilder<offset_type>;
  const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
  const ArraySpan& input = batch[0].array;

  if constexpr (!I::is_utf8 && O::is_utf8) {
    if (!options.allow_invalid_utf8) {
      InitializeUTF8();
      ArraySpanVisitor<I> visitor;
      Utf8Validator validator;
      RETURN_NOT_OK(visitor.Visit(input, &validator));
    }
  }

  ArrayData* output = out->array_data().get();
  output->length = input.length;
  output->SetNullCount(input.null_count);

  // Set up validity bitmap
  ARROW_ASSIGN_OR_RAISE(output->buffers[0],
                        GetOrCopyNullBitmapBuffer(input, ctx->memory_pool()));

  // Set up offset and data buffer
  OffsetBuilder offset_builder(ctx->memory_pool());
  RETURN_NOT_OK(offset_builder.Reserve(input.length + 1));
  offset_builder.UnsafeAppend(0);  // offsets start at 0
  const int64_t sum_of_binary_view_sizes = util::SumOfBinaryViewSizes(
      input.GetValues<BinaryViewType::c_type>(1), input.length);
  DataBuilder data_builder(ctx->memory_pool());
  RETURN_NOT_OK(data_builder.Reserve(sum_of_binary_view_sizes));
  VisitArraySpanInline<I>(
      input,
      [&](std::string_view s) {
        // for non-null value, append string view to buffer and calculate offset
        data_builder.UnsafeAppend(reinterpret_cast<const uint8_t*>(s.data()),
                                  static_cast<int64_t>(s.size()));
        offset_builder.UnsafeAppend(static_cast<offset_type>(data_builder.length()));
      },
      [&]() {
        // for null value, no need to update data buffer
        offset_builder.UnsafeAppend(static_cast<offset_type>(data_builder.length()));
      });
  RETURN_NOT_OK(offset_builder.Finish(&output->buffers[1]));
  RETURN_NOT_OK(data_builder.Finish(&output->buffers[2]));
  return Status::OK();
}

// Offset String -> String View
template <typename O, typename I>
enable_if_t<is_base_binary_type<I>::value && is_binary_view_like_type<O>::value, Status>
BinaryToBinaryCastExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  using offset_type = typename I::offset_type;
  const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
  const ArraySpan& input = batch[0].array;

  if constexpr (!I::is_utf8 && O::is_utf8) {
    if (!options.allow_invalid_utf8) {
      InitializeUTF8();
      ArraySpanVisitor<I> visitor;
      Utf8Validator validator;
      RETURN_NOT_OK(visitor.Visit(input, &validator));
    }
  }

  // Start with a zero-copy cast, then reconfigure the view and data buffers
  RETURN_NOT_OK(ZeroCopyCastExec(ctx, batch, out));
  ArrayData* output = out->array_data().get();

  const int64_t total_length = input.offset + input.length;
  const auto* validity = input.GetValues<uint8_t>(0, 0);
  const auto* input_offsets = input.GetValues<offset_type>(1);
  const auto* input_data = input.GetValues<uint8_t>(2, 0);

  // Turn buffers[1] into a buffer of empty BinaryViewType::c_type entries.
  ARROW_ASSIGN_OR_RAISE(output->buffers[1],
                        ctx->Allocate(total_length * BinaryViewType::kSize));
  memset(output->buffers[1]->mutable_data(), 0, total_length * BinaryViewType::kSize);

  // Check against offset overflow
  if constexpr (sizeof(offset_type) > 4) {
    if (total_length > 0) {
      // Offsets are monotonically increasing, that is, offsets[j] <= offsets[j+1] for
      // 0 <= j < length, even for null slots. So we only need to check the last offset.
      const int64_t max_data_offset = input_offsets[input.length];
      if (ARROW_PREDICT_FALSE(max_data_offset > std::numeric_limits<int32_t>::max())) {
        // A more complicated loop could work by slicing the data buffer into
        // more than one variadic buffer, but this is probably overkill for now
        // before someone hits this problem in practice.
        return Status::CapacityError("Failed casting from ", input.type->ToString(),
                                     " to ", output->type->ToString(),
                                     ": input array too large for efficient conversion.");
      }
    }
  }

  auto* out_views = output->GetMutableValues<BinaryViewType::c_type>(1);

  // If all entries are inline, we can drop the extra data buffer for
  // large strings in output->buffers[2].
  bool all_entries_are_inline = true;
  VisitSetBitRunsVoid(
      validity, output->offset, output->length,
      [&](int64_t start_offset, int64_t run_length) {
        for (int64_t i = start_offset; i < start_offset + run_length; i++) {
          const offset_type data_offset = input_offsets[i];
          const offset_type data_length = input_offsets[i + 1] - data_offset;
          auto& out_view = out_views[i];
          if (data_length <= BinaryViewType::kInlineSize) {
            out_view.inlined.size = static_cast<int32_t>(data_length);
            memcpy(out_view.inlined.data.data(), input_data + data_offset, data_length);
          } else {
            out_view.ref.size = static_cast<int32_t>(data_length);
            memcpy(out_view.ref.prefix.data(), input_data + data_offset,
                   BinaryViewType::kPrefixSize);
            // (buffer_index is 0'd by the memset of the buffer 1 above)
            // out_view.ref.buffer_index = 0;
            out_view.ref.offset = static_cast<int32_t>(data_offset);
            all_entries_are_inline = false;
          }
        }
      });
  if (all_entries_are_inline) {
    output->buffers[2] = nullptr;
  }
  return Status::OK();
}

// String View -> String View
template <typename O, typename I>
enable_if_t<is_binary_view_like_type<I>::value && is_binary_view_like_type<O>::value,
            Status>
BinaryToBinaryCastExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
  const ArraySpan& input = batch[0].array;

  if constexpr (!I::is_utf8 && O::is_utf8) {
    if (!options.allow_invalid_utf8) {
      InitializeUTF8();
      ArraySpanVisitor<I> visitor;
      Utf8Validator validator;
      RETURN_NOT_OK(visitor.Visit(input, &validator));
    }
  }

  return ZeroCopyCastExec(ctx, batch, out);
}

// Fixed -> String View
template <typename O, typename I>
enable_if_t<std::is_same<I, FixedSizeBinaryType>::value &&
                is_binary_view_like_type<O>::value,
            Status>
BinaryToBinaryCastExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
  const ArraySpan& input = batch[0].array;

  if constexpr (!I::is_utf8 && O::is_utf8) {
    if (!options.allow_invalid_utf8) {
      InitializeUTF8();
      ArraySpanVisitor<I> visitor;
      Utf8Validator validator;
      RETURN_NOT_OK(visitor.Visit(input, &validator));
    }
  }

  const int32_t fixed_size_width = input.type->byte_width();
  const int64_t total_length = input.offset + input.length;

  ArrayData* output = out->array_data().get();
  DCHECK_EQ(output->length, input.length);
  output->offset = input.offset;
  output->buffers.resize(3);
  output->SetNullCount(input.null_count);
  // Share the validity bitmap buffer
  output->buffers[0] = input.GetBuffer(0);
  // Init buffers[1] with input.length empty BinaryViewType::c_type entries.
  ARROW_ASSIGN_OR_RAISE(output->buffers[1],
                        ctx->Allocate(total_length * BinaryViewType::kSize));
  memset(output->buffers[1]->mutable_data(), 0, total_length * BinaryViewType::kSize);
  auto* out_views = output->GetMutableValues<BinaryViewType::c_type>(1);

  auto data_buffer = input.GetBuffer(1);
  const auto* data = data_buffer->data();

  // Check against offset overflow
  if (total_length > 0) {
    const int64_t max_data_offset = (total_length - 1) * fixed_size_width;
    if (ARROW_PREDICT_FALSE(max_data_offset > std::numeric_limits<int32_t>::max())) {
      // A more complicated loop could work by slicing the data buffer into
      // more than one variadic buffer, but this is probably overkill for now
      // before someone hits this problem in practice.
      return Status::CapacityError("Failed casting from ", input.type->ToString(), " to ",
                                   output->type->ToString(),
                                   ": input array too large for efficient conversion.");
    }
  }

  // Inline string and non-inline string loops
  if (fixed_size_width <= BinaryViewType::kInlineSize) {
    int32_t data_offset = static_cast<int32_t>(input.offset) * fixed_size_width;
    for (int64_t i = 0; i < input.length; i++) {
      auto& out_view = out_views[i];
      out_view.inlined.size = fixed_size_width;
      memcpy(out_view.inlined.data.data(), data + data_offset, fixed_size_width);
      data_offset += fixed_size_width;
    }
  } else {
    // We share the fixed-size string array data buffer as variadic data
    // buffer 0 (index=2+0) and set every buffer_index to 0.
    output->buffers[2] = std::move(data_buffer);
    int32_t data_offset = static_cast<int32_t>(input.offset) * fixed_size_width;
    for (int64_t i = 0; i < input.length; i++) {
      auto& out_view = out_views[i];
      out_view.ref.size = fixed_size_width;
      memcpy(out_view.ref.prefix.data(), data + data_offset, BinaryViewType::kPrefixSize);
      // (buffer_index is 0'd by the memset of the buffer 1 above)
      // out_view.ref.buffer_index = 0;
      out_view.ref.offset = static_cast<int32_t>(data_offset);
      data_offset += fixed_size_width;
    }
  }
  return Status::OK();
}

// Fixed -> Offset String
template <typename O, typename I>
enable_if_t<std::is_same<I, FixedSizeBinaryType>::value && is_base_binary_type<O>::value,
            Status>
BinaryToBinaryCastExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
  const ArraySpan& input = batch[0].array;

  if constexpr (O::is_utf8) {
    if (!options.allow_invalid_utf8) {
      InitializeUTF8();
      ArraySpanVisitor<I> visitor;
      Utf8Validator validator;
      RETURN_NOT_OK(visitor.Visit(input, &validator));
    }
  }

  // Check for overflow
  using output_offset_type = typename O::offset_type;
  constexpr output_offset_type kMaxOffset =
      std::numeric_limits<output_offset_type>::max();
  const int32_t width = input.type->byte_width();
  const int64_t max_offset = width * input.length;
  if (max_offset > kMaxOffset) {
    return Status::Invalid("Failed casting from ", input.type->ToString(), " to ",
                           out->type()->ToString(), ": input array too large");
  }

  // This presupposes that one was created in the invocation layer
  ArrayData* output = out->array_data().get();

  // Copy buffers over, then generate indices
  output->length = input.length;
  output->SetNullCount(input.null_count);
  if (input.offset == output->offset) {
    output->buffers[0] = input.GetBuffer(0);
  } else {
    // When the offsets are different (e.g., due to slice operation), we need to check if
    // the null bitmap buffer is not null before copying it. The null bitmap buffer can be
    // null if the input array value does not contain any null value.
    if (input.buffers[0].data != NULLPTR) {
      ARROW_ASSIGN_OR_RAISE(
          output->buffers[0],
          arrow::internal::CopyBitmap(ctx->memory_pool(), input.buffers[0].data,
                                      input.offset, input.length));
    }
  }

  // This buffer is preallocated
  auto* offsets = output->GetMutableValues<output_offset_type>(1);
  offsets[0] = static_cast<output_offset_type>(input.offset * width);
  for (int64_t i = 0; i < input.length; i++) {
    offsets[i + 1] = offsets[i] + width;
  }

  // Data buffer (index 1) for FWBinary becomes data buffer for VarBinary
  // (index 2). After ARROW-16757, we need to copy this memory instead of
  // zero-copy it because a Scalar value promoted to an ArraySpan may be
  // referencing a temporary buffer whose scope does not extend beyond the
  // kernel execution. In that scenario, the validity bitmap above can be
  // zero-copied because it points to static memory (either a byte with a 1 or
  // a 0 depending on whether the value is null or not).
  std::shared_ptr<Buffer> input_data = input.GetBuffer(1);
  if (input_data != nullptr) {
    ARROW_ASSIGN_OR_RAISE(output->buffers[2], input_data->CopySlice(0, input_data->size(),
                                                                    ctx->memory_pool()));
  } else {
    // TODO(wesm): it should already be nullptr, so we may be able to remove
    // this
    output->buffers[2] = nullptr;
  }

  return Status::OK();
}

// Fixed -> Fixed
template <typename O, typename I>
enable_if_t<std::is_same<I, FixedSizeBinaryType>::value &&
                std::is_same<O, FixedSizeBinaryType>::value,
            Status>
BinaryToBinaryCastExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
  const int32_t in_width = batch[0].type()->byte_width();
  const int32_t out_width =
      checked_cast<const FixedSizeBinaryType&>(*options.to_type).byte_width();
  if (in_width != out_width) {
    return Status::Invalid("Failed casting from ", batch[0].type()->ToString(), " to ",
                           options.to_type.ToString(), ": widths must match");
  }
  return ZeroCopyCastExec(ctx, batch, out);
}

// Offset String | String View -> Fixed
template <typename O, typename I>
enable_if_t<(is_base_binary_type<I>::value || is_binary_view_like_type<I>::value) &&
                std::is_same<O, FixedSizeBinaryType>::value,
            Status>
BinaryToBinaryCastExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
  FixedSizeBinaryBuilder builder(options.to_type.GetSharedPtr(), ctx->memory_pool());
  const ArraySpan& input = batch[0].array;
  RETURN_NOT_OK(builder.Reserve(input.length));

  RETURN_NOT_OK(VisitArraySpanInline<I>(
      input,
      [&](std::string_view v) {
        if (v.size() != static_cast<size_t>(builder.byte_width())) {
          return Status::Invalid("Failed casting from ", input.type->ToString(), " to ",
                                 options.to_type.ToString(), ": widths must match");
        }
        builder.UnsafeAppend(v);
        return Status::OK();
      },
      [&] {
        builder.UnsafeAppendNull();
        return Status::OK();
      }));

  return builder.FinishInternal(&std::get<std::shared_ptr<ArrayData>>(out->value));
}

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

// ----------------------------------------------------------------------
// Cast functions registration

template <typename OutType>
void AddNumberToStringCasts(CastFunction* func) {
  auto out_ty = TypeTraits<OutType>::type_singleton();

  DCHECK_OK(func->AddKernel(Type::BOOL, {boolean()}, out_ty,
                            NumericToStringCastFunctor<OutType, BooleanType>::Exec,
                            NullHandling::COMPUTED_NO_PREALLOCATE));

  for (const std::shared_ptr<DataType>& in_ty : NumericTypes()) {
    DCHECK_OK(
        func->AddKernel(in_ty->id(), {in_ty}, out_ty,
                        GenerateNumeric<NumericToStringCastFunctor, OutType>(*in_ty),
                        NullHandling::COMPUTED_NO_PREALLOCATE));
  }

  DCHECK_OK(func->AddKernel(Type::HALF_FLOAT, {float16()}, out_ty,
                            NumericToStringCastFunctor<OutType, HalfFloatType>::Exec,
                            NullHandling::COMPUTED_NO_PREALLOCATE));
}

template <typename OutType>
void AddDecimalToStringCasts(CastFunction* func) {
  auto out_ty = TypeTraits<OutType>::type_singleton();
  for (const auto& in_tid : DecimalTypeIds()) {
    DCHECK_OK(
        func->AddKernel(in_tid, {in_tid}, out_ty,
                        GenerateDecimal<DecimalToStringCastFunctor, OutType>(in_tid),
                        NullHandling::COMPUTED_NO_PREALLOCATE));
  }
}

template <typename OutType>
void AddTemporalToStringCasts(CastFunction* func) {
  auto out_ty = TypeTraits<OutType>::type_singleton();
  for (const auto& types : {TemporalTypes(), DurationTypes()}) {
    for (const std::shared_ptr<DataType>& in_ty : types) {
      DCHECK_OK(
          func->AddKernel(in_ty->id(), {InputType(in_ty->id())}, out_ty,
                          GenerateTemporal<TemporalToStringCastFunctor, OutType>(*in_ty),
                          NullHandling::COMPUTED_NO_PREALLOCATE));
    }
  }
}

template <typename OutType, typename InType>
void AddBinaryToBinaryCast(CastFunction* func) {
  auto out_ty = TypeTraits<OutType>::type_singleton();

  DCHECK_OK(func->AddKernel(InType::type_id, {InputType(InType::type_id)}, out_ty,
                            BinaryToBinaryCastExec<OutType, InType>,
                            NullHandling::COMPUTED_NO_PREALLOCATE));
}

template <typename OutType>
void AddBinaryToBinaryCast(CastFunction* func) {
  AddBinaryToBinaryCast<OutType, StringType>(func);
  AddBinaryToBinaryCast<OutType, StringViewType>(func);
  AddBinaryToBinaryCast<OutType, BinaryType>(func);
  AddBinaryToBinaryCast<OutType, BinaryViewType>(func);
  AddBinaryToBinaryCast<OutType, LargeStringType>(func);
  AddBinaryToBinaryCast<OutType, LargeBinaryType>(func);
  AddBinaryToBinaryCast<OutType, FixedSizeBinaryType>(func);
}

template <typename InType>
void AddBinaryToFixedSizeBinaryCast(CastFunction* func) {
  auto resolver_fsb = [](KernelContext* ctx, const std::vector<TypeHolder>&) {
    const CastOptions& options = checked_cast<const CastState&>(*ctx->state()).options;
    return options.to_type;
  };

  DCHECK_OK(func->AddKernel(InType::type_id, {InputType(InType::type_id)}, resolver_fsb,
                            BinaryToBinaryCastExec<FixedSizeBinaryType, InType>,
                            NullHandling::COMPUTED_NO_PREALLOCATE));
}

void AddBinaryToFixedSizeBinaryCast(CastFunction* func) {
  AddBinaryToFixedSizeBinaryCast<StringType>(func);
  AddBinaryToFixedSizeBinaryCast<StringViewType>(func);
  AddBinaryToFixedSizeBinaryCast<BinaryType>(func);
  AddBinaryToFixedSizeBinaryCast<BinaryViewType>(func);
  AddBinaryToFixedSizeBinaryCast<LargeStringType>(func);
  AddBinaryToFixedSizeBinaryCast<LargeBinaryType>(func);
  AddBinaryToFixedSizeBinaryCast<FixedSizeBinaryType>(func);
}

}  // namespace

std::vector<std::shared_ptr<CastFunction>> GetBinaryLikeCasts() {
  // cast_binary / cast_binary_view / cast_large_binary

  auto cast_binary = std::make_shared<CastFunction>("cast_binary", Type::BINARY);
  AddCommonCasts(Type::BINARY, binary(), cast_binary.get());
  AddBinaryToBinaryCast<BinaryType>(cast_binary.get());

  auto cast_binary_view =
      std::make_shared<CastFunction>("cast_binary_view", Type::BINARY_VIEW);
  AddCommonCasts(Type::BINARY_VIEW, binary_view(), cast_binary_view.get());
  AddBinaryToBinaryCast<BinaryViewType>(cast_binary_view.get());

  auto cast_large_binary =
      std::make_shared<CastFunction>("cast_large_binary", Type::LARGE_BINARY);
  AddCommonCasts(Type::LARGE_BINARY, large_binary(), cast_large_binary.get());
  AddBinaryToBinaryCast<LargeBinaryType>(cast_large_binary.get());

  // cast_string / cast_string_view / cast_large_string

  auto cast_string = std::make_shared<CastFunction>("cast_string", Type::STRING);
  AddCommonCasts(Type::STRING, utf8(), cast_string.get());
  AddNumberToStringCasts<StringType>(cast_string.get());
  AddDecimalToStringCasts<StringType>(cast_string.get());
  AddTemporalToStringCasts<StringType>(cast_string.get());
  AddBinaryToBinaryCast<StringType>(cast_string.get());

  auto cast_string_view =
      std::make_shared<CastFunction>("cast_string_view", Type::STRING_VIEW);
  AddCommonCasts(Type::STRING_VIEW, utf8_view(), cast_string_view.get());
  AddNumberToStringCasts<StringViewType>(cast_string_view.get());
  AddDecimalToStringCasts<StringViewType>(cast_string_view.get());
  AddTemporalToStringCasts<StringViewType>(cast_string_view.get());
  AddBinaryToBinaryCast<StringViewType>(cast_string_view.get());

  auto cast_large_string =
      std::make_shared<CastFunction>("cast_large_string", Type::LARGE_STRING);
  AddCommonCasts(Type::LARGE_STRING, large_utf8(), cast_large_string.get());
  AddNumberToStringCasts<LargeStringType>(cast_large_string.get());
  AddDecimalToStringCasts<LargeStringType>(cast_large_string.get());
  AddTemporalToStringCasts<LargeStringType>(cast_large_string.get());
  AddBinaryToBinaryCast<LargeStringType>(cast_large_string.get());

  // cast_fixed_size_binary

  auto cast_fsb =
      std::make_shared<CastFunction>("cast_fixed_size_binary", Type::FIXED_SIZE_BINARY);
  AddCommonCasts(Type::FIXED_SIZE_BINARY, OutputType(ResolveOutputFromOptions),
                 cast_fsb.get());
  AddBinaryToFixedSizeBinaryCast(cast_fsb.get());

  return {
      std::move(cast_binary), std::move(cast_binary_view), std::move(cast_large_binary),
      std::move(cast_string), std::move(cast_string_view), std::move(cast_large_string),
      std::move(cast_fsb),
  };
}

}  // namespace internal
}  // namespace compute
}  // namespace arrow
