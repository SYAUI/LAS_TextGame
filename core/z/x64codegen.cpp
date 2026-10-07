// x64codegen.cpp
#include "x64codegen.h"
#include <cstring>

// ============================================================================
// 工具
// ============================================================================
uint32_t X64CodeGen::hash(const std::string& s)
{
    uint32_t h = 5381u;
    for (size_t i = 0; i < s.size(); ++i)
        h = h * 33u + (unsigned char)s[i];
    return h;
}

std::string X64CodeGen::funcCName(const std::string& cnName)
{
    if (cnName == "main" || cnName == "Main" || cnName == "主函数")
        return "main";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "func_%08x", hash(cnName));
    return std::string(buf);
}

std::string X64CodeGen::varCName(const std::string& cnName)
{
    // 仅用于调试显示，本后端不对外暴露
    char buf[32];
    std::snprintf(buf, sizeof(buf), "v_%08x", hash(cnName));
    return std::string(buf);
}

std::string X64CodeGen::cTypeOf(const std::string& t) const
{
    if (t == "整型" || t == "整数")     return "int";
    if (t == "长整型" || t == "长整数") return "long";
    if (t == "浮点" || t == "小数")     return "double";
    if (t == "字符")                     return "int";
    if (t == "空")                       return "void";
    return "int";
}

std::string X64CodeGen::inferType(const ExprPtr& e) const
{
    if (std::dynamic_pointer_cast<IntLit>(e) ||
        std::dynamic_pointer_cast<CharLit>(e))
        return "整型";
    if (std::dynamic_pointer_cast<FloatLit>(e)) return "浮点";
    if (std::dynamic_pointer_cast<StrLit>(e))   return "字符串";

    if (std::shared_ptr<IdentExpr> p = std::dynamic_pointer_cast<IdentExpr>(e))
        return typeOfName(p->name);

    if (std::shared_ptr<IndexExpr> p = std::dynamic_pointer_cast<IndexExpr>(e)) {
        if (std::shared_ptr<IdentExpr> id =
            std::dynamic_pointer_cast<IdentExpr>(p->base))
            return typeOfName(id->name);
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

void X64CodeGen::emit(const std::string& s) { text_ << "    " << s << "\n"; }
void X64CodeGen::emitL(const std::string& s) { text_ << s << ":\n"; }

std::string X64CodeGen::newLabel(const std::string& pre)
{
    return ".L" + pre + std::to_string(labelCounter_++);
}

std::string X64CodeGen::escapeAsm(const std::string& s) const
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        case '\r': out += "\\r";  break;
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        default:
            if (c < 0x20 || c >= 0x7F) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\%03o", c);
                out += buf;
            }
            else out += (char)c;
        }
    }
    return out;
}

std::string X64CodeGen::addString(const std::string& s)
{
    std::string label = ".Lstr" + std::to_string(strCounter_++);
    data_ << label << ":\n";
    data_ << "    .asciz \"" << escapeAsm(s) << "\"\n";
    return label;
}

// ============================================================================
// 栈帧
// ============================================================================
int X64CodeGen::allocLocal(const std::string& name, const std::string& type, int count)
{
    int bytes = 8 * (count < 1 ? 1 : count);
    int off = stackUsed_ + bytes;
    stackUsed_ = off;
    localOffsets_[name] = off;
    localTypes_[name] = type;
    return off;
}

int X64CodeGen::offsetOf(const std::string& name) const
{
    std::unordered_map<std::string, int>::const_iterator it = localOffsets_.find(name);
    if (it == localOffsets_.end())
        throw std::runtime_error("未定义的变量: " + name);
    return it->second;
}

std::string X64CodeGen::typeOfName(const std::string& name) const
{
    std::unordered_map<std::string, std::string>::const_iterator it = localTypes_.find(name);
    return it != localTypes_.end() ? it->second : "整型";
}

bool X64CodeGen::isLocal(const std::string& name) const
{
    return localOffsets_.find(name) != localOffsets_.end();
}

void X64CodeGen::preScanStmt(const StmtPtr& s)
{
    if (!s) return;

    if (std::shared_ptr<BlockStmt> p = std::dynamic_pointer_cast<BlockStmt>(s)) {
        for (size_t i = 0; i < p->list.size(); ++i) preScanStmt(p->list[i]);
        return;
    }
    if (std::shared_ptr<VarDeclStmt> p = std::dynamic_pointer_cast<VarDeclStmt>(s)) {
        int cnt = p->isArray ? p->arraySize : 1;
        if (cnt < 1) cnt = 1;
        allocLocal(p->name, p->type, cnt);
        return;
    }
    if (std::shared_ptr<IfStmt> p = std::dynamic_pointer_cast<IfStmt>(s)) {
        preScanStmt(p->thenS); preScanStmt(p->elseS);
        return;
    }
    if (std::shared_ptr<WhileStmt> p = std::dynamic_pointer_cast<WhileStmt>(s)) {
        preScanStmt(p->body); return;
    }
    if (std::shared_ptr<ForStmt> p = std::dynamic_pointer_cast<ForStmt>(s)) {
        if (std::shared_ptr<VarDeclStmt> vd =
            std::dynamic_pointer_cast<VarDeclStmt>(p->init)) {
            int cnt = vd->isArray ? vd->arraySize : 1;
            if (cnt < 1) cnt = 1;
            allocLocal(vd->name, vd->type, cnt);
        }
        preScanStmt(p->body);
        return;
    }
}

