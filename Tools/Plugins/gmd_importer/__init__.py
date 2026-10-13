# __init__.py
bl_info = {
    "name": "GMD Model Format (.gmd)",
    "author": "SYAUI",
    "version": (1, 0, 0),
    "blender": (3, 2, 0),
    "location": "File > Import / File > Export",
    "description": "GMD v1.0 自定义模型格式导入 / 导出（对齐 gmd.h）",
    "category": "Import-Export",
}

import bpy
from bpy.props import (StringProperty, BoolProperty, FloatProperty,
                       EnumProperty)
from bpy_extras.io_utils import ImportHelper, ExportHelper

from . import importer
from . import exporter


# ================================================================ 导入
class ImportGMD(bpy.types.Operator, ImportHelper):
    bl_idname = "import_scene.gmd"
    bl_label = "Import GMD"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".gmd"
    filter_glob: StringProperty(default="*.gmd", options={'HIDDEN'})

    scale: FloatProperty(name="Scale", default=1.0, min=0.0001, max=1000.0)
    uv_flip_mode: EnumProperty(
        name="UV V",
        items=[
            ('AUTO',    '按文件标志', '依据 FILE_FLAG_UV_TOP_LEFT 自动判断'),
            ('FLIP',    '强制翻转',   'V = 1 - V'),
            ('NO_FLIP', '不翻转',     '保持原样'),
        ],
        default='AUTO')
    import_textures: BoolProperty(name="Import Textures", default=True)
    import_skeleton: BoolProperty(name="Import Skeleton", default=True)
    import_animations: BoolProperty(name="Import Animations", default=True)
    import_lods: BoolProperty(name="Import All LODs", default=False,
                              description="勾选则导入全部 LOD 的子网格（否则仅 LOD0）")

    def execute(self, context):
        try:
            return importer.load(context, **self.as_keywords(
                ignore=("filter_glob",)))
        except Exception as e:
            self.report({'ERROR'}, str(e))
            import traceback
            traceback.print_exc()
            return {'CANCELLED'}

    def draw(self, context):
        L = self.layout
        L.prop(self, "scale")
        L.prop(self, "uv_flip_mode")
        L.separator()
        L.prop(self, "import_textures")
        L.prop(self, "import_skeleton")
        L.prop(self, "import_animations")
        L.prop(self, "import_lods")


# ================================================================ 导出
class ExportGMD(bpy.types.Operator, ExportHelper):
    bl_idname = "export_scene.gmd"
    bl_label = "Export GMD"
    bl_options = {'REGISTER'}

    filename_ext = ".gmd"
    filter_glob: StringProperty(default="*.gmd", options={'HIDDEN'})
    
    scale: FloatProperty(name="Scale", default=1.0, min=0.0001, max=1000.0)
    apply_modifiers: BoolProperty(name="Apply Modifiers", default=True)
    export_textures: BoolProperty(name="Export Textures", default=True)
    embed_textures: BoolProperty(name="Embed Textures", default=True)
    export_skeleton: BoolProperty(name="Export Skeleton", default=True)
    export_animations: BoolProperty(name="Export Animations", default=True)

    compression: EnumProperty(
        name="Compression",
        items=[
            ('NONE',    'None',    '无压缩'),
            ('DEFLATE', 'Deflate', 'zlib/deflate，Python 内建'),
            ('ZSTD',    'ZSTD',    '需要 pip install zstandard'),
            ('LZ4',     'LZ4',     '需要 pip install lz4'),
        ],
        default='NONE')

    def execute(self, context):
        try:
            return exporter.save(context, **self.as_keywords(
                ignore=("filter_glob",)))
        except Exception as e:
            self.report({'ERROR'}, str(e))
            import traceback
            traceback.print_exc()
            return {'CANCELLED'}

    def draw(self, context):
        L = self.layout
        L.prop(self, "scale")
        L.prop(self, "apply_modifiers")
        L.separator()
        L.prop(self, "export_textures")
        L.prop(self, "embed_textures")
        L.prop(self, "compression")
        L.separator()
        L.prop(self, "export_skeleton")
        L.prop(self, "export_animations")


# ================================================================ 菜单
def _mi(self, ctx):
    self.layout.operator(ImportGMD.bl_idname, text="GMD Model (.gmd)")


def _me(self, ctx):
    self.layout.operator(ExportGMD.bl_idname, text="GMD Model (.gmd)")


_classes = (ImportGMD, ExportGMD)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(_mi)
    bpy.types.TOPBAR_MT_file_export.append(_me)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(_mi)
    bpy.types.TOPBAR_MT_file_export.remove(_me)
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
