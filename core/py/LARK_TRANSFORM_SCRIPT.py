# LARK_TRANSFORM_SCRIPT.py
import sys
import os
import marshal
import importlib.util
import time
import re

def transform_script(script):
    def replace_textprint(match):
        content = match.group(1)
        start = match.start()
        line_start = script.rfind('\n', 0, start) + 1
        indent = script[line_start:start]  # 缩进字符

        lines = [line.strip() for line in content.splitlines() if line.strip()]
        if lines:
            list_str = '[' + ', '.join(f'"{line}"' for line in lines) + ']'
        else:
            list_str = '[]'
        return f"{indent}textprint({list_str})"

    pattern = r'textprint\s*\{((?:[^{}]|\{[^{}]*\})*)\}'
    return re.sub(pattern, replace_textprint, script, flags=re.DOTALL)

def compile_and_save(source_code, cache_path, source_mtime=None):

    try:
        code_obj = compile(source_code, '<string>', 'exec')
    except SyntaxError as e:
        # 保存错误代码以便调试
        with open("error_source.txt", "w", encoding="utf-8") as f:
            f.write(source_code)
        # 重新抛出，由上层处理
        raise
    # 使用源文件时间戳或当前时间
    if source_mtime is not None:
        timestamp = int(source_mtime)
    else:
        timestamp = int(time.time())

    
    magic = importlib.util.MAGIC_NUMBER 
    bitmask = 0
    size = 0
    header = magic + bitmask.to_bytes(4, 'little') + timestamp.to_bytes(4, 'little') + size.to_bytes(4, 'little')
    data = marshal.dumps(code_obj)
    with open(cache_path, 'wb') as f:
        f.write(header + data)

def load_and_execute(cache_path):
    with open(cache_path, 'rb') as f:
        data = f.read()
    code_data = data[16:] 
    code_obj = marshal.loads(code_data)
    exec(code_obj)

def run_script(source_path, source_code):
    cache_path = source_path + '.pyc'

    if os.path.exists(cache_path) and os.path.exists(source_path):
        if os.path.getmtime(cache_path) >= os.path.getmtime(source_path):
            load_and_execute(cache_path)
            return

    transformed = transform_script(source_code)
    # 调试保存
    # with open("debug_transformed.txt", "w", encoding="utf-8") as f:
    #     f.write(transformed)

    src_mtime = os.path.getmtime(source_path) if os.path.exists(source_path) else None
    compile_and_save(transformed, cache_path)

    load_and_execute(cache_path)
    