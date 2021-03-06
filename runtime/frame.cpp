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

#include "precompiled.hpp"
#include "gc_interface/collectedHeap.inline.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/oopMapCache.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.inline.hpp"
#include "oops/markOop.hpp"
#include "oops/methodDataOop.hpp"
#include "oops/methodOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oop.inline2.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/monitorChunk.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/signature.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "utilities/decoder.hpp"

#ifdef TARGET_ARCH_x86
# include "nativeInst_x86.hpp"
#endif
#ifdef TARGET_ARCH_sparc
# include "nativeInst_sparc.hpp"
#endif
#ifdef TARGET_ARCH_zero
# include "nativeInst_zero.hpp"
#endif
#ifdef TARGET_ARCH_arm
# include "nativeInst_arm.hpp"
#endif
#ifdef TARGET_ARCH_ppc
# include "nativeInst_ppc.hpp"
#endif

RegisterMap::RegisterMap(JavaThread *thread, bool update_map) {
  _thread         = thread;
  _update_map     = update_map;
  clear();
  debug_only(_update_for_id = NULL;)
#ifndef PRODUCT
  for (int i = 0; i < reg_count ; i++ ) _location[i] = NULL;
#endif /* PRODUCT */
}

RegisterMap::RegisterMap(const RegisterMap* map) {
  assert(map != this, "bad initialization parameter");
  assert(map != NULL, "RegisterMap must be present");
  _thread                = map->thread();
  _update_map            = map->update_map();
  _include_argument_oops = map->include_argument_oops();
  debug_only(_update_for_id = map->_update_for_id;)
  pd_initialize_from(map);
  if (update_map()) {
    for(int i = 0; i < location_valid_size; i++) {
      LocationValidType bits = !update_map() ? 0 : map->_location_valid[i];
      _location_valid[i] = bits;
      // for whichever bits are set, pull in the corresponding map->_location
      int j = i*location_valid_type_size;
      while (bits != 0) {
        if ((bits & 1) != 0) {
          assert(0 <= j && j < reg_count, "range check");
          _location[j] = map->_location[j];
        }
        bits >>= 1;
        j += 1;
      }
    }
  }
}

void RegisterMap::clear() {
  set_include_argument_oops(true);
  if (_update_map) {
    for(int i = 0; i < location_valid_size; i++) {
      _location_valid[i] = 0;
    }
    pd_clear();
  } else {
    pd_initialize();
  }
}

#ifndef PRODUCT

void RegisterMap::print_on(outputStream* st) const {
  st->print_cr("Register map");
  for(int i = 0; i < reg_count; i++) {

    VMReg r = VMRegImpl::as_VMReg(i);
    intptr_t* src = (intptr_t*) location(r);
    if (src != NULL) {

      r->print_on(st);
      st->print(" [" INTPTR_FORMAT "] = ", src);
      if (((uintptr_t)src & (sizeof(*src)-1)) != 0) {
        st->print_cr("<misaligned>");
      } else {
        st->print_cr(INTPTR_FORMAT, *src);
      }
    }
  }
}

void RegisterMap::print() const {
  print_on(tty);
}

#endif
// This returns the pc that if you were in the debugger you'd see. Not
// the idealized value in the frame object. This undoes the magic conversion
// that happens for deoptimized frames. In addition it makes the value the
// hardware would want to see in the native frame. The only user (at this point)
// is deoptimization. It likely no one else should ever use it.

address frame::raw_pc() const {
  if (is_deoptimized_frame()) {
    nmethod* nm = cb()->as_nmethod_or_null();
    if (nm->is_method_handle_return(pc()))
      return nm->deopt_mh_handler_begin() - pc_return_offset;
    else
      return nm->deopt_handler_begin() - pc_return_offset;
  } else {
    return (pc() - pc_return_offset);
  }
}

// Change the pc in a frame object. This does not change the actual pc in
// actual frame. To do that use patch_pc.
//
void frame::set_pc(address   newpc ) {
#ifdef ASSERT
  if (_cb != NULL && _cb->is_nmethod()) {
    assert(!((nmethod*)_cb)->is_deopt_pc(_pc), "invariant violation");
  }
#endif // ASSERT

  // Unsafe to use the is_deoptimzed tester after changing pc
  _deopt_state = unknown;
  _pc = newpc;
  _cb = CodeCache::find_blob_unsafe(_pc);

}

// type testers
bool frame::is_ricochet_frame() const {
  RicochetBlob* rcb = SharedRuntime::ricochet_blob();
  return (_cb == rcb && rcb != NULL && rcb->returns_to_bounce_addr(_pc));
}

bool frame::is_deoptimized_frame() const {
  assert(_deopt_state != unknown, "not answerable");
  return _deopt_state == is_deoptimized;
}

bool frame::is_native_frame() const {
  return (_cb != NULL &&
          _cb->is_nmethod() &&
          ((nmethod*)_cb)->is_native_method());
}

bool frame::is_java_frame() const {
  if (is_interpreted_frame()) return true;
  if (is_compiled_frame())    return true;
  return false;
}


bool frame::is_compiled_frame() const {
  if (_cb != NULL &&
      _cb->is_nmethod() &&
      ((nmethod*)_cb)->is_java_method()) {
    return true;
  }
  return false;
}


bool frame::is_runtime_frame() const {
  return (_cb != NULL && _cb->is_runtime_stub());
}

bool frame::is_safepoint_blob_frame() const {
  return (_cb != NULL && _cb->is_safepoint_stub());
}

// testers

bool frame::is_first_java_frame() const {
  RegisterMap map(JavaThread::current(), false); // No update
  frame s;
  for (s = sender(&map); !(s.is_java_frame() || s.is_first_frame()); s = s.sender(&map));
  return s.is_first_frame();
}


bool frame::entry_frame_is_first() const {
  return entry_frame_call_wrapper()->anchor()->last_Java_sp() == NULL;
}


bool frame::should_be_deoptimized() const {
  if (_deopt_state == is_deoptimized ||
      !is_compiled_frame() ) return false;
  assert(_cb != NULL && _cb->is_nmethod(), "must be an nmethod");
  nmethod* nm = (nmethod *)_cb;
  if (TraceDependencies) {
    tty->print("checking (%s) ", nm->is_marked_for_deoptimization() ? "true" : "false");
    nm->print_value_on(tty);
    tty->cr();
  }

  if( !nm->is_marked_for_deoptimization() )
    return false;

  // If at the return point, then the frame has already been popped, and
  // only the return needs to be executed. Don't deoptimize here.
  return !nm->is_at_poll_return(pc());
}

bool frame::can_be_deoptimized() const {
  if (!is_compiled_frame()) return false;
  nmethod* nm = (nmethod*)_cb;

  if( !nm->can_be_deoptimized() )
    return false;

  return !nm->is_at_poll_return(pc());
}

void frame::deoptimize(JavaThread* thread) {
  // Schedule deoptimization of an nmethod activation with this frame.
  assert(_cb != NULL && _cb->is_nmethod(), "must be");
  nmethod* nm = (nmethod*)_cb;

  // This is a fix for register window patching race
  if (NeedsDeoptSuspend && Thread::current() != thread) {
    assert(SafepointSynchronize::is_at_safepoint(),
           "patching other threads for deopt may only occur at a safepoint");

    // It is possible especially with DeoptimizeALot/DeoptimizeRandom that
    // we could see the frame again and ask for it to be deoptimized since
    // it might move for a long time. That is harmless and we just ignore it.
    if (id() == thread->must_deopt_id()) {
      assert(thread->is_deopt_suspend(), "lost suspension");
      return;
    }

    // We are at a safepoint so the target thread can only be
    // in 4 states:
    //     blocked - no problem
    //     blocked_trans - no problem (i.e. could have woken up from blocked
    //                                 during a safepoint).
    //     native - register window pc patching race
    //     native_trans - momentary state
    //
    // We could just wait out a thread in native_trans to block.
    // Then we'd have all the issues that the safepoint code has as to
    // whether to spin or block. It isn't worth it. Just treat it like
    // native and be done with it.
    //
    // Examine the state of the thread at the start of safepoint since
    // threads that were in native at the start of the safepoint could
    // come to a halt during the safepoint, changing the current value
    // of the safepoint_state.
    JavaThreadState state = thread->safepoint_state()->orig_thread_state();
    if (state == _thread_in_native || state == _thread_in_native_trans) {
      // Since we are at a safepoint the target thread will stop itself
      // before it can return to java as long as we remain at the safepoint.
      // Therefore we can put an additional request for the thread to stop
      // no matter what no (like a suspend). This will cause the thread
      // to notice it needs to do the deopt on its own once it leaves native.
      //
      // The only reason we must do this is because on machine with register
      // windows we have a race with patching the return address and the
      // window coming live as the thread returns to the Java code (but still
      // in native mode) and then blocks. It is only this top most frame
      // that is at risk. So in truth we could add an additional check to
      // see if this frame is one that is at risk.
      RegisterMap map(thread, false);
      frame at_risk =  thread->last_frame().sender(&map);
      if (id() == at_risk.id()) {
        thread->set_must_deopt_id(id());
        thread->set_deopt_suspend();
        return;
      }
    }
  } // NeedsDeoptSuspend


  // If the call site is a MethodHandle call site use the MH deopt
  // handler.
  address deopt = nm->is_method_handle_return(pc()) ?
    nm->deopt_mh_handler_begin() :
    nm->deopt_handler_begin();

  // Save the original pc before we patch in the new one
  nm->set_original_pc(this, pc());
  patch_pc(thread, deopt);

#ifdef ASSERT
  {
    RegisterMap map(thread, false);
    frame check = thread->last_frame();
    while (id() != check.id()) {
      check = check.sender(&map);
    }
    assert(check.is_deoptimized_frame(), "missed deopt");
  }
#endif // ASSERT
}

