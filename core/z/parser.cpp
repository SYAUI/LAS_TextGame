// parser.cpp
#include "common.h"

namespace {
    long long foldInt(const ExprPtr& e)
    {
        if (std::shared_ptr<IntLit> p = std::dynamic_pointer_cast<IntLit>(e))   return p->v;
        if (std::shared_ptr<CharLit> p = std::dynamic_pointer_cast<CharLit>(e))  return p->v;
        if (std::shared_ptr<BinaryExpr> p = std::dynamic_pointer_cast<BinaryExpr>(e)) {
            long long a = foldInt(p->l), b = foldInt(p->r);
            switch (p->op) {
            case Tok::PLUS:    return a + b;
            case Tok::MINUS:   return a - b;
            case Tok::STAR:    return a * b;
            case Tok::SLASH:   return b ? a / b : 0;
            case Tok::PERCENT: return b ? a % b : 0;
            default: break;
            }
        }
        throw std::runtime_error("数组长度必须是常量整数表达式");
    }
}

Parser::Parser(std::vector<Token> toks) : t_(std::move(toks)) {}
const Token& Parser::cur() const { return t_[i_]; }
bool Parser::check(Tok k) const { return cur().type == k; }
void Parser::next() { if (i_ + 1 < t_.size()) ++i_; }
bool Parser::accept(Tok k) { if (check(k)) { next(); return true; } return false; }
void Parser::err(const std::string& msg) const
{
    throw std::runtime_error("第 " + std::to_string(cur().line) + " 行: " + msg);
}
void Parser::expect(Tok k, const char* what)
{
    if (!check(k)) err(std::string("期望 ") + what);
    next();
}

std::shared_ptr<Program> Parser::parse()
{
    std::shared_ptr<Program> prog(new Program());
    while (!check(Tok::END)) {
        if (check(Tok::KW_FUNC)) {
            next();
            prog->funcs.push_back(parseFuncDecl());
            continue;
        }
        bool isConst = false;
        if (check(Tok::KW_CONST)) { isConst = true; next(); }
        std::string type = parseTypeName();
        if (!check(Tok::IDENT)) err("期望标识符");
        std::string name = cur().text;
        next();
        std::shared_ptr<VarDeclStmt> d = finishVarDecl(type, name, isConst);
        expect(Tok::SEMI, "';'");
        prog->globals.push_back(d);
    }
    return prog;
}

std::string Parser::parseTypeName()
{
    if (check(Tok::KW_INT)) { next(); return "整型"; }
    if (check(Tok::KW_LONG)) { next(); return "长整型"; } 
    if (check(Tok::KW_FLOAT)) { next(); return "浮点"; }
    if (check(Tok::KW_CHAR)) { next(); return "字符"; }
    if (check(Tok::KW_VOID)) { next(); return "空"; }
    err("期望类型名（整数 / 浮点 / 字符 / 空）");
    return "";
}

bool Parser::isTypeStart() const
{
    Tok k = cur().type;
    return k == Tok::KW_INT || k == Tok::KW_LONG || 
        k == Tok::KW_FLOAT || k == Tok::KW_CHAR ||
        k == Tok::KW_VOID || k == Tok::KW_CONST;
}

std::shared_ptr<FuncDecl> Parser::parseFuncDecl()
{
    std::shared_ptr<FuncDecl> fn(new FuncDecl());
    if (!check(Tok::IDENT)) err("期望函数名");
    fn->name = cur().text;
    next();
    expect(Tok::LPAREN, "'('");
    if (!check(Tok::RPAREN)) {
        for (;;) {
            Param p;
            p.type = parseTypeName();
            if (!check(Tok::IDENT)) err("期望参数名");
            p.name = cur().text;
            next();
            fn->params.push_back(p);
            if (!accept(Tok::COMMA)) break;
        }
    }
    expect(Tok::RPAREN, "')'");
    expect(Tok::KW_RETURN, "'返回'");
    fn->retType = parseTypeName();

    std::shared_ptr<BlockStmt> body(new BlockStmt());
    while (!check(Tok::KW_END_FUNC) && !check(Tok::END))
        body->list.push_back(parseStmt());
    expect(Tok::KW_END_FUNC, "'结束函数'");
    fn->body = body;
    return fn;
}

std::shared_ptr<VarDeclStmt> Parser::finishVarDecl(const std::string& type,
    const std::string& name,
    bool isConst)
{
    std::shared_ptr<VarDeclStmt> d(new VarDeclStmt());
    d->line = cur().line;
    d->type = type;
    d->name = name;
    d->isConst = isConst;

    if (accept(Tok::LBRACKET)) {
        d->isArray = true;
        d->arraySize = (int)foldInt(parseExpr());
        expect(Tok::RBRACKET, "']'");
    }
    if (accept(Tok::ASSIGN)) {
        if (check(Tok::LBRACE)) {
            next();
            while (!check(Tok::RBRACE)) {
                d->initList.push_back(parseExpr());
                if (!accept(Tok::COMMA)) break;
            }
            expect(Tok::RBRACE, "'}'");
        }
        else d->init = parseExpr();
    }
    return d;
}

