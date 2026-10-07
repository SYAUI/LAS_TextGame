// common.h
#pragma once

#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <algorithm>

// ============================================================================
// 1. 记号
// ============================================================================
enum class Tok {
    END, IDENT, INT_LIT, FLOAT_LIT, CHAR_LIT, STR_LIT,

    // ---- 类型 ----
    KW_INT,        // 整数 / 整型          → int
    KW_LONG,       // 长整型 / 长整数      → long
    KW_FLOAT,      // 浮点 / 小数          → double
    KW_CHAR,       // 字符                 → int
    KW_VOID,       // 空                   → void

    KW_CONST, KW_FUNC, KW_END_FUNC,
    KW_IF, KW_THEN, KW_ELSE_IF, KW_ELSE, KW_END_IF,
    KW_WHILE, KW_LOOP, KW_END_LOOP,
    KW_RETURN, KW_BREAK, KW_CONTINUE,
    KW_TRUE, KW_FALSE,
    PLUS, MINUS, STAR, SLASH, PERCENT,
    ASSIGN, EQ, NE, LT, GT, LE, GE,
    ANDAND, OROR, NOT,
    INC, DEC,
    PLUSEQ, MINUSEQ, STAREQ, SLASHEQ, PERCENTEQ,
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    SEMI, COMMA, QUESTION, COLON
};

struct Token {
    Tok         type = Tok::END;
    std::string text;
    long long   i = 0;
    double      d = 0;
    std::string s;
    int         line = 1;
};

Tok  keywordOf(const std::string& s);
void normalizePunct(std::string& s);

// ============================================================================
// 2. 词法分析器
// ============================================================================
class Lexer {
public:
    explicit Lexer(std::string src);
    std::vector<Token> run();
private:
    std::string src_;
    size_t      p_ = 0;
    int         line_ = 1;
    bool eof() const;
    char peek(size_t k = 0) const;
    static bool isIdentStart(unsigned char c);
    static bool isIdentPart(unsigned char c);
    void  skipTrivia();
    Token scan();
    Token scanNumber();
    Token scanString();
    Token scanChar();
    Token scanOp();
};

// ============================================================================
// 3. AST
// ============================================================================
struct Expr { int line = 0; virtual ~Expr() {} };
struct Stmt { int line = 0; virtual ~Stmt() {} };

typedef std::shared_ptr<Expr> ExprPtr;
typedef std::shared_ptr<Stmt> StmtPtr;

struct IntLit : Expr { long long   v; IntLit(long long x) : v(x) {} };
struct FloatLit : Expr { double      v; FloatLit(double x) : v(x) {} };
struct CharLit : Expr { int         v; CharLit(int x) : v(x) {} };
struct StrLit : Expr { std::string v; StrLit(const std::string& s) : v(s) {} };
struct IdentExpr : Expr { std::string name; IdentExpr(const std::string& n) : name(n) {} };

struct BinaryExpr : Expr {
    Tok op; ExprPtr l, r;
    BinaryExpr(Tok o, ExprPtr a, ExprPtr b) : op(o), l(a), r(b) {}
};
struct UnaryExpr : Expr {
    Tok op; ExprPtr e; bool prefix;
    UnaryExpr(Tok o, ExprPtr x, bool pre) : op(o), e(x), prefix(pre) {}
};
struct AssignExpr : Expr {
    Tok op; ExprPtr target, value;
    AssignExpr(Tok o, ExprPtr t, ExprPtr v) : op(o), target(t), value(v) {}
};
struct CallExpr : Expr {
    std::string name; std::vector<ExprPtr> args;
    CallExpr(const std::string& n) : name(n) {}
};
struct IndexExpr : Expr {
    ExprPtr base, index;
    IndexExpr(ExprPtr b, ExprPtr i) : base(b), index(i) {}
};
struct TernaryExpr : Expr {
    ExprPtr c, a, b;
    TernaryExpr(ExprPtr x, ExprPtr y, ExprPtr z) : c(x), a(y), b(z) {}
};

struct BlockStmt : Stmt { std::vector<StmtPtr> list; };

struct VarDeclStmt : Stmt {
    std::string type;
    std::string name;
    bool        isConst = false;
    bool        isArray = false;
    int         arraySize = 0;
    ExprPtr     init;
    std::vector<ExprPtr> initList;
};

struct IfStmt : Stmt { ExprPtr cond; StmtPtr thenS, elseS; };
struct WhileStmt : Stmt { ExprPtr cond; StmtPtr body; };
struct ForStmt : Stmt { StmtPtr init; ExprPtr cond, step; StmtPtr body; };
struct ReturnStmt : Stmt { ExprPtr value; };
struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};
struct ExprStmt : Stmt { ExprPtr expr; };
struct EmptyStmt : Stmt {};

struct Param { std::string type, name; };

struct FuncDecl {
    std::string retType;
    std::string name;
    std::vector<Param>          params;
    std::shared_ptr<BlockStmt>  body;
};

struct Program {
    std::vector<std::shared_ptr<FuncDecl> > funcs;
    std::vector<StmtPtr>                    globals;
};

