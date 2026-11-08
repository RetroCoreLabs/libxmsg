# Entry point for consumers that add the repository folder itself:
#   add_subdirectory(external/libxmsg)
# Everything that builds lives under src/ (house layout rule); this file only forwards there.
cmake_minimum_required(VERSION 3.13)
add_subdirectory(src)
