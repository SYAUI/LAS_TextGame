// lexer.cpp
#include "common.h"

Tok keywordOf(const std::string& s)
{
    static const std::map<std::string, Tok> kw = {
        // ---- 类型 ----
        { "整数",     Tok::KW_INT      }, { "整型",     Tok::KW_INT      },
        { "长整数",   Tok::KW_LONG     }, { "长整型",   Tok::KW_LONG     }, 
        { "浮点",     Tok::KW_FLOAT    }, { "小数",     Tok::KW_FLOAT    },
        { "字符",     Tok::KW_CHAR     },
        { "空",       Tok::KW_VOID     },

        // ---- 声明 ----
        { "常量",     Tok::KW_CONST    },
        { "函数",     Tok::KW_FUNC     },
        { "结束函数", Tok::KW_END_FUNC },

        // ---- 分支 ----
        { "如果",     Tok::KW_IF       },
        { "则",       Tok::KW_THEN     },
        { "否则如果", Tok::KW_ELSE_IF  },
        { "否则",     Tok::KW_ELSE     },
        { "结束如果", Tok::KW_END_IF   },

        // ---- 循环 ----
        { "当",       Tok::KW_WHILE    },
        { "循环",     Tok::KW_LOOP     },
        { "结束循环", Tok::KW_END_LOOP },

        // ---- 控制 ----
        { "返回",     Tok::KW_RETURN   },
        { "中断",     Tok::KW_BREAK    },
        { "继续",     Tok::KW_CONTINUE },
        { "真",       Tok::KW_TRUE     },
        { "假",       Tok::KW_FALSE    },
    };
    std::map<std::string, Tok>::const_iterator it = kw.find(s);
    return it == kw.end() ? Tok::IDENT : it->second;
}

void normalizePunct(std::string& s)
{
    struct P { const char* from; const char* to; };
    static const P tbl[] = {
        { "，", "," }, { "；", ";" }, { "：", ":" },
        { "（", "(" }, { "）", ")" }, { "｛", "{" }, { "｝", "}" },
        { "【", "[" }, { "】", "]" },
        { "“", "\"" }, { "”", "\"" }, { "‘", "'" }, { "’", "'" },
        { "！", "!" }, { "＝", "=" }, { "＋", "+" }, { "－", "-" },
        { "＊", "*" }, { "／", "/" }, { "＜", "<" }, { "＞", ">" },
    };
    const size_t TN = sizeof(tbl) / sizeof(tbl[0]);

    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    bool inStr = false, inChr = false;
    while (i < s.size()) {
        char c = s[i];
        if (!inStr && !inChr) {
            bool matched = false;
            for (size_t k = 0; k < TN; ++k) {
                size_t n = std::char_traits<char>::length(tbl[k].from);
                if (s.compare(i, n, tbl[k].from) == 0) {
                    out += tbl[k].to;
                    i += n;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }
        if (c == '"' && !inChr) {
            size_t bs = 0;
            for (size_t k = out.size(); k > 0 && out[k - 1] == '\\'; --k) ++bs;
            if (bs % 2 == 0) inStr = !inStr;
        }
        else if (c == '\'' && !inStr) {
            size_t bs = 0;
            for (size_t k = out.size(); k > 0 && out[k - 1] == '\\'; --k) ++bs;
            if (bs % 2 == 0) inChr = !inChr;
        }
        out += c;
        ++i;
    }
    s.swap(out);
}

Lexer::Lexer(std::string src) : src_(std::move(src)) {}
bool Lexer::eof() const { return p_ >= src_.size(); }
char Lexer::peek(size_t k) const { return p_ + k < src_.size() ? src_[p_ + k] : '\0'; }
bool Lexer::isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool Lexer::isIdentPart(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

std::vector<Token> Lexer::run()
{
    std::vector<Token> out;
    for (;;) {
        skipTrivia();
        if (eof()) { Token t; t.type = Tok::END; t.line = line_; out.push_back(t); break; }
        out.push_back(scan());
    }
    return out;
}

void Lexer::skipTrivia()
{
    for (;;) {
        char c = peek();
        if (c == '\0') return;
        if (c == '\n') { ++line_; ++p_; }
        else if (std::isspace((unsigned char)c)) { ++p_; }
        else if (c == '/' && peek(1) == '/') { while (!eof() && peek() != '\n') ++p_; }
        else if (c == '/' && peek(1) == '*') {
            p_ += 2;
            while (!eof() && !(peek() == '*' && peek(1) == '/')) {
                if (peek() == '\n') ++line_;
                ++p_;
            }
            if (!eof()) p_ += 2;
        }
        else return;
    }
}

Token Lexer::scan()
{
    Token t; t.line = line_;
    char c = peek();

    if (isIdentStart((unsigned char)c)) {
        size_t st = p_;
        while (!eof() && isIdentPart((unsigned char)peek())) ++p_;
        t.text = src_.substr(st, p_ - st);
        t.type = keywordOf(t.text);
        return t;
    }
    if (std::isdigit((unsigned char)c) ||
        (c == '.' && std::isdigit((unsigned char)peek(1)))) return scanNumber();
    if (c == '"')  return scanString();
    if (c == '\'') return scanChar();
    return scanOp();
}

Token Lexer::scanNumber()
{
    Token t; t.line = line_;
    size_t st = p_;
    bool isF = false;
    while (!eof() && std::isdigit((unsigned char)peek())) ++p_;
    if (peek() == '.' && std::isdigit((unsigned char)peek(1))) {
        isF = true; ++p_;
        while (!eof() && std::isdigit((unsigned char)peek())) ++p_;
    }
    if (peek() == 'e' || peek() == 'E') {
        size_t save = p_;
        ++p_;
        if (peek() == '+' || peek() == '-') ++p_;
        if (std::isdigit((unsigned char)peek())) {
            isF = true;
            while (!eof() && std::isdigit((unsigned char)peek())) ++p_;
        }
        else p_ = save;
    }
    std::string num = src_.substr(st, p_ - st);
    if (isF) { t.type = Tok::FLOAT_LIT; t.d = std::strtod(num.c_str(), NULL); }
    else { t.type = Tok::INT_LIT;   t.i = std::strtoll(num.c_str(), NULL, 10); }
    return t;
}

Token Lexer::scanString()
{
    Token t; t.line = line_;
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
            case '\'': s += '\''; break;
            default:   s += e;    break;
            }
        }
        else {
            if (c == '\n') ++line_;
            s += c;
            ++p_;
        }
    }
    if (!eof()) ++p_;
    t.type = Tok::STR_LIT;
    t.s = s;
    return t;
}