// ============================================================================
// 表达式
// ============================================================================
void X64CodeGen::genExprInt(const ExprPtr& e)
{
    genExpr(e);
    if (inferType(e) == "浮点")
        emit("cvttsd2si rax, xmm0");
}

void X64CodeGen::genExprFloat(const ExprPtr& e)
{
    genExpr(e);
    if (inferType(e) != "浮点")
        emit("cvtsi2sd xmm0, rax");
}

void X64CodeGen::genExpr(const ExprPtr& e)
{
    if (!e) { emit("xor eax, eax"); return; }

    // ---- 字面量 ----
    if (std::shared_ptr<IntLit> p = std::dynamic_pointer_cast<IntLit>(e)) {
        emit("mov rax, " + std::to_string(p->v));
        return;
    }
    if (std::shared_ptr<CharLit> p = std::dynamic_pointer_cast<CharLit>(e)) {
        emit("mov rax, " + std::to_string((long long)p->v));
        return;
    }
    if (std::shared_ptr<FloatLit> p = std::dynamic_pointer_cast<FloatLit>(e)) {
        // 用位模式加载双精度常量
        uint64_t bits;
        std::memcpy(&bits, &p->v, 8);
        emit("mov rax, " + std::to_string((long long)bits));
        emit("movq xmm0, rax");
        return;
    }
    if (std::shared_ptr<StrLit> p = std::dynamic_pointer_cast<StrLit>(e)) {
        std::string lbl = addString(p->v);
        emit("lea rax, [rip + " + lbl + "]");
        return;
    }

    // ---- 标识符 ----
    if (std::shared_ptr<IdentExpr> p = std::dynamic_pointer_cast<IdentExpr>(e)) {
        int off = offsetOf(p->name);
        if (typeOfName(p->name) == "浮点")
            emit("movsd xmm0, qword ptr [rbp - " + std::to_string(off) + "]");
        else
            emit("mov rax, qword ptr [rbp - " + std::to_string(off) + "]");
        return;
    }

    // ---- 数组读取 ----
    if (std::shared_ptr<IndexExpr> p = std::dynamic_pointer_cast<IndexExpr>(e)) {
        genIndexRead(p);
        return;
    }

    // ---- 二元 ----
    if (std::shared_ptr<BinaryExpr> p = std::dynamic_pointer_cast<BinaryExpr>(e)) {
        genBinary(p);
        return;
    }
    // ---- 一元 ----
    if (std::shared_ptr<UnaryExpr> p = std::dynamic_pointer_cast<UnaryExpr>(e)) {
        genUnary(p);
        return;
    }
    // ---- 赋值 ----
    if (std::shared_ptr<AssignExpr> p = std::dynamic_pointer_cast<AssignExpr>(e)) {
        genAssign(p);
        return;
    }
    // ---- 三元 ----
    if (std::shared_ptr<TernaryExpr> p = std::dynamic_pointer_cast<TernaryExpr>(e)) {
        genTernary(p);
        return;
    }
    // ---- 调用 ----
    if (std::shared_ptr<CallExpr> p = std::dynamic_pointer_cast<CallExpr>(e)) {
        // 内建不应出现在表达式位置
        genUserCall(p);
        return;
    }

    emit("xor eax, eax");
}

