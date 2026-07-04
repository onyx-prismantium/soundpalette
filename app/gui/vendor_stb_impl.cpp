// Single translation unit for the vendored stb_image_write implementation (--smoke PNG, §10).
// Compiled without our -Wall -Wextra -Werror, same policy as core's vendored audio TU.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
