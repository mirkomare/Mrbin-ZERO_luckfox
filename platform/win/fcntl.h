#pragma once
#ifdef _WIN32
#include <fcntl.h>
#include "posix_compat.h"
#else
#include_next <fcntl.h>
#endif