// ---------------------------------------------------------------------------
// 二元运算
// ---------------------------------------------------------------------------
void X64CodeGen::genBinary(const std::shared_ptr<BinaryExpr>& p)
{
    // 逻辑运算：短路求值
    if (p->op == Tok::ANDAND || p->op == Tok::OROR) {
        std::string endLbl = newLabel("lg_end");
        genExprInt(p->l);
        emit("test rax, rax");
        if (p->op == Tok::ANDAND) {
            emit("jz " + endLbl);            // 左为假 → 结果为 0（rax 已是 0）
        }
        else {
            std::string trueLbl = newLabel("lg_true");
            emit("jnz " + trueLbl);          // 左为真 → 结果为 1
            genExprInt(p->r);
            emit("test rax, rax");
            emit("setnz al");
            emit("movzx rax, al");
            emit("jmp " + endLbl);
            emitL(trueLbl);
            emit("mov rax, 1");
        }
        if (p->op == Tok::ANDAND) {
            genExprInt(p->r);
            emit("test rax, rax");
            emit("setnz al");
            emit("movzx rax, al");
        }
        emitL(endLbl);
        return;
    }

    std::string lt = inferType(p->l);
    std::string rt = inferType(p->r);
    bool useFloat = (lt == "浮点" || rt == "浮点");

    // 比较运算总是整型结果
    bool isCmp = (p->op == Tok::EQ || p->op == Tok::NE || p->op == Tok::LT ||
        p->op == Tok::GT || p->op == Tok::LE || p->op == Tok::GE);

    if (useFloat) {
        // 浮点：左放 xmm1，右在 xmm0，结果 → xmm0（或比较后 rax）
        genExprFloat(p->l);
        emit("sub rsp, 8");
        emit("movsd qword ptr [rsp], xmm0");
        genExprFloat(p->r);
        emit("movsd xmm1, qword ptr [rsp]");
        emit("add rsp, 8");
        // 现在 xmm1 = 左，xmm0 = 右

        if (isCmp) {
            emit("ucomisd xmm1, xmm0");
            const char* cc = "";
            switch (p->op) {
            case Tok::EQ: cc = "sete";  break;
            case Tok::NE: cc = "setne"; break;
            case Tok::LT: cc = "setb";  break;
            case Tok::GT: cc = "seta";  break;
            case Tok::LE: cc = "setbe"; break;
            case Tok::GE: cc = "setae"; break;
            default: cc = "sete"; break;
            }
            emit(std::string(cc) + " al");
            emit("movzx rax, al");
            return;
        }
        switch (p->op) {
        case Tok::PLUS:  emit("addsd xmm1, xmm0"); break;
        case Tok::MINUS: emit("subsd xmm1, xmm0"); break;
        case Tok::STAR:  emit("mulsd xmm1, xmm0"); break;
        case Tok::SLASH: emit("divsd xmm1, xmm0"); break;
        default: throw std::runtime_error("浮点不支持的运算符");
        }
        emit("movsd xmm0, xmm1");
        return;
    }

    // 整型：左 push，右计算，pop 到 rcx
    genExprInt(p->l);
    emit("push rax");
    genExprInt(p->r);
    emit("pop rcx");
    // rcx = 左，rax = 右

    if (isCmp) {
        emit("cmp rcx, rax");
        const char* cc = "";
        switch (p->op) {
        case Tok::EQ: cc = "sete";  break;
        case Tok::NE: cc = "setne"; break;
        case Tok::LT: cc = "setl";  break;
        case Tok::GT: cc = "setg";  break;
        case Tok::LE: cc = "setle"; break;
        case Tok::GE: cc = "setge"; break;
        default: cc = "sete"; break;
        }
        emit(std::string(cc) + " al");
        emit("movzx rax, al");
        return;
    }
    switch (p->op) {
    case Tok::PLUS:  emit("add rax, rcx"); break;
    case Tok::MINUS: emit("sub rcx, rax"); emit("mov rax, rcx"); break;
    case Tok::STAR:  emit("imul rax, rcx"); break;
    case Tok::SLASH:
        emit("mov r8, rax");   // r8 = 右
        emit("mov rax, rcx");  // rax = 左
        emit("cqo");           // 符号扩展到 rdx:rax
        emit("idiv r8");
        break;
    case Tok::PERCENT:
        emit("mov r8, rax");
        emit("mov rax, rcx");
        emit("cqo");
        emit("idiv r8");
        emit("mov rax, rdx");
        break;
    default:
        throw std::runtime_error("不支持的二元运算符");
    }
}

