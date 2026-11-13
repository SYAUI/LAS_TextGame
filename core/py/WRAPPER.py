import sys
import inspect

# 流程控制信号
class _JumpSignal(Exception):
    def __init__(self, label): self.label = label

class _CallSignal(Exception):
    def __init__(self, label): self.label = label

class _StopSignal(Exception):
    """显式终止游戏。"""
    pass

# 标签（Label）与流程控制
_LABELS = {}

def label(name=None):
    """
    @label                  # 用函数名作标签名
    @label("chapter_one")   # 自定义标签名
    """
    if callable(name):
        func = name
        _LABELS[func.__name__] = func
        return func
    def decorator(func):
        label_name = name if name is not None else func.__name__
        _LABELS[label_name] = func
        return func
    return decorator


# --- 控制流指令：只 yield，不抛异常 ---
def jump(name):
    """跳转，不返回调用点。用法：yield from jump("xxx")"""
    yield ("jump", name)

def call(name):
    """调用，执行完后返回调用点。用法：yield from call("xxx")"""
    yield ("call", name)

def stop():
    """显式终止游戏。用法：yield from stop()"""
    yield ("stop", None)


def _ensure_gen(func):
    """把 label 调用结果统一成生成器。
       生成器函数 -> 原样返回；普通函数 -> 立即执行后包成空生成器。"""
    result = func()
    if hasattr(result, "__next__"):
        return result
    def _empty():
        return
        yield
    return _empty()


def run_game(start_label="start"):
    """主循环：用生成器栈驱动 label。"""
    def push(stack, name):
        func = _LABELS.get(name)
        if func is None:
            print(f"[脚本] 未找到标签: {name}", file=sys.stderr)
            return False
        stack.append(_ensure_gen(func))
        return True

    stack = []
    if not push(stack, start_label):
        return

    while stack:
        g = stack[-1]
        try:
            cmd = next(g)            # 恢复最上层生成器
        except StopIteration:
            stack.pop()              # label 正常结束：弹栈，回到调用者
            continue

        if not cmd:                  # yield None 忽略
            continue

        op, arg = cmd
        if op == "call":
            if not push(stack, arg): # 压栈：执行完后回到当前生成器的下一行
                stack.pop()
        elif op == "jump":
            stack.clear()            # 丢弃整条栈
            if not push(stack, arg):
                break
        elif op == "stop":
            break                    # 退出主循环

# 角色系统

class Character:
    """
    Character(name, color=None, sprite=None, avatar=None, side=None)
      sprite : 立绘路径（128x256 像素图）
      avatar : 头像路径（128x128 像素图）
      side   : "left" / "right"（立绘显示在哪边）
    """
    def __init__(self, name, color=None,
                 sprite=None, avatar=None, side=None, **options):
        self.name   = name
        self.color  = color
        self.sprite = sprite or ""
        self.avatar = avatar or ""
        self.side   = side   or ""
        self.options = options

    def __call__(self, text):
        frame = sys._getframe(1)
        scope = {**frame.f_globals, **frame.f_locals}
        try:
            text = text.format(**scope)
        except KeyError:
            pass
        return _core_speak_character(
            self.name,
            self.color or "",
            text,
            self.sprite,
            self.avatar,
            self.side,
        )

    def __repr__(self):
        return f"Character({self.name!r}, side={self.side!r})"

def clear_characters():
    """清空左右立绘、头像、说话侧。不改背景。"""
    return _core_clear_characters()

# 场景

def load_scene(path):
    """加载背景图。path 相对 exe 工作目录。"""
    frame = sys._getframe(1)
    scope = {**frame.f_globals, **frame.f_locals}
    try:
        path = path.format(**scope)
    except KeyError:
        pass
    return _core_load_scene(path)

# 菜单

def menu(title, choices):
    """
    显示选择菜单，返回玩家所选选项在原始列表中的下标。
    
    choices 元素支持：
        "选项文本"                      # 无条件显示
        ("选项文本", 条件表达式)         # 条件为真时才显示
    
    用法：
        choice = menu("要做什么？", [
            "喝咖啡",
            ("喝茶", has_tea),
            "离开"
        ])
        if choice == 0: jump("drink_coffee")
        elif choice == 1: jump("drink_tea")
        else: stop()
    """
    valid = []
    for idx, item in enumerate(choices):
        if isinstance(item, tuple):
            text, cond = item
            if cond:
                valid.append((idx, text))
        else:
            valid.append((idx, item))

    texts = [t for _, t in valid]
    selected = _core_menu(title, texts)
    if selected < 0 or selected >= len(valid):
        return -1
    return valid[selected][0]

# 其他

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

def textprint(*args):
    frame = sys._getframe(1)
    locals_dict = frame.f_locals
    globals_dict = frame.f_globals
    scope = {**globals_dict, **locals_dict}

    # 如果只有一个参数且是列表，则视作块语法转换来的
    if len(args) == 1 and isinstance(args[0], list):
        lines = args[0]
    else:
        lines = list(args)

    new_lines = []
    for line in lines:
        try:
            new_line = line.format(**scope)
        except (KeyError, AttributeError):
            new_line = line
        new_lines.append(new_line)

    return _core_textprint(new_lines)