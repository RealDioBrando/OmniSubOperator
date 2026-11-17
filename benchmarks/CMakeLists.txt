# Copyright (c) 2026 OmniSubOperator contributors.

add_executable(count_benchmark count_benchmark.cpp)
target_link_libraries(count_benchmark PRIVATE omni_subop::omni_subop)
target_compile_features(count_benchmark PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(count_benchmark PRIVATE -O3 -DNDEBUG -Wall -Wextra -Wpedantic)
endif()
add_executable(sum_benchmark sum_benchmark.cpp)
target_link_libraries(sum_benchmark PRIVATE omni_subop::omni_subop)
target_compile_features(sum_benchmark PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sum_benchmark PRIVATE -O3 -DNDEBUG -Wall -Wextra -Wpedantic)
endif()