// ---------------------------------------------------------------------------
// 一元运算
// ---------------------------------------------------------------------------
void X64CodeGen::genUnary(const std::shared_ptr<UnaryExpr>& p)
{
    if (p->op == Tok::INC || p->op == Tok::DEC) {
        std::shared_ptr<IdentExpr> id = std::dynamic_pointer_cast<IdentExpr>(p->e);
        if (!id) throw std::runtime_error("++/-- 只支持变量");
        int off = offsetOf(id->name);
        std::string delta = (p->op == Tok::INC) ? "1" : "-1";

        if (typeOfName(id->name) == "浮点") {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", (p->op == Tok::INC) ? 1.0 : -1.0);
            uint64_t bits;
            double v = (p->op == Tok::INC) ? 1.0 : -1.0;
            std::memcpy(&bits, &v, 8);
            emit("mov rax, " + std::to_string((long long)bits));
            emit("movq xmm1, rax");
            if (p->prefix) {
                emit("movsd xmm0, qword ptr [rbp - " + std::to_string(off) + "]");
                emit("addsd xmm0, xmm1");
                emit("movsd qword ptr [rbp - " + std::to_string(off) + "], xmm0");
            }
            else {
                emit("movsd xmm0, qword ptr [rbp - " + std::to_string(off) + "]");
                emit("movsd xmm2, xmm0");
                emit("addsd xmm2, xmm1");
                emit("movsd qword ptr [rbp - " + std::to_string(off) + "], xmm2");
            }
            return;
        }

        emit("mov rax, qword ptr [rbp - " + std::to_string(off) + "]");
        if (p->prefix) {
            emit("add rax, " + delta);
            emit("mov qword ptr [rbp - " + std::to_string(off) + "], rax");
        }
        else {
            emit("mov rcx, rax");
            emit("add rcx, " + delta);
            emit("mov qword ptr [rbp - " + std::to_string(off) + "], rcx");
        }
        return;
    }

    if (p->op == Tok::MINUS) {
        if (inferType(p->e) == "浮点") {
            genExprFloat(p->e);
            // 取反：与符号位异或
            emit("mov rax, 0x8000000000000000");
            emit("movq xmm1, rax");
            emit("xorpd xmm0, xmm1");
        }
        else {
            genExprInt(p->e);
            emit("neg rax");
        }
        return;
    }
    if (p->op == Tok::NOT) {
        genExprInt(p->e);
        emit("test rax, rax");
        emit("sete al");
        emit("movzx rax, al");
        return;
    }
    if (p->op == Tok::PLUS) { genExpr(p->e); return; }

    throw std::runtime_error("不支持的一元运算符");
}

// ---------------------------------------------------------------------------
// 赋值
// ---------------------------------------------------------------------------
void X64CodeGen::genAssign(const std::shared_ptr<AssignExpr>& p)
{
    // 简单情形：左值是变量
    std::shared_ptr<IdentExpr> id = std::dynamic_pointer_cast<IdentExpr>(p->target);
    if (!id) {
        // 数组元素赋值
        std::shared_ptr<IndexExpr> ix =
            std::dynamic_pointer_cast<IndexExpr>(p->target);
        if (!ix) throw std::runtime_error("不支持的赋值目标");
        std::shared_ptr<IdentExpr> base =
            std::dynamic_pointer_cast<IdentExpr>(ix->base);
        if (!base) throw std::runtime_error("不支持的数组访问");

        int off = offsetOf(base->name);
        // 计算下标
        genExprInt(ix->index);
        emit("push rax");
        // 计算右值
        genExprInt(p->value);
        emit("pop rcx");                  // rcx = 下标
        // 数组元素位置：rbp - off + 8*idx（off 是数组最高地址）
        emit("lea r10, [rbp - " + std::to_string(off) + "]");
        emit("mov qword ptr [r10 + rcx*8], rax");
        return;
    }

    int off = offsetOf(id->name);
    std::string lt = typeOfName(id->name);

    if (p->op == Tok::ASSIGN) {
        if (lt == "浮点") {
            genExprFloat(p->value);
            emit("movsd qword ptr [rbp - " + std::to_string(off) + "], xmm0");
        }
        else {
            genExprInt(p->value);
            emit("mov qword ptr [rbp - " + std::to_string(off) + "], rax");
        }
        return;
    }

    // 复合赋值：先加载、运算、存储
    if (lt == "浮点") {
        // 加载左
        emit("movsd xmm2, qword ptr [rbp - " + std::to_string(off) + "]");
        // 保存左
        emit("sub rsp, 8");
        emit("movsd qword ptr [rsp], xmm2");
        genExprFloat(p->value);
        emit("movsd xmm1, qword ptr [rsp]");   // xmm1 = 左
        emit("add rsp, 8");
        switch (p->op) {
        case Tok::PLUSEQ:  emit("addsd xmm1, xmm0"); break;
        case Tok::MINUSEQ: emit("subsd xmm1, xmm0"); break;
        case Tok::STAREQ:  emit("mulsd xmm1, xmm0"); break;
        case Tok::SLASHEQ: emit("divsd xmm1, xmm0"); break;
        default: throw std::runtime_error("浮点不支持的复合赋值");
        }
        emit("movsd xmm0, xmm1");
        emit("movsd qword ptr [rbp - " + std::to_string(off) + "], xmm0");
        return;
    }

    // 整型复合
    emit("mov rcx, qword ptr [rbp - " + std::to_string(off) + "]");
    emit("push rcx");
    genExprInt(p->value);
    emit("pop rcx");
    switch (p->op) {
    case Tok::PLUSEQ:  emit("add rcx, rax"); break;
    case Tok::MINUSEQ: emit("sub rcx, rax"); break;
    case Tok::STAREQ:  emit("imul rcx, rax"); break;
    case Tok::SLASHEQ:
        emit("mov r8, rax");
        emit("mov rax, rcx");
        emit("cqo");
        emit("idiv r8");
        emit("mov rcx, rax");
        break;
    case Tok::PERCENTEQ:
        emit("mov r8, rax");
        emit("mov rax, rcx");
        emit("cqo");
        emit("idiv r8");
        emit("mov rcx, rdx");
        break;
    default: throw std::runtime_error("不支持的复合赋值");
    }
    emit("mov qword ptr [rbp - " + std::to_string(off) + "], rcx");
    emit("mov rax, rcx");
}

