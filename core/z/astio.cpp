// astio.cpp
#include "common.h"

// ============================================================================
// 运算符名映射
// ============================================================================
std::string AstWriter::opName(Tok t)
{
    switch (t) {
    case Tok::PLUS:    return "+";
    case Tok::MINUS:   return "-";
    case Tok::STAR:    return "*";
    case Tok::SLASH:   return "/";
    case Tok::PERCENT: return "%";
    case Tok::ASSIGN:  return "=";
    case Tok::EQ:      return "==";
    case Tok::NE:      return "!=";
    case Tok::LT:      return "<";
    case Tok::GT:      return ">";
    case Tok::LE:      return "<=";
    case Tok::GE:      return ">=";
    case Tok::ANDAND:  return "&&";
    case Tok::OROR:    return "||";
    case Tok::NOT:     return "!";
    case Tok::INC:     return "++";
    case Tok::DEC:     return "--";
    case Tok::PLUSEQ:  return "+=";
    case Tok::MINUSEQ: return "-=";
    case Tok::STAREQ:  return "*=";
    case Tok::SLASHEQ: return "/=";
    case Tok::PERCENTEQ: return "%=";
    default:           return "?";
    }
}

Tok AstReader::opFromName(const std::string& s)
{
    static const std::map<std::string, Tok> M = {
        { "+",  Tok::PLUS    }, { "-",  Tok::MINUS   }, { "*",  Tok::STAR    },
        { "/",  Tok::SLASH   }, { "%",  Tok::PERCENT }, { "=",  Tok::ASSIGN  },
        { "==", Tok::EQ      }, { "!=", Tok::NE      }, { "<",  Tok::LT      },
        { ">",  Tok::GT      }, { "<=", Tok::LE      }, { ">=", Tok::GE      },
        { "&&", Tok::ANDAND  }, { "||", Tok::OROR    }, { "!",  Tok::NOT     },
        { "++", Tok::INC     }, { "--", Tok::DEC     },
        { "+=", Tok::PLUSEQ  }, { "-=", Tok::MINUSEQ }, { "*=", Tok::STAREQ  },
        { "/=", Tok::SLASHEQ }, { "%=", Tok::PERCENTEQ },
    };
    std::map<std::string, Tok>::const_iterator it = M.find(s);
    if (it == M.end())
        throw std::runtime_error("未知运算符: " + s);
    return it->second;
}

// ============================================================================
// AstWriter
// ============================================================================
AstWriter::AstWriter(std::shared_ptr<Program> prog) : prog_(prog) {}

void AstWriter::indent()
{
    // 简单缩进：每层 2 空格
    // 这里不用缩进，保持 S 表达式紧凑（更易机器读取，同时人类可读）
}

void AstWriter::nl()
{
    out_ << "\n";
}

std::string AstWriter::escapeStr(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        case '\r': out += "\\r";  break;
        case '\a': out += "\\a";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\v': out += "\\v";  break;
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\0': out += "\\0";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\x%02X", c);
                out += buf;
            }
            else out += (char)c;
        }
    }
    return out;
}

std::string AstWriter::write()
{
    out_.str("");
    writeProgram();
    return out_.str();
}

void AstWriter::writeProgram()
{
    out_ << "(Program\n";

    // 全局变量
    for (size_t i = 0; i < prog_->globals.size(); ++i) {
        out_ << "  (Global\n    ";
        writeStmt(prog_->globals[i]);
        out_ << ")\n";
    }

    // 函数
    for (size_t i = 0; i < prog_->funcs.size(); ++i)
        writeFunc(prog_->funcs[i]);

    out_ << ")\n";
}

void AstWriter::writeFunc(const std::shared_ptr<FuncDecl>& fn)
{
    out_ << "  (Func \"" << escapeStr(fn->name) << "\" \"" << escapeStr(fn->retType) << "\"\n";

    out_ << "    (Params";
    for (size_t i = 0; i < fn->params.size(); ++i) {
        out_ << " (\"" << escapeStr(fn->params[i].type) << "\" \""
            << escapeStr(fn->params[i].name) << "\")";
    }
    out_ << ")\n";

    out_ << "    (Body ";
    if (fn->body) writeStmt(fn->body);
    else out_ << "NIL";
    out_ << "))\n";
}

