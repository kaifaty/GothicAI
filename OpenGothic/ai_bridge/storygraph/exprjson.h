#pragma once

#include "expr.h"
#include "program.h"

#include <rapidjson/document.h>

#include <string>

namespace storygraph {

std::string toJson(const ExprP& e);

// An expression in the JSON form the converter writes (server/src/gothic_ai/gamedata/storygraph.py describes it):
// ["&&", ["==", ["var", "MIS_X"], ["int", 1]], ["call", "NPC_KNOWSINFO", ["inst", "OTHER"], ["sym", "DIA_X"]]].
// Names may be in any case; whether they exist is found out when the expression is evaluated.
// nullptr and `error` if the form is wrong.
ExprP readExpr(const rapidjson::Value& v, std::string& error);

// Steps in the JSON form toJson(Program) writes (program.h), plus what the director writes: {"do":"say",
// "speaker":"self"|"other","line":"DIA_X_15_00"} or with "text" (UTF-8) for a line of its own. Expressions are
// taken as written, nothing folded: they run as the program says (an `&&` of more terms is computed as the
// compiler builds it, left to right). `returns`: the function returns a value. Names are not looked up here.
// A program with `error` if the form is wrong.
Program readProgram(const rapidjson::Value& steps, bool returns);

// UTF-8 symbol name (as ident() writes it) -> the DAT's Windows-1252, uppercase; empty if it can't be one
std::string symbolName(std::string_view utf8);

}