// ---------------------------------------------------------------------------
// 数组读取
// ---------------------------------------------------------------------------
void X64CodeGen::genIndexRead(const std::shared_ptr<IndexExpr>& p)
{
    std::shared_ptr<IdentExpr> base =
        std::dynamic_pointer_cast<IdentExpr>(p->base);
    if (!base) throw std::runtime_error("不支持的数组访问形式");

    int off = offsetOf(base->name);
    genExprInt(p->index);
    emit("lea r10, [rbp - " + std::to_string(off) + "]");
    emit("mov rax, qword ptr [r10 + rax*8]");
    // 若元素是浮点，还需要搬到 xmm0
    if (typeOfName(base->name) == "浮点")
        emit("movq xmm0, rax");
}

// ---------------------------------------------------------------------------
// 三元
// ---------------------------------------------------------------------------
void X64CodeGen::genTernary(const std::shared_ptr<TernaryExpr>& p)
{
    bool isF = (inferType(p->a) == "浮点" || inferType(p->b) == "浮点");
    std::string elseLbl = newLabel("tern_e");
    std::string endLbl = newLabel("tern_x");

    genExprInt(p->c);
    emit("test rax, rax");
    emit("jz " + elseLbl);

    if (isF) genExprFloat(p->a); else genExprInt(p->a);
    emit("jmp " + endLbl);

    emitL(elseLbl);
    if (isF) genExprFloat(p->b); else genExprInt(p->b);

    emitL(endLbl);
}

// ============================================================================
// 函数调用
// ============================================================================
void X64CodeGen::genUserCall(const std::shared_ptr<CallExpr>& c)
{
    int n = (int)c->args.size();
    if (n > 4)
        throw std::runtime_error("x64 后端暂不支持超过 4 个参数");

    // 从右到左算参数，push 到栈
    for (int i = n - 1; i >= 0; --i) {
        std::string t = inferType(c->args[i]);
        if (t == "浮点") {
            genExprFloat(c->args[i]);
            emit("movq rax, xmm0");
            emit("push rax");
        }
        else {
            genExprInt(c->args[i]);
            emit("push rax");
        }
    }

    // 预留 shadow space
    emit("sub rsp, 32");

    // 装载寄存器
    static const char* iregs[] = { "rcx", "rdx", "r8", "r9" };
    for (int i = 0; i < n; ++i) {
        std::string t = inferType(c->args[i]);
        int slotOff = 32 + 8 * (n - 1 - i);
        std::string src = "qword ptr [rsp + " + std::to_string(slotOff) + "]";
        if (t == "浮点") {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "xmm%d", i);
            emit("movsd " + std::string(buf) + ", " + src);
        }
        else {
            emit("mov " + std::string(iregs[i]) + ", " + src);
        }
    }

    // 调用
    std::string fname = funcCName(c->name);
    emit("call " + fname);

    // 清理参数 + shadow
    emit("add rsp, " + std::to_string(32 + n * 8));

    // 浮点返回值：xmm0 已经正确
    // 整型返回值：rax 已经正确
}

