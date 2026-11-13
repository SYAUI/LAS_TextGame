# LARK_TRANSFORM_SCRIPT.py
import sys
import os
import ast
import marshal
import importlib.util
import time
import re

# 正则阶段：textprint { ... }  ->  textprint([ ... ])

_RE_TP_SINGLE = re.compile(r'^([ \t]*)textprint\s*\{\s*(.*?)\s*\}\s*$')
_RE_TP_OPEN   = re.compile(r'^([ \t]*)textprint\s*\{\s*$')
_RE_TP_CLOSE  = re.compile(r'^[ \t]*\}\s*$')

def transform_script(script):
    lines = script.splitlines()
    out = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]

        # ---- 单行形式： textprint { a b c } ----
        m1 = _RE_TP_SINGLE.match(line)
        if m1 and m1.group(2):
            indent = m1.group(1)
            content = [m1.group(2).strip()]
            list_str = '[' + ', '.join(f'"{c}"' for c in content) + ']'
            out.append(f'{indent}textprint({list_str})')
            i += 1
            continue

        # ---- 多行形式：
        #         textprint {
        #             a
        #             b
        #         }
        # ----
        m2 = _RE_TP_OPEN.match(line)
        if m2:
            indent = m2.group(1)
            i += 1
            content = []
            closed = False
            while i < n:
                if _RE_TP_CLOSE.match(lines[i]):
                    closed = True
                    i += 1
                    break
                content.append(lines[i].strip())
                i += 1

            if not closed:
                # 没找到闭合，原样保留，让 Python 去报语法错误
                out.append(line)
                continue

            content = [c for c in content if c]
            list_str = '[' + ', '.join(f'"{c}"' for c in content) + ']'
            out.append(f'{indent}textprint({list_str})')
            continue

        out.append(line)
        i += 1

    return '\n'.join(out)


# AST阶段改写：把裸语句 jump(...) / call(...) / stop() 自动变成 yield from jump(...) 等

class _ControlFlowRewriter(ast.NodeTransformer):
    _NAMES = frozenset({"jump", "call", "stop"})

    def __init__(self):
        super().__init__()
        self._depth = 0  # 是否处于函数体内

    def visit_FunctionDef(self, node):
        self._depth += 1
        self.generic_visit(node)
        self._depth -= 1
        return node

    visit_AsyncFunctionDef = visit_FunctionDef

    def visit_Expr(self, node):
        # 只在函数体内、且是裸语句时改写
        if (self._depth > 0
                and isinstance(node.value, ast.Call)
                and isinstance(node.value.func, ast.Name)
                and node.value.func.id in self._NAMES):
            new = ast.Expr(value=ast.YieldFrom(value=node.value))
            return ast.copy_location(new, node)
        return self.generic_visit(node)


def transform_control_flow(source):
    tree = ast.parse(source)
    tree = _ControlFlowRewriter().visit(tree)
    ast.fix_missing_locations(tree)
    return tree

# 编译并写入 .pyc

def compile_and_save(source_code, cache_path, source_mtime=None):
    # 先正则，再 AST
    transformed = transform_script(source_code)
    try:
        tree = transform_control_flow(transformed)
        code_obj = compile(tree, '<string>', 'exec')
    except SyntaxError:
        # 保存出错代码以便调试
        with open("error_source.txt", "w", encoding="utf-8") as f:
            f.write(source_code)
         # 重新抛出，由上层处理
        raise
    # 使用源文件时间戳或当前时间
    timestamp = int(source_mtime) if source_mtime is not None else int(time.time())

    magic = importlib.util.MAGIC_NUMBER
    bitmask = 0
    size = 0
    header = (magic
              + bitmask.to_bytes(4, 'little')
              + timestamp.to_bytes(4, 'little')
              + size.to_bytes(4, 'little'))
    data = marshal.dumps(code_obj)

    with open(cache_path, 'wb') as f:
        f.write(header + data)


def load_and_execute(cache_path):
    main_dict = sys.modules['__main__'].__dict__
    with open(cache_path, 'rb') as f:
        data = f.read()
    code_data = data[16:]  # 跳过 16 字节头部
    code_obj = marshal.loads(code_data)
    exec(code_obj, main_dict)

# 对外入口
def run_script(source_path, source_code):
    cache_path = source_path + '.pyc'

    # 缓存命中：只有源文件不存在，或者 pyc 比源文件新，才直接用 pyc
    if os.path.exists(cache_path):
        if (not os.path.exists(source_path)
                or os.path.getmtime(cache_path) >= os.path.getmtime(source_path)):
            load_and_execute(cache_path)
            return

    # 源文件不存在又没有可用 pyc —— 直接报错
    if not os.path.exists(source_path):
        raise FileNotFoundError(f"[Error] FILE_NOT_FOUND: {source_path}")

    src_mtime = os.path.getmtime(source_path)
    compile_and_save(source_code, cache_path, source_mtime=src_mtime)
    load_and_execute(cache_path)
    