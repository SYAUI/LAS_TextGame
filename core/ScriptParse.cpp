#include "PythonRuntime.h"
#include "TextGameAPI.h"

#include <Python.h>
#include <string>
#include <vector>
#include <iostream>

namespace textgame {
    namespace {

        // ---------------- 包装：从 PyObject → TextGameCore 成员 ----------------

        PyObject* Py_Speak(PyObject*, PyObject* args) {
            const char* text = nullptr;
            if (!PyArg_ParseTuple(args, "s", &text)) return nullptr;

            auto* core = GetActiveCore();
            if (!core) { PyErr_SetString(PyExc_RuntimeError, "no active core"); return nullptr; }

            return PyBool_FromLong(core->Speak(text) ? 1 : 0);
        }

        PyObject* Py_TextPrint(PyObject*, PyObject* args) {
            PyObject* list = nullptr;
            if (!PyArg_ParseTuple(args, "O", &list)) return nullptr;

            if (!PyList_Check(list)) {
                PyErr_SetString(PyExc_TypeError, "expected list[str]");
                return nullptr;
            }

            Py_ssize_t n = PyList_Size(list);
            std::vector<std::string> lines;
            lines.reserve(static_cast<size_t>(n));
            for (Py_ssize_t i = 0; i < n; ++i) {
                PyObject* it = PyList_GetItem(list, i);   // borrowed
                if (!PyUnicode_Check(it)) {
                    PyErr_SetString(PyExc_TypeError, "list items must be str");
                    return nullptr;
                }
                lines.emplace_back(PyUnicode_AsUTF8(it));
            }

            auto* core = GetActiveCore();
            if (!core) { PyErr_SetString(PyExc_RuntimeError, "no active core"); return nullptr; }
            return PyBool_FromLong(core->TextPrint(lines) ? 1 : 0);
        }

        PyObject* Py_Load(PyObject*, PyObject* args) {
            const char* path = nullptr;
            if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;

            auto* core = GetActiveCore();
            if (!core) { PyErr_SetString(PyExc_RuntimeError, "no active core"); return nullptr; }
            return PyBool_FromLong(core->Load(path) ? 1 : 0);
        }

        PyObject* Py_SpeakCharacter(PyObject*, PyObject* args) {
            const char* name = nullptr;
            const char* color = nullptr;
            const char* text = nullptr;
            const char* sprite = "";
            const char* avatar = "";
            const char* side = "";

            if (!PyArg_ParseTuple(args, "sss|sss",
                &name, &color, &text,
                &sprite, &avatar, &side))
                return nullptr;

            auto* core = GetActiveCore();
            if (!core) { PyErr_SetString(PyExc_RuntimeError, "no active core"); return nullptr; }
            return PyBool_FromLong(core->SpeakCharacter(name, color, text, sprite, avatar, side) ? 1 : 0);
        }

        PyObject* Py_LoadScene(PyObject*, PyObject* args) {
            const char* path = nullptr;
            if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;

            auto* core = GetActiveCore();
            if (!core) { PyErr_SetString(PyExc_RuntimeError, "no active core"); return nullptr; }
            return PyBool_FromLong(core->LoadScene(path) ? 1 : 0);
        }

        PyObject* Py_ClearCharacters(PyObject*, PyObject*) {
            auto* core = GetActiveCore();
            if (!core) { PyErr_SetString(PyExc_RuntimeError, "no active core"); return nullptr; }
            return PyBool_FromLong(core->ClearCharacters() ? 1 : 0);
        }

        PyObject* Py_Menu(PyObject*, PyObject* args) {
            const char* title = nullptr;
            PyObject* list = nullptr;
            if (!PyArg_ParseTuple(args, "sO", &title, &list)) return nullptr;

            if (!PyList_Check(list)) {
                PyErr_SetString(PyExc_TypeError, "choices must be list[str]");
                return nullptr;
            }
            Py_ssize_t n = PyList_Size(list);
            std::vector<std::string> choices;
            choices.reserve(static_cast<size_t>(n));
            for (Py_ssize_t i = 0; i < n; ++i) {
                PyObject* it = PyList_GetItem(list, i);
                if (!PyUnicode_Check(it)) {
                    PyErr_SetString(PyExc_TypeError, "choice items must be str");
                    return nullptr;
                }
                choices.emplace_back(PyUnicode_AsUTF8(it));
            }

            auto* core = GetActiveCore();
            if (!core) { PyErr_SetString(PyExc_RuntimeError, "no active core"); return nullptr; }
            int result = core->Menu(title, choices);

            // 回调里可能通过 RaiseScriptTermination 挂了异常
            if (PyErr_Occurred()) return nullptr;

            return PyLong_FromLong(result);
        }

        // ---------------- 方法表 ----------------

        PyMethodDef kCoreMethods[] = {
            {"_core_speak",           Py_Speak,           METH_VARARGS, "Speak text"},
            {"_core_textprint",       Py_TextPrint,       METH_VARARGS, "Print lines"},
            {"_core_load",            Py_Load,            METH_VARARGS, "Load resource"},
            {"_core_speak_character", Py_SpeakCharacter,  METH_VARARGS, "Character speak"},
            {"_core_load_scene",      Py_LoadScene,       METH_VARARGS, "Load scene"},
            {"_core_menu",            Py_Menu,            METH_VARARGS, "Show menu"},
            {"_core_clear_characters",Py_ClearCharacters, METH_NOARGS,  "clear characters"},
            {nullptr, nullptr, 0, nullptr}
        };

        // 把 core 模块 + __main__ 同时挂上
        void InjectInto_Module_and_Main(const char* modName, PyMethodDef* methods) {
            PyObject* mod = PyImport_AddModule(modName);
            if (!mod) return;
            if (PyModule_AddFunctions(mod, methods) != 0) {
                PyErr_Print();
                return;
            }

            PyObject* main = PyImport_ImportModule("__main__");
            if (!main) return;
            PyObject* dict = PyModule_GetDict(main);
            for (PyMethodDef* def = methods; def->ml_name; ++def) {
                PyObject* fn = PyObject_GetAttrString(mod, def->ml_name);
                if (fn) {
                    PyDict_SetItemString(dict, def->ml_name, fn);
                    Py_DECREF(fn);
                }
            }
            Py_DECREF(main);
        }

    } // namespace

    // 由 PythonRuntime.cpp 调用
    bool RegisterCoreModuleBindings() {
        InjectInto_Module_and_Main("core", kCoreMethods);
        return true;
    }

} // namespace textgame