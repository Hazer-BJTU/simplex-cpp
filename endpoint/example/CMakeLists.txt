# Interactive examples that talk to live services over HTTPS. Built by default
# (cheap) but intentionally not registered with CTest: running them needs
# network access and real credentials.
add_executable(deepseek_chat deepseek_chat.cpp)
target_link_libraries(deepseek_chat PRIVATE endpoint_lib)
