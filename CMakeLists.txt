set(LLVM_LINK_COMPONENTS jit interpreter nativecodegen)

add_llvm_executable(Milkshake
  main.cpp
  parser.cpp
  parser.hpp
  Test.mlk
  Prelude.mlk
  )
  
install(TARGETS Milkshake RUNTIME DESTINATION projects)
set_target_properties(Milkshake PROPERTIES FOLDER "Projects")
set_target_properties(Milkshake PROPERTIES COMPILE_FLAGS "-std=c++0x -fexceptions")
