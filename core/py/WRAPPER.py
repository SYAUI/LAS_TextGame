# WRAPPER.py
# 定义 speak、load 和 textprint 的 Python 包装函数，支持 str.format 插值

import sys

def speak(text):
    frame = sys._getframe(1)
    locals_dict = frame.f_locals
    globals_dict = frame.f_globals
    scope = {**globals_dict, **locals_dict}
    try:
        new_text = text.format(**scope)
    except KeyError:
        new_text = text
    return _core_speak(new_text)

def load(path):
    frame = sys._getframe(1)
    locals_dict = frame.f_locals
    globals_dict = frame.f_globals
    scope = {**globals_dict, **locals_dict}
    try:
        new_path = path.format(**scope)
    except KeyError:
        new_path = path
    return _core_load(new_path)

def textprint(lines):
    frame = sys._getframe(1)
    locals_dict = frame.f_locals
    globals_dict = frame.f_globals
    scope = {**globals_dict, **locals_dict}
    new_lines = []
    for line in lines:
        try:
            new_line = line.format(**scope)
        except KeyError:
            new_line = line
        new_lines.append(new_line)
    return _core_textprint(new_lines)