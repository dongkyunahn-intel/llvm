///
/// Perform several driver tests for SYCL offloading with -foffload-static-lib
///
// REQUIRES: clang-driver
// REQUIRES: x86-registered-target

/// test behaviors of passing a fat static lib
// Build a fat static lib that will be used for all tests
// RUN: echo "void foo(void) {}" > %t1.cpp
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl %t1.cpp -c -o %t1_bundle.o
// RUN: llvm-ar cr %t.a %t1_bundle.o
// RUN: llvm-ar cr %t_2.a %t1_bundle.o

/// ###########################################################################

/// test behaviors of -foffload-static-lib=<lib>
// RUN: touch %t.a
// RUN: touch %t.o
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -L/dummy/dir -foffload-static-lib=%t.a -### %t.o 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB
// FOFFLOAD_STATIC_LIB: clang-offload-bundler{{.*}} "-type=a" {{.*}} "-outputs=[[OUTLIB:.+\.a]]"
// FOFFLOAD_STATIC_LIB: llvm-link{{.*}} "[[OUTLIB]]"

/// Use of -foffload-static-lib and -foffload-whole-static-lib are deprecated
// RUN: touch dummy.a
// RUN: %clangxx -fsycl -foffload-static-lib=dummy.a -foffload-whole-static-lib=dummy.a -### 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_DEPRECATED
// RUN: %clang_cl -fsycl -foffload-static-lib=dummy.a -foffload-whole-static-lib=dummy.a -### 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_DEPRECATED
// FOFFLOAD_STATIC_LIB_DEPRECATED: option '-foffload-whole-static-lib=dummy.a' is deprecated, use 'dummy.a' directly instead

/// ###########################################################################

/// test behaviors of -foffload-static-lib=<lib> with multiple objects
// RUN: touch %t.a
// RUN: touch %t-1.o
// RUN: touch %t-2.o
// RUN: touch %t-3.o
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -foffload-static-lib=%t.a -### %t-1.o %t-2.o %t-3.o 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_MULTI_O
// FOFFLOAD_STATIC_LIB_MULTI_O: clang-offload-bundler{{.*}} "-type=o" {{.*}} "-inputs={{.+}}-1.o"
// FOFFLOAD_STATIC_LIB_MULTI_O: clang-offload-bundler{{.*}} "-type=o" {{.*}} "-inputs={{.+}}-2.o"
// FOFFLOAD_STATIC_LIB_MULTI_O: clang-offload-bundler{{.*}} "-type=o" {{.*}} "-inputs={{.+}}-3.o"
// FOFFLOAD_STATIC_LIB_MULTI_O: clang-offload-bundler{{.*}} "-type=a" {{.*}} "-outputs=[[OUTLIB:.+\.a]]"
// FOFFLOAD_STATIC_LIB_MULTI_O: llvm-link{{.*}} "[[OUTLIB]]"

/// ###########################################################################

/// test behaviors of -foffload-static-lib=<lib> from source
// RUN: touch %t.a
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -fno-sycl-device-lib=all -foffload-static-lib=%t.a -ccc-print-phases %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_SRC

// FOFFLOAD_STATIC_LIB_SRC: 0: input, "[[INPUTA:.+\.a]]", object, (host-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 1: input, "[[INPUTC:.+\.cpp]]", c++, (host-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 2: preprocessor, {1}, c++-cpp-output, (host-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 3: input, "[[INPUTC]]", c++, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 4: preprocessor, {3}, c++-cpp-output, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 5: compiler, {4}, sycl-header, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 6: offload, "host-sycl (x86_64-unknown-linux-gnu)" {2}, "device-sycl (spir64-unknown-unknown-sycldevice)" {5}, c++-cpp-output
// FOFFLOAD_STATIC_LIB_SRC: 7: compiler, {6}, ir, (host-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 8: backend, {7}, assembler, (host-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 9: assembler, {8}, object, (host-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 10: linker, {0, 9}, image, (host-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 11: compiler, {4}, ir, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 12: input, "[[INPUTA]]", archive
// FOFFLOAD_STATIC_LIB_SRC: 13: clang-offload-unbundler, {12}, archive
// FOFFLOAD_STATIC_LIB_SRC: 14: linker, {11, 13}, ir, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 15: sycl-post-link, {14}, tempfiletable, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 16: file-table-tform, {15}, tempfilelist, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 17: llvm-spirv, {16}, tempfilelist, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 18: file-table-tform, {15, 17}, tempfiletable, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 19: clang-offload-wrapper, {18}, object, (device-sycl)
// FOFFLOAD_STATIC_LIB_SRC: 20: offload, "host-sycl (x86_64-unknown-linux-gnu)" {10}, "device-sycl (spir64-unknown-unknown-sycldevice)" {19}, image

/// ###########################################################################

