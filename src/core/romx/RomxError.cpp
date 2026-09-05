#include "RomxError.hpp"

#include <cstring>
#include <sstream>

namespace beiklive::romx
{
std::string errorText(const char* operation, const romx_error_t& error,
                      romx_result_t result)
{
    std::ostringstream stream;
    stream << operation << " failed (" << result << ")";
    if (error.message[0] != '\0')
        stream << ": " << error.message;
    if (error.system_code != 0)
    {
        stream << " [system_code=" << error.system_code;
        const char* systemMessage = std::strerror(error.system_code);
        if (systemMessage != nullptr && *systemMessage != '\0')
            stream << ": " << systemMessage;
        stream << "]";
    }
    return stream.str();
}

void assignError(std::string* output, const std::string& message)
{
    if (output != nullptr)
        *output = message;
}
}
