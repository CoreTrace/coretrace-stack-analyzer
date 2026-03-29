// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <llvm/IR/Module.h>

namespace ctrace::stack
{
    void runFunctionAttrsPass(llvm::Module& mod);
}