frame frame::java_sender() const {
  RegisterMap map(JavaThread::current(), false);
  frame s;
  for (s = sender(&map); !(s.is_java_frame() || s.is_first_frame()); s = s.sender(&map)) ;
  guarantee(s.is_java_frame(), "tried to get caller of first java frame");
  return s;
}

frame frame::real_sender(RegisterMap* map) const {
  frame result = sender(map);
  while (result.is_runtime_frame() ||
         result.is_ricochet_frame()) {
    result = result.sender(map);
  }
  return result;
}

frame frame::sender_for_ricochet_frame(RegisterMap* map) const {
  assert(is_ricochet_frame(), "");
  return MethodHandles::ricochet_frame_sender(*this, map);
}

// Note: called by profiler - NOT for current thread
frame frame::profile_find_Java_sender_frame(JavaThread *thread) {
// If we don't recognize this frame, walk back up the stack until we do
  RegisterMap map(thread, false);
  frame first_java_frame = frame();

  // Find the first Java frame on the stack starting with input frame
  if (is_java_frame()) {
    // top frame is compiled frame or deoptimized frame
    first_java_frame = *this;
  } else if (safe_for_sender(thread)) {
    for (frame sender_frame = sender(&map);
      sender_frame.safe_for_sender(thread) && !sender_frame.is_first_frame();
      sender_frame = sender_frame.sender(&map)) {
      if (sender_frame.is_java_frame()) {
        first_java_frame = sender_frame;
        break;
      }
    }
  }
  return first_java_frame;
}

// Interpreter frames


void frame::interpreter_frame_set_locals(intptr_t* locs)  {
  assert(is_interpreted_frame(), "Not an interpreted frame");
  *interpreter_frame_locals_addr() = locs;
}

methodOop frame::interpreter_frame_method() const {
  assert(is_interpreted_frame(), "interpreted frame expected");
  methodOop m = *interpreter_frame_method_addr();
  assert(m->is_perm(), "bad methodOop in interpreter frame");
  assert(m->is_method(), "not a methodOop");
  return m;
}

void frame::interpreter_frame_set_method(methodOop method) {
  assert(is_interpreted_frame(), "interpreted frame expected");
  *interpreter_frame_method_addr() = method;
}

void frame::interpreter_frame_set_bcx(intptr_t bcx) {
  assert(is_interpreted_frame(), "Not an interpreted frame");
  if (ProfileInterpreter) {
    bool formerly_bci = is_bci(interpreter_frame_bcx());
    bool is_now_bci = is_bci(bcx);
    *interpreter_frame_bcx_addr() = bcx;

    intptr_t mdx = interpreter_frame_mdx();

    if (mdx != 0) {
      if (formerly_bci) {
        if (!is_now_bci) {
          // The bcx was just converted from bci to bcp.
          // Convert the mdx in parallel.
          methodDataOop mdo = interpreter_frame_method()->method_data();
          assert(mdo != NULL, "");
          int mdi = mdx - 1; // We distinguish valid mdi from zero by adding one.
          address mdp = mdo->di_to_dp(mdi);
          interpreter_frame_set_mdx((intptr_t)mdp);
        }
      } else {
        if (is_now_bci) {
          // The bcx was just converted from bcp to bci.
          // Convert the mdx in parallel.
          methodDataOop mdo = interpreter_frame_method()->method_data();
          assert(mdo != NULL, "");
          int mdi = mdo->dp_to_di((address)mdx);
          interpreter_frame_set_mdx((intptr_t)mdi + 1); // distinguish valid from 0.
        }
      }
    }
  } else {
    *interpreter_frame_bcx_addr() = bcx;
  }
}

jint frame::interpreter_frame_bci() const {
  assert(is_interpreted_frame(), "interpreted frame expected");
  intptr_t bcx = interpreter_frame_bcx();
  return is_bci(bcx) ? bcx : interpreter_frame_method()->bci_from((address)bcx);
}

void frame::interpreter_frame_set_bci(jint bci) {
  assert(is_interpreted_frame(), "interpreted frame expected");
  assert(!is_bci(interpreter_frame_bcx()), "should not set bci during GC");
  interpreter_frame_set_bcx((intptr_t)interpreter_frame_method()->bcp_from(bci));
}

address frame::interpreter_frame_bcp() const {
  assert(is_interpreted_frame(), "interpreted frame expected");
  intptr_t bcx = interpreter_frame_bcx();
  return is_bci(bcx) ? interpreter_frame_method()->bcp_from(bcx) : (address)bcx;
}

void frame::interpreter_frame_set_bcp(address bcp) {
  assert(is_interpreted_frame(), "interpreted frame expected");
  assert(!is_bci(interpreter_frame_bcx()), "should not set bcp during GC");
  interpreter_frame_set_bcx((intptr_t)bcp);
}

void frame::interpreter_frame_set_mdx(intptr_t mdx) {
  assert(is_interpreted_frame(), "Not an interpreted frame");
  assert(ProfileInterpreter, "must be profiling interpreter");
  *interpreter_frame_mdx_addr() = mdx;
}

address frame::interpreter_frame_mdp() const {
  assert(ProfileInterpreter, "must be profiling interpreter");
  assert(is_interpreted_frame(), "interpreted frame expected");
  intptr_t bcx = interpreter_frame_bcx();
  intptr_t mdx = interpreter_frame_mdx();

  assert(!is_bci(bcx), "should not access mdp during GC");
  return (address)mdx;
}

void frame::interpreter_frame_set_mdp(address mdp) {
  assert(is_interpreted_frame(), "interpreted frame expected");
  if (mdp == NULL) {
    // Always allow the mdp to be cleared.
    interpreter_frame_set_mdx((intptr_t)mdp);
  }
  intptr_t bcx = interpreter_frame_bcx();
  assert(!is_bci(bcx), "should not set mdp during GC");
  interpreter_frame_set_mdx((intptr_t)mdp);
}

BasicObjectLock* frame::next_monitor_in_interpreter_frame(BasicObjectLock* current) const {
  assert(is_interpreted_frame(), "Not an interpreted frame");
#ifdef ASSERT
  interpreter_frame_verify_monitor(current);
#endif
  BasicObjectLock* next = (BasicObjectLock*) (((intptr_t*) current) + interpreter_frame_monitor_size());
  return next;
}

BasicObjectLock* frame::previous_monitor_in_interpreter_frame(BasicObjectLock* current) const {
  assert(is_interpreted_frame(), "Not an interpreted frame");
#ifdef ASSERT
//   // This verification needs to be checked before being enabled
//   interpreter_frame_verify_monitor(current);
#endif
  BasicObjectLock* previous = (BasicObjectLock*) (((intptr_t*) current) - interpreter_frame_monitor_size());
  return previous;
}

// Interpreter locals and expression stack locations.

intptr_t* frame::interpreter_frame_local_at(int index) const {
  const int n = Interpreter::local_offset_in_bytes(index)/wordSize;
  return &((*interpreter_frame_locals_addr())[n]);
}

intptr_t* frame::interpreter_frame_expression_stack_at(jint offset) const {
  const int i = offset * interpreter_frame_expression_stack_direction();
  const int n = i * Interpreter::stackElementWords;
  return &(interpreter_frame_expression_stack()[n]);
}

jint frame::interpreter_frame_expression_stack_size() const {
  // Number of elements on the interpreter expression stack
  // Callers should span by stackElementWords
  int element_size = Interpreter::stackElementWords;
  if (frame::interpreter_frame_expression_stack_direction() < 0) {
    return (interpreter_frame_expression_stack() -
            interpreter_frame_tos_address() + 1)/element_size;
  } else {
    return (interpreter_frame_tos_address() -
            interpreter_frame_expression_stack() + 1)/element_size;
  }
}


// (frame::interpreter_frame_sender_sp accessor is in frame_<arch>.cpp)

const char* frame::print_name() const {
  if (is_native_frame())      return "Native";
  if (is_interpreted_frame()) return "Interpreted";
  if (is_ricochet_frame())    return "Ricochet";
  if (is_compiled_frame()) {
    if (is_deoptimized_frame()) return "Deoptimized";
    return "Compiled";
  }
  if (sp() == NULL)            return "Empty";
  return "C";
}

void frame::print_value_on(outputStream* st, JavaThread *thread) const {
  NOT_PRODUCT(address begin = pc()-40;)
  NOT_PRODUCT(address end   = NULL;)

  st->print("%s frame (sp=" INTPTR_FORMAT " unextended sp=" INTPTR_FORMAT, print_name(), sp(), unextended_sp());
  if (sp() != NULL)
    st->print(", fp=" INTPTR_FORMAT ", pc=" INTPTR_FORMAT, fp(), pc());

  if (StubRoutines::contains(pc())) {
    st->print_cr(")");
    st->print("(");
    StubCodeDesc* desc = StubCodeDesc::desc_for(pc());
    st->print("~Stub::%s", desc->name());
    NOT_PRODUCT(begin = desc->begin(); end = desc->end();)
  } else if (Interpreter::contains(pc())) {
    st->print_cr(")");
    st->print("(");
    InterpreterCodelet* desc = Interpreter::codelet_containing(pc());
    if (desc != NULL) {
      st->print("~");
      desc->print();
      NOT_PRODUCT(begin = desc->code_begin(); end = desc->code_end();)
    } else {
      st->print("~interpreter");
    }
  }
  st->print_cr(")");

  if (_cb != NULL) {
    st->print("     ");
    _cb->print_value_on(st);
    st->cr();
#ifndef PRODUCT
    if (end == NULL) {
      begin = _cb->code_begin();
      end   = _cb->code_end();
    }
#endif
  }
  NOT_PRODUCT(if (WizardMode && Verbose) Disassembler::decode(begin, end);)
}