Token Lexer::scanChar()
{
    Token t; t.line = line_;
    ++p_;
    int val = 0;
    if (peek() == '\\') {
        ++p_;
        char e = peek(); ++p_;
        switch (e) {
        case 'n': val = '\n'; break;
        case 't': val = '\t'; break;
        case 'r': val = '\r'; break;
        case '0': val = 0;    break;
        case '\\': val = '\\'; break;
        case '\'': val = '\''; break;
        default: val = (unsigned char)e; break;
        }
    }
    else { val = (unsigned char)peek(); ++p_; }
    if (peek() == '\'') ++p_;
    t.type = Tok::CHAR_LIT;
    t.i = val;
    return t;
}

Token Lexer::scanOp()
{
    Token t; t.line = line_;
    char c = peek();
    char c1 = peek(1);

    struct TwoOp { char a, b; Tok k; };
    static const TwoOp two[] = {
        { '+', '+', Tok::INC },   { '-', '-', Tok::DEC },
        { '+', '=', Tok::PLUSEQ },{ '-', '=', Tok::MINUSEQ },
        { '*', '=', Tok::STAREQ },{ '/', '=', Tok::SLASHEQ },
        { '%', '=', Tok::PERCENTEQ },
        { '=', '=', Tok::EQ },    { '!', '=', Tok::NE },
        { '<', '=', Tok::LE },    { '>', '=', Tok::GE },
        { '&', '&', Tok::ANDAND },{ '|', '|', Tok::OROR },
    };
    for (size_t k = 0; k < sizeof(two) / sizeof(two[0]); ++k) {
        if (c == two[k].a && c1 == two[k].b) {
            p_ += 2; t.type = two[k].k; return t;
        }
    }
    ++p_;
    switch (c) {
    case '+': t.type = Tok::PLUS;    break;
    case '-': t.type = Tok::MINUS;   break;
    case '*': t.type = Tok::STAR;    break;
    case '/': t.type = Tok::SLASH;   break;
    case '%': t.type = Tok::PERCENT; break;
    case '=': t.type = Tok::ASSIGN;  break;
    case '<': t.type = Tok::LT;      break;
    case '>': t.type = Tok::GT;      break;
    case '!': t.type = Tok::NOT;     break;
    case '(': t.type = Tok::LPAREN;  break;
    case ')': t.type = Tok::RPAREN;  break;
    case '{': t.type = Tok::LBRACE;  break;
    case '}': t.type = Tok::RBRACE;  break;
    case '[': t.type = Tok::LBRACKET; break;
    case ']': t.type = Tok::RBRACKET; break;
    case ';': t.type = Tok::SEMI;    break;
    case ',': t.type = Tok::COMMA;   break;
    case '?': t.type = Tok::QUESTION; break;
    case ':': t.type = Tok::COLON;   break;
    default:
        throw std::runtime_error(
            "第 " + std::to_string(t.line) + " 行: 无法识别的字符 '" +
            std::string(1, c) + "'");
    }
    return t;
}