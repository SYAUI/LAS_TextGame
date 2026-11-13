#include "PythonRuntime.h"
#include "TextCore.h"

#include <Python.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace textgame {

    // ------------------------- 沙盒分配器桥接 -------------------------

    namespace {

        inline TextGameCoreImpl* ImplOf(void* ctx) {
            return static_cast<TextGameCoreImpl*>(ctx);
        }

        void* Sandbox_Malloc(void* ctx, size_t size) {
            if (size == 0) size = 1; // CPython 文档规定请求 0 字节时，必须返回一个唯一的非 NULL 指针
            return ImplOf(ctx)->Malloc(size);
        }
        void* Sandbox_Calloc(void* ctx, size_t n, size_t sz) {
            return ImplOf(ctx)->Calloc(n, sz);
        }
        void* Sandbox_Realloc(void* ctx, void* p, size_t sz) {
            return ImplOf(ctx)->Realloc(p, sz);
        }
        void  Sandbox_Free(void* ctx, void* p) {
            ImplOf(ctx)->Free(p);
        }

        // 读整个文件
        bool ReadFileAll(const std::string& path, std::string& out) {
            std::ifstream f(path, std::ios::binary);
            if (!f) return false;
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            return true;
        }

    } // namespace

    // ------------------------- Python 生命周期 -------------------------

    bool InitializePythonRuntime(TextGameCore* core) {
        if (!core) return false;
        auto* impl = core->GetImpl();

        // 沙盒模式下要求 TLSF 已经就绪
        if (impl->cfg.useSandbox && !impl->MemReady()) {
            std::cerr << "[Py] 沙盒模式但 TLSF 未就绪\n";
            return false;
        }

        PyPreConfig pre;
        PyPreConfig_InitPythonConfig(&pre);
        //pre.allocator = PYMEM_ALLOCATOR_MALLOC;

        if (PyStatus_Exception(Py_PreInitialize(&pre))) {
            std::cerr << "[Py] Py_PreInitialize 失败\n";
            return false;
        }

        if (impl->cfg.useSandbox) {
            static PyMemAllocatorEx alloc{
                /*ctx*/    impl,
                Sandbox_Malloc,
                Sandbox_Calloc,
                Sandbox_Realloc,
                Sandbox_Free,
            };
            PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &alloc);
            PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &alloc);
            PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &alloc);
        }

        PyConfig cfg;
        PyConfig_InitPythonConfig(&cfg);
        std::wstring home(impl->cfg.pythonHome.begin(), impl->cfg.pythonHome.end());
        PyConfig_SetString(&cfg, &cfg.home, home.c_str());

        PyStatus st = Py_InitializeFromConfig(&cfg);
        PyConfig_Clear(&cfg);
        if (PyStatus_Exception(st)) {
            std::cerr << "[Py] Py_InitializeFromConfig 失败: " << st.err_msg << "\n";
            return false;
        }

        std::cout << "[Py] 已初始化 ("
            << (impl->cfg.useSandbox ? "sandbox" : "system")
            << ", " << impl->cfg.sandboxMB << " MB)\n";
        return true;
    }

    void ShutdownPythonRuntime() {
        Py_FinalizeEx();
    }


    // GIL 辅助


    // 任意线程进入作用域自动获取 GIL
    GILGuard::GILGuard()
        : state_(static_cast<int>(PyGILState_Ensure())) {}

    GILGuard::~GILGuard() {
        PyGILState_Release(static_cast<PyGILState_STATE>(state_));
    }

    // 主线程：让出 / 恢复 GIL（在 Init / Shutdown 生命周期调用）
    void* ReleasePythonGIL() {
        return PyEval_SaveThread();
    }

    void RestorePythonGIL(void* state) {
        if (state) PyEval_RestoreThread(static_cast<PyThreadState*>(state));
    }

    // Python 回调内部：临时让出 GIL 用于阻塞等待
    GILRelease::GILRelease()
        : saved_(PyEval_SaveThread()) {}

    GILRelease::~GILRelease() {
        if (saved_) PyEval_RestoreThread(static_cast<PyThreadState*>(saved_));
    }

    // 在 Python 回调里请求终止脚本执行（抛 SystemExit
    void RaiseScriptTermination(const char* reason) {
        PyErr_SetString(PyExc_SystemExit, reason ? reason : "script terminated");
    }


    // ------------------------- 注册 core 模块 -------------------------

    // 在 ScriptParse.cpp 里实现，本文件声明为 extern
    extern bool RegisterCoreModuleBindings();

    bool RegisterCoreModule(TextGameCore* /*core*/) {
        return RegisterCoreModuleBindings();
    }

    // ------------------------- 加载运行时脚本 -------------------------

    bool LoadRuntimeScripts(TextGameCore* core) {
        const auto& root = core->GetConfig().assetRoot;

        auto runFile = [&](const std::string& rel) -> bool {
            std::string path = root + "/" + rel;
            std::string content;
            if (!ReadFileAll(path, content)) {
                std::cerr << "[Py] 无法读取: " << path << "\n";
                return false;
            }
            if (PyRun_SimpleString(content.c_str()) != 0) {
                PyErr_Print();
                std::cerr << "[Py] 执行失败: " << path << "\n";
                return false;
            }
            return true;
            };

        return runFile("py/LARK_TRANSFORM_SCRIPT.py")
            && runFile("py/WRAPPER.py");
    }

    // ------------------------- 执行脚本 -------------------------

    ScriptResult ExecuteScriptInternal(TextGameCore* core,
        const std::string& filepath,
        const std::string& scriptContent) {
        PyObject* main = PyImport_ImportModule("__main__");
        if (!main) return ScriptResult::Failure("无法取得 __main__");

        PyObject* func = PyObject_GetAttrString(main, "run_script");
        if (!func || !PyCallable_Check(func)) {
            Py_XDECREF(func); Py_DECREF(main);
            return ScriptResult::Failure("找不到 run_script");
        }

        PyObject* args = PyTuple_New(2);
        PyObject* pPath = PyUnicode_FromString(filepath.c_str());
        PyObject* pCode = PyUnicode_FromString(scriptContent.c_str());
        if (!args || !pPath || !pCode) {
            Py_XDECREF(args); Py_XDECREF(pPath); Py_XDECREF(pCode);
            Py_DECREF(func);  Py_DECREF(main);
            return ScriptResult::Failure("构造参数失败");
        }
        PyTuple_SetItem(args, 0, pPath);
        PyTuple_SetItem(args, 1, pCode);

        PyObject* ret = PyObject_CallObject(func, args);
        Py_DECREF(args); Py_DECREF(func); Py_DECREF(main);

        if (!ret) {
            PyErr_Print();
            return ScriptResult::Failure("run_script 抛出异常");
        }
        Py_DECREF(ret);
        return ScriptResult::Success();
    }

    void RunGameLoop(const std::string& startLabel) {
        // label 只允许字母/数字/下划线
        for (char c : startLabel) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
                std::cerr << "[Py] 非法 label: " << startLabel << "\n";
                return;
            }
        }
        std::string cmd = "run_game('" + startLabel + "')\n";
        if (PyRun_SimpleString(cmd.c_str()) != 0) {
            if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
                PyErr_Clear();
                std::cout << "[Py] 脚本已终止\n";
            }
            else {
                PyErr_Print();
                std::cerr << "[Py] run_game 执行失败\n";
            }
        }
    }

} // namespace textgame