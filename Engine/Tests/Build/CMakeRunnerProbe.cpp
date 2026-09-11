#include <iostream>
#include <string_view>

/// @brief CMake Runner Process Test用に受信Argumentを検証して終了するProbe
int main(int a_count, char **a_arguments)
{
    if (a_count == 7 && std::string_view(a_arguments[1]) == "--preset" && std::string_view(a_arguments[3]) == "-B" &&
        std::string_view(a_arguments[5]) == "-T" &&
        std::string_view(a_arguments[6]) == "version=14.51.36231")
    {
        std::cout << "configure:" << a_arguments[2] << '\n';
        return 0;
    }
    if (a_count == 9 && std::string_view(a_arguments[1]) == "--build" &&
        std::string_view(a_arguments[3]) == "--config" && std::string_view(a_arguments[5]) == "--target" &&
        std::string_view(a_arguments[6]) == "CueGameModule" && std::string_view(a_arguments[7]) == "--" &&
        std::string_view(a_arguments[8]) == "/p:VCToolsVersion=14.51.36231")
    {
        std::cerr << "build:" << a_arguments[4] << '\n';
        return 0;
    }
    return 7;
}
