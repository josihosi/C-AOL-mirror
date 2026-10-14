#!/bin/bash

set -eu
set -o pipefail

build_dir=${CATA_BUILD_DIR:-build}
plugin=${CATA_CLANG_TIDY_PLUGIN:-${build_dir}/tools/clang-tidy-plugin/libCataAnalyzerPlugin.so}
clang_tidy=${CATA_CLANG_TIDY:-clang-tidy}

# Tolerate GCC-built compile_commands driving clang-tidy (silence unknown
# -W flags) and dodge the clang>=22 c2y-extension warning on Catch2
# __COUNTER__ macros (llvm/llvm-project#189645).
extra_args=(
    --extra-arg=-Wno-unknown-warning-option
    --extra-arg=-DCATCH_CONFIG_NO_COUNTER
)

if [ -f "$plugin" ]
then
    set -x
    "$clang_tidy" --load="$plugin" --enable-check-profile --store-check-profile=clang-tidy-trace "${extra_args[@]}" "$@"
else
    set -x
    "$clang_tidy" "${extra_args[@]}" "$@"
fi