// RUN: touch %t.a
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -foffload-static-lib=%t.a -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_SRC2
// FOFFLOAD_STATIC_LIB_SRC2: clang-offload-bundler{{.*}} "-type=a" {{.*}} "-outputs=[[OUTLIB:.+\.a]]"
// FOFFLOAD_STATIC_LIB_SRC2: llvm-link{{.*}} "[[OUTLIB]]"
// FOFFLOAD_STATIC_LIB_SRC2: clang{{.*}} "-emit-obj" {{.*}} "-o" "[[HOSTOBJ:.+\.o]]"
// FOFFLOAD_STATIC_LIB_SRC2: ld{{(.exe)?}}" {{.*}} "[[HOSTOBJ]]"

/// ###########################################################################

// RUN: touch %t.a
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -foffload-static-lib=%t.a -o output_name -lOpenCL -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_SRC3
// FOFFLOAD_STATIC_LIB_SRC3: clang-offload-bundler{{.*}} "-type=a" {{.*}} "-outputs=[[OUTLIB:.+\.a]]"
// FOFFLOAD_STATIC_LIB_SRC3: llvm-link{{.*}} "[[OUTLIB]]"
// FOFFLOAD_STATIC_LIB_SRC3: ld{{(.exe)?}}" {{.*}} "-o" "output_name" {{.*}} "-lOpenCL"

/// ###########################################################################

// RUN: touch %t.a
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -foffload-static-lib=%t.a -o output_name -lstdc++ -z relro -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_SRC4
// FOFFLOAD_STATIC_LIB_SRC4: clang-offload-bundler{{.*}} "-type=a" {{.*}} "-outputs=[[OUTLIB:.+\.a]]"
// FOFFLOAD_STATIC_LIB_SRC4: llvm-link{{.*}} "[[OUTLIB]]"
// FOFFLOAD_STATIC_LIB_SRC4: ld{{(.exe)?}}" {{.*}} "-o" "output_name" {{.*}} "-lstdc++" "-z" "relro"

/// ###########################################################################

/// test behaviors of -foffload-whole-static-lib=<lib>
// RUN: touch %t.a
// RUN: touch %t_2.a
// RUN: touch %t.o
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -L/dummy/dir -foffload-whole-static-lib=%t.a -foffload-whole-static-lib=%t_2.a -### %t.o 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_WHOLE_STATIC_LIB
// FOFFLOAD_WHOLE_STATIC_LIB: clang-offload-bundler{{.*}} "-type=o" {{.*}}
// FOFFLOAD_WHOLE_STATIC_LIB: clang-offload-bundler{{.*}} "-type=a" {{.*}} "-inputs=[[INPUTA:.+\.a]]" "-outputs=[[OUTLIBA:.+\.a]]"
// FOFFLOAD_WHOLE_STATIC_LIB: clang-offload-bundler{{.*}} "-type=a" {{.*}} "-inputs=[[INPUTB:.+\.a]]" "-outputs=[[OUTLIBB:.+\.a]]"
// FOFFLOAD_WHOLE_STATIC_LIB: llvm-link{{.*}} "[[OUTLIBA]]" "[[OUTLIBB]]"
// FOFFLOAD_WHOLE_STATIC_LIB: llvm-spirv{{.*}}
// FOFFLOAD_WHOLE_STATIC_LIB: clang-offload-wrapper{{.*}}
// FOFFLOAD_WHOLE_STATIC_LIB: llc{{.*}}
// FOFFLOAD_WHOLE_STATIC_LIB: ld{{.*}} "[[INPUTA]]" "[[INPUTB]]"

/// ###########################################################################

/// test behaviors of -foffload-static-lib with no source/object
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -fno-sycl-device-lib=all -L/dummy/dir -foffload-static-lib=%t.a -### -ccc-print-phases 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_NOSRC_PHASES
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -fno-sycl-device-lib=all -L/dummy/dir -foffload-whole-static-lib=%t.a -### -ccc-print-phases 2>&1 \
// RUN:   | FileCheck %s -check-prefix=FOFFLOAD_STATIC_LIB_NOSRC_PHASES
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 0: input, "[[INPUTA:.+\.a]]", object, (host-sycl)
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 1: linker, {0}, image, (host-sycl)
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 2: input, "[[INPUTA]]", archive
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 3: clang-offload-unbundler, {2}, archive
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 4: linker, {3}, ir, (device-sycl)
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 5: sycl-post-link, {4}, tempfiletable, (device-sycl)
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 6: file-table-tform, {5}, tempfilelist, (device-sycl)
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 7: llvm-spirv, {6}, tempfilelist, (device-sycl)
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 8: file-table-tform, {5, 7}, tempfiletable, (device-sycl)
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 9: clang-offload-wrapper, {8}, object, (device-sycl)
// FOFFLOAD_STATIC_LIB_NOSRC_PHASES: 10: offload, "host-sycl (x86_64-unknown-linux-gnu)" {1}, "device-sycl (spir64-unknown-unknown-sycldevice)" {9}, image

/// ###########################################################################

/// test behaviors of -foffload-static-lib with no value
// RUN: %clangxx -target x86_64-unknown-linux-gnu -fsycl -foffload-static-lib= -c %s 2>&1 \
// RUN:   | FileCheck %s -check-prefixes=FOFFLOAD_STATIC_LIB_NOVALUE
// FOFFLOAD_STATIC_LIB_NOVALUE: warning: argument unused during compilation: '-foffload-static-lib='
