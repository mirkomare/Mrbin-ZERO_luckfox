#pragma once
#include "posix_compat.h"
#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include_next <unistd.h>
#endif