// ============================================================================
// 内建：打印
// ============================================================================
void X64CodeGen::genPrintCall(const std::shared_ptr<CallExpr>& c)
{
    if (c->args.empty()) return;

    std::shared_ptr<StrLit> firstStr =
        std::dynamic_pointer_cast<StrLit>(c->args[0]);

    // 无格式串：简单空格分隔
    if (!firstStr) {
        for (size_t i = 0; i < c->args.size(); ++i) {
            if (i > 0) {
                std::string sp = addString(" ");
                emit("lea rcx, [rip + " + sp + "]");
                emit("sub rsp, 32");
                emit("call printf");
                emit("add rsp, 32");
            }
            std::string t = inferType(c->args[i]);
            std::string fmt;
            if (t == "浮点") {
                fmt = addString("%f");
                genExprFloat(c->args[i]);
                emit("movsd xmm1, xmm0");
                emit("lea rcx, [rip + " + fmt + "]");
                emit("sub rsp, 32");
                emit("call printf");
                emit("add rsp, 32");
            }
            else if (t == "字符串") {
                fmt = addString("%s");
                genExpr(c->args[i]);
                emit("mov rdx, rax");
                emit("lea rcx, [rip + " + fmt + "]");
                emit("sub rsp, 32");
                emit("call printf");
                emit("add rsp, 32");
            }
            else {
                fmt = addString("%lld");
                genExprInt(c->args[i]);
                emit("mov rdx, rax");
                emit("lea rcx, [rip + " + fmt + "]");
                emit("sub rsp, 32");
                emit("call printf");
                emit("add rsp, 32");
            }
        }
        return;
    }

    // 单参数：直接打字符串
    if (c->args.size() == 1) {
        std::string lbl = addString(firstStr->v);
        emit("lea rcx, [rip + " + lbl + "]");
        emit("sub rsp, 32");
        emit("call printf");
        emit("add rsp, 32");
        return;
    }

    // 带格式串
    // 简化：把 %d/%i/%u/%x/%X/%o 全部替换成 %lld，参数用整型寄存器传
    // 浮点格式符 %f/%e/%g 保持原样，参数用 XMM
    const std::string& rawFmt = firstStr->v;
    std::string newFmt;
    std::vector<char> convs;   // 'i' 或 'f' 或 'c' 或 's'

    for (size_t i = 0; i < rawFmt.size(); ++i) {
        char ch = rawFmt[i];
        if (ch != '%') { newFmt += ch; continue; }
        newFmt += '%';
        ++i;
        if (i >= rawFmt.size()) break;

        // 标志、宽度、精度
        while (i < rawFmt.size() &&
            (rawFmt[i] == '-' || rawFmt[i] == '+' || rawFmt[i] == ' ' || rawFmt[i] == '#' || rawFmt[i] == '0'))
            newFmt += rawFmt[i++];
        while (i < rawFmt.size() && std::isdigit((unsigned char)rawFmt[i]))
            newFmt += rawFmt[i++];
        if (i < rawFmt.size() && rawFmt[i] == '.') {
            newFmt += rawFmt[i++];
            while (i < rawFmt.size() && std::isdigit((unsigned char)rawFmt[i]))
                newFmt += rawFmt[i++];
        }
        if (i >= rawFmt.size()) break;
        // 跳过长度修饰
        while (i < rawFmt.size() &&
            (rawFmt[i] == 'h' || rawFmt[i] == 'l' || rawFmt[i] == 'L' ||
                rawFmt[i] == 'z' || rawFmt[i] == 'j' || rawFmt[i] == 't')) ++i;
        if (i >= rawFmt.size()) break;
        char conv = rawFmt[i];
        if (conv == '%') { newFmt += '%'; continue; }

        switch (conv) {
        case 'd': case 'i': case 'u':
        case 'x': case 'X': case 'o':
            newFmt += "ll";
            newFmt += conv;
            convs.push_back('i');
            break;
        case 'f': case 'e': case 'g':
            newFmt += conv;
            convs.push_back('f');
            break;
        case 'c':
            newFmt += conv;
            convs.push_back('c');
            break;
        case 's':
            newFmt += conv;
            convs.push_back('s');
            break;
        default:
            newFmt += conv;
            convs.push_back('i');
        }
    }

    // 计算所有参数（从右到左压栈）
    int n = (int)(c->args.size() - 1);   // 参数个数
    for (int i = n; i >= 1; --i) {
        char kind = (i - 1 < (int)convs.size()) ? convs[i - 1] : 'i';
        if (kind == 'f') {
            genExprFloat(c->args[i]);
            emit("movq rax, xmm0");
            emit("push rax");
        }
        else {
            genExprInt(c->args[i]);
            emit("push rax");
        }
    }

    emit("sub rsp, 32");
    std::string fmtLbl = addString(newFmt);
    emit("lea rcx, [rip + " + fmtLbl + "]");

    // 装载参数：整数用 rdx/r8/r9；浮点用 xmm1/xmm2/xmm3
    // 参数位置 1 对应 rcx（已被格式串占用）
    int intSlot = 1;   // 下一个整型位置
    int fltSlot = 1;   // 下一个浮点位置
    for (int i = 1; i <= n; ++i) {
        char kind = (i - 1 < (int)convs.size()) ? convs[i - 1] : 'i';
        int stackOff = 32 + 8 * (n - i);
        std::string src = "qword ptr [rsp + " + std::to_string(stackOff) + "]";
        if (kind == 'f') {
            if (fltSlot < 4) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "xmm%d", fltSlot);
                emit("movsd " + std::string(buf) + ", " + src);
            }
            ++fltSlot;
        }
        else {
            static const char* iregs[] = { "rcx", "rdx", "r8", "r9" };
            if (intSlot < 4) {
                emit("mov " + std::string(iregs[intSlot]) + ", " + src);
            }
            ++intSlot;
        }
    }

    emit("call printf");
    emit("add rsp, " + std::to_string(32 + n * 8));
}