void frame::print_on(outputStream* st) const {
  print_value_on(st,NULL);
  if (is_interpreted_frame()) {
    interpreter_frame_print_on(st);
  }
}


void frame::interpreter_frame_print_on(outputStream* st) const {
#ifndef PRODUCT
  assert(is_interpreted_frame(), "Not an interpreted frame");
  jint i;
  for (i = 0; i < interpreter_frame_method()->max_locals(); i++ ) {
    intptr_t x = *interpreter_frame_local_at(i);
    st->print(" - local  [" INTPTR_FORMAT "]", x);
    st->fill_to(23);
    st->print_cr("; #%d", i);
  }
  for (i = interpreter_frame_expression_stack_size() - 1; i >= 0; --i ) {
    intptr_t x = *interpreter_frame_expression_stack_at(i);
    st->print(" - stack  [" INTPTR_FORMAT "]", x);
    st->fill_to(23);
    st->print_cr("; #%d", i);
  }
  // locks for synchronization
  for (BasicObjectLock* current = interpreter_frame_monitor_end();
       current < interpreter_frame_monitor_begin();
       current = next_monitor_in_interpreter_frame(current)) {
    st->print(" - obj    [");
    current->obj()->print_value_on(st);
    st->print_cr("]");
    st->print(" - lock   [");
    current->lock()->print_on(st);
    st->print_cr("]");
  }
  // monitor
  st->print_cr(" - monitor[" INTPTR_FORMAT "]", interpreter_frame_monitor_begin());
  // bcp
  st->print(" - bcp    [" INTPTR_FORMAT "]", interpreter_frame_bcp());
  st->fill_to(23);
  st->print_cr("; @%d", interpreter_frame_bci());
  // locals
  st->print_cr(" - locals [" INTPTR_FORMAT "]", interpreter_frame_local_at(0));
  // method
  st->print(" - method [" INTPTR_FORMAT "]", (address)interpreter_frame_method());
  st->fill_to(23);
  st->print("; ");
  interpreter_frame_method()->print_name(st);
  st->cr();
#endif
}

// Return whether the frame is in the VM or os indicating a Hotspot problem.
// Otherwise, it's likely a bug in the native library that the Java code calls,
// hopefully indicating where to submit bugs.
static void print_C_frame(outputStream* st, char* buf, int buflen, address pc) {
  // C/C++ frame
  bool in_vm = os::address_is_in_vm(pc);
  st->print(in_vm ? "V" : "C");

  int offset;
  bool found;

  // libname
  found = os::dll_address_to_library_name(pc, buf, buflen, &offset);
  if (found) {
    // skip directory names
    const char *p1, *p2;
    p1 = buf;
    int len = (int)strlen(os::file_separator());
    while ((p2 = strstr(p1, os::file_separator())) != NULL) p1 = p2 + len;
    st->print("  [%s+0x%x]", p1, offset);
  } else {
    st->print("  " PTR_FORMAT, pc);
  }

  // function name - os::dll_address_to_function_name() may return confusing
  // names if pc is within jvm.dll or libjvm.so, because JVM only has
  // JVM_xxxx and a few other symbols in the dynamic symbol table. Do this
  // only for native libraries.
  if (!in_vm || Decoder::can_decode_C_frame_in_vm()) {
    found = os::dll_address_to_function_name(pc, buf, buflen, &offset);

    if (found) {
      st->print("  %s+0x%x", buf, offset);
    }
  }
}

// frame::print_on_error() is called by fatal error handler. Notice that we may
// crash inside this function if stack frame is corrupted. The fatal error
// handler can catch and handle the crash. Here we assume the frame is valid.
//
// First letter indicates type of the frame:
//    J: Java frame (compiled)
//    j: Java frame (interpreted)
//    V: VM frame (C/C++)
//    v: Other frames running VM generated code (e.g. stubs, adapters, etc.)
//    C: C/C++ frame
//
// We don't need detailed frame type as that in frame::print_name(). "C"
// suggests the problem is in user lib; everything else is likely a VM bug.

void frame::print_on_error(outputStream* st, char* buf, int buflen, bool verbose) const {
  if (_cb != NULL) {
    if (Interpreter::contains(pc())) {
      methodOop m = this->interpreter_frame_method();
      if (m != NULL) {
        m->name_and_sig_as_C_string(buf, buflen);
        st->print("j  %s", buf);
        st->print("+%d", this->interpreter_frame_bci());
      } else {
        st->print("j  " PTR_FORMAT, pc());
      }
    } else if (StubRoutines::contains(pc())) {
      StubCodeDesc* desc = StubCodeDesc::desc_for(pc());
      if (desc != NULL) {
        st->print("v  ~StubRoutines::%s", desc->name());
      } else {
        st->print("v  ~StubRoutines::" PTR_FORMAT, pc());
      }
    } else if (_cb->is_buffer_blob()) {
      st->print("v  ~BufferBlob::%s", ((BufferBlob *)_cb)->name());
    } else if (_cb->is_nmethod()) {
      methodOop m = ((nmethod *)_cb)->method();
      if (m != NULL) {
        m->name_and_sig_as_C_string(buf, buflen);
        st->print("J  %s", buf);
      } else {
        st->print("J  " PTR_FORMAT, pc());
      }
    } else if (_cb->is_runtime_stub()) {
      st->print("v  ~RuntimeStub::%s", ((RuntimeStub *)_cb)->name());
    } else if (_cb->is_deoptimization_stub()) {
      st->print("v  ~DeoptimizationBlob");
    } else if (_cb->is_ricochet_stub()) {
      st->print("v  ~RichochetBlob");
    } else if (_cb->is_exception_stub()) {
      st->print("v  ~ExceptionBlob");
    } else if (_cb->is_safepoint_stub()) {
      st->print("v  ~SafepointBlob");
    } else {
      st->print("v  blob " PTR_FORMAT, pc());
    }
  } else {
    print_C_frame(st, buf, buflen, pc());
  }
}


/*
  The interpreter_frame_expression_stack_at method in the case of SPARC needs the
  max_stack value of the method in order to compute the expression stack address.
  It uses the methodOop in order to get the max_stack value but during GC this
  methodOop value saved on the frame is changed by reverse_and_push and hence cannot
  be used. So we save the max_stack value in the FrameClosure object and pass it
  down to the interpreter_frame_expression_stack_at method
*/
class InterpreterFrameClosure : public OffsetClosure {
 private:
  frame* _fr;
  OopClosure* _f;
  int    _max_locals;
  int    _max_stack;

 public:
  InterpreterFrameClosure(frame* fr, int max_locals, int max_stack,
                          OopClosure* f) {
    _fr         = fr;
    _max_locals = max_locals;
    _max_stack  = max_stack;
    _f          = f;
  }

  void offset_do(int offset) {
    oop* addr;
    if (offset < _max_locals) {
      addr = (oop*) _fr->interpreter_frame_local_at(offset);
      assert((intptr_t*)addr >= _fr->sp(), "must be inside the frame");
      _f->do_oop(addr);
    } else {
      addr = (oop*) _fr->interpreter_frame_expression_stack_at((offset - _max_locals));
      // In case of exceptions, the expression stack is invalid and the esp will be reset to express
      // this condition. Therefore, we call f only if addr is 'inside' the stack (i.e., addr >= esp for Intel).
      bool in_stack;
      if (frame::interpreter_frame_expression_stack_direction() > 0) {
        in_stack = (intptr_t*)addr <= _fr->interpreter_frame_tos_address();
      } else {
        in_stack = (intptr_t*)addr >= _fr->interpreter_frame_tos_address();
      }
      if (in_stack) {
        _f->do_oop(addr);
      }
    }
  }

  int max_locals()  { return _max_locals; }
  frame* fr()       { return _fr; }
};


class InterpretedArgumentOopFinder: public SignatureInfo {
 private:
  OopClosure* _f;        // Closure to invoke
  int    _offset;        // TOS-relative offset, decremented with each argument
  bool   _has_receiver;  // true if the callee has a receiver
  frame* _fr;

  void set(int size, BasicType type) {
    _offset -= size;
    if (type == T_OBJECT || type == T_ARRAY) oop_offset_do();
  }

  void oop_offset_do() {
    oop* addr;
    addr = (oop*)_fr->interpreter_frame_tos_at(_offset);
    _f->do_oop(addr);
  }

 public:
  InterpretedArgumentOopFinder(Symbol* signature, bool has_receiver, frame* fr, OopClosure* f) : SignatureInfo(signature), _has_receiver(has_receiver) {
    // compute size of arguments
    int args_size = ArgumentSizeComputer(signature).size() + (has_receiver ? 1 : 0);
    assert(!fr->is_interpreted_frame() ||
           args_size <= fr->interpreter_frame_expression_stack_size(),
            "args cannot be on stack anymore");
    // initialize InterpretedArgumentOopFinder
    _f         = f;
    _fr        = fr;
    _offset    = args_size;
  }