std::shared_ptr<VarDeclStmt> Parser::parseVarDeclCore()
{
    bool isConst = false;
    if (check(Tok::KW_CONST)) { isConst = true; next(); }
    std::string type = parseTypeName();
    if (!check(Tok::IDENT)) err("期望变量名");
    std::string name = cur().text;
    next();
    return finishVarDecl(type, name, isConst);
}

std::shared_ptr<BlockStmt> Parser::parseStmtList(Tok terminator)
{
    std::shared_ptr<BlockStmt> b(new BlockStmt());
    while (!check(terminator) && !check(Tok::END))
        b->list.push_back(parseStmt());
    return b;
}

StmtPtr Parser::parseStmt()
{
    int ln = cur().line;

    if (check(Tok::SEMI)) { next(); return StmtPtr(new EmptyStmt()); }
    if (check(Tok::KW_IF)) { next(); return parseIf(); }
    if (check(Tok::KW_WHILE)) { next(); return parseWhile(); }
    if (check(Tok::KW_LOOP)) { next(); return parseFor(); }
    if (check(Tok::KW_RETURN)) { next(); return parseReturn(); }
    if (check(Tok::KW_BREAK)) { next(); expect(Tok::SEMI, "';'"); return StmtPtr(new BreakStmt()); }
    if (check(Tok::KW_CONTINUE)) { next(); expect(Tok::SEMI, "';'"); return StmtPtr(new ContinueStmt()); }

    if (isTypeStart()) {
        std::shared_ptr<VarDeclStmt> d = parseVarDeclCore();
        expect(Tok::SEMI, "';'");
        return d;
    }

    std::shared_ptr<ExprStmt> s(new ExprStmt());
    s->line = ln;
    s->expr = parseExpr();
    expect(Tok::SEMI, "';'");
    return s;
}

std::shared_ptr<IfStmt> Parser::parseIf()
{
    expect(Tok::LPAREN, "'('");
    ExprPtr cond = parseExpr();
    expect(Tok::RPAREN, "')'");
    accept(Tok::KW_THEN);

    std::shared_ptr<IfStmt> first(new IfStmt());
    first->cond = cond;
    std::shared_ptr<IfStmt> curIf = first;

    for (;;) {
        std::shared_ptr<BlockStmt> body(new BlockStmt());
        while (!check(Tok::KW_ELSE_IF) && !check(Tok::KW_ELSE) &&
            !check(Tok::KW_END_IF) && !check(Tok::END))
            body->list.push_back(parseStmt());
        curIf->thenS = body;

        if (accept(Tok::KW_ELSE_IF)) {
            expect(Tok::LPAREN, "'('");
            ExprPtr c2 = parseExpr();
            expect(Tok::RPAREN, "')'");
            accept(Tok::KW_THEN);

            std::shared_ptr<IfStmt> nextIf(new IfStmt());
            nextIf->cond = c2;
            curIf->elseS = nextIf;
            curIf = nextIf;
            continue;
        }
        if (accept(Tok::KW_ELSE)) {
            std::shared_ptr<BlockStmt> elseBody(new BlockStmt());
            while (!check(Tok::KW_END_IF) && !check(Tok::END))
                elseBody->list.push_back(parseStmt());
            curIf->elseS = elseBody;
        }
        break;
    }
    expect(Tok::KW_END_IF, "'结束如果'");
    return first;
}

std::shared_ptr<WhileStmt> Parser::parseWhile()
{
    expect(Tok::LPAREN, "'('");
    std::shared_ptr<WhileStmt> s(new WhileStmt());
    s->cond = parseExpr();
    expect(Tok::RPAREN, "')'");
    expect(Tok::KW_LOOP, "'循环'");
    s->body = parseStmtList(Tok::KW_END_LOOP);
    expect(Tok::KW_END_LOOP, "'结束循环'");
    return s;
}

std::shared_ptr<ForStmt> Parser::parseFor()
{
    expect(Tok::LPAREN, "'('");
    std::shared_ptr<ForStmt> s(new ForStmt());
    if (!check(Tok::SEMI)) {
        if (isTypeStart()) s->init = parseVarDeclCore();
        else {
            std::shared_ptr<ExprStmt> es(new ExprStmt());
            es->expr = parseExpr();
            s->init = es;
        }
    }
    expect(Tok::SEMI, "';'");
    if (!check(Tok::SEMI)) s->cond = parseExpr();
    expect(Tok::SEMI, "';'");
    if (!check(Tok::RPAREN)) s->step = parseExpr();
    expect(Tok::RPAREN, "')'");
    s->body = parseStmtList(Tok::KW_END_LOOP);
    expect(Tok::KW_END_LOOP, "'结束循环'");
    return s;
}

