// cmm.h

#pragma once

#include "cmm_basic_types.h"
#include "cmm_error.h"

namespace cmm
{

// We can use vector OR hash_set as value list container
// vector has better performance but require there isn't enclave in memory, for LINUX
// hash_set is more safety, for WINDOWS
#define USE_VECTOR_IN_VALUE_LIST      0

// File path realtives
enum
{
    MAX_PATH_LEN = 256,
    PATH_SEPARATOR = '/',
};

}
