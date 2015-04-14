/*
 * Copyright (c) 1997, 2011, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_INTERPRETER_CPPINTERPRETERGENERATOR_HPP
#define SHARE_VM_INTERPRETER_CPPINTERPRETERGENERATOR_HPP

// This file contains the platform-independent parts
// of the template interpreter generator.

#ifdef CC_INTERP

class CppInterpreterGenerator: public AbstractInterpreterGenerator {
  protected:
  // shared code sequences
  // Converter for native abi result to tosca result
  address generate_result_handler_for(BasicType type);
  address generate_tosca_to_stack_converter(BasicType type);
  address generate_stack_to_stack_converter(BasicType type);
  address generate_stack_to_native_abi_converter(BasicType type);

  void generate_all();

 public:
  CppInterpreterGenerator(StubQueue* _code);

#ifdef TARGET_ARCH_x86
//# include "cppInterpreterGenerator_x86.hpp"
protected:

#if 0
  address generate_asm_interpreter_entry(bool synchronized);
  address generate_native_entry(bool synchronized);
  address generate_abstract_entry(void);
  address generate_math_entry(AbstractInterpreter::MethodKind kind);
  address generate_empty_entry(void);
  address generate_accessor_entry(void);
  address generate_Reference_get_entry(void);
  void lock_method(void);
  void generate_stack_overflow_check(void);

  void generate_counter_incr(Label* overflow, Label* profile_method, Label* profile_method_continue);
  void generate_counter_overflow(Label* do_continue);
#endif

  void generate_more_monitors();
  void generate_deopt_handling();
  address generate_interpreter_frame_manager(bool synchronized); // C++ interpreter only
  void generate_compute_interpreter_state(const Register state,
                                          const Register prev_state,
                                          const Register sender_sp,
                                          bool native); // C++ interpreter only
#endif
#ifdef TARGET_ARCH_sparc
# include "cppInterpreterGenerator_sparc.hpp"
#endif
#ifdef TARGET_ARCH_zero
# include "cppInterpreterGenerator_zero.hpp"
#endif
#ifdef TARGET_ARCH_arm
# include "cppInterpreterGenerator_arm.hpp"
#endif
#ifdef TARGET_ARCH_ppc
# include "cppInterpreterGenerator_ppc.hpp"
#endif

};

#endif // CC_INTERP

#endif // SHARE_VM_INTERPRETER_CPPINTERPRETERGENERATOR_HPP
