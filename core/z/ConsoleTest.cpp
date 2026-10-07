#include "common.h"
#include "x64codegen.h"

#ifdef _WIN32
#  include <windows.h>
#endif

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
static std::string readAll(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) throw std::runtime_error("无法打开文件: " + path);
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string s = ss.str();
    // 去 UTF-8 BOM
    if (s.size() >= 3 &&
        (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    return s;
}

static void writeAll(const std::string& path, const std::string& content)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) throw std::runtime_error("无法写入文件: " + path);
    ofs << content;
}

static bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
        s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string replaceExt(const std::string& path, const std::string& newExt)
{
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + newExt;
    return path.substr(0, dot) + newExt;
}

// ---------------------------------------------------------------------------
// 使用说明
// ---------------------------------------------------------------------------
static void usage(const char* exe)
{
    std::cout
        << "Help:\n"
        << "  " << exe << " <src>.cn              生成 <src>.ast 与 <src>.c\n"
        << "  " << exe << " <src>.cn <src>.ast              只生成 AST 文件\n"
        << "  " << exe << " --from-ast <src>.ast <src>.c  由 AST 生成 C 代码\n"
        << "  " << exe << " --ast-to-asm <src>.ast <src>.s        生成汇编码\n"
        << "\n"
        << "Examples:\n"
        << "  " << exe << " program.cn\n"
        << "  " << exe << " program.cn program.ast\n"
        << "  " << exe << " --from-ast program.ast program.c\n";
}
// ---------------------------------------------------------------------------


int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    try {
        // ---- 模式 0：AST → x86-64 汇编 ----
        if (argc >= 2 && std::string(argv[1]) == "--ast-to-asm") {
            if (argc < 4) {
                std::cerr << "Help: " << argv[0]
                    << " --ast-to-asm <file.ast> <file.s>\n";
                return 1;
            }
            std::string astPath = argv[2];
            std::string asmPath = argv[3];

            std::string astText = readAll(astPath);
            AstReader reader(astText);
            std::shared_ptr<Program> prog = reader.read();

            X64CodeGen gen(prog);
            std::string asmCode = gen.generate();
            writeAll(asmPath, asmCode);

            std::cout << "x86-64 assembly generated:" << asmPath << std::endl;
            std::cout << "\nUse MinGW or Clang to assemble and link:\n"
                << "    gcc " << asmPath << " -o app.exe\n"
                << "    app.exe\n";
            return 0;
        }
        // ---- 模式 1：由 AST 生成 C ----
        if (argc >= 2 && std::string(argv[1]) == "--from-ast") {
            if (argc < 4) { usage(argv[0]); return 1; }
            std::string astPath = argv[2];
            std::string cPath = argv[3];

            std::string astText = readAll(astPath);
            AstReader reader(astText);
            std::shared_ptr<Program> prog = reader.read();

            CodeGen cg(prog);
            std::string cCode = cg.generate();
            writeAll(cPath, cCode);

            std::cout << "C code generated from AST:" << cPath << std::endl;
            return 0;
        }

        // ---- 读取源文件 ----
        std::string source;
        std::string baseName;
        bool fromEmbedded = false;

        if (argc >= 2) {
            baseName = argv[1];
            source = readAll(baseName);
        }
        else {
            std::cout << "Error: Cannot find the file\n" << std::endl;
            return -1;
        }
        normalizePunct(source);

        // ---- 词法 + 语法 ----
        Lexer lexer(source);
        std::vector<Token> toks = lexer.run();

        Parser parser(toks);
        std::shared_ptr<Program> prog = parser.parse();

        // ---- 决定输出文件名 ----
        std::string astPath;
        std::string cPath;

        if (argc >= 3) {
            // 只生成 AST
            astPath = argv[2];
            std::string astText = AstWriter(prog).write();
            writeAll(astPath, astText);
            std::cout << "AST file generated:" << astPath << std::endl;
            std::cout << "To generate C code, please run:" << std::endl;
            std::cout << "    " << argv[0] << " --from-ast "
                << astPath << " " << replaceExt(astPath, ".c") << std::endl;
            return 0;
        }

        // 生成 .ast + .c
        astPath = replaceExt(baseName, ".ast");
        cPath = replaceExt(baseName, ".c");

        std::string astText = AstWriter(prog).write();
        writeAll(astPath, astText);

        CodeGen cg(prog);
        std::string cCode = cg.generate();
        writeAll(cPath, cCode);

        std::cout << "AST file generated:" << astPath << std::endl;
        std::cout << "C code file generated:" << cPath << std::endl;
        std::cout << "\nTo compile the C code file:\n"
            << "    cl  " << cPath << "  /utf-8  /Fe:app.exe\n"
            << "    gcc " << cPath << "  -o app\n";
    }
    catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}