void AstWriter::writeVarDecl(const std::shared_ptr<VarDeclStmt>& d)
{
    out_ << "(VarDecl \"" << escapeStr(d->type) << "\" \"" << escapeStr(d->name) << "\" "
        << (d->isConst ? "1" : "0") << " "
        << (d->isArray ? "1" : "0") << " "
        << d->arraySize << " ";

    if (d->init) { out_ << "(Init "; writeExpr(d->init); out_ << ")"; }
    else out_ << "NIL";
    out_ << " ";

    if (!d->initList.empty()) {
        out_ << "(List";
        for (size_t i = 0; i < d->initList.size(); ++i) {
            out_ << " ";
            writeExpr(d->initList[i]);
        }
        out_ << ")";
    }
    else out_ << "NIL";

    out_ << ")";
}

void AstWriter::writeStmt(const StmtPtr& s)
{
    if (!s) { out_ << "NIL"; return; }

    if (std::shared_ptr<BlockStmt> p = std::dynamic_pointer_cast<BlockStmt>(s)) {
        out_ << "(Block";
        for (size_t i = 0; i < p->list.size(); ++i) {
            out_ << " ";
            writeStmt(p->list[i]);
        }
        out_ << ")";
        return;
    }
    if (std::shared_ptr<VarDeclStmt> p = std::dynamic_pointer_cast<VarDeclStmt>(s)) {
        writeVarDecl(p);
        return;
    }
    if (std::shared_ptr<IfStmt> p = std::dynamic_pointer_cast<IfStmt>(s)) {
        out_ << "(If (Cond ";
        writeExpr(p->cond);
        out_ << ") (Then ";
        if (p->thenS) writeStmt(p->thenS); else out_ << "NIL";
        out_ << ") (Else ";
        if (p->elseS) writeStmt(p->elseS); else out_ << "NIL";
        out_ << "))";
        return;
    }
    if (std::shared_ptr<WhileStmt> p = std::dynamic_pointer_cast<WhileStmt>(s)) {
        out_ << "(While (Cond ";
        writeExpr(p->cond);
        out_ << ") (Body ";
        if (p->body) writeStmt(p->body); else out_ << "NIL";
        out_ << "))";
        return;
    }
    if (std::shared_ptr<ForStmt> p = std::dynamic_pointer_cast<ForStmt>(s)) {
        out_ << "(For (Init ";
        if (p->init) writeStmt(p->init); else out_ << "NIL";
        out_ << ") (Cond ";
        if (p->cond) writeExpr(p->cond); else out_ << "NIL";
        out_ << ") (Step ";
        if (p->step) writeExpr(p->step); else out_ << "NIL";
        out_ << ") (Body ";
        if (p->body) writeStmt(p->body); else out_ << "NIL";
        out_ << "))";
        return;
    }
    if (std::shared_ptr<ReturnStmt> p = std::dynamic_pointer_cast<ReturnStmt>(s)) {
        out_ << "(Return ";
        if (p->value) writeExpr(p->value); else out_ << "NIL";
        out_ << ")";
        return;
    }
    if (std::dynamic_pointer_cast<BreakStmt>(s)) { out_ << "(Break)";    return; }
    if (std::dynamic_pointer_cast<ContinueStmt>(s)) { out_ << "(Continue)"; return; }
    if (std::dynamic_pointer_cast<EmptyStmt>(s)) { out_ << "(Empty)";    return; }

    if (std::shared_ptr<ExprStmt> p = std::dynamic_pointer_cast<ExprStmt>(s)) {
        out_ << "(ExprStmt ";
        writeExpr(p->expr);
        out_ << ")";
        return;
    }
    out_ << "(Unknown)";
}

