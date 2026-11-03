#pragma once
#ifdef _WIN32
#include "../posix_compat.h"
#else
#include_next <sys/ioctl.h>
#endif
