/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/dwio/common/compression/PagedInputStream.h"

namespace facebook::velox::dwio::common::compression {

void PagedInputStream::prepareOutputBuffer(uint64_t uncompressedLength) {
  if (!outputBuffer_ || uncompressedLength > outputBuffer_->capacity()) {
    outputBuffer_ = std::make_unique<dwio::common::DataBuffer<char>>(
        pool_, uncompressedLength);
  }
}

void PagedInputStream::readBuffer(bool failOnEof) {
  int32_t length;
  if (!input_->Next(
          reinterpret_cast<const void**>(&inputBufferPtr_), &length)) {
    DWIO_ENSURE(!failOnEof, getName(), ", read past EOF");
    state_ = State::END;
    inputBufferStart_ = nullptr;
    inputBufferPtr_ = nullptr;
    inputBufferPtrEnd_ = nullptr;
  } else {
    inputBufferStart_ = inputBufferPtr_;
    inputBufferPtrEnd_ = inputBufferPtr_ + length;
  }
}

uint32_t PagedInputStream::readByte(bool failOnEof) {
  if (UNLIKELY(inputBufferPtr_ == inputBufferPtrEnd_)) {
    readBuffer(failOnEof);
    if (state_ == State::END) {
      return 0;
    }
  }
  return static_cast<unsigned char>(*(inputBufferPtr_++));
}

void PagedInputStream::readHeader() {
  uint32_t header = readByte(false);

  lastHeaderOffset_ =
      input_->ByteCount() - (inputBufferPtrEnd_ - inputBufferPtr_) - 1;
  bytesReturnedAtLastHeaderOffset_ = bytesReturned_;

  if (state_ != State::END) {
    header |= readByte(true) << 8;
    header |= readByte(true) << 16;
    if (header & 1) {
      state_ = State::ORIGINAL;
    } else {
      state_ = State::START;
    }
    remainingLength_ = header >> 1;
  } else {
    remainingLength_ = 0;
  }
}

const char* PagedInputStream::ensureInput(size_t availableInputBytes) {
  auto input = inputBufferPtr_;
  if (remainingLength_ <= availableInputBytes) {
    inputBufferPtr_ += availableInputBytes;
    return input;
  }
  // make sure input buffer has capacity
  if (inputBuffer_.capacity() < remainingLength_) {
    inputBuffer_.reserve(remainingLength_);
  }

  std::copy(
      inputBufferPtr_,
      inputBufferPtr_ + availableInputBytes,
      inputBuffer_.data());
  inputBufferPtr_ += availableInputBytes;

  for (size_t pos = availableInputBytes; pos < remainingLength_;) {
    readBuffer(true);
    availableInputBytes = std::min(
        static_cast<size_t>(inputBufferPtrEnd_ - inputBufferPtr_),
        remainingLength_ - pos);
    std::copy(
        inputBufferPtr_,
        inputBufferPtr_ + availableInputBytes,
        inputBuffer_.data() + pos);
    pos += availableInputBytes;
    inputBufferPtr_ += availableInputBytes;
  }
  return inputBuffer_.data();
}

bool PagedInputStream::Next(const void** data, int32_t* size) {
  VELOX_CHECK_NOT_NULL(data);
  skipAllPending();
  return readOrSkip(data, size);
}

// Read into `data' if it is not null; otherwise skip some of the pending.
bool PagedInputStream::readOrSkip(const void** data, int32_t* size) {
  if (data) {
    VELOX_CHECK_EQ(pendingSkip_, 0);
  }
  // if the user pushed back, return them the partial buffer
  if (outputBufferLength_) {
    if (data) {
      *data = outputBufferPtr_;
    }
    *size = static_cast<int32_t>(outputBufferLength_);
    outputBufferPtr_ += outputBufferLength_;
    bytesReturned_ += outputBufferLength_;
    outputBufferLength_ = 0;
    // This is a rewind of previous output, does not count for
    // 'lastWindowSize_'.
    return true;
  }

  // release previous decryption buffer
  decryptionBuffer_ = nullptr;

  if (state_ == State::HEADER || remainingLength_ == 0) {
    readHeader();
  }
  if (state_ == State::END) {
    return false;
  }
  if (inputBufferPtr_ == inputBufferPtrEnd_) {
    readBuffer(true);
  }

  size_t availSize = std::min(
      static_cast<size_t>(inputBufferPtrEnd_ - inputBufferPtr_),
      remainingLength_);
  // in the case when decompression or decryption is needed, need to copy data
  // to input buffer if the input doesn't contain the entire block
  bool original = !decrypter_ && (state_ == State::ORIGINAL);
  const char* input = nullptr;
  // if no decompression or decryption is needed, simply adjust the output
  // pointer. Otherwise, make sure we have continuous block
  if (original) {
    if (data) {
      *data = inputBufferPtr_;
    }
    *size = static_cast<int32_t>(availSize);
    outputBufferPtr_ = inputBufferPtr_ + availSize;
    inputBufferPtr_ += availSize;
    remainingLength_ -= availSize;
  } else {
    input = ensureInput(availSize);
  }

  // perform decryption
  if (decrypter_) {
    decryptionBuffer_ =
        decrypter_->decrypt(folly::StringPiece{input, remainingLength_});
    input = reinterpret_cast<const char*>(decryptionBuffer_->data());
    remainingLength_ = decryptionBuffer_->length();
    if (data) {
      *data = input;
    }
    *size = remainingLength_;
    outputBufferPtr_ = input + remainingLength_;
  }

  // perform decompression
  if (state_ == State::START) {
    DWIO_ENSURE_NOT_NULL(decompressor_.get(), "invalid stream state");
    DWIO_ENSURE_NOT_NULL(input);
    auto [decompressedLength, exact] =
        decompressor_->getDecompressedLength(input, remainingLength_);
    if (!data && exact && decompressedLength <= pendingSkip_) {
      *size = decompressedLength;
      outputBufferPtr_ = nullptr;
    } else {
      prepareOutputBuffer(decompressedLength);
      outputBufferLength_ = decompressor_->decompress(
          input,
          remainingLength_,
          outputBuffer_->data(),
          outputBuffer_->capacity());
      if (data) {
        *data = outputBuffer_->data();
      }
      *size = static_cast<int32_t>(outputBufferLength_);
      outputBufferPtr_ = outputBuffer_->data() + outputBufferLength_;
    }
    // release decryption buffer
    decryptionBuffer_ = nullptr;
  }

  if (!original) {
    remainingLength_ = 0;
    state_ = State::HEADER;
  }

  outputBufferLength_ = 0;
  bytesReturned_ += *size;
  lastWindowSize_ = *size;
  return true;
}

void PagedInputStream::BackUp(int32_t count) {
  VELOX_CHECK_GE(count, 0);
  if (pendingSkip_ > 0) {
    auto len = std::min<int64_t>(count, pendingSkip_);
    pendingSkip_ -= len;
    count -= len;
    if (count == 0) {
      return;
    }
  }
  DWIO_ENSURE(
      outputBufferPtr_ != nullptr,
      "Backup without previous Next in ",
      getName());
  if (state_ == State::ORIGINAL) {
    VELOX_CHECK(
        outputBufferPtr_ >= inputBufferStart_ &&
        outputBufferPtr_ <= inputBufferPtrEnd_);
    // 'outputBufferPtr_' ranges over the input buffer if there is no
    // decompression / decryption. Check that we do not back out of
    // the last range returned from input_->Next().
    VELOX_CHECK_GE(
        inputBufferPtr_ - inputBufferStart_, static_cast<size_t>(count));
  }
  outputBufferPtr_ -= static_cast<size_t>(count);
  outputBufferLength_ += static_cast<size_t>(count);
  bytesReturned_ -= count;
}

bool PagedInputStream::skipAllPending() {
  while (pendingSkip_ > 0) {
    int32_t len;
    if (!readOrSkip(nullptr, &len)) {
      return false;
    }
    if (len > pendingSkip_) {
      auto toBackUp = len - pendingSkip_;
      pendingSkip_ = 0;
      BackUp(toBackUp);
    } else {
      pendingSkip_ -= len;
    }
  }
  return true;
}

bool PagedInputStream::skip(int64_t count) {
  VELOX_CHECK_GE(count, 0);
  pendingSkip_ += count;
  // We never use the return value of this function so this is OK.
  return true;
}

void PagedInputStream::clearDecompressionState() {
  state_ = State::HEADER;
  outputBufferLength_ = 0;
  remainingLength_ = 0;
  inputBufferPtr_ = nullptr;
  inputBufferPtrEnd_ = nullptr;
}

void PagedInputStream::seekToPosition(
    dwio::common::PositionProvider& positionProvider) {
  auto compressedOffset = positionProvider.next();
  auto uncompressedOffset = positionProvider.next();

  // If we are directly returning views into input, we can only backup
  // to the beginning of the last view or last header, whichever is
  // later. If we are returning views into the decompression buffer,
  // we can backup to the beginning of the decompressed buffer
  auto alreadyRead =
      bytesReturned_ - bytesReturnedAtLastHeaderOffset_ + pendingSkip_;

  // outsideOriginalWindow is true if we are returning views into
  // the input stream's buffer and we are seeking below the start of the last
  // window. The last window began with a header or a window from the underlying
  // stream. If seeking below that, we must seek the input.
  auto outsideOriginalWindow = [&]() {
    return state_ == State::ORIGINAL && compressedOffset == lastHeaderOffset_ &&
        uncompressedOffset < alreadyRead &&
        lastWindowSize_ < alreadyRead - uncompressedOffset;
  };

  if (compressedOffset != lastHeaderOffset_ || outsideOriginalWindow()) {
    std::vector<uint64_t> positions = {compressedOffset};
    auto provider = dwio::common::PositionProvider(positions);
    input_->seekToPosition(provider);
    clearDecompressionState();
    pendingSkip_ = uncompressedOffset;
  } else {
    if (uncompressedOffset < alreadyRead) {
      BackUp(alreadyRead - uncompressedOffset);
    } else {
      pendingSkip_ += uncompressedOffset - alreadyRead;
    }
  }
}

} // namespace facebook::velox::dwio::common::compression