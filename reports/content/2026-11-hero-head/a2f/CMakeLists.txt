add_executable(a2f-weights)
set_target_properties(a2f-weights PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${A2F_BINARY_DIR}/bin)
target_sources(a2f-weights PRIVATE a2f-weights.cpp)
target_link_libraries(a2f-weights PRIVATE audio2x AudioFile::AudioFile)