// ============================================================================
// 内建：输入（暂不支持，仅占位）
// ============================================================================
void X64CodeGen::genInputCall(const std::shared_ptr<CallExpr>&)
{
    throw std::runtime_error("x64 后端暂不支持 输入() 函数");
}

// ============================================================================
// 语句
// ============================================================================
void X64CodeGen::genExprStmt(const std::shared_ptr<ExprStmt>& s)
{
    if (std::shared_ptr<CallExpr> c =
        std::dynamic_pointer_cast<CallExpr>(s->expr)) {
        if (c->name == "打印" || c->name == "输出" || c->name == "printf") {
            genPrintCall(c);
            return;
        }
        if (c->name == "输入" || c->name == "scanf") {
            genInputCall(c);
            return;
        }
        genUserCall(c);
        return;
    }
    genExpr(s->expr);
}

void X64CodeGen::genVarDecl(const std::shared_ptr<VarDeclStmt>& d)
{
    int off = offsetOf(d->name);
    int base = off;   // 数组最高地址

    if (d->isArray) {
        for (int i = 0; i < d->arraySize; ++i) {
            int elemOff = base - 8 * i;   // 第 i 个元素在 [rbp - (base - 8i)]
            if (i < (int)d->initList.size()) {
                genExprInt(d->initList[i]);
            }
            else {
                emit("xor eax, eax");
            }
            emit("mov qword ptr [rbp - " + std::to_string(elemOff) + "], rax");
        }
        return;
    }

    if (d->init) {
        if (d->type == "浮点") {
            genExprFloat(d->init);
            emit("movsd qword ptr [rbp - " + std::to_string(off) + "], xmm0");
        }
        else {
            genExprInt(d->init);
            emit("mov qword ptr [rbp - " + std::to_string(off) + "], rax");
        }
    }
    else {
        if (d->type == "浮点") {
            emit("mov rax, 0");
            emit("mov qword ptr [rbp - " + std::to_string(off) + "], rax");
        }
        else {
            emit("mov qword ptr [rbp - " + std::to_string(off) + "], 0");
        }
    }
}

void X64CodeGen::genBlock(const std::shared_ptr<BlockStmt>& b)
{
    for (size_t i = 0; i < b->list.size(); ++i) genStmt(b->list[i]);
}

void X64CodeGen::genIf(const std::shared_ptr<IfStmt>& s)
{
    std::string elseLbl = newLabel("else");
    std::string endLbl = newLabel("endif");

    genExprInt(s->cond);
    emit("test rax, rax");
    emit("jz " + elseLbl);

    genStmt(s->thenS);
    emit("jmp " + endLbl);

    emitL(elseLbl);
    if (s->elseS) genStmt(s->elseS);
    emitL(endLbl);
}

void X64CodeGen::genWhile(const std::shared_ptr<WhileStmt>& s)
{
    std::string topLbl = newLabel("while");
    std::string endLbl = newLabel("endwhile");

    loops_.push_back(LoopCtx{ endLbl, topLbl });

    emitL(topLbl);
    genExprInt(s->cond);
    emit("test rax, rax");
    emit("jz " + endLbl);
    genStmt(s->body);
    emit("jmp " + topLbl);
    emitL(endLbl);

    loops_.pop_back();
}

void X64CodeGen::genFor(const std::shared_ptr<ForStmt>& s)
{
    if (s->init) genStmt(s->init);

    std::string topLbl = newLabel("for");
    std::string contLbl = newLabel("for_cont");
    std::string endLbl = newLabel("endfor");

    loops_.push_back(LoopCtx{ endLbl, contLbl });

    emitL(topLbl);
    if (s->cond) {
        genExprInt(s->cond);
        emit("test rax, rax");
        emit("jz " + endLbl);
    }
    genStmt(s->body);
    emitL(contLbl);
    if (s->step) genExpr(s->step);
    emit("jmp " + topLbl);
    emitL(endLbl);

    loops_.pop_back();
}

void X64CodeGen::genReturn(const std::shared_ptr<ReturnStmt>& s)
{
    if (s->value) {
        if (inferType(s->value) == "浮点")
            genExprFloat(s->value);
        else
            genExprInt(s->value);
    }
    emit("jmp " + curRetLabel_);
}

