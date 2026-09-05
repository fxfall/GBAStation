#pragma once

#include <romx/romx.h>

#include <string>

namespace beiklive::romx
{
std::string errorText(const char* operation, const romx_error_t& error,
                      romx_result_t result);
void assignError(std::string* output, const std::string& message);
}
