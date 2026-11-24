// Copyright © 2021-2024 GothicKit Contributors.
// SPDX-License-Identifier: MIT
// Copy of ZenKit 1.3.0 with project modifications.
// MIT license: OpenGothic/lib/ZenKit/license.md (../../lib/ZenKit/license.md).

#pragma once

#include "vm.h"

namespace daedalus {
	/// \brief A helper class for preventing stack corruption.
	///
	/// This class can be used to guard against stack corruption when a value is expected to be
	/// pushed onto the stack but the push does not happen for some reason. Unless #inhibit is called,
	/// when an instance of this class is destructed, a default value of the given datatype is pushed
	/// into the stack.
	///
	/// \code
	/// // construct the stack guard
	/// stack_guard guard {this, DaedalusDataType::int_};
	///
	/// // ... do something that might fail to push an int to the stack ...
	///
	/// // make sure to inhibit the guard if pushing the value succeeded
	/// guard.inhibit();
	/// \endcode
	struct StackGuard {
		/// \brief Creates a new stack guard.
		/// \param machine The VM this instance is guarding.
		/// \param type The type of value to push if the guard is triggered.
		StackGuard(DaedalusVm* machine, DaedalusDataType type) : _m_type(type), _m_machine(machine) {}

		/// \brief Triggers this guard.
		///
		/// Unless #inhibit was called, this destructor will push a value of the datatype passed in the
		/// constructor onto the stack of the VM passed in the constructor.
		~StackGuard() {
			if (_m_inhibited) return;

			StackGuard::fix(_m_machine, _m_type);
		}

		/// \brief Inhibits this guard.
		///
		/// Calling this function will cause the guard to disengage and thus not push a
		/// value onto the stack.
		void inhibit() {
			_m_inhibited = true;
		}

		static void fix(DaedalusVm* vm, DaedalusDataType type) {
			switch (type) {
			case DaedalusDataType::FLOAT:
				vm->push_float(0);
				break;
			case DaedalusDataType::INT:
			case DaedalusDataType::FUNCTION:
				vm->push_int(0);
				break;
			case DaedalusDataType::STRING:
				vm->push_string("");
				break;
			case DaedalusDataType::INSTANCE:
				vm->push_instance(nullptr);
				break;
			case DaedalusDataType::VOID:
			case DaedalusDataType::CLASS:
			case DaedalusDataType::PROTOTYPE:
				break;
			}
		}

	private:
		DaedalusDataType _m_type;
		DaedalusVm* _m_machine;
		bool _m_inhibited {false};
	};

}