  void oops_do() {
    if (_has_receiver) {
      --_offset;
      oop_offset_do();
    }
    iterate_parameters();
  }
};


// Entry frame has following form (n arguments)
//         +-----------+
//   sp -> |  last arg |
//         +-----------+
//         :    :::    :
//         +-----------+
// (sp+n)->|  first arg|
//         +-----------+



// visits and GC's all the arguments in entry frame
class EntryFrameOopFinder: public SignatureInfo {
 private:
  bool   _is_static;
  int    _offset;
  frame* _fr;
  OopClosure* _f;

  void set(int size, BasicType type) {
    assert (_offset >= 0, "illegal offset");
    if (type == T_OBJECT || type == T_ARRAY) oop_at_offset_do(_offset);
    _offset -= size;
  }

  void oop_at_offset_do(int offset) {
    assert (offset >= 0, "illegal offset");
    oop* addr = (oop*) _fr->entry_frame_argument_at(offset);
    _f->do_oop(addr);
  }

 public:
   EntryFrameOopFinder(frame* frame, Symbol* signature, bool is_static) : SignatureInfo(signature) {
     _f = NULL; // will be set later
     _fr = frame;
     _is_static = is_static;
     _offset = ArgumentSizeComputer(signature).size() - 1; // last parameter is at index 0
   }

  void arguments_do(OopClosure* f) {
    _f = f;
    if (!_is_static) oop_at_offset_do(_offset+1); // do the receiver
    iterate_parameters();
  }

};

oop* frame::interpreter_callee_receiver_addr(Symbol* signature) {
  ArgumentSizeComputer asc(signature);
  int size = asc.size();
  return (oop *)interpreter_frame_tos_at(size);
}


void frame::oops_interpreted_do(OopClosure* f, const RegisterMap* map, bool query_oop_map_cache) {
  assert(is_interpreted_frame(), "Not an interpreted frame");
  assert(map != NULL, "map must be set");
  Thread *thread = Thread::current();
  methodHandle m (thread, interpreter_frame_method());
  jint      bci = interpreter_frame_bci();

  assert(Universe::heap()->is_in(m()), "must be valid oop");
  assert(m->is_method(), "checking frame value");
  assert((m->is_native() && bci == 0)  || (!m->is_native() && bci >= 0 && bci < m->code_size()), "invalid bci value");

  // Handle the monitor elements in the activation
  for (
    BasicObjectLock* current = interpreter_frame_monitor_end();
    current < interpreter_frame_monitor_begin();
    current = next_monitor_in_interpreter_frame(current)
  ) {
#ifdef ASSERT
    interpreter_frame_verify_monitor(current);
#endif
    current->oops_do(f);
  }

  // process fixed part
  f->do_oop((oop*)interpreter_frame_method_addr());
  f->do_oop((oop*)interpreter_frame_cache_addr());

  // Hmm what about the mdp?
#ifdef CC_INTERP
  // Interpreter frame in the midst of a call have a methodOop within the
  // object.
  interpreterState istate = get_interpreterState();
  if (istate->msg() == BytecodeInterpreter::call_method) {
    f->do_oop((oop*)&istate->_result._to_call._callee);
  }

#endif /* CC_INTERP */

#if !defined(PPC) || defined(ZERO)
  if (m->is_native()) {
#ifdef CC_INTERP
    f->do_oop((oop*)&istate->_oop_temp);
#else
    f->do_oop((oop*)( fp() + interpreter_frame_oop_temp_offset ));
#endif /* CC_INTERP */
  }
#else // PPC
  if (m->is_native() && m->is_static()) {
    f->do_oop(interpreter_frame_mirror_addr());
  }
#endif // PPC

  int max_locals = m->is_native() ? m->size_of_parameters() : m->max_locals();

  Symbol* signature = NULL;
  bool has_receiver = false;

  // Process a callee's arguments if we are at a call site
  // (i.e., if we are at an invoke bytecode)
  // This is used sometimes for calling into the VM, not for another
  // interpreted or compiled frame.
  if (!m->is_native()) {
    Bytecode_invoke call = Bytecode_invoke_check(m, bci);
    if (call.is_valid()) {
      signature = call.signature();
      has_receiver = call.has_receiver();
      if (map->include_argument_oops() &&
          interpreter_frame_expression_stack_size() > 0) {
        ResourceMark rm(thread);  // is this right ???
        // we are at a call site & the expression stack is not empty
        // => process callee's arguments
        //
        // Note: The expression stack can be empty if an exception
        //       occurred during method resolution/execution. In all
        //       cases we empty the expression stack completely be-
        //       fore handling the exception (the exception handling
        //       code in the interpreter calls a blocking runtime
        //       routine which can cause this code to be executed).
        //       (was bug gri 7/27/98)
        oops_interpreted_arguments_do(signature, has_receiver, f);
      }
    }
  }

  InterpreterFrameClosure blk(this, max_locals, m->max_stack(), f);

  // process locals & expression stack
  InterpreterOopMap mask;
  if (query_oop_map_cache) {
    m->mask_for(bci, &mask);
  } else {
    OopMapCache::compute_one_oop_map(m, bci, &mask);
  }
  mask.iterate_oop(&blk);
}


void frame::oops_interpreted_arguments_do(Symbol* signature, bool has_receiver, OopClosure* f) {
  InterpretedArgumentOopFinder finder(signature, has_receiver, this, f);
  finder.oops_do();
}

void frame::oops_code_blob_do(OopClosure* f, CodeBlobClosure* cf, const RegisterMap* reg_map) {
  assert(_cb != NULL, "sanity check");
  if (_cb == SharedRuntime::ricochet_blob()) {
    oops_ricochet_do(f, reg_map);
  }
  if (_cb->oop_maps() != NULL) {
    OopMapSet::oops_do(this, reg_map, f);

    // Preserve potential arguments for a callee. We handle this by dispatching
    // on the codeblob. For c2i, we do
    if (reg_map->include_argument_oops()) {
      _cb->preserve_callee_argument_oops(*this, reg_map, f);
    }
  }
  // In cases where perm gen is collected, GC will want to mark
  // oops referenced from nmethods active on thread stacks so as to
  // prevent them from being collected. However, this visit should be
  // restricted to certain phases of the collection only. The
  // closure decides how it wants nmethods to be traced.
  if (cf != NULL)
    cf->do_code_blob(_cb);
}

void frame::oops_ricochet_do(OopClosure* f, const RegisterMap* map) {
  assert(is_ricochet_frame(), "");
  MethodHandles::ricochet_frame_oops_do(*this, f, map);
}

class CompiledArgumentOopFinder: public SignatureInfo {
 protected:
  OopClosure*     _f;
  int             _offset;        // the current offset, incremented with each argument
  bool            _has_receiver;  // true if the callee has a receiver
  frame           _fr;
  RegisterMap*    _reg_map;
  int             _arg_size;
  VMRegPair*      _regs;        // VMReg list of arguments

  void set(int size, BasicType type) {
    if (type == T_OBJECT || type == T_ARRAY) handle_oop_offset();
    _offset += size;
  }

  virtual void handle_oop_offset() {
    // Extract low order register number from register array.
    // In LP64-land, the high-order bits are valid but unhelpful.
    VMReg reg = _regs[_offset].first();
    oop *loc = _fr.oopmapreg_to_location(reg, _reg_map);
    _f->do_oop(loc);
  }

 public:
  CompiledArgumentOopFinder(Symbol* signature, bool has_receiver, OopClosure* f, frame fr,  const RegisterMap* reg_map)
    : SignatureInfo(signature) {

    // initialize CompiledArgumentOopFinder
    _f         = f;
    _offset    = 0;
    _has_receiver = has_receiver;
    _fr        = fr;
    _reg_map   = (RegisterMap*)reg_map;
    _arg_size  = ArgumentSizeComputer(signature).size() + (has_receiver ? 1 : 0);

    int arg_size;
    _regs = SharedRuntime::find_callee_arguments(signature, has_receiver, &arg_size);
    assert(arg_size == _arg_size, "wrong arg size");
  }

  void oops_do() {
    if (_has_receiver) {
      handle_oop_offset();
      _offset++;
    }
    iterate_parameters();
  }
};

void frame::oops_compiled_arguments_do(Symbol* signature, bool has_receiver, const RegisterMap* reg_map, OopClosure* f) {
  ResourceMark rm;
  CompiledArgumentOopFinder finder(signature, has_receiver, f, *this, reg_map);
  finder.oops_do();
}


// Get receiver out of callers frame, i.e. find parameter 0 in callers
// frame.  Consult ADLC for where parameter 0 is to be found.  Then
// check local reg_map for it being a callee-save register or argument
// register, both of which are saved in the local frame.  If not found
// there, it must be an in-stack argument of the caller.
// Note: caller.sp() points to callee-arguments
oop frame::retrieve_receiver(RegisterMap* reg_map) {
  frame caller = *this;

  // First consult the ADLC on where it puts parameter 0 for this signature.
  VMReg reg = SharedRuntime::name_for_receiver();
  oop r = *caller.oopmapreg_to_location(reg, reg_map);
  assert( Universe::heap()->is_in_or_null(r), "bad receiver" );
  return r;
}


