#pragma once

#include "expr.h"
#include "textcodec.h"

#include <rapidjson/rapidjson.h>

namespace storygraph::json {

template<class W>
void writeStr(W& w, const std::string& s) {
  w.String(s.data(), rapidjson::SizeType(s.size()));
  }

template<class W>
void writeExpr(W& w, const ExprP& e) {
  if(e==nullptr) {
    w.Null();
    return;
    }
  w.StartArray();
  auto tag = [&w](const char* t) { w.String(t); };
  switch(e->kind) {
    case Expr::Int:
      tag("int");
      w.Int(e->value);
      if(!e->name.empty())
        writeStr(w, ident(e->name));
      break;
    case Expr::Str:
      tag("str");
      writeStr(w, TextCodec::toUtf8(e->text));
      if(!e->name.empty())
        writeStr(w, ident(e->name));
      break;
    case Expr::Var:
      tag("var");
      writeStr(w, ident(e->name));
      if(e->index>=0)
        w.Int(e->index);
      break;
    case Expr::Field:
      tag("field");
      writeExpr(w, e->args[0]);
      writeStr(w, ident(e->name));
      if(e->index>=0) {
        w.Int(e->index);
        if(!e->text.empty())
          writeStr(w, ident(e->text));
        }
      break;
    case Expr::Inst:
      tag("inst");
      writeStr(w, ident(e->name));
      break;
    case Expr::Sym:
      tag("sym");
      writeStr(w, ident(e->name));
      break;
    case Expr::Param:
      tag("param");
      w.Int(e->index);
      break;
    case Expr::Local:
      tag("unknown");
      writeStr(w, "local " + ident(e->name));
      break;
    case Expr::Call:
    case Expr::Fn:
      tag(e->kind==Expr::Call ? "call" : "fn");
      writeStr(w, ident(e->name));
      for(auto& a : e->args)
        writeExpr(w, a);
      break;
    case Expr::Op:
      writeStr(w, e->name);
      for(auto& a : e->args)
        writeExpr(w, a);
      break;
    case Expr::Cases:
      tag("cases");
      for(auto& a : e->args)
        writeExpr(w, a);
      break;
    case Expr::Unknown:
      tag("unknown");
      writeStr(w, e->text);
      break;
    }
  w.EndArray();
  }

}