// ============================================================================
// 4. 语法分析器
// ============================================================================
class Parser {
public:
    explicit Parser(std::vector<Token> toks);
    std::shared_ptr<Program> parse();
private:
    std::vector<Token> t_;
    size_t             i_ = 0;
    const Token& cur() const;
    bool  check(Tok k) const;
    void  next();
    bool  accept(Tok k);
    void  err(const std::string& msg) const;
    void  expect(Tok k, const char* what);
    std::string parseTypeName();
    bool        isTypeStart() const;
    StmtPtr parseStmt();
    std::shared_ptr<BlockStmt> parseStmtList(Tok terminator);
    std::shared_ptr<IfStmt>    parseIf();
    std::shared_ptr<WhileStmt> parseWhile();
    std::shared_ptr<ForStmt>   parseFor();
    std::shared_ptr<ReturnStmt>parseReturn();
    std::shared_ptr<FuncDecl>    parseFuncDecl();
    std::shared_ptr<VarDeclStmt> finishVarDecl(const std::string& type,
        const std::string& name,
        bool isConst);
    std::shared_ptr<VarDeclStmt> parseVarDeclCore();
    ExprPtr parseExpr();
    ExprPtr parseAssign();
    ExprPtr parseTernary();
    static int binPrec(Tok t);
    ExprPtr parseBinary(int minPrec);
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();
};

// ============================================================================
// 5. AST 写出器（S 表达式文本）
// ============================================================================
class AstWriter {
public:
    explicit AstWriter(std::shared_ptr<Program> prog);
    std::string write();

private:
    std::shared_ptr<Program> prog_;
    std::ostringstream       out_;

    void        indent();
    void        nl();
    static std::string opName(Tok t);

    void writeProgram();
    void writeFunc(const std::shared_ptr<FuncDecl>& fn);
    void writeStmt(const StmtPtr& s);
    void writeVarDecl(const std::shared_ptr<VarDeclStmt>& d);
    void writeExpr(const ExprPtr& e);

    static std::string escapeStr(const std::string& s);
};

// ============================================================================
// 6. AST 读取器（S 表达式文本）
// ============================================================================
class AstReader {
public:
    explicit AstReader(std::string text);
    std::shared_ptr<Program> read();

private:
    // S 表达式节点
    struct SNode {
        bool                  isList = false;
        std::string           atom;     // 非列表时有效
        std::vector<SNode>    items;    // 列表时有效
    };

    std::string text_;
    size_t      p_ = 0;
    int         line_ = 1;

    // S 表达式解析
    void        skipWS();
    bool        eof() const;
    char        peek(size_t k = 0) const;
    SNode       parseNode();
    std::string parseAtom();

    // S 表达式 → AST
    std::shared_ptr<Program>  fromProgram(const SNode& n);
    std::shared_ptr<FuncDecl> fromFunc(const SNode& n);
    StmtPtr fromStmt(const SNode& n);
    ExprPtr fromExpr(const SNode& n);

    static long long  toInt(const std::string& s);
    static double     toFloat(const std::string& s);
    static Tok        opFromName(const std::string& s);

    void err(const std::string& msg) const;
    static const std::string& label(const SNode& n);
};

// ============================================================================
// 7. 代码生成器（AST → C 源码）
// ============================================================================
class CodeGen {
public:
    explicit CodeGen(std::shared_ptr<Program> prog);
    std::string generate();

private:
    std::shared_ptr<Program> prog_;
    std::ostringstream       out_;
    std::string              ind_;

    struct Scope {
        std::unordered_map<std::string, std::string> vars;    // 中文名 -> 语言类型
        std::unordered_map<std::string, std::string> cnames;  // 中文名 -> C 英文名
    };
    std::vector<Scope> scopes_;
    std::unordered_map<std::string, std::string> funcRet_;    // 中文函数名 -> 返回类型
    std::unordered_map<std::string, std::string> funcCName_;  // 中文函数名 -> C 英文名

    // --- 标识符英文化 ---
    static uint32_t    quickHash(const std::string& s);
    static std::string makeVarCName(const std::string& langType, const std::string& cnName);
    static std::string makeFuncCName(const std::string& cnName);
    void               collectFuncNames();
    std::string        cNameOfVar(const std::string& n) const;
    std::string        cNameOfFunc(const std::string& n) const;

    // --- 输出工具 ---
    void emitLine(const std::string& s);
    void push() { ind_ += "    "; }
    void pop() { if (ind_.size() >= 4) ind_.resize(ind_.size() - 4); }
    void pushScope() { scopes_.push_back(Scope()); }
    void popScope() { if (!scopes_.empty()) scopes_.pop_back(); }
    void declareVar(const std::string& n, const std::string& t);
    std::string typeOfVar(const std::string& n) const;

    std::string cTypeOf(const std::string& langType) const;
    std::string inferType(const ExprPtr& e) const;

    void genFunction(const std::shared_ptr<FuncDecl>& fn);
    void genStmt(const StmtPtr& s);
    void genStmtBody(const StmtPtr& s);
    void genIfStmt(const std::shared_ptr<IfStmt>& s);
    std::string genExpr(const ExprPtr& e);
    void genPrint(const std::shared_ptr<CallExpr>& c);
    void genInput(const std::shared_ptr<CallExpr>& c);

    std::string escapeCString(const std::string& s) const;
};