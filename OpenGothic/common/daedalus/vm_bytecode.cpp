// Copyright © 2021-2024 GothicKit Contributors.
// SPDX-License-Identifier: MIT
// Copy of ZenKit 1.3.0 with project modifications.
// MIT license: OpenGothic/lib/ZenKit/license.md (../../lib/ZenKit/license.md).

#include "vm.h"
#include "vm_stackguard.h"

#include <utility>

namespace daedalus {
	DaedalusVm::DaedalusVm(DaedalusScript&& scr, std::uint8_t flags)
	    : DaedalusVm(std::move(scr), flags, [](DaedalusVm& vm) {
		    while (vm.exec())
			    ;
	    }) {}

	bool DaedalusVm::exec() {
		auto instr = instruction_at(_m_pc);
		for (auto const& observer : _m_instruction_observers) observer.second(*this, instr);
		if (_m_instruction_observer) _m_instruction_observer(instr);

		try {
			std::int32_t a, b;
			DaedalusSymbol* sym;

			switch (instr.op) {
			case DaedalusOpcode::ADD:
				push_int(pop_int() + pop_int());
				break;
			case DaedalusOpcode::SUB:
				a = pop_int();
				b = pop_int();
				push_int(a - b);
				break;
			case DaedalusOpcode::MUL:
				push_int(pop_int() * pop_int());
				break;
			case DaedalusOpcode::DIV:
				a = pop_int();
				b = pop_int();

				if (b == 0) throw DaedalusVmException {"vm: division by zero"};

				push_int(a / b);
				break;
			case DaedalusOpcode::MOD:
				a = pop_int();
				b = pop_int();

				if (b == 0) throw DaedalusVmException {"vm: division by zero"};

				push_int(a % b);
				break;
			case DaedalusOpcode::OR:
				push_int(pop_int() | pop_int());
				break;
			case DaedalusOpcode::ANDB:
				push_int(pop_int() & pop_int());
				break;
			case DaedalusOpcode::LT:
				a = pop_int();
				b = pop_int();
				push_int(a < b);
				break;
			case DaedalusOpcode::GT:
				a = pop_int();
				b = pop_int();
				push_int(a > b);
				break;
			case DaedalusOpcode::LSL:
				a = pop_int();
				b = pop_int();
				push_int(a << b);
				break;
			case DaedalusOpcode::LSR:
				a = pop_int();
				b = pop_int();
				push_int(a >> b);
				break;
			case DaedalusOpcode::LTE:
				a = pop_int();
				b = pop_int();
				push_int(a <= b);
				break;
			case DaedalusOpcode::EQ:
				push_int(pop_int() == pop_int());
				break;
			case DaedalusOpcode::NEQ:
				push_int(pop_int() != pop_int());
				break;
			case DaedalusOpcode::GTE:
				a = pop_int();
				b = pop_int();
				push_int(a >= b);
				break;
			case DaedalusOpcode::PLUS:
				push_int(+pop_int());
				break;
			case DaedalusOpcode::NEGATE:
				push_int(-pop_int());
				break;
			case DaedalusOpcode::NOT:
				push_int(!pop_int());
				break;
			case DaedalusOpcode::CMPL:
				push_int(~pop_int());
				break;
			case DaedalusOpcode::ORR:
				a = pop_int();
				b = pop_int();
				push_int(a || b);
				break;
			case DaedalusOpcode::AND:
				a = pop_int();
				b = pop_int();
				push_int(a && b);
				break;
			case DaedalusOpcode::NOP:
				// Do nothing
				break;
			case DaedalusOpcode::RSR:
				return false;
			case DaedalusOpcode::BL: {
				sym = find_symbol_by_address(instr.address);
				if (sym == nullptr) {
					throw DaedalusVmException {"bl: no symbol found for address " + std::to_string(instr.address)};
				}
				unsafe_call_bl(sym);
				break;
			}
			case DaedalusOpcode::BE: {
				sym = find_symbol_by_index(instr.symbol);
				if (sym == nullptr) {
					throw DaedalusVmException {"be: no external found for index"};
				}

				// Guard against exceptions during external invocation.
				if (_m_external_observer) _m_external_observer(*sym);
				StackGuard guard {this, sym->rtype()};

				auto cb = _m_externals.find(sym);
				if (cb == _m_externals.end()) {
					if (_m_default_external.has_value()) {
						(*_m_default_external)(*this, *sym);
						guard.inhibit();
						break;
					}

					throw DaedalusVmException {"be: no external registered for " + sym->name()};
				}

				push_call(sym);
				cb->second(*this);
				pop_call();

				// The stack is left intact.
				guard.inhibit();
				break;
			}
			case DaedalusOpcode::PUSHI:
				push_int(instr.immediate);
				break;
			case DaedalusOpcode::PUSHVI:
			case DaedalusOpcode::PUSHV:
				sym = find_symbol_by_index(instr.symbol);
				if (sym == nullptr) {
					throw DaedalusVmException {"pushv: no symbol found for index"};
				}
				if (sym->has_access_trap() && _m_access_trap) {
					_m_access_trap(*sym);
				} else {
					push_reference(sym, 0);
				}
				break;
			case DaedalusOpcode::MOVI:
			case DaedalusOpcode::MOVVF: {
				auto [ref, idx, context] = pop_reference();
				auto value = pop_int();

				this->set_int(context, ref, idx, value);
				break;
			}
			case DaedalusOpcode::MOVF: {
				auto [ref, idx, context] = pop_reference();
				auto value = pop_float();

				this->set_float(context, ref, idx, value);
				break;
			}
			case DaedalusOpcode::MOVS: {
				auto [target, target_idx, context] = pop_reference();
				auto source = pop_string();

				this->set_string(context, target, target_idx, source);
				break;
			}
			case DaedalusOpcode::MOVSS:
				throw DaedalusVmException {"not implemented: movss"};
			case DaedalusOpcode::ADDMOVI: {
				auto [ref, idx, context] = pop_reference();
				auto value = pop_int();

				if (ref->is_const() && !(_m_flags & DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER)) {
					throw DaedalusIllegalConstAccess(ref);
				}

				if (!ref->is_member() || context != nullptr ||
				    !(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
					if (_m_read_observer) _m_read_observer(*ref);
					if (_m_member_read_observer && ref->is_member()) _m_member_read_observer(*ref, idx, context.get());
					auto result = ref->get_int(idx, context.get()) + value;
					if (_m_write_observer) _m_write_observer(*ref);
					ref->set_int(result, idx, context.get());
					if (_m_member_write_observer && ref->is_member()) _m_member_write_observer(*ref, idx, context.get());
				} else if (ref->is_member()) {
					log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", ref->name().c_str());
				}

				break;
			}
			case DaedalusOpcode::SUBMOVI: {
				auto [ref, idx, context] = pop_reference();
				auto value = pop_int();

				if (ref->is_const() && !(_m_flags & DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER)) {
					throw DaedalusIllegalConstAccess(ref);
				}

				if (!ref->is_member() || context != nullptr ||
				    !(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
					if (_m_read_observer) _m_read_observer(*ref);
					if (_m_member_read_observer && ref->is_member()) _m_member_read_observer(*ref, idx, context.get());
					auto result = ref->get_int(idx, context.get()) - value;
					if (_m_write_observer) _m_write_observer(*ref);
					ref->set_int(result, idx, context.get());
					if (_m_member_write_observer && ref->is_member()) _m_member_write_observer(*ref, idx, context.get());
				} else if (ref->is_member()) {
					log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", ref->name().c_str());
				}
				break;
			}
			case DaedalusOpcode::MULMOVI: {
				auto [ref, idx, context] = pop_reference();
				auto value = pop_int();

				if (ref->is_const() && !(_m_flags & DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER)) {
					throw DaedalusIllegalConstAccess(ref);
				}

				if (!ref->is_member() || context != nullptr ||
				    !(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
					if (_m_read_observer) _m_read_observer(*ref);
					if (_m_member_read_observer && ref->is_member()) _m_member_read_observer(*ref, idx, context.get());
					auto result = ref->get_int(idx, context.get()) * value;
					if (_m_write_observer) _m_write_observer(*ref);
					ref->set_int(result, idx, context.get());
					if (_m_member_write_observer && ref->is_member()) _m_member_write_observer(*ref, idx, context.get());
				} else if (ref->is_member()) {
					log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", ref->name().c_str());
				}

				break;
			}
			case DaedalusOpcode::DIVMOVI: {
				auto [ref, idx, context] = pop_reference();
				auto value = pop_int();

				if (value == 0) {
					throw DaedalusVmException {"vm: division by zero"};
				}

				if (ref->is_const() && !(_m_flags & DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER)) {
					throw DaedalusIllegalConstAccess(ref);
				}

				if (!ref->is_member() || context != nullptr ||
				    !(_m_flags & DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS)) {
					if (_m_read_observer) _m_read_observer(*ref);
					if (_m_member_read_observer && ref->is_member()) _m_member_read_observer(*ref, idx, context.get());
					auto result = ref->get_int(idx, context.get()) / value;
					if (_m_write_observer) _m_write_observer(*ref);
					ref->set_int(result, idx, context.get());
					if (_m_member_write_observer && ref->is_member()) _m_member_write_observer(*ref, idx, context.get());
				} else if (ref->is_member()) {
					log(LogLevel::ERROR, "DaedalusVm", "Accessing member \"%s\" without an instance set", ref->name().c_str());
				}

				break;
			}
			case DaedalusOpcode::MOVVI: {
				auto [target, target_idx, _] = pop_reference();
				auto value = pop_instance();
				if (_m_write_observer) _m_write_observer(*target);
				target->set_instance(value);
				break;
			}
			case DaedalusOpcode::B:
				jump(instr.address);
				return true;
			case DaedalusOpcode::BZ:
				if (pop_int() == 0) {
					jump(instr.address);
					return true;
				}
				break;
			case DaedalusOpcode::GMOVI: {
				sym = find_symbol_by_index(instr.symbol);
				if (sym == nullptr) {
					throw DaedalusVmException {"gmovi: no symbol found for index"};
				}
				if (_m_read_observer) _m_read_observer(*sym);
				_m_instance = sym->get_instance();
				break;
			}
			case DaedalusOpcode::PUSHVV:
				sym = find_symbol_by_index(instr.symbol);
				if (sym == nullptr) {
					throw DaedalusVmException {"pushvv: no symbol found for index"};
				}

				push_reference(sym, instr.index);
				break;
			}

			_m_pc += instr.size;
		} catch (DaedalusScriptError& err) {
			uint32_t prev_pc = _m_pc;

			if (_m_exception_handler) {
				auto strategy = (*_m_exception_handler)(*this, err, instr);

				if (strategy == DaedalusVmExceptionStrategy::FAIL) {
					log(LogLevel::ERROR, "DaedalusVm", "+++ Error while executing script: %s +++", err.what());
					print_stack_trace();
					throw;
				}

				if (strategy == DaedalusVmExceptionStrategy::RETURN) {
					return false;
				}
			} else {
				log(LogLevel::ERROR, "DaedalusVm", "+++ Error while executing script: %s +++", err.what());
				print_stack_trace();
				throw;
			}

			if (_m_pc == prev_pc) {
				_m_pc += instr.size;
			}
		}

		return true;
	}


	DaedalusVmExceptionStrategy
	lenient_vm_exception_handler(DaedalusVm& v, DaedalusScriptError const& exc, DaedalusInstruction const& instr) {
		log(LogLevel::ERROR, "DaedalusVm", "Internal Exception: %s", exc.what());

		switch (instr.op) {
		case DaedalusOpcode::ADD:
		case DaedalusOpcode::SUB:
		case DaedalusOpcode::MUL:
		case DaedalusOpcode::DIV:
		case DaedalusOpcode::MOD:
		case DaedalusOpcode::OR:
		case DaedalusOpcode::ANDB:
		case DaedalusOpcode::LT:
		case DaedalusOpcode::GT:
		case DaedalusOpcode::ORR:
		case DaedalusOpcode::AND:
		case DaedalusOpcode::LSL:
		case DaedalusOpcode::LSR:
		case DaedalusOpcode::LTE:
		case DaedalusOpcode::EQ:
		case DaedalusOpcode::NEQ:
		case DaedalusOpcode::GTE:
		case DaedalusOpcode::PLUS:
		case DaedalusOpcode::NEGATE:
		case DaedalusOpcode::NOT:
		case DaedalusOpcode::CMPL:
			v.push_int(0);
			break;

		case DaedalusOpcode::ADDMOVI:
		case DaedalusOpcode::SUBMOVI:
		case DaedalusOpcode::MULMOVI:
		case DaedalusOpcode::DIVMOVI:
		case DaedalusOpcode::MOVI:
		case DaedalusOpcode::MOVS:
		case DaedalusOpcode::MOVSS:
		case DaedalusOpcode::MOVVF:
		case DaedalusOpcode::MOVF:
		case DaedalusOpcode::MOVVI:
			// do nothing for now but ideally, this would check the exception and/or remove the offending values
			// from the stack.
			break;
		case DaedalusOpcode::NOP:
		case DaedalusOpcode::RSR:
		case DaedalusOpcode::B:
		case DaedalusOpcode::BZ:
			// if these fail, something has gone horribly wrong. still ignore the error though.
			break;

		case DaedalusOpcode::BL:
		case DaedalusOpcode::BE:
			// ignore the branch
			break;

		case DaedalusOpcode::PUSHI:
			v.push_int(0);
			break;
		case DaedalusOpcode::PUSHV:
		case DaedalusOpcode::PUSHVI:
		case DaedalusOpcode::PUSHVV:
			// push an int and hope it's the right type!
			v.push_int(0);
			break;

		case DaedalusOpcode::GMOVI:
			// do nothing for now but ideally, this would set the `null` instance
			break;
		}

		return DaedalusVmExceptionStrategy::CONTINUE;
	}

}