void AstWriter::writeExpr(const ExprPtr& e)
{
    if (!e) { out_ << "NIL"; return; }

    if (std::shared_ptr<IntLit> p = std::dynamic_pointer_cast<IntLit>(e)) {
        out_ << "(IntLit " << p->v << ")";
        return;
    }
    if (std::shared_ptr<FloatLit> p = std::dynamic_pointer_cast<FloatLit>(e)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", p->v);
        out_ << "(FloatLit " << buf << ")";
        return;
    }
    if (std::shared_ptr<CharLit> p = std::dynamic_pointer_cast<CharLit>(e)) {
        out_ << "(CharLit " << p->v << ")";
        return;
    }
    if (std::shared_ptr<StrLit> p = std::dynamic_pointer_cast<StrLit>(e)) {
        out_ << "(StrLit \"" << escapeStr(p->v) << "\")";
        return;
    }
    if (std::shared_ptr<IdentExpr> p = std::dynamic_pointer_cast<IdentExpr>(e)) {
        out_ << "(Ident \"" << escapeStr(p->name) << "\")";
        return;
    }
    if (std::shared_ptr<BinaryExpr> p = std::dynamic_pointer_cast<BinaryExpr>(e)) {
        out_ << "(Binary " << opName(p->op) << " ";
        writeExpr(p->l); out_ << " ";
        writeExpr(p->r); out_ << ")";
        return;
    }
    if (std::shared_ptr<UnaryExpr> p = std::dynamic_pointer_cast<UnaryExpr>(e)) {
        out_ << "(Unary " << opName(p->op) << " "
            << (p->prefix ? "pre" : "post") << " ";
        writeExpr(p->e); out_ << ")";
        return;
    }
    if (std::shared_ptr<AssignExpr> p = std::dynamic_pointer_cast<AssignExpr>(e)) {
        out_ << "(Assign " << opName(p->op) << " ";
        writeExpr(p->target); out_ << " ";
        writeExpr(p->value);  out_ << ")";
        return;
    }
    if (std::shared_ptr<CallExpr> p = std::dynamic_pointer_cast<CallExpr>(e)) {
        out_ << "(Call \"" << escapeStr(p->name) << "\"";
        for (size_t i = 0; i < p->args.size(); ++i) {
            out_ << " ";
            writeExpr(p->args[i]);
        }
        out_ << ")";
        return;
    }
    if (std::shared_ptr<IndexExpr> p = std::dynamic_pointer_cast<IndexExpr>(e)) {
        out_ << "(Index ";
        writeExpr(p->base); out_ << " ";
        writeExpr(p->index); out_ << ")";
        return;
    }
    if (std::shared_ptr<TernaryExpr> p = std::dynamic_pointer_cast<TernaryExpr>(e)) {
        out_ << "(Ternary ";
        writeExpr(p->c); out_ << " ";
        writeExpr(p->a); out_ << " ";
        writeExpr(p->b); out_ << ")";
        return;
    }
    out_ << "(UnknownExpr)";
}

// ============================================================================
// AstReader —— S 表达式解析
// ============================================================================
AstReader::AstReader(std::string text) : text_(std::move(text)) {}

bool AstReader::eof() const { return p_ >= text_.size(); }

char AstReader::peek(size_t k) const
{
    return p_ + k < text_.size() ? text_[p_ + k] : '\0';
}

void AstReader::err(const std::string& msg) const
{
    throw std::runtime_error("AST 解析错误（第 " + std::to_string(line_) + " 行）: " + msg);
}

void AstReader::skipWS()
{
    for (;;) {
        char c = peek();
        if (c == '\0') return;
        if (c == '\n') { ++line_; ++p_; }
        else if (std::isspace((unsigned char)c)) ++p_;
        else if (c == ';') { // 允许 ; 作注释到行尾
            while (!eof() && peek() != '\n') ++p_;
        }
        else return;
    }
}

std::string AstReader::parseAtom()
{
    // 支持字符串字面量（带转义）和裸字
    if (peek() == '"') {
        ++p_;
        std::string s;
        while (!eof() && peek() != '"') {
            char c = peek();
            if (c == '\\') {
                ++p_;
                char e = peek(); ++p_;
                switch (e) {
                case 'n':  s += '\n'; break;
                case 't':  s += '\t'; break;
                case 'r':  s += '\r'; break;
                case '0':  s += '\0'; break;
                case 'a':  s += '\a'; break;
                case 'b':  s += '\b'; break;
                case 'f':  s += '\f'; break;
                case 'v':  s += '\v'; break;
                case '\\': s += '\\'; break;
                case '"':  s += '"';  break;
                case 'x': {
                    int v = 0;
                    for (int i = 0; i < 2 && std::isxdigit((unsigned char)peek()); ++i) {
                        char h = peek(); ++p_;
                        v = v * 16 + (std::isdigit((unsigned char)h) ? h - '0'
                            : std::tolower(h) - 'a' + 10);
                    }
                    s += (char)v;
                    break;
                }
                default: s += e; break;
                }
            }
            else {
                if (c == '\n') ++line_;
                s += c;
                ++p_;
            }
        }
        if (!eof()) ++p_;
        return s;
    }

    // 裸字：非空白、非括号
    std::string s;
    while (!eof()) {
        char c = peek();
        if (std::isspace((unsigned char)c) || c == '(' || c == ')') break;
        s += c;
        ++p_;
    }
    return s;
}