oop* frame::oopmapreg_to_location(VMReg reg, const RegisterMap* reg_map) const {
  if(reg->is_reg()) {
    // If it is passed in a register, it got spilled in the stub frame.
    return (oop *)reg_map->location(reg);
  } else {
    int sp_offset_in_bytes = reg->reg2stack() * VMRegImpl::stack_slot_size;
    return (oop*)(((address)unextended_sp()) + sp_offset_in_bytes);
  }
}

BasicLock* frame::get_native_monitor() {
  nmethod* nm = (nmethod*)_cb;
  assert(_cb != NULL && _cb->is_nmethod() && nm->method()->is_native(),
         "Should not call this unless it's a native nmethod");
  int byte_offset = in_bytes(nm->native_basic_lock_sp_offset());
  assert(byte_offset >= 0, "should not see invalid offset");
  return (BasicLock*) &sp()[byte_offset / wordSize];
}

oop frame::get_native_receiver() {
  nmethod* nm = (nmethod*)_cb;
  assert(_cb != NULL && _cb->is_nmethod() && nm->method()->is_native(),
         "Should not call this unless it's a native nmethod");
  int byte_offset = in_bytes(nm->native_receiver_sp_offset());
  assert(byte_offset >= 0, "should not see invalid offset");
  oop owner = ((oop*) sp())[byte_offset / wordSize];
  assert( Universe::heap()->is_in(owner), "bad receiver" );
  return owner;
}

void frame::oops_entry_do(OopClosure* f, const RegisterMap* map) {
  assert(map != NULL, "map must be set");
  if (map->include_argument_oops()) {
    // must collect argument oops, as nobody else is doing it
    Thread *thread = Thread::current();
    methodHandle m (thread, entry_frame_call_wrapper()->callee_method());
    EntryFrameOopFinder finder(this, m->signature(), m->is_static());
    finder.arguments_do(f);
  }
  // Traverse the Handle Block saved in the entry frame
  entry_frame_call_wrapper()->oops_do(f);
}


void frame::oops_do_internal(OopClosure* f, CodeBlobClosure* cf, RegisterMap* map, bool use_interpreter_oop_map_cache) {
#ifndef PRODUCT
  // simulate GC crash here to dump java thread in error report
  if (CrashGCForDumpingJavaThread) {
    char *t = NULL;
    *t = 'c';
  }
#endif
  if (is_interpreted_frame()) {
    oops_interpreted_do(f, map, use_interpreter_oop_map_cache);
  } else if (is_entry_frame()) {
    oops_entry_do(f, map);
  } else if (CodeCache::contains(pc())) {
    oops_code_blob_do(f, cf, map);
#ifdef SHARK
  } else if (is_fake_stub_frame()) {
    // nothing to do
#endif // SHARK
  } else {
    ShouldNotReachHere();
  }
}

void frame::nmethods_do(CodeBlobClosure* cf) {
  if (_cb != NULL && _cb->is_nmethod()) {
    cf->do_code_blob(_cb);
  }
}


void frame::gc_prologue() {
  if (is_interpreted_frame()) {
    // set bcx to bci to become methodOop position independent during GC
    interpreter_frame_set_bcx(interpreter_frame_bci());
  }
}


void frame::gc_epilogue() {
  if (is_interpreted_frame()) {
    // set bcx back to bcp for interpreter
    interpreter_frame_set_bcx((intptr_t)interpreter_frame_bcp());
  }
  // call processor specific epilog function
  pd_gc_epilog();
}


# ifdef ENABLE_ZAP_DEAD_LOCALS

void frame::CheckValueClosure::do_oop(oop* p) {
  if (CheckOopishValues && Universe::heap()->is_in_reserved(*p)) {
    warning("value @ " INTPTR_FORMAT " looks oopish (" INTPTR_FORMAT ") (thread = " INTPTR_FORMAT ")", p, (address)*p, Thread::current());
  }
}
frame::CheckValueClosure frame::_check_value;


void frame::CheckOopClosure::do_oop(oop* p) {
  if (*p != NULL && !(*p)->is_oop()) {
    warning("value @ " INTPTR_FORMAT " should be an oop (" INTPTR_FORMAT ") (thread = " INTPTR_FORMAT ")", p, (address)*p, Thread::current());
 }
}
frame::CheckOopClosure frame::_check_oop;

void frame::check_derived_oop(oop* base, oop* derived) {
  _check_oop.do_oop(base);
}


void frame::ZapDeadClosure::do_oop(oop* p) {
  if (TraceZapDeadLocals) tty->print_cr("zapping @ " INTPTR_FORMAT " containing " INTPTR_FORMAT, p, (address)*p);
  // Need cast because on _LP64 the conversion to oop is ambiguous.  Constant
  // can be either long or int.
  *p = (oop)(int)0xbabebabe;
}
frame::ZapDeadClosure frame::_zap_dead;

void frame::zap_dead_locals(JavaThread* thread, const RegisterMap* map) {
  assert(thread == Thread::current(), "need to synchronize to do this to another thread");
  // Tracing - part 1
  if (TraceZapDeadLocals) {
    ResourceMark rm(thread);
    tty->print_cr("--------------------------------------------------------------------------------");
    tty->print("Zapping dead locals in ");
    print_on(tty);
    tty->cr();
  }
  // Zapping
       if (is_entry_frame      ()) zap_dead_entry_locals      (thread, map);
  else if (is_interpreted_frame()) zap_dead_interpreted_locals(thread, map);
  else if (is_compiled_frame()) zap_dead_compiled_locals   (thread, map);

  else
    // could be is_runtime_frame
    // so remove error: ShouldNotReachHere();
    ;
  // Tracing - part 2
  if (TraceZapDeadLocals) {
    tty->cr();
  }
}


void frame::zap_dead_interpreted_locals(JavaThread *thread, const RegisterMap* map) {
  // get current interpreter 'pc'
  assert(is_interpreted_frame(), "Not an interpreted frame");
  methodOop m   = interpreter_frame_method();
  int       bci = interpreter_frame_bci();

  int max_locals = m->is_native() ? m->size_of_parameters() : m->max_locals();

  // process dynamic part
  InterpreterFrameClosure value_blk(this, max_locals, m->max_stack(),
                                    &_check_value);
  InterpreterFrameClosure   oop_blk(this, max_locals, m->max_stack(),
                                    &_check_oop  );
  InterpreterFrameClosure  dead_blk(this, max_locals, m->max_stack(),
                                    &_zap_dead   );

  // get frame map
  InterpreterOopMap mask;
  m->mask_for(bci, &mask);
  mask.iterate_all( &oop_blk, &value_blk, &dead_blk);
}


void frame::zap_dead_compiled_locals(JavaThread* thread, const RegisterMap* reg_map) {

  ResourceMark rm(thread);
  assert(_cb != NULL, "sanity check");
  if (_cb->oop_maps() != NULL) {
    OopMapSet::all_do(this, reg_map, &_check_oop, check_derived_oop, &_check_value);
  }
}


void frame::zap_dead_entry_locals(JavaThread*, const RegisterMap*) {
  if (TraceZapDeadLocals) warning("frame::zap_dead_entry_locals unimplemented");
}


void frame::zap_dead_deoptimized_locals(JavaThread*, const RegisterMap*) {
  if (TraceZapDeadLocals) warning("frame::zap_dead_deoptimized_locals unimplemented");
}

# endif // ENABLE_ZAP_DEAD_LOCALS

void frame::verify(const RegisterMap* map) {
  // for now make sure receiver type is correct
  if (is_interpreted_frame()) {
    methodOop method = interpreter_frame_method();
    guarantee(method->is_method(), "method is wrong in frame::verify");
    if (!method->is_static()) {
      // fetch the receiver
      oop* p = (oop*) interpreter_frame_local_at(0);
      // make sure we have the right receiver type
    }
  }
  COMPILER2_PRESENT(assert(DerivedPointerTable::is_empty(), "must be empty before verify");)
  oops_do_internal(&VerifyOopClosure::verify_oop, NULL, (RegisterMap*)map, false);
}


#ifdef ASSERT
bool frame::verify_return_pc(address x) {
  if (StubRoutines::returns_to_call_stub(x)) {
    return true;
  }
  if (CodeCache::contains(x)) {
    return true;
  }
  if (Interpreter::contains(x)) {
    return true;
  }
  return false;
}
#endif


#ifdef ASSERT
void frame::interpreter_frame_verify_monitor(BasicObjectLock* value) const {
  assert(is_interpreted_frame(), "Not an interpreted frame");
  // verify that the value is in the right part of the frame
  address low_mark  = (address) interpreter_frame_monitor_end();
  address high_mark = (address) interpreter_frame_monitor_begin();
  address current   = (address) value;

  const int monitor_size = frame::interpreter_frame_monitor_size();
  guarantee((high_mark - current) % monitor_size  ==  0         , "Misaligned top of BasicObjectLock*");
  guarantee( high_mark > current                                , "Current BasicObjectLock* higher than high_mark");

  guarantee((current - low_mark) % monitor_size  ==  0         , "Misaligned bottom of BasicObjectLock*");
  guarantee( current >= low_mark                               , "Current BasicObjectLock* below than low_mark");
}


