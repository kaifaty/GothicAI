#include "exprjson.h"

#include "exprjsonwrite.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <set>

using namespace storygraph;

std::string storygraph::symbolName(std::string_view utf8) {
  std::string r;
  for(size_t i=0; i<utf8.size(); ++i) {
    auto c = uint8_t(utf8[i]);
    if(c<0x80) {
      r += char(std::toupper(c));
      continue;
      }
    // two-byte sequences up to U+00FF, the inverse of ident()
    if((c & 0xE0)!=0xC0 || c>0xC3 || i+1>=utf8.size())
      return {};
    auto cp = uint32_t(((c & 0x1F) << 6) | (uint8_t(utf8[++i]) & 0x3F));
    r += char(cp);
    }
  return r;
  }

namespace {

const std::set<std::string> Binary = {
  "==", "!=", "<", ">", "<=", ">=", "+", "-", "*", "/", "%", "&", "|", "<<", ">>",
  };

struct Reader final {
  std::string& error;
  bool         exact = false;  // programs: operators as written, nothing folded

  ExprP opr(const std::string& o, std::vector<ExprP> a) {
    if(!exact) {
      if(o=="&&") return ex::land(std::move(a));
      if(o=="||") return ex::lor(std::move(a));
      if(o=="!")  return ex::lnot(a[0]);
      return ex::op(o, std::move(a));
      }
    // a && b && c is (a && b) && c
    ExprP r = a[0];
    if(a.size()==1)
      return ex::exact(o, {r});
    for(size_t i=1; i<a.size(); ++i)
      r = ex::exact(o, {r, a[i]});
    return r;
    }

  ExprP fail(std::string why) {
    if(error.empty())
      error = std::move(why);
    return nullptr;
    }

  std::string name(const rapidjson::Value& v, const char* what) {
    if(!v.IsString()) {
      fail(std::string(what) + " is not a string");
      return {};
      }
    auto n = symbolName(std::string_view(v.GetString(), v.GetStringLength()));
    if(n.empty())
      fail(std::string("bad ") + what);
    return n;
    }

  ExprP read(const rapidjson::Value& v, int depth) {
    if(depth>200)
      return fail("expression nested too deep");
    if(!v.IsArray() || v.Empty() || !v[0].IsString())
      return fail("an expression is an array that starts with its form");
    const std::string tag = v[0].GetString();
    const auto        n   = v.Size();
    auto args = [&](rapidjson::SizeType from) {
      std::vector<ExprP> r;
      for(auto i=from; i<n; ++i) {
        auto a = read(v[i], depth+1);
        if(a==nullptr)
          return std::vector<ExprP>();
        r.push_back(std::move(a));
        }
      return r;
      };
    auto optName = [&](rapidjson::SizeType i) {
      return i<n && v[i].IsString() ? symbolName(std::string_view(v[i].GetString(), v[i].GetStringLength())) : std::string();
      };

    if(tag=="int") {
      if(n<2 || !v[1].IsInt())
        return fail("int: no value");
      return ex::integer(v[1].GetInt(), optName(2));
      }
    if(tag=="str") {
      if(n<2 || !v[1].IsString())
        return fail("str: no text");
      return ex::str(TextCodec::fromUtf8(std::string_view(v[1].GetString(), v[1].GetStringLength())), optName(2));
      }
    if(tag=="var") {
      if(n<2)
        return fail("var: no name");
      auto nm = name(v[1], "variable");
      if(nm.empty())
        return nullptr;
      return ex::var(nm, n>2 && v[2].IsInt() ? v[2].GetInt() : -1);
      }
    if(tag=="field") {
      if(n<3)
        return fail("field: needs an instance and a member");
      auto inst = read(v[1], depth+1);
      auto nm   = inst ? name(v[2], "member") : std::string();
      if(nm.empty())
        return nullptr;
      return ex::field(inst, nm, n>3 && v[3].IsInt() ? v[3].GetInt() : -1, optName(4));
      }
    if(tag=="inst" || tag=="sym") {
      if(n<2)
        return fail(tag + ": no name");
      auto nm = name(v[1], tag=="inst" ? "instance" : "symbol");
      if(nm.empty())
        return nullptr;
      return tag=="inst" ? ex::inst(nm) : ex::sym(nm);
      }
    if(tag=="param") {
      if(n<2 || !v[1].IsInt())
        return fail("param: no index");
      return ex::param(v[1].GetInt());
      }
    if(tag=="call" || tag=="fn") {
      if(n<2)
        return fail(tag + ": no function");
      auto nm = name(v[1], "function");
      if(nm.empty())
        return nullptr;
      auto a = args(2);
      if(a.size()!=n-2)
        return nullptr;
      return tag=="call" ? ex::call(nm, std::move(a)) : ex::fn(nm, std::move(a));
      }
    if(tag=="cases") {
      if(exact)
        return fail("cases are not in programs");
      auto a = args(1);
      if(a.size()!=n-1)
        return nullptr;
      if(a.size()%2!=0)
        return fail("cases: guards and values come in pairs");
      return ex::cases(std::move(a));
      }
    if(tag=="unknown")
      return exact ? fail("unknown is not in programs") : ex::unknown(n>1 && v[1].IsString() ? v[1].GetString() : "");
    if(tag=="&&" || tag=="||") {
      auto a = args(1);
      if(a.size()!=n-1 || a.empty())
        return a.empty() && error.empty() ? fail(tag + " without terms") : nullptr;
      if(exact && a.size()<2)
        return fail(tag + " takes two terms or more");
      return opr(tag, std::move(a));
      }
    if(tag=="!" || tag=="neg" || tag=="~") {
      auto a = args(1);
      if(a.size()!=1)
        return error.empty() ? fail(tag + " takes one argument") : nullptr;
      return opr(tag, std::move(a));
      }
    if(Binary.count(tag)) {
      auto a = args(1);
      if(a.size()!=2)
        return error.empty() ? fail(tag + " takes two arguments") : nullptr;
      return opr(tag, std::move(a));
      }
    return fail("unknown form " + tag);
    }
  };

}