AstReader::SNode AstReader::parseNode()
{
    skipWS();
    if (eof()) err("意外结束");

    SNode n;
    char c = peek();

    if (c == '(') {
        n.isList = true;
        ++p_;
        for (;;) {
            skipWS();
            if (peek() == ')') { ++p_; break; }
            if (eof()) err("缺少 ')'");
            n.items.push_back(parseNode());
        }
        return n;
    }

    n.isList = false;
    n.atom = parseAtom();
    return n;
}

// ---- S 表达式 → AST ----
const std::string& AstReader::label(const SNode& n)
{
    static const std::string empty;
    if (!n.isList || n.items.empty()) return empty;
    return n.items[0].atom;
}

long long AstReader::toInt(const std::string& s)
{
    return std::strtoll(s.c_str(), NULL, 10);
}
double AstReader::toFloat(const std::string& s)
{
    return std::strtod(s.c_str(), NULL);
}

std::shared_ptr<Program> AstReader::read()
{
    SNode root = parseNode();
    if (label(root) != "Program") err("根节点必须是 (Program ...)");
    return fromProgram(root);
}

std::shared_ptr<Program> AstReader::fromProgram(const SNode& n)
{
    std::shared_ptr<Program> prog(new Program());
    for (size_t i = 1; i < n.items.size(); ++i) {
        const SNode& child = n.items[i];
        const std::string& lb = label(child);
        if (lb == "Global") {
            // (Global <stmt>)
            if (child.items.size() < 2) err("Global 缺少内容");
            prog->globals.push_back(fromStmt(child.items[1]));
        }
        else if (lb == "Func") {
            prog->funcs.push_back(fromFunc(child));
        }
        else {
            err("Program 内未知节点: " + lb);
        }
    }
    return prog;
}

std::shared_ptr<FuncDecl> AstReader::fromFunc(const SNode& n)
{
    // (Func "name" "retType" (Params ...) (Body <stmt>))
    std::shared_ptr<FuncDecl> fn(new FuncDecl());
    if (n.items.size() < 5) err("Func 参数不足");
    fn->name = n.items[1].atom;
    fn->retType = n.items[2].atom;

    const SNode& params = n.items[3];
    if (label(params) != "Params") err("Func 缺少 Params");
    for (size_t i = 1; i < params.items.size(); ++i) {
        const SNode& p = params.items[i];
        if (p.items.size() < 2) err("Param 格式错误");
        Param pm;
        pm.type = p.items[0].atom;
        pm.name = p.items[1].atom;
        fn->params.push_back(pm);
    }

    const SNode& body = n.items[4];
    if (label(body) != "Body") err("Func 缺少 Body");
    if (body.items.size() < 2 || body.items[1].atom == "NIL")
        fn->body = std::shared_ptr<BlockStmt>();
    else {
        StmtPtr s = fromStmt(body.items[1]);
        fn->body = std::dynamic_pointer_cast<BlockStmt>(s);
    }
    return fn;
}

