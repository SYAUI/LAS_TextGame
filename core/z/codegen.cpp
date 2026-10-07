// codegen.cpp
#include "common.h"

// ============================================================================
// 快速哈希：h = h * 33 + byte
// ============================================================================
uint32_t CodeGen::quickHash(const std::string& s)
{
    uint32_t h = 5381u;
    for (size_t i = 0; i < s.size(); ++i)
        h = h * 33u + (unsigned char)s[i];
    return h;
}

// ---------------------------------------------------------------------------
// 变量 C 名：<类型前缀>_<8 位十六进制哈希>
// ---------------------------------------------------------------------------
std::string CodeGen::makeVarCName(const std::string& langType, const std::string& cnName)
{
    const char* prefix = "var";
    if (langType == "整型" || langType == "整数")   prefix = "int";
    else if (langType == "长整型" || langType == "长整数") prefix = "lng";   // ← 新增
    else if (langType == "浮点" || langType == "小数")   prefix = "flt";
    else if (langType == "字符")                           prefix = "chr";
    else if (langType == "空")                             prefix = "void";

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s_%08x", prefix, quickHash(cnName));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 函数 C 名：func_<8 位十六进制哈希>（主函数/ main 保持为 main）
// ---------------------------------------------------------------------------
std::string CodeGen::makeFuncCName(const std::string& cnName)
{
    if (cnName == "main" || cnName == "Main" || cnName == "主函数")
        return "main";

    char buf[32];
    std::snprintf(buf, sizeof(buf), "func_%08x", quickHash(cnName));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 收集所有函数的中文名 -> 英文名映射（generate 开头调用一次）
// ---------------------------------------------------------------------------
void CodeGen::collectFuncNames()
{
    for (size_t i = 0; i < prog_->funcs.size(); ++i) {
        const std::string& n = prog_->funcs[i]->name;
        funcRet_[n] = prog_->funcs[i]->retType;
        funcCName_[n] = makeFuncCName(n);
    }
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
std::string CodeGen::cNameOfVar(const std::string& n) const
{
    for (int i = (int)scopes_.size() - 1; i >= 0; --i) {
        std::unordered_map<std::string, std::string>::const_iterator it =
            scopes_[i].cnames.find(n);
        if (it != scopes_[i].cnames.end()) return it->second;
    }
    return n;   // 未登记时退回原名（正常流程不会发生）
}

std::string CodeGen::cNameOfFunc(const std::string& n) const
{
    std::unordered_map<std::string, std::string>::const_iterator it =
        funcCName_.find(n);
    return it != funcCName_.end() ? it->second : n;
}

// ============================================================================
// 基础工具
// ============================================================================
CodeGen::CodeGen(std::shared_ptr<Program> prog) : prog_(prog) {}

void CodeGen::emitLine(const std::string& s)
{
    out_ << ind_ << s << "\n";
}

std::string CodeGen::escapeCString(const std::string& s) const
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\a': out += "\\a"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\v': out += "\\v"; break;
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
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

std::string CodeGen::cTypeOf(const std::string& langType) const
{
    if (langType == "整型" || langType == "整数")   return "int";
    if (langType == "长整型" || langType == "长整数") return "long";
    if (langType == "浮点" || langType == "小数")   return "double";
    if (langType == "字符")                           return "int";
    if (langType == "空")                             return "void";
    return "int";
}
// ---------------------------------------------------------------------------
// 作用域管理
// ---------------------------------------------------------------------------
void CodeGen::declareVar(const std::string& n, const std::string& t)
{
    if (scopes_.empty()) scopes_.push_back(Scope());
    scopes_.back().vars[n] = t;
    scopes_.back().cnames[n] = makeVarCName(t, n);
}

std::string CodeGen::typeOfVar(const std::string& n) const
{
    for (int i = (int)scopes_.size() - 1; i >= 0; --i) {
        std::unordered_map<std::string, std::string>::const_iterator it =
            scopes_[i].vars.find(n);
        if (it != scopes_[i].vars.end()) return it->second;
    }
    return "整型";
}

// ---------------------------------------------------------------------------
// 类型推导
// ---------------------------------------------------------------------------
std::string CodeGen::inferType(const ExprPtr& e) const
{
    if (std::dynamic_pointer_cast<IntLit>(e) ||
        std::dynamic_pointer_cast<CharLit>(e))
        return "整型";
    if (std::dynamic_pointer_cast<FloatLit>(e)) return "浮点";
    if (std::dynamic_pointer_cast<StrLit>(e))   return "字符串";

    if (std::shared_ptr<IdentExpr> p = std::dynamic_pointer_cast<IdentExpr>(e))
        return typeOfVar(p->name);

    if (std::shared_ptr<IndexExpr> p = std::dynamic_pointer_cast<IndexExpr>(e)) {
        if (std::shared_ptr<IdentExpr> id =
            std::dynamic_pointer_cast<IdentExpr>(p->base))
            return typeOfVar(id->name);
        return "整型";
    }
    if (std::shared_ptr<BinaryExpr> p = std::dynamic_pointer_cast<BinaryExpr>(e)) {
        if (p->op == Tok::EQ || p->op == Tok::NE || p->op == Tok::LT ||
            p->op == Tok::GT || p->op == Tok::LE || p->op == Tok::GE ||
            p->op == Tok::ANDAND || p->op == Tok::OROR)
            return "整型";
        if (inferType(p->l) == "浮点" || inferType(p->r) == "浮点")
            return "浮点";
        return "整型";
    }
    if (std::shared_ptr<UnaryExpr> p = std::dynamic_pointer_cast<UnaryExpr>(e)) {
        if (p->op == Tok::NOT) return "整型";
        return inferType(p->e);
    }
    if (std::shared_ptr<AssignExpr> p = std::dynamic_pointer_cast<AssignExpr>(e))
        return inferType(p->target);
    if (std::shared_ptr<TernaryExpr> p = std::dynamic_pointer_cast<TernaryExpr>(e)) {
        if (inferType(p->a) == "浮点" || inferType(p->b) == "浮点")
            return "浮点";
        return inferType(p->a);
    }
    if (std::shared_ptr<CallExpr> p = std::dynamic_pointer_cast<CallExpr>(e)) {
        std::unordered_map<std::string, std::string>::const_iterator it =
            funcRet_.find(p->name);
        if (it != funcRet_.end()) return it->second;
    }
    return "整型";
}

// ============================================================================
// 表达式
// ============================================================================
std::string CodeGen::genExpr(const ExprPtr& e)
{
    if (std::shared_ptr<IntLit> p = std::dynamic_pointer_cast<IntLit>(e))
        return std::to_string(p->v); 

    if (std::shared_ptr<FloatLit> p = std::dynamic_pointer_cast<FloatLit>(e)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", p->v);
        return std::string(buf);
    }
    if (std::shared_ptr<CharLit> p = std::dynamic_pointer_cast<CharLit>(e)) {
        char buf[32];
        int v = p->v;
        if (v >= 32 && v < 127 && v != '\'' && v != '\\')
            std::snprintf(buf, sizeof(buf), "'%c'", (char)v);
        else
            std::snprintf(buf, sizeof(buf), "%d", v);  
        return std::string(buf);
    }
    if (std::shared_ptr<StrLit> p = std::dynamic_pointer_cast<StrLit>(e))
        return "\"" + escapeCString(p->v) + "\"";

    // 标识符转成 C 英文名
    if (std::shared_ptr<IdentExpr> p = std::dynamic_pointer_cast<IdentExpr>(e))
        return cNameOfVar(p->name);

    if (std::shared_ptr<BinaryExpr> p = std::dynamic_pointer_cast<BinaryExpr>(e)) {
        std::string op;
        switch (p->op) {
        case Tok::PLUS:    op = "+";  break;
        case Tok::MINUS:   op = "-";  break;
        case Tok::STAR:    op = "*";  break;
        case Tok::SLASH:   op = "/";  break;
        case Tok::PERCENT: op = "%";  break;
        case Tok::EQ:      op = "=="; break;
        case Tok::NE:      op = "!="; break;
        case Tok::LT:      op = "<";  break;
        case Tok::GT:      op = ">";  break;
        case Tok::LE:      op = "<="; break;
        case Tok::GE:      op = ">="; break;
        case Tok::ANDAND:  op = "&&"; break;
        case Tok::OROR:    op = "||"; break;
        default: op = "?"; break;
        }
        return "(" + genExpr(p->l) + " " + op + " " + genExpr(p->r) + ")";
    }
    if (std::shared_ptr<UnaryExpr> p = std::dynamic_pointer_cast<UnaryExpr>(e)) {
        std::string op;
        switch (p->op) {
        case Tok::MINUS: op = "-";  break;
        case Tok::PLUS:  op = "+";  break;
        case Tok::NOT:   op = "!";  break;
        case Tok::INC:   op = "++"; break;
        case Tok::DEC:   op = "--"; break;
        default: op = "?"; break;
        }
        if (p->prefix) return "(" + op + genExpr(p->e) + ")";
        return "(" + genExpr(p->e) + op + ")";
    }
    if (std::shared_ptr<AssignExpr> p = std::dynamic_pointer_cast<AssignExpr>(e)) {
        std::string op;
        switch (p->op) {
        case Tok::ASSIGN:    op = "=";  break;
        case Tok::PLUSEQ:    op = "+="; break;
        case Tok::MINUSEQ:   op = "-="; break;
        case Tok::STAREQ:    op = "*="; break;
        case Tok::SLASHEQ:   op = "/="; break;
        case Tok::PERCENTEQ: op = "%="; break;
        default: op = "="; break;
        }
        return "(" + genExpr(p->target) + " " + op + " " + genExpr(p->value) + ")";
    }
    if (std::shared_ptr<IndexExpr> p = std::dynamic_pointer_cast<IndexExpr>(e))
        return genExpr(p->base) + "[" + genExpr(p->index) + "]";

    if (std::shared_ptr<TernaryExpr> p = std::dynamic_pointer_cast<TernaryExpr>(e))
        return "(" + genExpr(p->c) + " ? " + genExpr(p->a) + " : " + genExpr(p->b) + ")";

    // ★ 函数调用：内建保持，用户函数走 func_哈希
    if (std::shared_ptr<CallExpr> p = std::dynamic_pointer_cast<CallExpr>(e)) {
        if (p->name == "打印" || p->name == "输出" || p->name == "printf" ||
            p->name == "输入" || p->name == "scanf")
            return "0";   // 只能出现在语句位置

        std::string s = cNameOfFunc(p->name) + "(";
        for (size_t i = 0; i < p->args.size(); ++i) {
            if (i) s += ", ";
            s += genExpr(p->args[i]);
        }
        s += ")";
        return s;
    }
    return "0";
}

// ============================================================================
// 打印 / 输入
// ============================================================================
void CodeGen::genPrint(const std::shared_ptr<CallExpr>& c)
{
    if (c->args.empty()) return;

    std::shared_ptr<StrLit> firstStr = std::dynamic_pointer_cast<StrLit>(c->args[0]);

    if (firstStr) {
        if (c->args.size() == 1) {
            emitLine("printf(\"%s\", \"" + escapeCString(firstStr->v) + "\");");
            return;
        }

        const std::string& fmt = firstStr->v;
        std::string newFmt;
        std::vector<bool> isCharConv;

        for (size_t i = 0; i < fmt.size(); ++i) {
            char ch = fmt[i];
            if (ch != '%') { newFmt += ch; continue; }
            newFmt += '%';
            ++i;
            if (i >= fmt.size()) break;

            while (i < fmt.size() &&
                (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '0'))
                newFmt += fmt[i++];
            while (i < fmt.size() && std::isdigit((unsigned char)fmt[i]))
                newFmt += fmt[i++];
            if (i < fmt.size() && fmt[i] == '.') {
                newFmt += fmt[i++];
                while (i < fmt.size() && std::isdigit((unsigned char)fmt[i]))
                    newFmt += fmt[i++];
            }
            if (i >= fmt.size()) break;

            while (i < fmt.size() &&
                (fmt[i] == 'h' || fmt[i] == 'l' || fmt[i] == 'L' ||
                    fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't')) ++i;
            if (i >= fmt.size()) break;

            char conv = fmt[i];
            if (conv == '%') { newFmt += '%'; continue; }

            if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                newFmt += "ll";
                newFmt += conv;
            }
            else newFmt += conv;

            isCharConv.push_back(conv == 'c');
        }

        std::string line = "printf(\"" + escapeCString(newFmt) + "\"";
        for (size_t i = 1; i < c->args.size(); ++i) {
            std::string ex = genExpr(c->args[i]);
            if (i - 1 < isCharConv.size() && isCharConv[i - 1])
                ex = "(int)(" + ex + ")";
            line += ", " + ex;
        }
        line += ");";
        emitLine(line);
        return;
    }

    std::string line;
    for (size_t i = 0; i < c->args.size(); ++i) {
        if (i > 0) line += "putchar(' '); ";
        std::string t = inferType(c->args[i]);
        std::string ex = genExpr(c->args[i]);
        if (t == "浮点")
            line += "printf(\"%g\", (double)(" + ex + ")); ";
        else if (t == "字符串")
            line += "printf(\"%s\", " + ex + "); ";
        else
            line += "printf(\"%lld\", (long long)(" + ex + ")); ";
    }
    emitLine(line);
}

void CodeGen::genInput(const std::shared_ptr<CallExpr>& c)
{
    std::string line;
    for (size_t i = 0; i < c->args.size(); ++i) {
        std::shared_ptr<IdentExpr> id = std::dynamic_pointer_cast<IdentExpr>(c->args[i]);
        if (!id) continue;
        std::string t = typeOfVar(id->name);
        std::string cn = cNameOfVar(id->name);   // ★ 转英文名
        if (t == "浮点" || t == "小数")
            line += "scanf(\"%lf\", &" + cn + "); ";
        else
            line += "scanf(\"%lld\", &" + cn + "); ";
    }
    emitLine(line);
}

// ============================================================================
// 语句
// ============================================================================
void CodeGen::genStmtBody(const StmtPtr& s)
{
    if (std::shared_ptr<BlockStmt> b = std::dynamic_pointer_cast<BlockStmt>(s)) {
        for (size_t i = 0; i < b->list.size(); ++i) genStmt(b->list[i]);
    }
    else if (s) {
        genStmt(s);
    }
}

void CodeGen::genIfStmt(const std::shared_ptr<IfStmt>& s)
{
    std::vector<std::pair<ExprPtr, StmtPtr> > branches;
    StmtPtr elseStmt;

    std::shared_ptr<IfStmt> cur = s;
    while (cur) {
        branches.push_back(std::make_pair(cur->cond, cur->thenS));
        if (cur->elseS) {
            std::shared_ptr<IfStmt> nextIf =
                std::dynamic_pointer_cast<IfStmt>(cur->elseS);
            if (nextIf) cur = nextIf;
            else { elseStmt = cur->elseS; cur = nullptr; }
        }
        else cur = nullptr;
    }

    for (size_t i = 0; i < branches.size(); ++i) {
        std::string prefix = (i == 0) ? "if (" : "} else if (";
        emitLine(prefix + genExpr(branches[i].first) + ") {");
        push(); pushScope();
        genStmtBody(branches[i].second);
        popScope(); pop();
    }
    if (elseStmt) {
        emitLine("} else {");
        push(); pushScope();
        genStmtBody(elseStmt);
        popScope(); pop();
    }
    emitLine("}");
}

void CodeGen::genStmt(const StmtPtr& s)
{
    if (!s) return;

    if (std::dynamic_pointer_cast<EmptyStmt>(s)) { emitLine(";"); return; }

    if (std::shared_ptr<BlockStmt> p = std::dynamic_pointer_cast<BlockStmt>(s)) {
        emitLine("{");
        push(); pushScope();
        for (size_t i = 0; i < p->list.size(); ++i) genStmt(p->list[i]);
        popScope(); pop();
        emitLine("}");
        return;
    }

    // ★ 变量声明
    if (std::shared_ptr<VarDeclStmt> p = std::dynamic_pointer_cast<VarDeclStmt>(s)) {
        std::string cname = makeVarCName(p->type, p->name);  // 先算好英文名

        std::string line;
        if (p->isConst) line += "const ";
        line += cTypeOf(p->type) + " " + cname;

        if (p->isArray) line += "[" + std::to_string(p->arraySize) + "]";

        if (p->init) {
            // 先登记，再生成表达式（自引用场景）
            declareVar(p->name, p->type);
            line += " = " + genExpr(p->init);
        }
        else if (p->isArray && !p->initList.empty()) {
            declareVar(p->name, p->type);
            line += " = {";
            for (size_t i = 0; i < p->initList.size(); ++i) {
                if (i) line += ", ";
                line += genExpr(p->initList[i]);
            }
            line += "}";
        }
        line += ";";
        emitLine(line);
        declareVar(p->name, p->type);   // 幂等登记
        return;
    }

    if (std::shared_ptr<IfStmt> p = std::dynamic_pointer_cast<IfStmt>(s)) {
        genIfStmt(p);
        return;
    }

    if (std::shared_ptr<WhileStmt> p = std::dynamic_pointer_cast<WhileStmt>(s)) {
        emitLine("while (" + genExpr(p->cond) + ") {");
        push(); pushScope();
        genStmtBody(p->body);
        popScope(); pop();
        emitLine("}");
        return;
    }

    if (std::shared_ptr<ForStmt> p = std::dynamic_pointer_cast<ForStmt>(s)) {
        std::string initStr, condStr, stepStr;

        // for-init 里的 VarDeclStmt 需要先把变量登记好，再生成条件/步进
        pushScope();   // 为 for 循环单独开一个作用域

        if (p->init) {
            if (std::shared_ptr<ExprStmt> es = std::dynamic_pointer_cast<ExprStmt>(p->init))
                initStr = genExpr(es->expr);
            else if (std::shared_ptr<VarDeclStmt> vd =
                std::dynamic_pointer_cast<VarDeclStmt>(p->init)) {
                std::string cn = makeVarCName(vd->type, vd->name);
                std::string s2 = cTypeOf(vd->type) + " " + cn;
                declareVar(vd->name, vd->type);
                if (vd->init) s2 += " = " + genExpr(vd->init);
                initStr = s2;
            }
        }
        if (p->cond) condStr = genExpr(p->cond);
        if (p->step) stepStr = genExpr(p->step);

        emitLine("for (" + initStr + "; " + condStr + "; " + stepStr + ") {");
        push();
        genStmtBody(p->body);
        pop();
        emitLine("}");
        popScope();
        return;
    }

    if (std::shared_ptr<ReturnStmt> p = std::dynamic_pointer_cast<ReturnStmt>(s)) {
        if (p->value) emitLine("return " + genExpr(p->value) + ";");
        else          emitLine("return;");
        return;
    }
    if (std::dynamic_pointer_cast<BreakStmt>(s)) { emitLine("break;");    return; }
    if (std::dynamic_pointer_cast<ContinueStmt>(s)) { emitLine("continue;"); return; }

    if (std::shared_ptr<ExprStmt> p = std::dynamic_pointer_cast<ExprStmt>(s)) {
        if (std::shared_ptr<CallExpr> call =
            std::dynamic_pointer_cast<CallExpr>(p->expr)) {
            if (call->name == "打印" || call->name == "输出" || call->name == "printf") {
                genPrint(call);
                return;
            }
            if (call->name == "输入" || call->name == "scanf") {
                genInput(call);
                return;
            }
        }
        emitLine(genExpr(p->expr) + ";");
        return;
    }
}

// ============================================================================
// 函数
// ============================================================================
void CodeGen::genFunction(const std::shared_ptr<FuncDecl>& fn)
{
    std::string cname = cNameOfFunc(fn->name);   // ★ 中文函数名 → func_哈希 / main
    std::string retType;

    if (cname == "main") retType = "int";
    else                 retType = cTypeOf(fn->retType);

    // 参数名也映射
    std::string sig = retType + " " + cname + "(";
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) sig += ", ";
        sig += cTypeOf(fn->params[i].type) + " " +
            makeVarCName(fn->params[i].type, fn->params[i].name);
    }
    if (fn->params.empty() && cname == "main") sig += "void";
    sig += ") {";
    emitLine(sig);

    push(); pushScope();
    for (size_t i = 0; i < fn->params.size(); ++i)
        declareVar(fn->params[i].name, fn->params[i].type);
    for (size_t i = 0; i < fn->body->list.size(); ++i)
        genStmt(fn->body->list[i]);
    popScope(); pop();

    emitLine("}");
    emitLine("");
}