void frame::describe(FrameValues& values, int frame_no) {
  if (is_entry_frame() || is_compiled_frame() || is_interpreted_frame() || is_native_frame()) {
    // Label values common to most frames
    values.describe(-1, unextended_sp(), err_msg("unextended_sp for #%d", frame_no));
    values.describe(-1, sp(), err_msg("sp for #%d", frame_no));
    values.describe(-1, fp(), err_msg("fp for #%d", frame_no));
  }
  if (is_interpreted_frame()) {
    methodOop m = interpreter_frame_method();
    int bci = interpreter_frame_bci();

    // Label the method and current bci
    values.describe(-1, MAX2(sp(), fp()),
                    FormatBuffer<1024>("#%d method %s @ %d", frame_no, m->name_and_sig_as_C_string(), bci), 2);
    values.describe(-1, MAX2(sp(), fp()),
                    err_msg("- %d locals %d max stack", m->max_locals(), m->max_stack()), 1);
    if (m->max_locals() > 0) {
      intptr_t* l0 = interpreter_frame_local_at(0);
      intptr_t* ln = interpreter_frame_local_at(m->max_locals() - 1);
      values.describe(-1, MAX2(l0, ln), err_msg("locals for #%d", frame_no), 1);
      // Report each local and mark as owned by this frame
      for (int l = 0; l < m->max_locals(); l++) {
        intptr_t* l0 = interpreter_frame_local_at(l);
        values.describe(frame_no, l0, err_msg("local %d", l));
      }
    }

    // Compute the actual expression stack size
    InterpreterOopMap mask;
    OopMapCache::compute_one_oop_map(m, bci, &mask);
    intptr_t* tos = NULL;
    // Report each stack element and mark as owned by this frame
    for (int e = 0; e < mask.expression_stack_size(); e++) {
      tos = MAX2(tos, interpreter_frame_expression_stack_at(e));
      values.describe(frame_no, interpreter_frame_expression_stack_at(e),
                      err_msg("stack %d", e));
    }
    if (tos != NULL) {
      values.describe(-1, tos, err_msg("expression stack for #%d", frame_no), 1);
    }
    if (interpreter_frame_monitor_begin() != interpreter_frame_monitor_end()) {
      values.describe(frame_no, (intptr_t*)interpreter_frame_monitor_begin(), "monitors begin");
      values.describe(frame_no, (intptr_t*)interpreter_frame_monitor_end(), "monitors end");
    }
  } else if (is_entry_frame()) {
    // For now just label the frame
    values.describe(-1, MAX2(sp(), fp()), err_msg("#%d entry frame", frame_no), 2);
  } else if (is_compiled_frame()) {
    // For now just label the frame
    nmethod* nm = cb()->as_nmethod_or_null();
    values.describe(-1, MAX2(sp(), fp()),
                    FormatBuffer<1024>("#%d nmethod " INTPTR_FORMAT " for method %s%s", frame_no,
                                       nm, nm->method()->name_and_sig_as_C_string(),
                                       is_deoptimized_frame() ? " (deoptimized" : ""), 2);
  } else if (is_native_frame()) {
    // For now just label the frame
    nmethod* nm = cb()->as_nmethod_or_null();
    values.describe(-1, MAX2(sp(), fp()),
                    FormatBuffer<1024>("#%d nmethod " INTPTR_FORMAT " for native method %s", frame_no,
                                       nm, nm->method()->name_and_sig_as_C_string()), 2);
  }
  describe_pd(values, frame_no);
}

#endif


//-----------------------------------------------------------------------------------
// StackFrameStream implementation

StackFrameStream::StackFrameStream(JavaThread *thread, bool update) : _reg_map(thread, update) {
  assert(thread->has_last_Java_frame(), "sanity check");
  _fr = thread->last_frame();
  _is_done = false;
}


#ifdef ASSERT

void FrameValues::describe(int owner, intptr_t* location, const char* description, int priority) {
  FrameValue fv;
  fv.location = location;
  fv.owner = owner;
  fv.priority = priority;
  fv.description = NEW_RESOURCE_ARRAY(char, strlen(description) + 1);
  strcpy(fv.description, description);
  _values.append(fv);
}


void FrameValues::validate() {
  _values.sort(compare);
  bool error = false;
  FrameValue prev;
  prev.owner = -1;
  for (int i = _values.length() - 1; i >= 0; i--) {
    FrameValue fv = _values.at(i);
    if (fv.owner == -1) continue;
    if (prev.owner == -1) {
      prev = fv;
      continue;
    }
    if (prev.location == fv.location) {
      if (fv.owner != prev.owner) {
        tty->print_cr("overlapping storage");
        tty->print_cr(" " INTPTR_FORMAT ": " INTPTR_FORMAT " %s", prev.location, *prev.location, prev.description);
        tty->print_cr(" " INTPTR_FORMAT ": " INTPTR_FORMAT " %s", fv.location, *fv.location, fv.description);
        error = true;
      }
    } else {
      prev = fv;
    }
  }
  assert(!error, "invalid layout");
}


void FrameValues::print() {
  _values.sort(compare);
  JavaThread* thread = JavaThread::current();

  // Sometimes values like the fp can be invalid values if the
  // register map wasn't updated during the walk.  Trim out values
  // that aren't actually in the stack of the thread.
  int min_index = 0;
  int max_index = _values.length() - 1;
  intptr_t* v0 = _values.at(min_index).location;
  while (!thread->is_in_stack((address)v0)) {
    v0 = _values.at(++min_index).location;
  }
  intptr_t* v1 = _values.at(max_index).location;
  while (!thread->is_in_stack((address)v1)) {
    v1 = _values.at(--max_index).location;
  }
  intptr_t* min = MIN2(v0, v1);
  intptr_t* max = MAX2(v0, v1);
  intptr_t* cur = max;
  intptr_t* last = NULL;
  for (int i = max_index; i >= min_index; i--) {
    FrameValue fv = _values.at(i);
    while (cur > fv.location) {
      tty->print_cr(" " INTPTR_FORMAT ": " INTPTR_FORMAT, cur, *cur);
      cur--;
    }
    if (last == fv.location) {
      const char* spacer = "          " LP64_ONLY("        ");
      tty->print_cr(" %s  %s %s", spacer, spacer, fv.description);
    } else {
      tty->print_cr(" " INTPTR_FORMAT ": " INTPTR_FORMAT " %s", fv.location, *fv.location, fv.description);
      last = fv.location;
      cur--;
    }
  }
}

#endif

#include "precompiled.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/resourceArea.hpp"
#include "oops/markOop.hpp"
#include "oops/methodOop.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/monitorChunk.hpp"
#include "runtime/signature.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "vmreg_x86.inline.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#include "runtime/vframeArray.hpp"
#endif

#ifdef ASSERT
void RegisterMap::check_location_valid() {
}
#endif


// Profiling/safepoint support

bool frame::safe_for_sender(JavaThread *thread) {
  address   sp = (address)_sp;
  address   fp = (address)_fp;
  address   unextended_sp = (address)_unextended_sp;
  // sp must be within the stack
  bool sp_safe = (sp <= thread->stack_base()) &&
                 (sp >= thread->stack_base() - thread->stack_size());

  if (!sp_safe) {
    return false;
  }

  // unextended sp must be within the stack and above or equal sp
  bool unextended_sp_safe = (unextended_sp <= thread->stack_base()) &&
                            (unextended_sp >= sp);

  if (!unextended_sp_safe) {
    return false;
  }

  // an fp must be within the stack and above (but not equal) sp
  bool fp_safe = (fp <= thread->stack_base()) && (fp > sp);

  // We know sp/unextended_sp are safe only fp is questionable here

  // If the current frame is known to the code cache then we can attempt to
  // to construct the sender and do some validation of it. This goes a long way
  // toward eliminating issues when we get in frame construction code

  if (_cb != NULL ) {

    // First check if frame is complete and tester is reliable
    // Unfortunately we can only check frame complete for runtime stubs and nmethod
    // other generic buffer blobs are more problematic so we just assume they are
    // ok. adapter blobs never have a frame complete and are never ok.

    if (!_cb->is_frame_complete_at(_pc)) {
      if (_cb->is_nmethod() || _cb->is_adapter_blob() || _cb->is_runtime_stub()) {
        return false;
      }
    }
    // Entry frame checks
    if (is_entry_frame()) {
      // an entry frame must have a valid fp.

      if (!fp_safe) return false;

      // Validate the JavaCallWrapper an entry frame must have

      address jcw = (address)entry_frame_call_wrapper();

      bool jcw_safe = (jcw <= thread->stack_base()) && ( jcw > fp);

      return jcw_safe;

    }

    intptr_t* sender_sp = NULL;
    address   sender_pc = NULL;

    if (is_interpreted_frame()) {
      // fp must be safe
      if (!fp_safe) {
        return false;
      }

      sender_pc = (address) this->fp()[return_addr_offset];
      sender_sp = (intptr_t*) addr_at(sender_sp_offset);

    } else {
      // must be some sort of compiled/runtime frame
      // fp does not have to be safe (although it could be check for c1?)

      sender_sp = _unextended_sp + _cb->frame_size();
      // On Intel the return_address is always the word on the stack
      sender_pc = (address) *(sender_sp-1);
    }

    // We must always be able to find a recognizable pc
    CodeBlob* sender_blob = CodeCache::find_blob_unsafe(sender_pc);
    if (sender_pc == NULL ||  sender_blob == NULL) {
      return false;
    }


    // If the potential sender is the interpreter then we can do some more checking
    if (Interpreter::contains(sender_pc)) {

      // ebp is always saved in a recognizable place in any code we generate. However
      // only if the sender is interpreted/call_stub (c1 too?) are we certain that the saved ebp
      // is really a frame pointer.

      intptr_t *saved_fp = (intptr_t*)*(sender_sp - frame::sender_sp_offset);
      bool saved_fp_safe = ((address)saved_fp <= thread->stack_base()) && (saved_fp > sender_sp);

      if (!saved_fp_safe) {
        return false;
      }

      // construct the potential sender

      frame sender(sender_sp, saved_fp, sender_pc);

      return sender.is_interpreted_frame_valid(thread);

    }

    // Could just be some random pointer within the codeBlob
    if (!sender_blob->code_contains(sender_pc)) {
      return false;
    }

    // We should never be able to see an adapter if the current frame is something from code cache
    if (sender_blob->is_adapter_blob()) {
      return false;
    }

    // Could be the call_stub

    if (StubRoutines::returns_to_call_stub(sender_pc)) {
      intptr_t *saved_fp = (intptr_t*)*(sender_sp - frame::sender_sp_offset);
      bool saved_fp_safe = ((address)saved_fp <= thread->stack_base()) && (saved_fp > sender_sp);

      if (!saved_fp_safe) {
        return false;
      }

      // construct the potential sender

      frame sender(sender_sp, saved_fp, sender_pc);

      // Validate the JavaCallWrapper an entry frame must have
      address jcw = (address)sender.entry_frame_call_wrapper();

      bool jcw_safe = (jcw <= thread->stack_base()) && ( jcw > (address)sender.fp());

      return jcw_safe;
    }

    // If the frame size is 0 something is bad because every nmethod has a non-zero frame size
    // because the return address counts against the callee's frame.

    if (sender_blob->frame_size() == 0) {
      assert(!sender_blob->is_nmethod(), "should count return address at least");
      return false;
    }

    // We should never be able to see anything here except an nmethod. If something in the
    // code cache (current frame) is called by an entity within the code cache that entity
    // should not be anything but the call stub (already covered), the interpreter (already covered)
    // or an nmethod.

    assert(sender_blob->is_nmethod(), "Impossible call chain");

    // Could put some more validation for the potential non-interpreted sender
    // frame we'd create by calling sender if I could think of any. Wait for next crash in forte...

    // One idea is seeing if the sender_pc we have is one that we'd expect to call to current cb

    // We've validated the potential sender that would be created
    return true;
  }

  // Must be native-compiled frame. Since sender will try and use fp to find
  // linkages it must be safe

  if (!fp_safe) {
    return false;
  }

  // Will the pc we fetch be non-zero (which we'll find at the oldest frame)

  if ( (address) this->fp()[return_addr_offset] == NULL) return false;


  // could try and do some more potential verification of native frame if we could think of some...

  return true;

}


