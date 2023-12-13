//===- Target.inc - Simulator target registration ---------------*- C++ -*-===//
//
// (C) Copyright IBM 2023.
//
// This code is part of Qiskit.
//
// This code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions. You may obtain a copy of this license in the LICENSE.txt
// file in the root directory of this source tree.
//
// Any modifications or derivative works of this code must retain this
// copyright notice, and modified files need to carry a notice indicating
// that they have been altered from the originals.
//
//===----------------------------------------------------------------------===//
//
//  This file defines static objects that register system targets
//  with the QSS compiler core.
//
//===----------------------------------------------------------------------===//

#ifndef HAL_TARGETS_AER_SIMULATOR_TARGET_H
#define HAL_TARGETS_AER_SIMULATOR_TARGET_H

#include "AerSimulator.h"

namespace qssc::targets::simulators::aer {

[[maybe_unused]] int registrar = init();

} // namespace qssc::targets::simulators::aer

#endif // HAL_TARGETS_AER_SIMULATOR_TARGET_H