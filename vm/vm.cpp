#include "vm.hpp"
#include "state.hpp"
#include "object_memory.hpp"
#include "global_cache.hpp"
#include "environment.hpp"
#include "gc/gc.hpp"

#include "object_utils.hpp"

#include "builtin/array.hpp"
#include "builtin/class.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/array.hpp"
#include "builtin/list.hpp"
#include "builtin/lookup_table.hpp"
#include "builtin/symbol.hpp"
#include "builtin/thread.hpp"
#include "builtin/tuple.hpp"
#include "builtin/string.hpp"
#include "builtin/system.hpp"
#include "builtin/fiber.hpp"
#include "builtin/location.hpp"
#include "builtin/native_method.hpp"
#include "builtin/channel.hpp"
#include "builtin/call_site.hpp"
#include "builtin/exception.hpp"
#include "builtin/jit.hpp"

#include "instruments/tooling.hpp"
#include "instruments/rbxti-internal.hpp"
#include "instruments/timing.hpp"

#include "config_parser.hpp"
#include "config.h"

#include "call_frame.hpp"
#include "signal.hpp"
#include "configuration.hpp"
#include "helpers.hpp"

#include "util/thread.hpp"

#include "park.hpp"

#include <iostream>
#include <iomanip>
#include <signal.h>
#ifndef RBX_WINDOWS
#include <sys/resource.h>
#endif

// Reset macros since we're inside state
#undef G
#undef GO
#define G(whatever) globals().whatever.get()
#define GO(whatever) globals().whatever

namespace rubinius {

  unsigned long VM::cStackDepthMax = 655300;

#ifndef RBX_WINDOWS
  /**
   * Maximum amount of stack space to use.
   * getrlimit can report there is 4G of stack (ie, unlimited).  Even when
   * there is unlimited stack, we clamp the max to this value (currently 128M).
   */
  static rlim_t cMaxStack = (1024 * 1024 * 128);
#endif

  VM::VM(uint32_t id, SharedState& shared, const char* name)
    : ManagedThread(id, shared, ManagedThread::eRuby, name)
    , thread_nexus_(shared.thread_nexus())
    , saved_call_frame_(0)
    , saved_call_site_information_(0)
    , fiber_stacks_(this, shared)
    , park_(new Park)
    , tooling_env_(NULL)
    , interrupt_lock_()
    , method_missing_reason_(eNone)
    , constant_missing_reason_(vFound)
    , zombie_(false)
    , main_thread_(false)
    , thread_phase_(ThreadNexus::Phase::cManaged)
    , shared(shared)
    , waiting_channel_(this, nil<Channel>())
    , interrupted_exception_(this, nil<Exception>())
    , thread(this, nil<Thread>())
    , current_fiber(this, nil<Fiber>())
    , root_fiber(this, nil<Fiber>())
    , waiting_object_(this, cNil)
    , custom_wakeup_(0)
    , custom_wakeup_data_(0)
    , om(shared.om)
    , thread_state_(this)
  {
    if(shared.om) {
      local_slab_.refill(0, 0);
    }

    tooling_env_ = rbxti::create_env(this);
    tooling_ = false;

    allocation_tracking_ = shared.config.allocation_tracking;
  }

  VM::~VM() {
    rbxti::destroy_env(tooling_env_);
    delete park_;
  }

  void VM::discard(STATE, VM* vm) {
    vm->saved_call_frame_ = 0;

    state->vm()->metrics().system.threads_destroyed++;

    delete vm;
  }

  void VM::checkpoint(STATE) {
    if(thread_nexus_->stop_p()) {
      if(thread_nexus_->lock_or_yield(this)) {
        om->collect_maybe(state);
        thread_nexus_->unlock();
      }
    }
  }

  void VM::become_managed() {
    thread_nexus_->become_managed(this);
  }

  void VM::set_zombie(STATE) {
    state->shared().thread_nexus()->delete_vm(this);
    thread.set(nil<Thread>());
    zombie_ = true;
  }

  void VM::initialize_config() {
    State state(this);

#ifdef ENABLE_LLVM
    Array* ary = Array::create(&state, 3);

    G(jit)->available(&state, cTrue);
    G(jit)->properties(&state, ary);

    if(!shared.config.jit_disabled) {
      ary->append(&state, state.symbol("usage"));
      if(shared.config.jit_inline_generic) {
        ary->append(&state, state.symbol("inline_generic"));
      }

      if(shared.config.jit_inline_blocks) {
        ary->append(&state, state.symbol("inline_blocks"));
      }
      G(jit)->enabled(&state, cTrue);
    } else {
      G(jit)->enabled(&state, cFalse);
    }
#else
    G(jit)->available(&state, cFalse);
    G(jit)->properties(&state, nil<Array>());
    G(jit)->enabled(&state, cFalse);
#endif
  }

