// Copyright © 2021-2024 GothicKit Contributors.
// SPDX-License-Identifier: MIT
// Copy of ZenKit 1.3.0 with project modifications.
// MIT license: OpenGothic/lib/ZenKit/license.md (../../lib/ZenKit/license.md).

#include "vm.h"
#include "vm_stackguard.h"


#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace daedalus {
	DaedalusVm::DaedalusVm(DaedalusScript&& scr, std::uint8_t flags, BytecodeExecutor execute_bytecode)
	    : DaedalusScript(std::move(scr)), _m_execute_bytecode(execute_bytecode), _m_flags(flags) {
		_m_temporary_strings = add_temporary_strings_symbol();
		_m_temporary_strings_free.push(0);

		_m_self_sym = find_symbol_by_name("SELF");
		_m_other_sym = find_symbol_by_name("OTHER");
		_m_victim_sym = find_symbol_by_name("VICTIM");
		_m_hero_sym = find_symbol_by_name("HERO");
		_m_item_sym = find_symbol_by_name("ITEM");
	}

	DaedalusVm DaedalusVm::without_legacy_dispatcher(DaedalusScript&& scr, std::uint8_t flags) {
		return DaedalusVm(std::move(scr), flags, nullptr);
	}

	std::shared_ptr<DaedalusInstance> DaedalusVm::init_opaque_instance(DaedalusSymbol* sym) {
		auto cls = sym;
		while (cls != nullptr && cls->type() != DaedalusDataType::CLASS) {
			cls = find_symbol_by_index(cls->parent());
		}
		if (cls == nullptr) {
			// We're probably trying to initialize $INSTANCE_HELP which is not permitted
			throw DaedalusVmException {"Cannot init " + sym->name() +
			                           ": parent class not found (did you try to initialize $INSTANCE_HELP?)"};
		}

		// create the instance
		auto inst = std::make_shared<DaedalusOpaqueInstance>(*cls, find_class_members(*cls));
		init_instance(inst, sym);
		return inst;
	}

	uint64_t DaedalusVm::add_instruction_observer(InstructionObserver callback) {
		auto id = ++_m_next_instruction_observer;
		_m_instruction_observers.emplace_back(id, std::move(callback));
		return id;
	}

	void DaedalusVm::remove_instruction_observer(uint64_t id) {
		std::erase_if(_m_instruction_observers, [id](auto const& entry) { return entry.first == id; });
	}

	void DaedalusVm::require_native_execution(std::string_view program, bool metadataOnly) {
		if (metadataOnly) arm_metadata_audit(program);
		add_instruction_observer([program = std::string(program)](const auto& vm, const auto& instruction) {
			const auto* symbol = vm._m_call_stack.empty() ? nullptr : vm._m_call_stack.back().function;
			std::fprintf(stderr, "native-script-audit: bytecode execution attempted program=%s symbol=%s pc=%" PRIu32 " opcode=%u\n",
			             program.c_str(), symbol ? symbol->name().c_str() : "<unknown>", vm.pc(), unsigned(instruction.op));
			std::fflush(stderr);
			// Compatibility paths catch script errors; an audit violation must reach the runner.
			std::_Exit(EXIT_FAILURE);
		});
	}

	void DaedalusVm::observe_calls(CallObserver callback) {
		_m_call_observers.clear();
		if (callback) add_call_observer(std::move(callback));
	}

	uint64_t DaedalusVm::add_call_observer(CallObserver callback) {
		auto id = ++_m_next_call_observer;
		_m_call_observers.emplace_back(id, std::move(callback));
		return id;
	}

	void DaedalusVm::remove_call_observer(uint64_t id) {
		std::erase_if(_m_call_observers, [id](auto const& entry) { return entry.first == id; });
	}

	void DaedalusVm::notify_call(DaedalusSymbol const* symbol) const {
		for (auto const& observer : _m_call_observers) observer.second(*symbol);
	}

	void DaedalusVm::unsafe_call(DaedalusSymbol const* sym) {
		if (metadata_audit_active())
			abort_metadata_access(sym != nullptr ? sym->address() : 0u, sym != nullptr ? sym->name().c_str() : "<null>");
		if (_m_execute_bytecode == nullptr)
			throw DaedalusVmException {"Cannot execute bytecode without the legacy dispatcher: " + sym->name()};
		if (call_observer_count() != 0 && !sym->is_external()) notify_call(sym);
		push_call(sym);
		jump(sym->address());

		_m_execute_bytecode(*this);

		pop_call();
	}

	void DaedalusVm::unsafe_call_bl(DaedalusSymbol const* sym) {
		// the same as the BL instruction
		if (auto* cb = active_override(*sym)) {
			notify_call(sym);
			StackGuard guard {this, sym->rtype()};
			cb->call(*this);
			guard.inhibit();
			return;
		}
		unsafe_call(sym);
	}

	bool DaedalusVm::has_active_override(DaedalusSymbol const& sym) const {
		return active_override(sym) != nullptr;
	}

	DaedalusVm::FunctionOverride const* DaedalusVm::active_override(DaedalusSymbol const& sym) const {
		auto cb = _m_function_overrides.find(sym.address());
		if (cb != _m_function_overrides.end() && (!cb->second.enabled || cb->second.enabled()))
			return &cb->second;
		return nullptr;
	}

	bool DaedalusVm::unsafe_call_external(DaedalusSymbol const* external) {
		auto* sym = external == nullptr ? nullptr : find_symbol_by_index(external->index());
		if (sym == nullptr || !sym->is_external()) {
			return false;
		}

		// the same as the BE instruction
		if (_m_external_observer) _m_external_observer(*sym);
		StackGuard guard {this, sym->rtype()};
		auto cb = _m_externals.find(sym);
		if (cb == _m_externals.end()) {
			if (!_m_default_external.has_value()) {
				// drop the arguments, push nothing
				auto count = static_cast<uint32_t>(find_parameters_for_function(sym).size());
				_m_stack_ptr = _m_stack_ptr > count ? _m_stack_ptr - count : 0;
				guard.inhibit();
				return false;
			}
			(*_m_default_external)(*this, *sym);
			guard.inhibit();
			return true;
		}

		invoke_external(*sym, cb->second);
		guard.inhibit();
		return true;
	}

	void DaedalusVm::invoke_external(DaedalusSymbol& sym, std::function<void(DaedalusVm&)> const& call) {
		push_call(&sym);
		call(*this);
		pop_call();
	}

	bool DaedalusVm::has_bound_native(DaedalusSymbol& sym) const {
		return sym.is_external() ? _m_externals.contains(&sym) : has_active_override(sym);
	}

	bool DaedalusVm::call_bound_native(DaedalusSymbol& sym, std::function<void()> const& push,
	                                  std::function<void()> const& pop) {
		auto const external = _m_externals.find(&sym);
		auto const* override = sym.is_external() ? nullptr : active_override(sym);
		if (external == _m_externals.end() && override == nullptr)
			return false;
		if (_m_stack_ptr + sym.count() > stack_size)
			throw DaedalusVmException {"Stack overflow entering bound function " + sym.name()};

		auto const stack = _m_stack_ptr;
		auto const calls = _m_call_stack.size();
		auto const pc = _m_pc;
		auto const context = _m_instance;
		try {
			push();
			if (_m_stack_ptr != stack + sym.count())
				throw DaedalusVmException {"Incorrect argument count for bound function " + sym.name()};
			if (override != nullptr) {
				notify_call(&sym);
				override->call(*this);
			} else {
				if (_m_external_observer) _m_external_observer(sym);
				invoke_external(sym, external->second);
			}
			if (_m_call_stack.size() != calls || _m_stack_ptr != stack + unsigned(sym.has_return()))
				throw DaedalusVmException {"Unbalanced stack after bound function " + sym.name()};
			pop();
			if (_m_stack_ptr != stack)
				throw DaedalusVmException {"Unconsumed result of bound function " + sym.name()};
		} catch (...) {
			while (_m_stack_ptr > stack) {
				if (top_is_reference())
					(void)pop_reference();
				else
					_m_stack[--_m_stack_ptr] = {};
			}
			_m_stack_ptr = stack;
			_m_call_stack.resize(calls);
			_m_pc = pc;
			_m_instance = context;
			throw;
		}
		return true;
	}

	void DaedalusVm::observe_bound_external(DaedalusSymbol const& sym, std::function<void()> const& push) {
		if (!_m_external_observer)
			return;
		auto const start = _m_stack_ptr;
		if (start + sym.count() > stack_size)
			throw DaedalusVmException {"Stack overflow observing bound function " + sym.name()};
		auto const clear = [&] {
			while (_m_stack_ptr > start) {
				if (top_is_reference())
					(void)pop_reference();
				else
					_m_stack[--_m_stack_ptr] = {};
			}
		};
		try {
			push();
			_m_external_observer(sym);
		} catch (...) {
			clear();
			throw;
		}
		clear();
	}

	void DaedalusVm::unsafe_jump(uint32_t address) {
		this->jump(address);
	}

	std::shared_ptr<DaedalusInstance> DaedalusVm::unsafe_get_gi() {
		return _m_instance;
	}

	void DaedalusVm::unsafe_set_gi(std::shared_ptr<DaedalusInstance> i) {
		_m_instance = std::move(i);
	}

	void DaedalusVm::push_call(DaedalusSymbol const* sym) {
		if (sym->has_local_variables_enabled()) {
			push_local_variables(sym);
		}
		auto var_count = sym->count();
		_m_call_stack.push_back({sym, _m_pc, _m_stack_ptr - static_cast<uint32_t>(var_count), _m_instance});
	}

	void DaedalusVm::pop_call() {
		auto const& call = _m_call_stack.back();

		// First, try to fix up the stack.
		if (!call.function->has_return()) {
			// No special logic needed, there are supposed to be no more stack frames for
			// this function, so just reset the stack for the caller.
			_m_stack_ptr = call.stack_ptr;
		} else {
			auto remaining_locals = _m_stack_ptr - call.stack_ptr;
			if (remaining_locals == 0) {
				// The function is supposed to have a return value, but it does not seem to have one.
				// To fix this, we just insert a default value (either 0, "" or NULL).
				StackGuard::fix(this, call.function->rtype());
			} else if (remaining_locals > 1) {
				// Now we have too many items left on the stack. Remove all of them, except the topmost one,
				// since that one is supposed to be the return value of the function.
				DaedalusStackFrame frame = _m_stack[--_m_stack_ptr];
				_m_stack_ptr = call.stack_ptr;
				_m_stack[_m_stack_ptr++] = std::move(frame);
			}
			// else {
			//     We have exactly one value to be returned (as indicated by the symbol's return type).
			//     That means, that the compiler did not mess up the stack management, so we can just
			//     return that value.
			// }
		}

		if (call.function->has_local_variables_enabled()) {
			pop_local_variables(call.function);
		}

		// Second, reset PC and context, then remove the call stack frame
		_m_pc = call.program_counter;
		_m_instance = call.context;
		_m_call_stack.pop_back();
	}

	void DaedalusVm::push_local_variables(DaedalusSymbol const* sym) {
		bool has_recursion = false;
		for (auto& i : _m_call_stack) {
			if (i.function == sym) {
				has_recursion = true;
				break;
			}
		}

		if (!has_recursion) {
			return;
		}

		auto params = this->find_parameters_for_function(sym);
		auto locals = this->find_locals_for_function(sym);

		// estimate stack storage for local copy of variables
		std::uint32_t locals_size = 0;
		for (auto& l : locals) {
			switch (l.type()) {
			case DaedalusDataType::VOID:
				break;
			case DaedalusDataType::FLOAT:
			case DaedalusDataType::FUNCTION:
			case DaedalusDataType::INT:
			case DaedalusDataType::STRING:
				locals_size += l.count();
				break;
			case DaedalusDataType::INSTANCE:
				locals_size += 1;
				break;
			case DaedalusDataType::CLASS:
			case DaedalusDataType::PROTOTYPE:
				break;
			}
		}

		if (_m_stack_ptr + locals_size > stack_size) {
			throw DaedalusVmException {"stack overflow"};
		}

		if (_m_stack_ptr < params.size()) {
			throw DaedalusVmException {"stack underoverflow"};
		}

		// Move function arguments further back, since we're currently at the call instruction and the
		// arguments for the next call have already been pushed.
		_m_stack_ptr -= params.size();
		for (size_t i = 0; i < params.size(); ++i) {
			_m_stack[_m_stack_ptr + locals_size + i] = std::move(_m_stack[_m_stack_ptr + i]);
		}

		for (auto& l : locals) {
			switch (l.type()) {
			case DaedalusDataType::VOID:
				break;
			case DaedalusDataType::FLOAT:
				for (std::uint32_t i = 0; i < l.count(); ++i) {
					push_float(l.get_float(i));
				}
				break;
			case DaedalusDataType::FUNCTION:
			case DaedalusDataType::INT:
				for (std::uint32_t i = 0; i < l.count(); ++i) {
					push_int(l.get_int(i));
				}
				break;
			case DaedalusDataType::STRING:
				for (std::uint32_t i = 0; i < l.count(); ++i) {
					push_string(l.get_string(i));
				}
				break;
			case DaedalusDataType::INSTANCE:
				push_instance(l.get_instance());
				break;
			case DaedalusDataType::CLASS:
			case DaedalusDataType::PROTOTYPE:
				log(LogLevel::WARNING, "DaedalusVm", "Attempted to save class or prototype in stack frame");
				break;
			}
		}

		_m_stack_ptr += params.size();
	}

	void DaedalusVm::pop_local_variables(DaedalusSymbol const* sym) {
		int has_recursion = 0;
		for (auto& i : _m_call_stack) {
			if (i.function == sym) {
				++has_recursion;
			}
		}

		if (has_recursion <= 1) {
			return;
		}

		DaedalusStackFrame ret;
		if (sym->has_return()) {
			ret = std::move(_m_stack[--_m_stack_ptr]);
		}

		auto locals = this->find_locals_for_function(sym);
		for (size_t i = locals.size(); i > 0;) {
			--i;
			auto& l = locals[i];
			switch (l.type()) {
			case DaedalusDataType::VOID:
				break;
			case DaedalusDataType::FLOAT:
				for (std::uint32_t r = l.count(); r > 0;) {
					--r;
					l.set_float(pop_float(), r);
				}
				break;
			case DaedalusDataType::FUNCTION:
			case DaedalusDataType::INT:
				for (std::uint32_t r = l.count(); r > 0;) {
					--r;
					l.set_int(pop_int(), r);
				}
				break;
			case DaedalusDataType::STRING:
				for (std::uint32_t r = l.count(); r > 0;) {
					--r;
					l.set_string(pop_string(), r);
				}
				break;
			case DaedalusDataType::INSTANCE:
				l.set_instance(pop_instance());
				break;
			case DaedalusDataType::CLASS:
			case DaedalusDataType::PROTOTYPE:
				log(LogLevel::WARNING, "DaedalusVm", "Attempted to restore class or prototype in stack frame");
				break;
			}
		}

		if (sym->has_return()) {
			_m_stack[_m_stack_ptr++] = std::move(ret);
		}
	}

	void DaedalusVm::push_int(std::int32_t value) {
		if (_m_stack_ptr == stack_size) {
			throw DaedalusVmException {"stack overflow"};
		}

		_m_stack[_m_stack_ptr++] = {nullptr, false, value};
	}

	void DaedalusVm::push_reference(DaedalusSymbol* value, std::uint8_t index) {
		if (_m_stack_ptr == stack_size) {
			throw DaedalusVmException {"stack overflow"};
		}

		_m_stack[_m_stack_ptr++] = {_m_instance, true, value, index};
	}

	void DaedalusVm::push_string(std::string_view value) {
		// We need to push strings without an explicitly attached variable to a
		// virtual string container variable.
		auto index = this->get_free_string();

		_m_temporary_strings->set_string(value, index);
		push_reference(_m_temporary_strings, index);
	}

	void DaedalusVm::push_float(float value) {
		if (_m_stack_ptr == stack_size) {
			throw DaedalusVmException {"stack overflow"};
		}

		_m_stack[_m_stack_ptr++] = {nullptr, false, value};
	}

	void DaedalusVm::push_instance(std::shared_ptr<DaedalusInstance> value) {
		if (_m_stack_ptr == stack_size) {
			throw DaedalusVmException {"stack overflow"};
		}

		_m_stack[_m_stack_ptr++] = {nullptr, false, value};
	}

	std::int32_t DaedalusVm::pop_int() {
		if (_m_stack_ptr == 0) {
			return 0;
		}

		DaedalusStackFrame v = std::move(_m_stack[--_m_stack_ptr]);

		if (v.reference) {
			return this->get_int(v.context, v.value, v.index);
		}

		if (std::holds_alternative<int32_t>(v.value)) {
			return std::get<int32_t>(v.value);
		}

		throw DaedalusVmException {"tried to pop_int but frame does not contain a int."};
	}

	float DaedalusVm::pop_float() {
		if (_m_stack_ptr == 0) {
			return 0.0f;
		}

		DaedalusStackFrame v = std::move(_m_stack[--_m_stack_ptr]);

		if (v.reference) {
			return this->get_float(v.context, v.value, v.index);
		}

		if (std::holds_alternative<float>(v.value)) {
			return std::get<float>(v.value);
		}

		if (std::holds_alternative<std::int32_t>(v.value)) {
			auto k = std::get<std::int32_t>(v.value);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
			return *reinterpret_cast<float*>(&k);
#pragma GCC diagnostic pop
		}

		throw DaedalusVmException {"tried to pop_float but frame does not contain a float."};
	}

	std::tuple<DaedalusSymbol*, std::uint8_t, std::shared_ptr<DaedalusInstance>> DaedalusVm::pop_reference() {
		if (_m_stack_ptr == 0) {
			throw DaedalusVmException {"popping reference from empty stack"};
		}

		DaedalusStackFrame v = std::move(_m_stack[--_m_stack_ptr]);

		if (!v.reference) {
			throw DaedalusVmException {"tried to pop_reference but frame does not contain a reference."};
		}

		auto sym = std::get<DaedalusSymbol*>(v.value);
		if (sym->index() == _m_temporary_strings->index()) {
			// For virtual strings, we'll mark this string as free when it's popped.
			// This is likely unreliable but until it causes issues, it'll stay as-is.
			this->mark_free_string(v.index);
		}

		return {std::get<DaedalusSymbol*>(v.value), v.index, v.context};
	}

	DaedalusStackFrame const& DaedalusVm::stack_frame(uint16_t depth) const {
		if (depth >= _m_stack_ptr) throw DaedalusVmException {"stack frame depth out of range"};
		return _m_stack[_m_stack_ptr - depth - 1];
	}

	bool DaedalusVm::top_is_reference() const {
		if (_m_stack_ptr == 0) {
			throw DaedalusVmException {"popping from empty stack"};
		}

		return _m_stack[_m_stack_ptr - 1].reference;
	}

	bool DaedalusVm::top_is_named_reference() const {
		return top_is_reference() && std::get<DaedalusSymbol*>(stack_frame(0).value) != _m_temporary_strings;
	}

	std::shared_ptr<DaedalusInstance> DaedalusVm::pop_instance() {
		if (_m_stack_ptr == 0) {
			throw DaedalusVmException {"popping instance from empty stack"};
		}

		DaedalusStackFrame v = std::move(_m_stack[--_m_stack_ptr]);

		if (v.reference) {
			auto* sym = std::get<DaedalusSymbol*>(v.value);
			if (_m_read_observer) _m_read_observer(*sym);
			return sym->get_instance();
		}

		if (std::holds_alternative<std::shared_ptr<DaedalusInstance>>(v.value)) {
			return std::get<std::shared_ptr<DaedalusInstance>>(v.value);
		}

		throw DaedalusVmException {"tried to pop_instance but frame does not contain am instance."};
	}

	std::string const& DaedalusVm::pop_string() {
		static std::string empty {};
		auto [s, i, context] = pop_reference();
		if (_m_read_observer) _m_read_observer(*s);
		if (_m_member_read_observer && s->is_member()) _m_member_read_observer(*s, i, context.get());

		// compatibility: sometimes the context might be zero, but we can't fail so when
		//                the compatibility flag is set, we just return 0
		if (s->is_member() && context == nullptr) {
			if (!(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
				throw DaedalusNoContextError {s};
			}

			log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", s->name().c_str());
			return empty;
		}

		return s->get_string(i, context.get());
	}

	void DaedalusVm::jump(std::uint32_t address) {
		if (address >= size()) {
			throw DaedalusVmException {"Cannot jump to " + std::to_string(address) + ": illegal address"};
		}

		_m_pc = address;
	}

	void DaedalusVm::register_default_external(std::function<void(std::string_view)> const& callback) {
		this->register_default_external([callback](DaedalusSymbol const& sym) { callback(sym.name()); });
	}

	void DaedalusVm::register_default_external(std::function<void(DaedalusSymbol const&)> const& callback) {
		_m_default_external = [this, callback](DaedalusVm& v, DaedalusSymbol const& sym) {
			// pop all parameters from the stack
			auto params = find_parameters_for_function(&sym);
			for (int i = static_cast<int>(params.size()) - 1; i >= 0; --i) {
				auto& par = params[static_cast<unsigned>(i)];
				if (par.type() == DaedalusDataType::INT)
					(void) v.pop_int();
				else if (par.type() == DaedalusDataType::FLOAT)
					(void) v.pop_float();
				else if (par.type() == DaedalusDataType::INSTANCE || par.type() == DaedalusDataType::STRING)
					(void) v.pop_reference();
			}

			if (sym.has_return()) {
				if (sym.rtype() == DaedalusDataType::FLOAT)
					v.push_float(0.0f);
				else if (sym.rtype() == DaedalusDataType::INT)
					v.push_int(0);
				else if (sym.rtype() == DaedalusDataType::STRING)
					v.push_string("");
				else if (sym.rtype() == DaedalusDataType::INSTANCE)
					v.push_instance(nullptr);
			}

			callback(sym);
		};
	}

	void
	DaedalusVm::register_default_external_custom(std::function<void(DaedalusVm&, DaedalusSymbol&)> const& callback) {
		_m_default_external = callback;
	}

	void DaedalusVm::register_access_trap(std::function<void(DaedalusSymbol&)> const& callback) {
		_m_access_trap = callback;
	}

	void DaedalusVm::register_exception_handler(
	    std::function<DaedalusVmExceptionStrategy(DaedalusVm&,
	                                              DaedalusScriptError const&,
	                                              DaedalusInstruction const&)> const& callback) {
		_m_exception_handler = callback;
	}

	void DaedalusVm::print_stack_trace() const {
		auto last_pc = _m_pc;
		auto tmp_stack_ptr = _m_stack_ptr;

		std::stack callstack {_m_call_stack};

		log(LogLevel::ERROR, "DaedalusVm", "------- CALL STACK (MOST RECENT CALL FIRST) -------");

		while (!callstack.empty()) {
			auto v = callstack.top();
			log(LogLevel::ERROR, "DaedalusVm", "in %s at %x", v.function->name().c_str(), last_pc);

			last_pc = v.program_counter;
			callstack.pop();
		}

		log(LogLevel::ERROR, "DaedalusVm", "------- STACK (MOST RECENT PUSH FIRST) -------");

		while (tmp_stack_ptr > 0) {
			auto& v = _m_stack[--tmp_stack_ptr];

			if (v.reference) {
				auto ref = std::get<DaedalusSymbol*>(v.value);
				std::string value;

				switch (ref->type()) {
				case DaedalusDataType::FLOAT:
					value = std::to_string(ref->get_float(v.index, _m_instance.get()));
					break;
				case DaedalusDataType::INT:
					value = std::to_string(ref->get_int(v.index, _m_instance.get()));
					break;
				case DaedalusDataType::STRING:
					value = "'" + ref->get_string(v.index, _m_instance.get()) + "'";
					break;
				case DaedalusDataType::FUNCTION: {
					auto index = ref->get_int(v.index, _m_instance.get());
					auto sym = find_symbol_by_index(static_cast<uint32_t>(index));
					value = "&" + sym->name();
					break;
				}
				case DaedalusDataType::INSTANCE: {
					if (auto& inst = ref->get_instance(); inst != nullptr) {
						value = "<instance of '" + std::string(inst->_m_type->name()) + "'>";
					} else {
						value = "NULL";
					}
					break;
				}
				default:
					value = "<invalid stack frame>";
				}

				log(LogLevel::ERROR, "DaedalusVm",
				       "%u: [REFERENCE] %s[%hu] = %s",
				       tmp_stack_ptr,
				       ref->name().c_str(),
				       v.index,
				       value.c_str());
			} else {
				if (std::holds_alternative<float>(v.value)) {
					log(LogLevel::ERROR, "DaedalusVm", "%d: [IMMEDIATE FLOAT] = %f", tmp_stack_ptr, std::get<float>(v.value));
				} else if (std::holds_alternative<int32_t>(v.value)) {
					log(LogLevel::ERROR, "DaedalusVm", "%d: [IMMEDIATE INT] = %d", tmp_stack_ptr, std::get<int32_t>(v.value));
				} else if (std::holds_alternative<std::shared_ptr<DaedalusInstance>>(v.value)) {
					if (auto& inst = std::get<std::shared_ptr<DaedalusInstance>>(v.value); inst == nullptr) {
						log(LogLevel::ERROR, "DaedalusVm", "%d: [IMMEDIATE INSTANCE] = NULL", tmp_stack_ptr);
					} else {
						log(LogLevel::ERROR, "DaedalusVm",
						       "%d: [IMMEDIATE INSTANCE] = <instance of '%s'>",
						       tmp_stack_ptr,
						       inst->_m_type->name());
					}
				}
			}
		}

		log(LogLevel::ERROR, "DaedalusVm", "------- GLOBAL VARIABLES -------");

		DaedalusSymbol* symbols[] = {
		    _m_hero_sym,
		    _m_self_sym,
		    _m_other_sym,
		    _m_victim_sym,
		    _m_item_sym,
		};

		for (auto* sym : symbols) {
			if (sym == nullptr) continue;

			auto instance = sym->get_instance();
			if (instance != nullptr && instance->symbol_index() != static_cast<uint32_t>(-1)) {
				DaedalusSymbol const* instance_symbol = find_symbol_by_instance(instance);
				log(LogLevel::ERROR, "DaedalusVm",
				       "%s = %s (%u)",
				       sym->name().c_str(),
				       instance_symbol->name().c_str(),
				       instance->symbol_index());
			} else if (instance == nullptr) {
				log(LogLevel::ERROR, "DaedalusVm", "%s = NULL", sym->name().c_str());
			} else {
				log(LogLevel::ERROR, "DaedalusVm", "%s = <TRANSIENT>", sym->name().c_str());
			}
		}

		if (_m_instance != nullptr && _m_instance->symbol_index() != static_cast<uint32_t>(-1)) {
			auto instance_symbol = find_symbol_by_instance(_m_instance);
			log(LogLevel::ERROR, "DaedalusVm", "<__THIS__> = %s (%u)", instance_symbol->name().c_str(), _m_instance->symbol_index());
		} else if (_m_instance == nullptr) {
			log(LogLevel::ERROR, "DaedalusVm", "<__THIS__> = NULL");
		} else {
			log(LogLevel::ERROR, "DaedalusVm", "<__THIS__> = <TRANSIENT>");
		}
	}

	DaedalusIllegalExternalDefinition::DaedalusIllegalExternalDefinition(DaedalusSymbol const* s, std::string&& msg)
	    : DaedalusScriptError(std::move(msg)), sym(s) {}

	DaedalusIllegalExternalReturnType::DaedalusIllegalExternalReturnType(DaedalusSymbol const* s,
	                                                                     std::string&& provided)
	    : DaedalusIllegalExternalDefinition(
	          s,
	          "external " + s->name() + " has illegal return type '" + provided + "', expected '" +
	              DAEDALUS_DATA_TYPE_NAMES[static_cast<std::uint32_t>(s->rtype())] + "'") {}

	DaedalusIllegalExternalParameter::DaedalusIllegalExternalParameter(DaedalusSymbol const* s,
	                                                                   std::string&& provided,
	                                                                   std::uint8_t i)
	    : DaedalusIllegalExternalDefinition(s,
	                                        "external " + s->name() + " has illegal parameter type '" + provided +
	                                            "' (no. " + std::to_string(i) + "), expected '" +
	                                            DAEDALUS_DATA_TYPE_NAMES[static_cast<std::uint32_t>(s->type())] + "'") {
	}

	std::int32_t
	DaedalusVm::get_int(std::shared_ptr<DaedalusInstance> const& context,
	                    std::variant<int32_t, float, DaedalusSymbol*, std::shared_ptr<DaedalusInstance>> const& value,
	                    uint16_t index) const {
		auto* sym = std::get<DaedalusSymbol*>(value);
		if (_m_read_observer) _m_read_observer(*sym);
		if (_m_member_read_observer && sym->is_member()) _m_member_read_observer(*sym, index, context.get());

		// compatibility: sometimes the context might be zero, but we can't fail so when
		//                the compatibility flag is set, we just return 0
		if (sym->is_member() && context == nullptr) {
			if (!(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
				throw DaedalusNoContextError {sym};
			}

			log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", sym->name().c_str());
			return 0;
		}

		return sym->get_int(index, context.get());
	}

	float
	DaedalusVm::get_float(std::shared_ptr<DaedalusInstance> const& context,
	                      std::variant<int32_t, float, DaedalusSymbol*, std::shared_ptr<DaedalusInstance>> const& value,
	                      uint16_t index) const {
		auto* sym = std::get<DaedalusSymbol*>(value);
		if (_m_read_observer) _m_read_observer(*sym);
		if (_m_member_read_observer && sym->is_member()) _m_member_read_observer(*sym, index, context.get());

		// compatibility: sometimes the context might be zero, but we can't fail so when
		//                the compatibility flag is set, we just return 0
		if (sym->is_member() && context == nullptr) {
			if (!(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
				throw DaedalusNoContextError {sym};
			}

			log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", sym->name().c_str());
			return 0;
		}

		return sym->get_float(index, context.get());
	}

	void DaedalusVm::set_int(std::shared_ptr<DaedalusInstance> const& context,
	                         DaedalusSymbol* ref,
	                         uint16_t index,
	                         std::int32_t value) {
		if (ref->is_const() && !(_m_flags & DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER)) {
			throw DaedalusIllegalConstAccess {ref};
		}

		if (!ref->is_member() || context != nullptr ||
		    !(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
			if (_m_write_observer) _m_write_observer(*ref);
			ref->set_int(value, index, context.get());
			if (_m_member_write_observer && ref->is_member()) _m_member_write_observer(*ref, index, context.get());
		} else if (ref->is_member()) {
			log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", ref->name().c_str());
		}
	}

	void DaedalusVm::set_float(std::shared_ptr<DaedalusInstance> const& context,
	                           DaedalusSymbol* ref,
	                           uint16_t index,
	                           float value) {
		if (ref->is_const() && !(_m_flags & DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER)) {
			throw DaedalusIllegalConstAccess {ref};
		}

		if (!ref->is_member() || context != nullptr ||
		    !(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
			if (_m_write_observer) _m_write_observer(*ref);
			ref->set_float(value, index, context.get());
			if (_m_member_write_observer && ref->is_member()) _m_member_write_observer(*ref, index, context.get());
		} else if (ref->is_member()) {
			log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", ref->name().c_str());
		}
	}

	void DaedalusVm::set_string(std::shared_ptr<DaedalusInstance> const& context,
	                            DaedalusSymbol* ref,
	                            uint16_t index,
	                            std::string_view value) {
		if (ref->is_const() && !(_m_flags & DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER)) {
			throw DaedalusIllegalConstAccess {ref};
		}

		if (!ref->is_member() || context != nullptr ||
		    !(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
			if (_m_write_observer) _m_write_observer(*ref);
			ref->set_string(value, index, context.get());
			if (_m_member_write_observer && ref->is_member()) _m_member_write_observer(*ref, index, context.get());
		} else if (ref->is_member()) {
			log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", ref->name().c_str());
		}
	}

	void DaedalusVm::mark_free_string(uint32_t index) {
		this->_m_temporary_strings_free.push(index);
	}

	uint32_t DaedalusVm::get_free_string() {
		if (_m_temporary_strings_free.size() == 0) {
			// Like a normal array, we'll grow exponentially.
			auto prev_size = _m_temporary_strings->count();
			_m_temporary_strings->grow(prev_size * 2);

			// Mark new strings as unused.
			for (uint32_t i = prev_size; i < _m_temporary_strings->count(); ++i) {
				_m_temporary_strings_free.push(i);
			}
		}

		auto index = this->_m_temporary_strings_free.top();
		this->_m_temporary_strings_free.pop();
		return index;
	}
} // namespace daedalus