std::shared_ptr<ReturnStmt> Parser::parseReturn()
{
    std::shared_ptr<ReturnStmt> s(new ReturnStmt());
    s->line = cur().line;
    if (!check(Tok::SEMI)) s->value = parseExpr();
    expect(Tok::SEMI, "';'");
    return s;
}

ExprPtr Parser::parseExpr() { return parseAssign(); }

ExprPtr Parser::parseAssign()
{
    ExprPtr lhs = parseTernary();
    Tok k = cur().type;
    if (k == Tok::ASSIGN || k == Tok::PLUSEQ || k == Tok::MINUSEQ ||
        k == Tok::STAREQ || k == Tok::SLASHEQ || k == Tok::PERCENTEQ) {
        next();
        ExprPtr rhs = parseAssign();
        return ExprPtr(new AssignExpr(k, lhs, rhs));
    }
    return lhs;
}

ExprPtr Parser::parseTernary()
{
    ExprPtr c = parseBinary(1);
    if (check(Tok::QUESTION)) {
        next();
        ExprPtr a = parseAssign();
        expect(Tok::COLON, "':'");
        ExprPtr b = parseAssign();
        return ExprPtr(new TernaryExpr(c, a, b));
    }
    return c;
}

int Parser::binPrec(Tok t)
{
    switch (t) {
    case Tok::OROR:    return 1;
    case Tok::ANDAND:  return 2;
    case Tok::EQ: case Tok::NE: return 3;
    case Tok::LT: case Tok::GT: case Tok::LE: case Tok::GE: return 4;
    case Tok::PLUS: case Tok::MINUS: return 5;
    case Tok::STAR: case Tok::SLASH: case Tok::PERCENT: return 6;
    default: return -1;
    }
}

ExprPtr Parser::parseBinary(int minPrec)
{
    ExprPtr lhs = parseUnary();
    for (;;) {
        int prec = binPrec(cur().type);
        if (prec < 0 || prec < minPrec) break;
        Tok op = cur().type;
        next();
        ExprPtr rhs = parseBinary(prec + 1);
        lhs = ExprPtr(new BinaryExpr(op, lhs, rhs));
    }
    return lhs;
}

ExprPtr Parser::parseUnary()
{
    Tok k = cur().type;
    if (k == Tok::MINUS || k == Tok::PLUS || k == Tok::NOT ||
        k == Tok::INC || k == Tok::DEC) {
        next();
        ExprPtr e = parseUnary();
        return ExprPtr(new UnaryExpr(k, e, true));
    }
    return parsePostfix();
}

ExprPtr Parser::parsePostfix()
{
    ExprPtr e = parsePrimary();
    for (;;) {
        if (check(Tok::LPAREN)) {
            std::shared_ptr<IdentExpr> id = std::dynamic_pointer_cast<IdentExpr>(e);
            if (!id) err("只能调用具名函数");
            next();
            std::shared_ptr<CallExpr> c(new CallExpr(id->name));
            if (!check(Tok::RPAREN)) {
                for (;;) {
                    c->args.push_back(parseExpr());
                    if (!accept(Tok::COMMA)) break;
                }
            }
            expect(Tok::RPAREN, "')'");
            e = c;
        }
        else if (check(Tok::LBRACKET)) {
            next();
            ExprPtr idx = parseExpr();
            expect(Tok::RBRACKET, "']'");
            e = ExprPtr(new IndexExpr(e, idx));
        }
        else if (check(Tok::INC) || check(Tok::DEC)) {
            Tok op = cur().type;
            next();
            e = ExprPtr(new UnaryExpr(op, e, false));
        }
        else break;
    }
    return e;
}

ExprPtr Parser::parsePrimary()
{
    const Token& t = cur();
    switch (t.type) {
    case Tok::INT_LIT: { long long v = t.i;       next(); return ExprPtr(new IntLit(v)); }
    case Tok::FLOAT_LIT: { double    v = t.d;       next(); return ExprPtr(new FloatLit(v)); }
    case Tok::CHAR_LIT: { int       v = (int)t.i;  next(); return ExprPtr(new CharLit(v)); }
    case Tok::STR_LIT: { std::string s = t.s;     next(); return ExprPtr(new StrLit(s)); }
    case Tok::KW_TRUE: { next(); return ExprPtr(new IntLit(1)); }
    case Tok::KW_FALSE: { next(); return ExprPtr(new IntLit(0)); }
    case Tok::IDENT: { std::string n = t.text;  next(); return ExprPtr(new IdentExpr(n)); }
    case Tok::LPAREN: {
        next();
        ExprPtr e = parseExpr();
        expect(Tok::RPAREN, "')'");
        return e;
    }
    default:
        err("意外的记号，无法构成表达式");
    }
    return ExprPtr();
}