ExprP storygraph::readExpr(const rapidjson::Value& v, std::string& error) {
  error.clear();
  Reader r{error};
  return r.read(v, 0);
  }

namespace {

struct StepReader final {
  Reader       expr;
  bool         returns = false;
  std::string& error;

  bool fail(std::string why) {
    if(error.empty())
      error = std::move(why);
    return false;
    }

  ExprP value(const rapidjson::Value& v, const char* key, size_t at) {
    auto it = v.FindMember(key);
    if(it==v.MemberEnd()) {
      fail("step " + std::to_string(at) + ": no " + key);
      return nullptr;
      }
    auto e = expr.read(it->value, 0);
    if(e==nullptr)
      error = "step " + std::to_string(at) + ": " + error;
    return e;
    }

  bool steps(const rapidjson::Value& v, std::vector<Step>& out, int depth) {
    if(!v.IsArray())
      return fail("steps are an array");
    if(depth>50)
      return fail("branches nested too deep");
    size_t at = 0;
    for(auto& s : v.GetArray()) {
      if(!s.IsObject())
        return fail("step " + std::to_string(at) + " is not an object");
      auto it = s.FindMember("do");
      const std::string kind = it!=s.MemberEnd() && it->value.IsString() ? it->value.GetString() : "";
      Step st;
      if(kind=="call") {
        st.kind = Step::Call;
        st.call = value(s, "call", at);
        if(st.call==nullptr)
          return false;
        } else if(kind=="set") {
        st.kind   = Step::Store;
        st.target = value(s, "target", at);
        if(st.target==nullptr)
          return false;
        if(st.target->kind!=Expr::Var && st.target->kind!=Expr::Field && st.target->kind!=Expr::Inst)
          return fail("step " + std::to_string(at) + ": a store goes into a variable, a member or an instance variable");
        st.value = value(s, "value", at);
        if(st.value==nullptr)
          return false;
        } else if(kind=="if") {
        st.kind = Step::If;
        st.cond = value(s, "cond", at);
        if(st.cond==nullptr)
          return false;
        auto th = s.FindMember("then");
        if(th==s.MemberEnd() || !steps(th->value, st.then, depth+1))
          return fail("step " + std::to_string(at) + ": no then");
        auto el = s.FindMember("else");
        if(el!=s.MemberEnd() && !el->value.IsNull() && !steps(el->value, st.otherwise, depth+1))
          return false;
        } else if(kind=="return") {
        st.kind = Step::Return;
        auto r = s.FindMember("value");
        if(r!=s.MemberEnd() && !r->value.IsNull()) {
          st.value = value(s, "value", at);
          if(st.value==nullptr)
            return false;
          }
        if(returns!=(st.value!=nullptr))
          return fail("step " + std::to_string(at) + (returns ? ": the function returns a value" : ": the function returns nothing"));
        } else if(kind=="say") {
        st.kind = Step::Say;
        auto sp = s.FindMember("speaker");
        const std::string speaker = sp!=s.MemberEnd() && sp->value.IsString() ? sp->value.GetString() : "";
        if(speaker!="self" && speaker!="other")
          return fail("step " + std::to_string(at) + ": the speaker is self or other");
        st.hero = speaker=="other";
        auto l = s.FindMember("line");
        auto t = s.FindMember("text");
        if(l!=s.MemberEnd() && l->value.IsString())
          st.line = symbolName(std::string_view(l->value.GetString(), l->value.GetStringLength()));
        if(t!=s.MemberEnd() && t->value.IsString())
          st.text = TextCodec::fromUtf8(std::string_view(t->value.GetString(), t->value.GetStringLength()));
        if(st.line.empty()==st.text.empty())
          return fail("step " + std::to_string(at) + ": say takes a line or a text");
        } else if(kind=="choice") {
        st.kind = Step::Choice;
        auto t  = s.FindMember("text");
        auto id = s.FindMember("id");
        if(t!=s.MemberEnd() && t->value.IsString())
          st.text = TextCodec::fromUtf8(std::string_view(t->value.GetString(), t->value.GetStringLength()));
        if(st.text.empty())
          return fail("step " + std::to_string(at) + ": a choice needs its text");
        if(id!=s.MemberEnd() && id->value.IsString()) {
          st.id = symbolName(std::string_view(id->value.GetString(), id->value.GetStringLength()));
          if(st.id.empty())
            return fail("step " + std::to_string(at) + ": bad choice id");
          }
        auto body = s.FindMember("steps");
        if(body==s.MemberEnd() || !steps(body->value, st.then, depth+1))
          return fail("step " + std::to_string(at) + ": a choice needs its steps");
        } else if(kind=="clear_choices") {
        st.kind = Step::ClearChoices;
        } else {
        return fail("step " + std::to_string(at) + ": unknown step " + kind);
        }
      out.push_back(std::move(st));
      ++at;
      }
    return true;
    }
  };

}