void frame::patch_pc(Thread* thread, address pc) {
  if (TracePcPatching) {
    tty->print_cr("patch_pc at address" INTPTR_FORMAT " [" INTPTR_FORMAT " -> " INTPTR_FORMAT "] ",
                  &((address *)sp())[-1], ((address *)sp())[-1], pc);
  }
  ((address *)sp())[-1] = pc;
  _cb = CodeCache::find_blob(pc);
  address original_pc = nmethod::get_deopt_original_pc(this);
  if (original_pc != NULL) {
    assert(original_pc == _pc, "expected original PC to be stored before patching");
    _deopt_state = is_deoptimized;
    // leave _pc as is
  } else {
    _deopt_state = not_deoptimized;
    _pc = pc;
  }
}

bool frame::is_interpreted_frame() const  {
  return Interpreter::contains(pc());
}

int frame::frame_size(RegisterMap* map) const {
  frame sender = this->sender(map);
  return sender.sp() - sp();
}

intptr_t* frame::entry_frame_argument_at(int offset) const {
  // convert offset to index to deal with tsi
  int index = (Interpreter::expr_offset_in_bytes(offset)/wordSize);
  // Entry frame's arguments are always in relation to unextended_sp()
  return &unextended_sp()[index];
}

// sender_sp
#ifdef CC_INTERP
intptr_t* frame::interpreter_frame_sender_sp() const {
  assert(is_interpreted_frame(), "interpreted frame expected");
  // QQQ why does this specialize method exist if frame::sender_sp() does same thing?
  // seems odd and if we always know interpreted vs. non then sender_sp() is really
  // doing too much work.
  return get_interpreterState()->sender_sp();
}

// monitor elements

BasicObjectLock* frame::interpreter_frame_monitor_begin() const {
  return get_interpreterState()->monitor_base();
}

BasicObjectLock* frame::interpreter_frame_monitor_end() const {
  return (BasicObjectLock*) get_interpreterState()->stack_base();
}

#else // CC_INTERP

intptr_t* frame::interpreter_frame_sender_sp() const {
  assert(is_interpreted_frame(), "interpreted frame expected");
  return (intptr_t*) at(interpreter_frame_sender_sp_offset);
}

void frame::set_interpreter_frame_sender_sp(intptr_t* sender_sp) {
  assert(is_interpreted_frame(), "interpreted frame expected");
  ptr_at_put(interpreter_frame_sender_sp_offset, (intptr_t) sender_sp);
}


// monitor elements

BasicObjectLock* frame::interpreter_frame_monitor_begin() const {
  return (BasicObjectLock*) addr_at(interpreter_frame_monitor_block_bottom_offset);
}

BasicObjectLock* frame::interpreter_frame_monitor_end() const {
  BasicObjectLock* result = (BasicObjectLock*) *addr_at(interpreter_frame_monitor_block_top_offset);
  // make sure the pointer points inside the frame
  assert(sp() <= (intptr_t*) result, "monitor end should be above the stack pointer");
  assert((intptr_t*) result < fp(),  "monitor end should be strictly below the frame pointer");
  return result;
}

void frame::interpreter_frame_set_monitor_end(BasicObjectLock* value) {
  *((BasicObjectLock**)addr_at(interpreter_frame_monitor_block_top_offset)) = value;
}

// Used by template based interpreter deoptimization
void frame::interpreter_frame_set_last_sp(intptr_t* sp) {
    *((intptr_t**)addr_at(interpreter_frame_last_sp_offset)) = sp;
}
#endif // CC_INTERP

frame frame::sender_for_entry_frame(RegisterMap* map) const {
  assert(map != NULL, "map must be set");
  // Java frame called from C; skip all C frames and return top C
  // frame of that chunk as the sender
  JavaFrameAnchor* jfa = entry_frame_call_wrapper()->anchor();
  assert(!entry_frame_is_first(), "next Java fp must be non zero");
  assert(jfa->last_Java_sp() > sp(), "must be above this frame on stack");
  map->clear();
  assert(map->include_argument_oops(), "should be set by clear");
  if (jfa->last_Java_pc() != NULL ) {
    frame fr(jfa->last_Java_sp(), jfa->last_Java_fp(), jfa->last_Java_pc());
    return fr;
  }
  frame fr(jfa->last_Java_sp(), jfa->last_Java_fp());
  return fr;
}

//------------------------------------------------------------------------------
// frame::verify_deopt_original_pc
//
// Verifies the calculated original PC of a deoptimization PC for the
// given unextended SP.  The unextended SP might also be the saved SP
// for MethodHandle call sites.
#if ASSERT
void frame::verify_deopt_original_pc(nmethod* nm, intptr_t* unextended_sp, bool is_method_handle_return) {
  frame fr;

  // This is ugly but it's better than to change {get,set}_original_pc
  // to take an SP value as argument.  And it's only a debugging
  // method anyway.
  fr._unextended_sp = unextended_sp;

  address original_pc = nm->get_original_pc(&fr);
  assert(nm->insts_contains(original_pc), "original PC must be in nmethod");
  assert(nm->is_method_handle_return(original_pc) == is_method_handle_return, "must be");
}
#endif

//------------------------------------------------------------------------------
// frame::adjust_unextended_sp
void frame::adjust_unextended_sp() {
  // If we are returning to a compiled MethodHandle call site, the
  // saved_fp will in fact be a saved value of the unextended SP.  The
  // simplest way to tell whether we are returning to such a call site
  // is as follows:

  nmethod* sender_nm = (_cb == NULL) ? NULL : _cb->as_nmethod_or_null();
  if (sender_nm != NULL) {
    // If the sender PC is a deoptimization point, get the original
    // PC.  For MethodHandle call site the unextended_sp is stored in
    // saved_fp.
    if (sender_nm->is_deopt_mh_entry(_pc)) {
      DEBUG_ONLY(verify_deopt_mh_original_pc(sender_nm, _fp));
      _unextended_sp = _fp;
    }
    else if (sender_nm->is_deopt_entry(_pc)) {
      DEBUG_ONLY(verify_deopt_original_pc(sender_nm, _unextended_sp));
    }
    else if (sender_nm->is_method_handle_return(_pc)) {
      _unextended_sp = _fp;
    }
  }
}

//------------------------------------------------------------------------------
// frame::update_map_with_saved_link
void frame::update_map_with_saved_link(RegisterMap* map, intptr_t** link_addr) {
  // The interpreter and compiler(s) always save EBP/RBP in a known
  // location on entry. We must record where that location is
  // so this if EBP/RBP was live on callout from c2 we can find
  // the saved copy no matter what it called.

  // Since the interpreter always saves EBP/RBP if we record where it is then
  // we don't have to always save EBP/RBP on entry and exit to c2 compiled
  // code, on entry will be enough.
  map->set_location(rbp->as_VMReg(), (address) link_addr);
#ifdef AMD64
  // this is weird "H" ought to be at a higher address however the
  // oopMaps seems to have the "H" regs at the same address and the
  // vanilla register.
  // XXXX make this go away
  if (true) {
    map->set_location(rbp->as_VMReg()->next(), (address) link_addr);
  }
#endif // AMD64
}