  /**
   * Returns the current VM executing on this pthread.
   */
  VM* VM::current() {
    return ManagedThread::current()->as_vm();
  }

  /**
   * Sets this VM instance as the current VM on this pthread.
   */
  void VM::set_current_thread() {
    ManagedThread::set_current_thread(this);
  }

  Object* VM::new_object_typed_dirty(Class* cls, size_t size, object_type type) {
    State state(this);

    if(unlikely(size > om->large_object_threshold)) {
      return om->new_object_typed_enduring_dirty(&state, cls, size, type);
    }

    Object* obj = local_slab().allocate(size).as<Object>();

    if(unlikely(!obj)) {
      if(shared.om->refill_slab(&state, local_slab())) {
        obj = local_slab().allocate(size).as<Object>();
      }

      // If refill_slab fails, obj will still be NULL.

      if(!obj) {
        return om->new_object_typed_dirty(&state, cls, size, type);
      }
    }

    obj->init_header(cls, YoungObjectZone, type);
#ifdef RBX_GC_STRESS
    state.shared().gc_soon();
#endif
    return obj;
  }

  Object* VM::new_object_typed(Class* cls, size_t size, object_type type) {
    Object* obj = new_object_typed_dirty(cls, size, type);
    if(obj) obj->clear_fields(size);
    return obj;
  }

  String* VM::new_young_string_dirty(STATE) {
    String* str = local_slab().allocate(sizeof(String)).as<String>();

    if(unlikely(!str)) {

      if(shared.om->refill_slab(state, local_slab())) {
        str = local_slab().allocate(sizeof(String)).as<String>();
      }

      if(!str) return 0;
    }

    str->init_header(G(string), YoungObjectZone, String::type);
#ifdef RBX_GC_STRESS
    state->shared().gc_soon();
#endif
    return str;
  }

  Tuple* VM::new_young_tuple_dirty(size_t fields) {
    State state(this);
    size_t bytes = Tuple::fields_offset + (sizeof(Object*) * fields);

    if(unlikely(bytes > om->large_object_threshold)) {
      return 0;
    }

    Tuple* tup = local_slab().allocate(bytes).as<Tuple>();

    if(unlikely(!tup)) {

      if(shared.om->refill_slab(&state, local_slab())) {
        tup = local_slab().allocate(bytes).as<Tuple>();
      }

      if(!tup) return 0;
    }

    tup->init_header(G(tuple), YoungObjectZone, Tuple::type);
    tup->full_size_ = bytes;
#ifdef RBX_GC_STRESS
    state.shared().gc_soon();
#endif
    return tup;
  }

  Object* VM::new_object_typed_mature(Class* cls, size_t bytes, object_type type) {
    State state(this);
#ifdef RBX_GC_STRESS
    state.shared().gc_soon();
#endif
    return om->new_object_typed_mature(&state, cls, bytes, type);
  }

  void type_assert(STATE, Object* obj, object_type type, const char* reason) {
    if((obj->reference_p() && obj->type_id() != type)
        || (type == FixnumType && !obj->fixnum_p())) {
      std::ostringstream msg;
      msg << reason << ": " << obj->to_string(state, true);
      Exception::type_error(state, type, obj, msg.str().c_str());
    }
  }

  void VM::init_stack_size() {
#ifndef RBX_WINDOWS
    struct rlimit rlim;
    if(getrlimit(RLIMIT_STACK, &rlim) == 0) {
      rlim_t space = rlim.rlim_cur/5;

      if(space > 1024*1024) space = 1024*1024;
      rlim_t adjusted = (rlim.rlim_cur - space);

      if(adjusted > cMaxStack) {
        cStackDepthMax = cMaxStack;
      } else {
        cStackDepthMax = adjusted;
      }
    }
#endif
  }

  TypeInfo* VM::find_type(int type) {
    return om->type_info[type];
  }

  void VM::run_gc_soon() {
    om->collect_young_now = true;
    om->collect_mature_now = true;
    shared.gc_soon();
    thread_nexus_->set_stop();
  }

  void VM::after_fork_child(STATE) {
    thread_nexus_->after_fork_child(state);

    interrupt_lock_.init();
    set_main_thread();
    become_managed();

    // TODO: Remove need for root_vm.
    state->shared().env()->set_root_vm(state->vm());
  }

  void VM::set_const(const char* name, Object* val) {
    State state(this);
    globals().object->set_const(&state, (char*)name, val);
  }

  void VM::set_const(Module* mod, const char* name, Object* val) {
    State state(this);
    mod->set_const(&state, (char*)name, val);
  }

  Object* VM::path2class(const char* path) {
    State state(this);
    Module* mod = shared.globals.object.get();

    char* copy = strdup(path);
    char* cur = copy;

    for(;;) {
      char* pos = strstr(cur, "::");
      if(pos) *pos = 0;

      Object* obj = mod->get_const(&state, state.symbol(cur));

      if(pos) {
        if(Module* m = try_as<Module>(obj)) {
          mod = m;
        } else {
          free(copy);
          return cNil;
        }
      } else {
        free(copy);
        return obj;
      }

      cur = pos + 2;
    }
  }

