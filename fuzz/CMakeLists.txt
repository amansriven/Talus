set(Talus_FUZZ_TARGETS block_reader sstable_reader wal_reader version_edit)

add_executable(talus_fuzz_seeds make_seeds.cc)
target_include_directories(talus_fuzz_seeds PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(talus_fuzz_seeds PRIVATE talus talus_warnings)

foreach(target IN LISTS Talus_FUZZ_TARGETS)
  if(Talus_BUILD_FUZZERS)
    # A real libFuzzer binary: run it as `fuzz_<target> -max_total_time=60 CORPUS_DIR`.
    add_executable(fuzz_${target} ${target}_fuzzer.cc)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
  else()
    # Without libFuzzer: the same target driven over files, as a regression test.
    add_executable(fuzz_${target} ${target}_fuzzer.cc standalone_main.cc)
  endif()
  target_include_directories(fuzz_${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(fuzz_${target} PRIVATE talus talus_warnings)
endforeach()

if(Talus_BUILD_TESTS AND NOT Talus_BUILD_FUZZERS)
  # Every build runs each target over a freshly generated seed corpus, so the
  # fuzz targets keep compiling and keep passing on valid and damaged inputs.
  set(seed_dir ${CMAKE_CURRENT_BINARY_DIR}/seeds)
  add_test(NAME FuzzSeeds COMMAND talus_fuzz_seeds ${seed_dir})
  set_tests_properties(FuzzSeeds PROPERTIES FIXTURES_SETUP fuzz_seeds)
  foreach(target IN LISTS Talus_FUZZ_TARGETS)
    add_test(NAME FuzzRegression.${target} COMMAND fuzz_${target} ${seed_dir}/${target})
    set_tests_properties(FuzzRegression.${target} PROPERTIES FIXTURES_REQUIRED fuzz_seeds)
  endforeach()
endif()