// ============================================================================
// 程序整体
// ============================================================================
std::string CodeGen::generate()
{
    collectFuncNames();            // ★ 先建立函数名映射
    scopes_.push_back(Scope());    // 全局作用域

    out_ << "#include <stdio.h>\n";
    out_ << "#include <stdlib.h>\n";
    out_ << "#include <string.h>\n\n";

    // 函数原型
    for (size_t i = 0; i < prog_->funcs.size(); ++i) {
        std::shared_ptr<FuncDecl> fn = prog_->funcs[i];
        std::string cname = cNameOfFunc(fn->name);
        std::string retType = (cname == "main") ? "int" : cTypeOf(fn->retType);

        out_ << retType << " " << cname << "(";
        for (size_t j = 0; j < fn->params.size(); ++j) {
            if (j) out_ << ", ";
            out_ << cTypeOf(fn->params[j].type) << " "
                << makeVarCName(fn->params[j].type, fn->params[j].name);
        }
        if (fn->params.empty() && cname == "main") out_ << "void";
        out_ << ");\n";
    }
    out_ << "\n";

    // 全局变量
    for (size_t i = 0; i < prog_->globals.size(); ++i)
        genStmt(prog_->globals[i]);
    if (!prog_->globals.empty()) out_ << "\n";

    // 函数体
    for (size_t i = 0; i < prog_->funcs.size(); ++i)
        genFunction(prog_->funcs[i]);

    return out_.str();
}