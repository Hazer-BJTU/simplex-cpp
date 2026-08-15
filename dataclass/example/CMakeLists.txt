# Offline PromptTemplate usage demo. Built by default (a single small
# translation unit); not registered with CTest — it is a readable walkthrough,
# not an assertion suite.
add_executable(prompt_template_demo prompt_template_demo.cpp)
target_link_libraries(prompt_template_demo PRIVATE dataclass_iface)