Program storygraph::readProgram(const rapidjson::Value& v, bool returns) {
  Program    p;
  StepReader r{Reader{p.error, true}, returns, p.error};
  p.returns = returns;
  if(!r.steps(v, p.steps, 0) && p.error.empty())
    p.error = "bad steps";
  if(!p.error.empty())
    p.steps.clear();
  return p;
  }

namespace {

template<class W>
void writeExprRaw(W& w, const ExprP& e) {
  auto text = toJson(e);
  w.RawValue(text.c_str(), text.size(), rapidjson::kArrayType);
  }

template<class W>
void writeSteps(W& w, const std::vector<Step>& steps) {
  w.StartArray();
  for(auto& s : steps) {
    w.StartObject();
    switch(s.kind) {
      case Step::Call:
        w.Key("do"); w.String("call");
        w.Key("call"); writeExprRaw(w, s.call);
        break;
      case Step::Store:
        w.Key("do"); w.String("set");
        w.Key("target"); writeExprRaw(w, s.target);
        w.Key("value");  writeExprRaw(w, s.value);
        break;
      case Step::Return:
        w.Key("do"); w.String("return");
        if(s.value) {
          w.Key("value"); writeExprRaw(w, s.value);
          }
        break;
      case Step::If:
        w.Key("do"); w.String("if");
        w.Key("cond"); writeExprRaw(w, s.cond);
        w.Key("then"); writeSteps(w, s.then);
        if(!s.otherwise.empty()) {
          w.Key("else"); writeSteps(w, s.otherwise);
          }
        break;
      case Step::Say: {
        w.Key("do");      w.String("say");
        w.Key("speaker"); w.String(s.hero ? "other" : "self");
        auto text = s.line.empty() ? TextCodec::toUtf8(s.text) : s.line;
        w.Key(s.line.empty() ? "text" : "line");
        w.String(text.c_str(), rapidjson::SizeType(text.size()));
        break;
        }
      case Step::Choice: {
        w.Key("do"); w.String("choice");
        if(!s.id.empty()) {
          w.Key("id"); w.String(s.id.c_str(), rapidjson::SizeType(s.id.size()));
          }
        auto text = TextCodec::toUtf8(s.text);
        w.Key("text");  w.String(text.c_str(), rapidjson::SizeType(text.size()));
        w.Key("steps"); writeSteps(w, s.then);
        break;
        }
      case Step::ClearChoices:
        w.Key("do"); w.String("clear_choices");
        break;
      }
    w.EndObject();
    }
  w.EndArray();
  }

}

std::string storygraph::toJson(const ExprP& e) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  json::writeExpr(w, e);
  return std::string(buf.GetString(), buf.GetSize());
  }

std::string storygraph::toJson(const Program& p) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  writeSteps(w, p.steps);
  return std::string(buf.GetString(), buf.GetSize());
  }
