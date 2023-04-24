// Copyright (c) 2023 Cloudflare, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#if KJ_HAS_BROTLI

#include "brotli.h"
#include <kj/debug.h>

namespace kj {

namespace _ {  // private

// Check number of window bits used by the stream, see RFC 7932 section 9.1 for the specification.
// Adapted from FL's ngx_brotli.
static int get_brotli_window_bits(uint8_t peek) {
    if ((peek & 0x01) == 0) {
        return 16;
    }

    if (((peek >> 1) & 0x07) != 0) {
        return 17 + (peek >> 1 & 0x07);
    }

    if (((peek >> 4) & 0x07) == 0) {
        return 17;
    }

    if (((peek >> 4) & 0x07) == 1) {
        // Large window brotli, not part of RFC 7932 and not supported in web contexts
        return BROTLI_MAX_WINDOW_BITS + 1;
    }

    return 8 + ((peek >> 4) & 0x07);
}

BrotliOutputContext::BrotliOutputContext(kj::Maybe<int> compressionLevel,
                                         kj::Maybe<int> _windowBits) : next_in(nullptr), available_in(0) {
  KJ_IF_MAYBE(level, compressionLevel) {
    compressing = true;
    // Emulate zlib's behavior of using -1 to signify the default quality
    if (*level == -1) {*level = KJ_BROTLI_DEFAULT_QTY;}
    KJ_REQUIRE(*level >= BROTLI_MIN_QUALITY && *level <= BROTLI_MAX_QUALITY,
        "invalid brotli compression level", *level);
    windowBits = _windowBits.orDefault(_::KJ_BROTLI_DEFAULT_WBITS);
    KJ_REQUIRE(windowBits >= BROTLI_MIN_WINDOW_BITS && windowBits <= BROTLI_MAX_WINDOW_BITS,
        "invalid brotli window size (window bits", windowBits, ")");
    cctx = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    KJ_REQUIRE(cctx, "brotli state allocation failed");
    // TODO(performance): Make window size configurable from the stream interface and consider
    // setting up BROTLI_PARAM_LGBLOCK if memory usage becomes an issue
    KJ_ASSERT(BrotliEncoderSetParameter(cctx, BROTLI_PARAM_QUALITY, *level) == BROTLI_TRUE);
    KJ_ASSERT(BrotliEncoderSetParameter(cctx, BROTLI_PARAM_LGWIN, windowBits) == BROTLI_TRUE);
  } else {
    compressing = false;
    // In the decoder, we manually check that the stream does not have a higher window size than
    // requested and reject it otherwise, no way to automate this step.
    // By default, we accept streams with a window size up to (1 << KJ_BROTLI_MAX_DEC_WBITS),
    // this is more than the default window size for compression (i.e. KJ_BROTLI_DEFAULT_WBITS)
    windowBits = _windowBits.orDefault(_::KJ_BROTLI_MAX_DEC_WBITS);
    KJ_REQUIRE(windowBits >= BROTLI_MIN_WINDOW_BITS && windowBits <= BROTLI_MAX_WINDOW_BITS,
        "invalid brotli window size (window bits", windowBits, ")");
    dctx = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    KJ_REQUIRE(dctx, "brotli state allocation failed");
  }
}

BrotliOutputContext::~BrotliOutputContext() noexcept(false) {
  compressing ? BrotliEncoderDestroyInstance(cctx) : BrotliDecoderDestroyInstance(dctx);
}

void BrotliOutputContext::setInput(const void* in, size_t size) {
  next_in = reinterpret_cast<const byte*>(in);
  available_in = size;
}

kj::Tuple<bool, kj::ArrayPtr<const byte>> BrotliOutputContext::pumpOnce(BrotliEncoderOperation flush) {
  byte* next_out = buffer;
  size_t available_out = sizeof(buffer);
  // Brotli does not accept a null input pointer; make sure there is a valid pointer even if we are
  // not actually reading from it.
  if (!next_in) {
    KJ_ASSERT(available_in == 0);
    next_in = buffer;
  }

  if (!compressing) {
    // Check window bits
    if (firstInput) {
      firstInput = false;
      int streamWbits = get_brotli_window_bits(next_in[0]);
      KJ_REQUIRE(streamWbits <= windowBits,
        "brotli window size too big", (1 << streamWbits));
    }
    BrotliDecoderResult result =
        BrotliDecoderDecompressStream(dctx, &available_in, &next_in, &available_out, &next_out,
                                      nullptr);
    if (result == BROTLI_DECODER_RESULT_ERROR) {
        // Note: Unlike BrotliInputStream, this will implicitly reject trailing data during
        // decompression, matching the behavior for gzip.
        KJ_FAIL_REQUIRE("brotli decompression failed",
                        BrotliDecoderErrorString(BrotliDecoderGetErrorCode(dctx)));
    }
  } else {
    BROTLI_BOOL result = BrotliEncoderCompressStream(cctx, flush, &available_in, &next_in,
                                                     &available_out, &next_out, nullptr);
    KJ_REQUIRE(result == BROTLI_TRUE, "brotli compression failed");
  }

  // The 'ok' parameter represented by the first parameter of the tuple indicates that pumpOnce()
  // should be called again as more output data can be produced. This is the case when the stream
  // is not finished and there is either pending output data (that didn't fit into the buffer) or
  // input that has not been processed yet.

  return kj::tuple(
      (compressing && BrotliEncoderHasMoreOutput(cctx)) ||
      (!compressing && BrotliDecoderHasMoreOutput(dctx)),
       kj::arrayPtr(buffer, sizeof(buffer) - available_out));
}

}  // namespace _ (private)

// =======================================================================================

BrotliInputStream::BrotliInputStream(InputStream& inner)
    : inner(inner), next_in(nullptr), available_in(0) {
  ctx = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
  KJ_REQUIRE(ctx, "brotli state allocation failed");
}

BrotliInputStream::~BrotliInputStream() noexcept(false) {
  BrotliDecoderDestroyInstance(ctx);
}

size_t BrotliInputStream::tryRead(void* out, size_t minBytes, size_t maxBytes) {
  if (maxBytes == 0) return size_t(0);

  return readImpl(reinterpret_cast<byte*>(out), minBytes, maxBytes, 0);
}

size_t BrotliInputStream::readImpl(
    byte* out, size_t minBytes, size_t maxBytes, size_t alreadyRead) {
  // Ask for more input unless there is pending output
  if (available_in == 0 && !BrotliDecoderHasMoreOutput(ctx)) {
    size_t amount = inner.tryRead(buffer, 1, sizeof(buffer));
    if (amount == 0) {
      KJ_REQUIRE(atValidEndpoint, "brotli compressed stream ended prematurely");
      return alreadyRead;
    } else {
      next_in = buffer;
      available_in = amount;
    }
  }

  byte* next_out = out;
  size_t available_out = maxBytes;
  // Check window bits
  if (firstInput) {
    firstInput = false;
    int streamWbits = _::get_brotli_window_bits(next_in[0]);
    KJ_REQUIRE(streamWbits <= _::KJ_BROTLI_MAX_DEC_WBITS,
        "brotli window size too big", (1 << streamWbits));
  }
  BrotliDecoderResult result = BrotliDecoderDecompressStream(
      ctx, &available_in, &next_in, &available_out, &next_out, nullptr);
  KJ_REQUIRE(result != BROTLI_DECODER_RESULT_ERROR, "brotli decompression failed",
             BrotliDecoderErrorString(BrotliDecoderGetErrorCode(ctx)));

  atValidEndpoint = result == BROTLI_DECODER_RESULT_SUCCESS;
  if (atValidEndpoint && available_in > 0) {
    // There's more data available. Assume start of new content.
    // Not sure if we actually want this, but there is limited potential for breakage as arbitrary
    // trailing data should still be rejected. Unfortunately this is kind of clunky as brotli does
    // not support resetting an instance.
    BrotliDecoderDestroyInstance(ctx);
    ctx = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    KJ_REQUIRE(ctx, "brotli state allocation failed");
    firstInput = true;
  }

  size_t n = maxBytes - available_out;
  if (n >= minBytes) {
    return n + alreadyRead;
  } else {
    return readImpl(out + n, minBytes - n, maxBytes - n, alreadyRead + n);
  }
}

// =======================================================================================

BrotliAsyncInputStream::BrotliAsyncInputStream(AsyncInputStream& inner)
    : inner(inner), next_in(nullptr), available_in(0) {
  ctx = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
  KJ_REQUIRE(ctx, "brotli state allocation failed");
}

BrotliAsyncInputStream::~BrotliAsyncInputStream() noexcept(false) {
  BrotliDecoderDestroyInstance(ctx);
}

Promise<size_t> BrotliAsyncInputStream::tryRead(void* out, size_t minBytes, size_t maxBytes) {
  if (maxBytes == 0) return constPromise<size_t, 0>();

  return readImpl(reinterpret_cast<byte*>(out), minBytes, maxBytes, 0);
}

Promise<size_t> BrotliAsyncInputStream::readImpl(
    byte* out, size_t minBytes, size_t maxBytes, size_t alreadyRead) {
  // Ask for more input unless there is pending output
  if (available_in == 0 && !BrotliDecoderHasMoreOutput(ctx)) {
    return inner.tryRead(buffer, 1, sizeof(buffer))
        .then([this,out,minBytes,maxBytes,alreadyRead](size_t amount) -> Promise<size_t> {
      if (amount == 0) {
        if (!atValidEndpoint) {
          return KJ_EXCEPTION(DISCONNECTED, "brotli compressed stream ended prematurely");
        }
        return alreadyRead;
      } else {
        next_in = buffer;
        available_in = amount;
        return readImpl(out, minBytes, maxBytes, alreadyRead);
      }
    });
  }

  byte* next_out = out;
  size_t available_out = maxBytes;
  // Check window bits
  if (firstInput) {
    firstInput = false;
    int streamWbits = _::get_brotli_window_bits(next_in[0]);
    KJ_REQUIRE(streamWbits <= _::KJ_BROTLI_MAX_DEC_WBITS,
        "brotli window size too big", (1 << streamWbits));
  }
  BrotliDecoderResult result = BrotliDecoderDecompressStream(
      ctx, &available_in, &next_in, &available_out, &next_out, nullptr);
  KJ_REQUIRE(result != BROTLI_DECODER_RESULT_ERROR, "brotli decompression failed",
             BrotliDecoderErrorString(BrotliDecoderGetErrorCode(ctx)));

  atValidEndpoint = result == BROTLI_DECODER_RESULT_SUCCESS;
  if (atValidEndpoint && available_in > 0) {
    // There's more data available. Assume start of new content.
    // Not sure if we actually want this, but there is limited potential for breakage as arbitrary
    // trailing data should still be rejected. Unfortunately this is kind of clunky as brotli does
    // not support resetting an instance.
    BrotliDecoderDestroyInstance(ctx);
    ctx = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    KJ_REQUIRE(ctx, "brotli state allocation failed");
    firstInput = true;
  }

  size_t n = maxBytes - available_out;
  if (n >= minBytes) {
    return n + alreadyRead;
  } else {
    return readImpl(out + n, minBytes - n, maxBytes - n, alreadyRead + n);
  }
}

}  // namespace kj

#endif  // KJ_HAS_BROTLI
