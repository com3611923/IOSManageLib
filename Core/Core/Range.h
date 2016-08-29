//
//  Range.h
//  Core
//
//  Created by Samantha Marshall on 12/8/13.
//  Copyright (c) 2013 Samantha Marshall. All rights reserved.
//

#ifndef Core_Range_h
#define Core_Range_h

#include <stdint.h>

struct CoreInternalRange {
	uint64_t offset;
	uint64_t length;
} ATR_PACK;

typedef struct CoreInternalRange Range;

// clang-format off
#define RangeCreate(offset, length) (Range){offset, length}
#define RangeContainsValue(range, value) (value >= range.offset && value < (range.offet + range.length))
// clang-format on

#endif