StmtPtr AstReader::fromStmt(const SNode& n)
{
    if (!n.isList || n.items.empty()) err("语句必须是列表");

    const std::string& lb = label(n);

    if (lb == "NIL") return StmtPtr();

    if (lb == "Block") {
        std::shared_ptr<BlockStmt> b(new BlockStmt());
        for (size_t i = 1; i < n.items.size(); ++i)
            b->list.push_back(fromStmt(n.items[i]));
        return b;
    }
    if (lb == "VarDecl") {
        // (VarDecl "type" "name" isConst isArray arraySize (Init x|NIL) (List ...|NIL))
        if (n.items.size() < 8) err("VarDecl 参数不足");
        std::shared_ptr<VarDeclStmt> d(new VarDeclStmt());
        d->type = n.items[1].atom;
        d->name = n.items[2].atom;
        d->isConst = toInt(n.items[3].atom) != 0;
        d->isArray = toInt(n.items[4].atom) != 0;
        d->arraySize = (int)toInt(n.items[5].atom);

        const SNode& init = n.items[6];
        if (label(init) == "Init" && init.items.size() >= 2)
            d->init = fromExpr(init.items[1]);

        const SNode& list = n.items[7];
        if (label(list) == "List")
            for (size_t i = 1; i < list.items.size(); ++i)
                d->initList.push_back(fromExpr(list.items[i]));

        return d;
    }
    if (lb == "If") {
        std::shared_ptr<IfStmt> s(new IfStmt());
        for (size_t i = 1; i < n.items.size(); ++i) {
            const SNode& c = n.items[i];
            if (label(c) == "Cond" && c.items.size() >= 2)
                s->cond = fromExpr(c.items[1]);
            else if (label(c) == "Then" && c.items.size() >= 2)
                s->thenS = fromStmt(c.items[1]);
            else if (label(c) == "Else" && c.items.size() >= 2)
                s->elseS = fromStmt(c.items[1]);
        }
        return s;
    }
    if (lb == "While") {
        std::shared_ptr<WhileStmt> s(new WhileStmt());
        for (size_t i = 1; i < n.items.size(); ++i) {
            const SNode& c = n.items[i];
            if (label(c) == "Cond" && c.items.size() >= 2)
                s->cond = fromExpr(c.items[1]);
            else if (label(c) == "Body" && c.items.size() >= 2)
                s->body = fromStmt(c.items[1]);
        }
        return s;
    }
    if (lb == "For") {
        std::shared_ptr<ForStmt> s(new ForStmt());
        for (size_t i = 1; i < n.items.size(); ++i) {
            const SNode& c = n.items[i];
            if (c.items.size() < 2) continue;
            const std::string& cl = label(c);
            if (cl == "Init") s->init = fromStmt(c.items[1]);
            else if (cl == "Cond") s->cond = fromExpr(c.items[1]);
            else if (cl == "Step") s->step = fromExpr(c.items[1]);
            else if (cl == "Body") s->body = fromStmt(c.items[1]);
        }
        return s;
    }
    if (lb == "Return") {
        std::shared_ptr<ReturnStmt> s(new ReturnStmt());
        if (n.items.size() >= 2) s->value = fromExpr(n.items[1]);
        return s;
    }
    if (lb == "Break")    return StmtPtr(new BreakStmt());
    if (lb == "Continue") return StmtPtr(new ContinueStmt());
    if (lb == "Empty")    return StmtPtr(new EmptyStmt());
    if (lb == "ExprStmt") {
        std::shared_ptr<ExprStmt> s(new ExprStmt());
        if (n.items.size() >= 2) s->expr = fromExpr(n.items[1]);
        return s;
    }

    err("未知语句节点: " + lb);
    return StmtPtr();
}

ExprPtr AstReader::fromExpr(const SNode& n)
{
    if (!n.isList || n.items.empty()) {
        if (!n.isList && n.atom == "NIL") return ExprPtr();
        err("表达式必须是列表");
    }
    const std::string& lb = label(n);

    if (lb == "NIL") return ExprPtr();

    if (lb == "IntLit")   return ExprPtr(new IntLit(toInt(n.items[1].atom)));
    if (lb == "FloatLit") return ExprPtr(new FloatLit(toFloat(n.items[1].atom)));
    if (lb == "CharLit")  return ExprPtr(new CharLit((int)toInt(n.items[1].atom)));
    if (lb == "StrLit")   return ExprPtr(new StrLit(n.items[1].atom));
    if (lb == "Ident")    return ExprPtr(new IdentExpr(n.items[1].atom));

    if (lb == "Binary") {
        if (n.items.size() < 4) err("Binary 参数不足");
        Tok op = opFromName(n.items[1].atom);
        return ExprPtr(new BinaryExpr(op, fromExpr(n.items[2]), fromExpr(n.items[3])));
    }
    if (lb == "Unary") {
        if (n.items.size() < 4) err("Unary 参数不足");
        Tok op = opFromName(n.items[1].atom);
        bool pre = (n.items[2].atom == "pre");
        return ExprPtr(new UnaryExpr(op, fromExpr(n.items[3]), pre));
    }
    if (lb == "Assign") {
        if (n.items.size() < 4) err("Assign 参数不足");
        Tok op = opFromName(n.items[1].atom);
        return ExprPtr(new AssignExpr(op, fromExpr(n.items[2]), fromExpr(n.items[3])));
    }
    if (lb == "Call") {
        if (n.items.size() < 2) err("Call 参数不足");
        std::shared_ptr<CallExpr> c(new CallExpr(n.items[1].atom));
        for (size_t i = 2; i < n.items.size(); ++i)
            c->args.push_back(fromExpr(n.items[i]));
        return c;
    }
    if (lb == "Index") {
        if (n.items.size() < 3) err("Index 参数不足");
        return ExprPtr(new IndexExpr(fromExpr(n.items[1]), fromExpr(n.items[2])));
    }
    if (lb == "Ternary") {
        if (n.items.size() < 4) err("Ternary 参数不足");
        return ExprPtr(new TernaryExpr(fromExpr(n.items[1]),
            fromExpr(n.items[2]),
            fromExpr(n.items[3])));
    }

    err("未知表达式节点: " + lb);
    return ExprPtr();
}