  void VM::print_backtrace() {
    abort();
  }

  void VM::interrupt_with_signal() {
    vm_jit_.interrupt_with_signal_ = true;
  }

  bool VM::wakeup(STATE, CallFrame* call_frame) {
    utilities::thread::SpinLock::LockGuard guard(interrupt_lock_);

    set_check_local_interrupts();
    Object* wait = waiting_object_.get();

    if(park_->parked_p()) {
      park_->unpark();
      return true;
    } else if(vm_jit_.interrupt_with_signal_) {
#ifdef RBX_WINDOWS
      // TODO: wake up the thread
#else
      pthread_kill(os_thread_, SIGVTALRM);
#endif
      interrupt_lock_.unlock();
      // Wakeup any locks hanging around with contention
      om->release_contention(state, call_frame);
      return true;
    } else if(!wait->nil_p()) {
      // We shouldn't hold the VM lock and the IH lock at the same time,
      // other threads can grab them and deadlock.
      InflatedHeader* ih = wait->inflated_header(state);
      interrupt_lock_.unlock();
      ih->wakeup(state, call_frame, wait);
      return true;
    } else {
      Channel* chan = waiting_channel_.get();

      if(!chan->nil_p()) {
        interrupt_lock_.unlock();
        om->release_contention(state, call_frame);
        chan->send(state, cNil, call_frame);
        return true;
      } else if(custom_wakeup_) {
        interrupt_lock_.unlock();
        om->release_contention(state, call_frame);
        (*custom_wakeup_)(custom_wakeup_data_);
        return true;
      }

      return false;
    }
  }

  void VM::clear_waiter() {
    utilities::thread::SpinLock::LockGuard guard(shared.wait_lock());

    vm_jit_.interrupt_with_signal_ = false;
    waiting_channel_.set(nil<Channel>());
    waiting_object_.set(cNil);
    custom_wakeup_ = 0;
    custom_wakeup_data_ = 0;
  }

  void VM::wait_on_channel(Channel* chan) {
    utilities::thread::SpinLock::LockGuard guard(interrupt_lock_);

    thread->sleep(this, cTrue);
    waiting_channel_.set(chan);
  }

  void VM::wait_on_inflated_lock(Object* wait) {
    utilities::thread::SpinLock::LockGuard guard(shared.wait_lock());

    waiting_object_.set(wait);
  }

  void VM::wait_on_custom_function(void (*func)(void*), void* data) {
    utilities::thread::SpinLock::LockGuard guard(shared.wait_lock());

    custom_wakeup_ = func;
    custom_wakeup_data_ = data;
  }

  void VM::set_sleeping() {
    thread->sleep(this, cTrue);
  }

  void VM::clear_sleeping() {
    thread->sleep(this, cFalse);
  }

  void VM::reset_parked() {
    park_->reset_parked();
  }

  void VM::register_raise(STATE, Exception* exc) {
    utilities::thread::SpinLock::LockGuard guard(interrupt_lock_);
    interrupted_exception_.set(exc);
    set_check_local_interrupts();
  }

  void VM::register_kill(STATE) {
    utilities::thread::SpinLock::LockGuard guard(interrupt_lock_);
    set_interrupt_by_kill();
    set_check_local_interrupts();
  }

  void VM::set_current_fiber(Fiber* fib) {
    set_stack_bounds((uintptr_t)fib->stack_start(), fib->stack_size());
    current_fiber.set(fib);
  }

  VariableRootBuffers& VM::current_root_buffers() {
    if(current_fiber->nil_p() || current_fiber->root_p()) {
      return variable_root_buffers();
    }

    return current_fiber->variable_root_buffers();
  }

  void VM::gc_scan(GarbageCollector* gc) {
    if(CallFrame* cf = saved_call_frame()) {
      gc->walk_call_frame(cf);
    }

    if(CallSiteInformation* info = saved_call_site_information()) {
      info->executable = as<Executable>(gc->mark_object(info->executable));
    }

    State ls(this);

    shared.tool_broker()->at_gc(&ls);
  }

  void VM::gc_fiber_clear_mark() {
    fiber_stacks_.gc_clear_mark();
  }

  void VM::gc_fiber_scan(GarbageCollector* gc, bool only_marked) {
    fiber_stacks_.gc_scan(gc, only_marked);
  }

  void VM::gc_verify(GarbageCollector* gc) {
    if(CallFrame* cf = saved_call_frame()) {
      gc->verify_call_frame(cf);
    }

    if(CallSiteInformation* info = saved_call_site_information()) {
      info->executable->validate();
    }
  }
};
