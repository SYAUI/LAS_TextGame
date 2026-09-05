#include <Python.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>

#include "TextCore.h"

// Python 包装函数

// 包装 _speak
static PyObject* py_speak(PyObject* self, PyObject* args) {
    const char* text;
    if (!PyArg_ParseTuple(args, "s", &text))
        return nullptr;
    bool result = _speak(std::string(text));
    return PyBool_FromLong(result ? 1 : 0);
}

// 包装 _textprint
static PyObject* py_textprint(PyObject* self, PyObject* args) {
    PyObject* listObj;
    if (!PyArg_ParseTuple(args, "O", &listObj))
        return nullptr;
    if (!PyList_Check(listObj)) {
        PyErr_SetString(PyExc_TypeError, "Expected a list of strings");
        return nullptr;
    }
    Py_ssize_t size = PyList_Size(listObj);
    std::vector<std::string> lines;
    lines.reserve(size);
    for (Py_ssize_t i = 0; i < size; ++i) {
        PyObject* item = PyList_GetItem(listObj, i);
        if (!PyUnicode_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "List items must be strings");
            return nullptr;
        }
        const char* s = PyUnicode_AsUTF8(item);
        lines.emplace_back(s);
    }

    // 传递 std::string 数组（指向 vector 内部数据）
    bool result = _textprint(lines.data(), static_cast<int>(lines.size()));
    return PyBool_FromLong(result ? 1 : 0);
}

// 包装 _load
static PyObject* py_load(PyObject* self, PyObject* args) {
    const char* path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return nullptr;
    bool result = _load(std::string(path));
    return PyBool_FromLong(result ? 1 : 0);
}

// 模块定义
static PyMethodDef CoreMethods[] = {
    {"_core_speak", py_speak, METH_VARARGS, "Core speak function."},
    {"_core_textprint", py_textprint, METH_VARARGS, "Core textprint function."},
    {"_core_load", py_load, METH_VARARGS, "Core load function."},
    {nullptr, nullptr, 0, nullptr}
};

// 初始化模块
static void init_core_module() {
    PyObject* mod = PyImport_AddModule("core");
    if (!mod) return;
    if (PyModule_AddFunctions(mod, CoreMethods) != 0) {
        std::cerr << "core 模块初始化失败！" << std::endl;
        return;
    }

    // 注入函数到 __main__（从 core 模块中提取）
    PyObject* main = PyImport_ImportModule("__main__");
    if (!main) return;
    PyObject* dict = PyModule_GetDict(main);
    for (PyMethodDef* def = CoreMethods; def->ml_name != nullptr; ++def) {
        PyObject* func = PyObject_GetAttrString(mod, def->ml_name);
        if (func) {
            PyDict_SetItemString(dict, def->ml_name, func);
            Py_DECREF(func);
        }
    }
    Py_DECREF(main);
}


// 主处理函数

// 执行脚本，返回是否成功
bool ExecuteScript(const std::string& filepath, const std::string& scriptContent) {
    // 初始化 Python（如果未初始化）
    static bool python_initialized = false;
    if (!python_initialized) {
        std::ifstream file("core\\py\\LARK_TRANSFORM_SCRIPT.py");
        if (!file.is_open()) {
            std::cerr << "无法打开转换脚本" << std::endl;
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        file.close();


        // 使用 PyConfig 初始化
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        // 设置 home 路径
        std::wstring home = L"Libraries/python/";
        PyConfig_SetString(&config, &config.home, home.c_str());

        PyStatus status = Py_InitializeFromConfig(&config);
        PyConfig_Clear(&config);
        if (PyStatus_Exception(status)) {
            std::cerr << "Python 初始化失败！" << std::endl;
            return false;
        }

        // 注册 core 模块
        init_core_module();
        // 执行转换脚本（定义 transform_script）
        if (PyRun_SimpleString(content.c_str()) != 0) {
            std::cerr << "加载转换脚本失败！" << std::endl;
            return false;
        }

        // 定义 Python 包装函数支持 str.format
        file.open("core\\py\\WRAPPER.py");
        if (!file.is_open()) {
            std::cerr << "无法打开包装函数文件" << std::endl;
            return false;
        }
        std::string wrapperContent((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        file.close();
        if (PyRun_SimpleString(wrapperContent.c_str()) != 0) {
            PyErr_Print();
            std::cerr << "执行包装函数文件失败！" << std::endl;
            return false;
        }

        python_initialized = true;
    }


    // run_script(source_path, source_code)
    PyObject* main = PyImport_ImportModule("__main__");
    PyObject* func = PyObject_GetAttrString(main, "run_script");
    if (!func || !PyCallable_Check(func)) {
        std::cerr << "找不到 run_script 函数！" << std::endl;
        Py_XDECREF(func);
        Py_DECREF(main);
        return false;
    }

    PyObject* args = PyTuple_New(2);
    if (!args) {
        PyErr_Print();
        std::cerr << "无法创建参数元组" << std::endl;
        Py_DECREF(func);
        Py_DECREF(main);
        return false;
    }
    PyObject* py_path = PyUnicode_FromString(filepath.c_str());
    PyObject* py_code = PyUnicode_FromString(scriptContent.c_str());
    if (!py_path || !py_code) {
        PyErr_Print();
        std::cerr << "无法将参数转换为 Unicode" << std::endl;
        Py_XDECREF(py_path);
        Py_XDECREF(py_code);
        Py_DECREF(args);
        Py_DECREF(func);
        Py_DECREF(main);
        return false;
    }
    PyTuple_SetItem(args, 0, py_path);
    PyTuple_SetItem(args, 1, py_code);

    PyObject* result = PyObject_CallObject(func, args);
    Py_DECREF(args);
    Py_DECREF(func);
    Py_DECREF(main);

    if (!result) {
        PyErr_Print();
        std::cerr << "执行 run_script 失败！" << std::endl;
        return false;
    }


    Py_DECREF(result); 
    return true;
}

bool RunScriptFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "无法打开脚本文件: " << filepath << std::endl;
        return false;
    }

    std::istreambuf_iterator<char> beg(file), end;
    std::string content(beg, end);
    file.close();

    // 传入文件路径和内容
    return ExecuteScript(filepath, content);
}