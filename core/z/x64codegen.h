// x64codegen.h
#pragma once
#include "common.h"

// ============================================================================
// x86-64 汇编生成器（Microsoft x64 ABI，GAS Intel 语法）
// ============================================================================
class X64CodeGen {
public:
    explicit X64CodeGen(std::shared_ptr<Program> prog);

    // 生成完整汇编文本（含 .intel_syntax 头部）
    std::string generate();

private:
    std::shared_ptr<Program> prog_;
    std::ostringstream       text_;   // .text 段
    std::ostringstream       data_;   // .rdata 段

    // ---- 当前函数上下文 ----
    std::string  curFunc_;              // 当前函数的中文名
    std::string  curRetLabel_;          // 函数返回标签
    int          stackUsed_ = 0;        // 已分配栈字节数
    std::unordered_map<std::string, int>         localOffsets_;  // 中文名 -> [rbp - off]
    std::unordered_map<std::string, std::string> localTypes_;    // 中文名 -> 语言类型

    // ---- 全局 ----
    std::unordered_map<std::string, std::string> funcRet_;       // 函数名 -> 返回类型
    std::unordered_map<std::string, std::string> funcCName_;     // 函数名 -> C 名

    // ---- 字符串池 ----
    int strCounter_ = 0;

    // ---- 标签生成 ----
    int labelCounter_ = 0;

    // ---- 循环上下文 ----
    struct LoopCtx { std::string breakLabel; std::string contLabel; };
    std::vector<LoopCtx> loops_;

    // ========================================================================
    // 工具
    // ========================================================================
    void        emit(const std::string& s);
    void        emitL(const std::string& s);
    std::string newLabel(const std::string& pre = "L");

    std::string addString(const std::string& s);
    std::string escapeAsm(const std::string& s) const;

    static uint32_t    hash(const std::string& s);
    static std::string funcCName(const std::string& cnName);
    static std::string varCName(const std::string& cnName);

    std::string cTypeOf(const std::string& langType) const;
    std::string inferType(const ExprPtr& e) const;

    // ========================================================================
    // 栈帧管理
    // ========================================================================
    int         allocLocal(const std::string& name, const std::string& type, int count = 1);
    int         offsetOf(const std::string& name) const;
    std::string typeOfName(const std::string& name) const;
    bool        isLocal(const std::string& name) const;

    // 预扫描：为函数体内所有 VarDeclStmt 分配槽位
    void preScanStmt(const StmtPtr& s);

    // ========================================================================
    // 生成
    // ========================================================================
    void collectFuncs();

    void genFunction(const std::shared_ptr<FuncDecl>& fn);
    void genStmt(const StmtPtr& s);
    void genBlock(const std::shared_ptr<BlockStmt>& b);
    void genIf(const std::shared_ptr<IfStmt>& s);
    void genWhile(const std::shared_ptr<WhileStmt>& s);
    void genFor(const std::shared_ptr<ForStmt>& s);
    void genReturn(const std::shared_ptr<ReturnStmt>& s);
    void genVarDecl(const std::shared_ptr<VarDeclStmt>& d);
    void genExprStmt(const std::shared_ptr<ExprStmt>& s);

    // 表达式：结果放 rax（整型）或 xmm0（浮点）
    void genExpr(const ExprPtr& e);
    void genExprInt(const ExprPtr& e);   // 保证结果在 rax
    void genExprFloat(const ExprPtr& e);   // 保证结果在 xmm0

    void genBinary(const std::shared_ptr<BinaryExpr>& p);
    void genUnary(const std::shared_ptr<UnaryExpr>& p);
    void genAssign(const std::shared_ptr<AssignExpr>& p);
    void genIndexRead(const std::shared_ptr<IndexExpr>& p);
    void genTernary(const std::shared_ptr<TernaryExpr>& p);

    // 内建
    void genPrintCall(const std::shared_ptr<CallExpr>& c);
    void genInputCall(const std::shared_ptr<CallExpr>& c);

    // 用户函数调用
    void genUserCall(const std::shared_ptr<CallExpr>& c);
};