//------------------------------------------------------------------------------
// frame::sender_for_interpreter_frame
frame frame::sender_for_interpreter_frame(RegisterMap* map) const {
  // SP is the raw SP from the sender after adapter or interpreter
  // extension.
  intptr_t* sender_sp = this->sender_sp();

  // This is the sp before any possible extension (adapter/locals).
  intptr_t* unextended_sp = interpreter_frame_sender_sp();

#ifdef COMPILER2
  if (map->update_map()) {
    update_map_with_saved_link(map, (intptr_t**) addr_at(link_offset));
  }
#endif // COMPILER2

  return frame(sender_sp, unextended_sp, link(), sender_pc());
}


//------------------------------------------------------------------------------
// frame::sender_for_compiled_frame
frame frame::sender_for_compiled_frame(RegisterMap* map) const {
  assert(map != NULL, "map must be set");
  assert(!is_ricochet_frame(), "caller must handle this");

  // frame owned by optimizing compiler
  assert(_cb->frame_size() >= 0, "must have non-zero frame size");
  intptr_t* sender_sp = unextended_sp() + _cb->frame_size();
  intptr_t* unextended_sp = sender_sp;

  // On Intel the return_address is always the word on the stack
  address sender_pc = (address) *(sender_sp-1);

  // This is the saved value of EBP which may or may not really be an FP.
  // It is only an FP if the sender is an interpreter frame (or C1?).
  intptr_t** saved_fp_addr = (intptr_t**) (sender_sp - frame::sender_sp_offset);

  if (map->update_map()) {
    // Tell GC to use argument oopmaps for some runtime stubs that need it.
    // For C1, the runtime stub might not have oop maps, so set this flag
    // outside of update_register_map.
    map->set_include_argument_oops(_cb->caller_must_gc_arguments(map->thread()));
    if (_cb->oop_maps() != NULL) {
      OopMapSet::update_register_map(this, map);
    }

    // Since the prolog does the save and restore of EBP there is no oopmap
    // for it so we must fill in its location as if there was an oopmap entry
    // since if our caller was compiled code there could be live jvm state in it.
    update_map_with_saved_link(map, saved_fp_addr);
  }

  assert(sender_sp != sp(), "must have changed");
  return frame(sender_sp, unextended_sp, *saved_fp_addr, sender_pc);
}


//------------------------------------------------------------------------------
// frame::sender
frame frame::sender(RegisterMap* map) const {
  // Default is we done have to follow them. The sender_for_xxx will
  // update it accordingly
  map->set_include_argument_oops(false);

  if (is_entry_frame())       return sender_for_entry_frame(map);
  if (is_interpreted_frame()) return sender_for_interpreter_frame(map);
  assert(_cb == CodeCache::find_blob(pc()),"Must be the same");
  if (is_ricochet_frame())    return sender_for_ricochet_frame(map);

  if (_cb != NULL) {
    return sender_for_compiled_frame(map);
  }
  // Must be native-compiled frame, i.e. the marshaling code for native
  // methods that exists in the core system.
  return frame(sender_sp(), link(), sender_pc());
}


bool frame::interpreter_frame_equals_unpacked_fp(intptr_t* fp) {
  assert(is_interpreted_frame(), "must be interpreter frame");
  methodOop method = interpreter_frame_method();
  // When unpacking an optimized frame the frame pointer is
  // adjusted with:
  int diff = (method->max_locals() - method->size_of_parameters()) *
             Interpreter::stackElementWords;
  return _fp == (fp - diff);
}

void frame::pd_gc_epilog() {
  // nothing done here now
}

bool frame::is_interpreted_frame_valid(JavaThread* thread) const {
// QQQ
#ifdef CC_INTERP
#else
  assert(is_interpreted_frame(), "Not an interpreted frame");
  // These are reasonable sanity checks
  if (fp() == 0 || (intptr_t(fp()) & (wordSize-1)) != 0) {
    return false;
  }
  if (sp() == 0 || (intptr_t(sp()) & (wordSize-1)) != 0) {
    return false;
  }
  if (fp() + interpreter_frame_initial_sp_offset < sp()) {
    return false;
  }
  // These are hacks to keep us out of trouble.
  // The problem with these is that they mask other problems
  if (fp() <= sp()) {        // this attempts to deal with unsigned comparison above
    return false;
  }

  // do some validation of frame elements

  // first the method

  methodOop m = *interpreter_frame_method_addr();

  // validate the method we'd find in this potential sender
  if (!Universe::heap()->is_valid_method(m)) return false;

  // stack frames shouldn't be much larger than max_stack elements

  if (fp() - sp() > 1024 + m->max_stack()*Interpreter::stackElementSize) {
    return false;
  }

  // validate bci/bcx

  intptr_t  bcx    = interpreter_frame_bcx();
  if (m->validate_bci_from_bcx(bcx) < 0) {
    return false;
  }

  // validate constantPoolCacheOop

  constantPoolCacheOop cp = *interpreter_frame_cache_addr();

  if (cp == NULL ||
      !Space::is_aligned(cp) ||
      !Universe::heap()->is_permanent((void*)cp)) return false;

  // validate locals

  address locals =  (address) *interpreter_frame_locals_addr();

  if (locals > thread->stack_base() || locals < (address) fp()) return false;

  // We'd have to be pretty unlucky to be mislead at this point

#endif // CC_INTERP
  return true;
}

BasicType frame::interpreter_frame_result(oop* oop_result, jvalue* value_result) {
#ifdef CC_INTERP
  // Needed for JVMTI. The result should always be in the
  // interpreterState object
  interpreterState istate = get_interpreterState();
#endif // CC_INTERP
  assert(is_interpreted_frame(), "interpreted frame expected");
  methodOop method = interpreter_frame_method();
  BasicType type = method->result_type();

  intptr_t* tos_addr;
  if (method->is_native()) {
    // Prior to calling into the runtime to report the method_exit the possible
    // return value is pushed to the native stack. If the result is a jfloat/jdouble
    // then ST0 is saved before EAX/EDX. See the note in generate_native_result
    tos_addr = (intptr_t*)sp();
    if (type == T_FLOAT || type == T_DOUBLE) {
    // QQQ seems like this code is equivalent on the two platforms
#ifdef AMD64
      // This is times two because we do a push(ltos) after pushing XMM0
      // and that takes two interpreter stack slots.
      tos_addr += 2 * Interpreter::stackElementWords;
#else
      tos_addr += 2;
#endif // AMD64
    }
  } else {
    tos_addr = (intptr_t*)interpreter_frame_tos_address();
  }

  switch (type) {
    case T_OBJECT  :
    case T_ARRAY   : {
      oop obj;
      if (method->is_native()) {
#ifdef CC_INTERP
        obj = istate->_oop_temp;
#else
        obj = (oop) at(interpreter_frame_oop_temp_offset);
#endif // CC_INTERP
      } else {
        oop* obj_p = (oop*)tos_addr;
        obj = (obj_p == NULL) ? (oop)NULL : *obj_p;
      }
      assert(obj == NULL || Universe::heap()->is_in(obj), "sanity check");
      *oop_result = obj;
      break;
    }
    case T_BOOLEAN : value_result->z = *(jboolean*)tos_addr; break;
    case T_BYTE    : value_result->b = *(jbyte*)tos_addr; break;
    case T_CHAR    : value_result->c = *(jchar*)tos_addr; break;
    case T_SHORT   : value_result->s = *(jshort*)tos_addr; break;
    case T_INT     : value_result->i = *(jint*)tos_addr; break;
    case T_LONG    : value_result->j = *(jlong*)tos_addr; break;
    case T_FLOAT   : {
#ifdef AMD64
        value_result->f = *(jfloat*)tos_addr;
#else
      if (method->is_native()) {
        jdouble d = *(jdouble*)tos_addr;  // Result was in ST0 so need to convert to jfloat
        value_result->f = (jfloat)d;
      } else {
        value_result->f = *(jfloat*)tos_addr;
      }
#endif // AMD64
      break;
    }
    case T_DOUBLE  : value_result->d = *(jdouble*)tos_addr; break;
    case T_VOID    : /* Nothing to do */ break;
    default        : ShouldNotReachHere();
  }

  return type;
}


intptr_t* frame::interpreter_frame_tos_at(jint offset) const {
  int index = (Interpreter::expr_offset_in_bytes(offset)/wordSize);
  return &interpreter_frame_tos_address()[index];
}

#ifdef ASSERT

#define DESCRIBE_FP_OFFSET(name) \
  values.describe(frame_no, fp() + frame::name##_offset, #name)

void frame::describe_pd(FrameValues& values, int frame_no) {
  if (is_interpreted_frame()) {
    DESCRIBE_FP_OFFSET(interpreter_frame_sender_sp);
    DESCRIBE_FP_OFFSET(interpreter_frame_last_sp);
    DESCRIBE_FP_OFFSET(interpreter_frame_method);
    DESCRIBE_FP_OFFSET(interpreter_frame_mdx);
    DESCRIBE_FP_OFFSET(interpreter_frame_cache);
    DESCRIBE_FP_OFFSET(interpreter_frame_locals);
    DESCRIBE_FP_OFFSET(interpreter_frame_bcx);
    DESCRIBE_FP_OFFSET(interpreter_frame_initial_sp);
  }

}
#endif
