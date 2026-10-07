// 将运行时的模型数据（顶点 / 索引 / 材质 / 纹理）导出为 .gmd 文件
#pragma once

#include "..\..\core\GmdIO.h"
#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace gmd {

    // 导出输入
    struct GmdExportInput
    {
        // ---------------- 顶点数据 ----------------
        const void* vertexData = nullptr;
        uint32_t        vertexCount = 0;
        uint32_t        vertexStride = 0;

        // ---------------- 索引 ----------------
        const uint32_t* indices = nullptr;
        uint32_t        indexCount = 0;

        // ---------------- 材质 ----------------
        struct MaterialInput
        {
            const char* name = nullptr;   // UTF-8
            float       baseColor[4] = { 1, 1, 1, 1 };
            int         textureIndex = -1;     // 指向 textureSRVs，-1 表示无
        };
        const MaterialInput* materials = nullptr;
        uint32_t             materialCount = 0;

        // ---------------- 子网格 ----------------
        struct SubmeshInput
        {
            uint32_t indexOffset = 0;   // 索引缓冲中的起始索引
            uint32_t indexCount = 0;
            uint32_t materialIndex = 0;
        };
        const SubmeshInput* submeshes = nullptr;
        uint32_t            submeshCount = 0;

        // ---------------- 纹理 ----------------
        // 由导出器从 SRV 读回像素、编码为 PNG 后内嵌
        ID3D11ShaderResourceView* const* textureSRVs = nullptr;
        const char** textureNames = nullptr;   // 可选
        uint32_t                         textureCount = 0;

        // ---------------- D3D 设备 ----------------
        ID3D11Device* device = nullptr;
    };

    // 将输入快照导出为 .gmd 文件。
    // 成功返回 true；失败返回 false 并通过 err 输出原因。
    bool ExportToFile(const std::string& path,
        const GmdExportInput& input,
        std::string* err = nullptr);

} // namespace gmd