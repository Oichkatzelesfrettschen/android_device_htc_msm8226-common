/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Random-access byte source the container parsers read through.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

namespace a11 {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Reads up to size bytes at offset; returns the count read or -1.
    virtual ssize_t readAt(int64_t offset, void *data, size_t size) = 0;
    // Total size in bytes, or a negative value when unknown.
    virtual int64_t size() = 0;
};

// Reads exactly size bytes at offset.
inline bool ReadFully(ByteSource *source, uint64_t offset, void *data, size_t size) {
    if (offset > static_cast<uint64_t>(INT64_MAX)) {
        return false;
    }
    return source->readAt(static_cast<int64_t>(offset), data, size) == static_cast<ssize_t>(size);
}

}  // namespace a11