void X64CodeGen::genStmt(const StmtPtr& s)
{
    if (!s) return;

    if (std::dynamic_pointer_cast<EmptyStmt>(s)) return;

    if (std::shared_ptr<BlockStmt> p = std::dynamic_pointer_cast<BlockStmt>(s)) {
        genBlock(p); return;
    }
    if (std::shared_ptr<VarDeclStmt> p = std::dynamic_pointer_cast<VarDeclStmt>(s)) {
        genVarDecl(p); return;
    }
    if (std::shared_ptr<IfStmt> p = std::dynamic_pointer_cast<IfStmt>(s)) {
        genIf(p); return;
    }
    if (std::shared_ptr<WhileStmt> p = std::dynamic_pointer_cast<WhileStmt>(s)) {
        genWhile(p); return;
    }
    if (std::shared_ptr<ForStmt> p = std::dynamic_pointer_cast<ForStmt>(s)) {
        genFor(p); return;
    }
    if (std::shared_ptr<ReturnStmt> p = std::dynamic_pointer_cast<ReturnStmt>(s)) {
        genReturn(p); return;
    }
    if (std::dynamic_pointer_cast<BreakStmt>(s)) {
        if (loops_.empty()) throw std::runtime_error("中断 不在循环内");
        emit("jmp " + loops_.back().breakLabel);
        return;
    }
    if (std::dynamic_pointer_cast<ContinueStmt>(s)) {
        if (loops_.empty()) throw std::runtime_error("继续 不在循环内");
        emit("jmp " + loops_.back().contLabel);
        return;
    }
    if (std::shared_ptr<ExprStmt> p = std::dynamic_pointer_cast<ExprStmt>(s)) {
        genExprStmt(p); return;
    }
}

// ============================================================================
// 函数
// ============================================================================
void X64CodeGen::collectFuncs()
{
    for (size_t i = 0; i < prog_->funcs.size(); ++i) {
        const std::shared_ptr<FuncDecl>& fn = prog_->funcs[i];
        funcRet_[fn->name] = fn->retType;
        funcCName_[fn->name] = funcCName(fn->name);
    }
}

void X64CodeGen::genFunction(const std::shared_ptr<FuncDecl>& fn)
{
    curFunc_ = fn->name;
    curRetLabel_ = newLabel("ret");
    stackUsed_ = 0;
    localOffsets_.clear();
    localTypes_.clear();

    // 参数先分配槽位
    for (size_t i = 0; i < fn->params.size(); ++i)
        allocLocal(fn->params[i].name, fn->params[i].type);

    // 预扫描，为所有局部变量分配槽位
    preScanStmt(fn->body);

    // 对齐到 16 字节
    int frameSize = (stackUsed_ + 15) & ~15;
    if (frameSize < 32) frameSize = 32;   // 至少 32 字节 shadow space

    // 函数头
    std::string cname = funcCName(fn->name);
    text_ << ".globl " << cname << "\n";
    text_ << cname << ":\n";
    text_ << "    push rbp\n";
    text_ << "    mov rbp, rsp\n";
    text_ << "    sub rsp, " << frameSize << "\n";

    // 保存参数到栈槽
    static const char* iregs[] = { "rcx", "rdx", "r8", "r9" };
    int ir = 0;
    for (size_t i = 0; i < fn->params.size(); ++i) {
        int off = offsetOf(fn->params[i].name);
        if (fn->params[i].type == "浮点") {
            if (i < 4) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "xmm%d", (int)i);
                text_ << "    movsd qword ptr [rbp - " << off << "], "
                    << buf << "\n";
            }
        }
        else {
            if (ir < 4) {
                text_ << "    mov qword ptr [rbp - " << off << "], "
                    << iregs[ir++] << "\n";
            }
        }
    }

    // 函数体
    genStmt(fn->body);

    // 默认返回 0
    text_ << "    xor eax, eax\n";

    // 返回标签
    text_ << curRetLabel_ << ":\n";
    text_ << "    leave\n";
    text_ << "    ret\n\n";
}

// ============================================================================
// 程序整体
// ============================================================================
std::string X64CodeGen::generate()
{
    collectFuncs();

    // 先处理全局变量（本后端暂不支持，忽略或报错）
    if (!prog_->globals.empty())
        throw std::runtime_error("x64 后端暂不支持全局变量");

    // 生成各函数（写入 text_）
    for (size_t i = 0; i < prog_->funcs.size(); ++i)
        genFunction(prog_->funcs[i]);

    // 汇总输出
    std::ostringstream out;
    out << "# =====================================================\n";
    out << "# x86-64 汇编（GAS Intel 语法，Microsoft x64 ABI）\n";
    out << "# 由中文脚本语言编译器生成\n";
    out << "# =====================================================\n";
    out << "    .intel_syntax noprefix\n\n";

    // .rdata 段
    out << "    .section .rdata\n";
    out << data_.str();
    out << "\n";

    // .text 段
    out << "    .section .text\n";
    out << "    .extern printf\n\n";
    out << text_.str();

    return out.str();
}

// ============================================================================
// 构造
// ============================================================================
X64CodeGen::X64CodeGen(std::shared_ptr<Program> prog) : prog_(prog) {}