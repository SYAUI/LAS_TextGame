#pragma once

#include "Gmd_format.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace gmd {

    // ============================================================================
    // 工具
    // ============================================================================

    // CRC32
    inline uint32_t Crc32(const void* data, size_t size, uint32_t init = 0) {
        static uint32_t table[256];
        static bool inited = false;
        if (!inited) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            inited = true;
        }
        uint32_t crc = ~init;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    inline uint64_t AlignUp(uint64_t v, uint64_t a) {
        return (v + a - 1) & ~(a - 1);
    }

    template <typename T>
    inline void PushPod(std::vector<uint8_t>& v, const T& x) {
        static_assert(std::is_trivially_copyable<T>::value, "POD required");
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        v.insert(v.end(), p, p + sizeof(T));
    }

    template <typename T>
    inline T ReadPod(const uint8_t* p) {
        static_assert(std::is_trivially_copyable<T>::value, "POD required");
        T x;
        std::memcpy(&x, p, sizeof(T));
        return x;
    }

    inline void PatchU64(std::vector<uint8_t>& buf, size_t off, uint64_t v) {
        std::memcpy(buf.data() + off, &v, sizeof(v));
    }
    inline void PatchU32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
        std::memcpy(buf.data() + off, &v, sizeof(v));
    }
    inline void PatchI64(std::vector<uint8_t>& buf, size_t off, int64_t v) {
        std::memcpy(buf.data() + off, &v, sizeof(v));
    }

    // ============================================================================
    // 内存模型
    // ============================================================================

    struct TextureData {
        TextureDesc              desc{};      // 描述（source/flags/format 等有效；偏移字段由 writer 覆盖）
        std::vector<TextureMipInfo> mips;     // 可选显式 mip 表
        std::string              name;
        std::string              uri;         // 外置路径；内嵌时为空
        std::vector<uint8_t>     embeddedData;// 内嵌时有效
    };

    struct MaterialData {
        Material                 mat{};       // 偏移字段（nameOffset/customDataOffset）由 writer 覆盖
        std::string              name;
        std::vector<uint8_t>     customData;
    };

    struct AnimationData {
        Animation                        anim{};
        std::vector<AnimationChannel>    channels;
        std::vector<std::vector<float>>  times;   // 每通道
        std::vector<std::vector<float>>  values;  // 每通道
    };

    struct GmdModel {
        // 元数据
        std::string name;
        uint32_t    flags = 0;
        float       boundsMin[3] = { 0, 0, 0 };
        float       boundsMax[3] = { 0, 0, 0 };

        // 几何
        std::vector<VertexStreamDesc> vertexStreams; // dataOffset 相对 vertexData
        std::vector<uint8_t>          vertexData;
        std::vector<IndexBufferDesc>  indexBuffers;  // dataOffset 相对 indexData
        std::vector<uint8_t>          indexData;
        std::vector<Submesh>          submeshes;

        // 材质
        std::vector<MaterialData>     materials;

        // 贴图
        std::vector<TextureData>      textures;
        std::vector<SamplerDesc>      samplers;

        // 骨骼
        std::vector<Skeleton>         skeletons;
        std::vector<Bone>             bones;
        std::vector<float>            inverseBindMatrices; // 每骨骼 16 个 float

        // 动画
        std::vector<AnimationData>    animations;

        // LOD
        std::vector<LodDesc>          lods;
    };

    // ============================================================================
    // GmdReader
    // ============================================================================

    class GmdReader {
    public:
        bool Open(const std::string& path, std::string* err = nullptr);
        bool OpenFromMemory(const void* data, size_t size, std::string* err = nullptr);

        const GmdModel& Model() const { return m_model; }

    private:
        bool Parse(std::string* err);
        const ChunkEntry* Find(uint32_t type) const;

        std::string Str(uint64_t off) const {
            if (off >= m_stringTable.size()) return {};
            return std::string(m_stringTable.c_str() + off);
        }

        std::vector<uint8_t>      m_buffer;
        FileHeader                m_header{};
        std::vector<ChunkEntry>   m_chunks;
        ModelMeta                 m_meta{};
        std::string               m_stringTable;
        GmdModel                  m_model;
    };

    inline bool GmdReader::Open(const std::string& path, std::string* err) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) { if (err) *err = "无法打开文件: " + path; return false; }
        auto sz = f.tellg();
        if (sz <= 0) { if (err) *err = "文件为空"; return false; }
        m_buffer.resize(static_cast<size_t>(sz));
        f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(m_buffer.data()), sz)) {
            if (err) *err = "读取文件失败";
            return false;
        }
        return Parse(err);
    }

    inline bool GmdReader::OpenFromMemory(const void* data, size_t size, std::string* err) {
        if (!data || size < kFileHeaderSize) { if (err) *err = "数据过小"; return false; }
        m_buffer.assign(static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + size);
        return Parse(err);
    }

    inline const ChunkEntry* GmdReader::Find(uint32_t type) const {
        for (const auto& c : m_chunks) if (c.type == type) return &c;
        return nullptr;
    }

    inline bool GmdReader::Parse(std::string* err) {
        auto Fail = [&](const char* m) { if (err) *err = m; return false; };

        if (m_buffer.size() < kFileHeaderSize) return Fail("文件小于文件头");
        std::memcpy(&m_header, m_buffer.data(), sizeof(FileHeader));

        if (m_header.magic != kMagic)                       return Fail("魔数不匹配");
        if (m_header.versionMajor != kVersionMajor)         return Fail("主版本不匹配");
        if (m_header.headerSize != kFileHeaderSize)         return Fail("header 大小不匹配");
        if (m_header.fileSize != m_buffer.size())           return Fail("文件大小不匹配");

        // 块表
        uint64_t tableBytes = static_cast<uint64_t>(m_header.chunkCount) * kChunkEntrySize;
        if (m_header.chunkTableOffset + tableBytes > m_buffer.size()) return Fail("块表越界");
        m_chunks.resize(m_header.chunkCount);
        std::memcpy(m_chunks.data(),
            m_buffer.data() + m_header.chunkTableOffset,
            static_cast<size_t>(tableBytes));

        // META
        const ChunkEntry* metaChunk = Find(CHUNK_META);
        if (!metaChunk) return Fail("缺少 META 块");
        if (metaChunk->size < sizeof(ModelMeta)) return Fail("META 过小");
        std::memcpy(&m_meta, m_buffer.data() + metaChunk->offset, sizeof(ModelMeta));

        // 字符串表
        if (m_meta.stringTableSize > 0) {
            if (m_meta.stringTableOffset + m_meta.stringTableSize > m_buffer.size())
                return Fail("字符串表越界");
            m_stringTable.assign(
                reinterpret_cast<const char*>(m_buffer.data() + m_meta.stringTableOffset),
                static_cast<size_t>(m_meta.stringTableSize));
        }

        m_model.name = Str(m_meta.nameOffset);
        m_model.flags = m_meta.flags;
        for (int i = 0; i < 3; ++i) {
            m_model.boundsMin[i] = m_meta.boundsMin[i];
            m_model.boundsMax[i] = m_meta.boundsMax[i];
        }

        // ---- VRTX ----
        if (auto* c = Find(CHUNK_VRTX)) {
            uint32_t n = m_meta.vertexStreamCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(VertexStreamDesc))
                return Fail("VRTX 过小");
            const uint8_t* p = m_buffer.data() + c->offset;
            m_model.vertexStreams.resize(n);
            std::memcpy(m_model.vertexStreams.data(), p,
                static_cast<size_t>(n) * sizeof(VertexStreamDesc));

            uint64_t maxEnd = 0;
            for (const auto& vs : m_model.vertexStreams)
                maxEnd = std::max(maxEnd, vs.dataOffset + vs.dataSize);
            if (maxEnd > c->size) return Fail("顶点数据越界");
            m_model.vertexData.assign(p, p + maxEnd);
        }

        // ---- INDX ----
        if (auto* c = Find(CHUNK_INDX)) {
            uint32_t n = m_meta.indexBufferCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(IndexBufferDesc))
                return Fail("INDX 过小");
            const uint8_t* p = m_buffer.data() + c->offset;
            m_model.indexBuffers.resize(n);
            std::memcpy(m_model.indexBuffers.data(), p,
                static_cast<size_t>(n) * sizeof(IndexBufferDesc));

            uint64_t maxEnd = 0;
            for (const auto& ib : m_model.indexBuffers)
                maxEnd = std::max(maxEnd, ib.dataOffset + ib.dataSize);
            if (maxEnd > c->size) return Fail("索引数据越界");
            m_model.indexData.assign(p, p + maxEnd);
        }

        // ---- SUBM ----
        if (auto* c = Find(CHUNK_SUBM)) {
            uint32_t n = m_meta.submeshCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(Submesh)) return Fail("SUBM 过小");
            m_model.submeshes.resize(n);
            std::memcpy(m_model.submeshes.data(),
                m_buffer.data() + c->offset,
                static_cast<size_t>(n) * sizeof(Submesh));
        }

        // ---- MATL ----
        if (auto* c = Find(CHUNK_MATL)) {
            uint32_t n = m_meta.materialCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(Material)) return Fail("MATL 过小");
            const uint8_t* p = m_buffer.data() + c->offset;
            m_model.materials.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                auto& M = m_model.materials[i];
                std::memcpy(&M.mat, p + i * sizeof(Material), sizeof(Material));
                M.name = Str(M.mat.nameOffset);
                if (M.mat.customDataSize > 0 &&
                    M.mat.customDataOffset + M.mat.customDataSize <= c->size) {
                    M.customData.assign(
                        p + M.mat.customDataOffset,
                        p + M.mat.customDataOffset + M.mat.customDataSize);
                }
            }
        }

        // ---- TEXI / TEXD ----
        if (auto* c = Find(CHUNK_TEXI)) {
            uint32_t n = m_meta.textureCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(TextureDesc))
                return Fail("TEXI 过小");
            const uint8_t* p = m_buffer.data() + c->offset;

            const uint8_t* texdBase = nullptr;
            uint64_t texdSize = 0;
            if (auto* d = Find(CHUNK_TEXD)) {
                texdBase = m_buffer.data() + d->offset;
                texdSize = d->size;
            }

            m_model.textures.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                auto& T = m_model.textures[i];
                std::memcpy(&T.desc, p + i * sizeof(TextureDesc), sizeof(TextureDesc));
                T.name = Str(T.desc.nameOffset);
                T.uri = T.desc.uriOffset ? Str(T.desc.uriOffset) : std::string();

                if (T.desc.mipsOffset && T.desc.mipCount > 0) {
                    uint64_t need = static_cast<uint64_t>(T.desc.mipCount) * sizeof(TextureMipInfo);
                    if (T.desc.mipsOffset + need > c->size) return Fail("mip 表越界");
                    T.mips.resize(T.desc.mipCount);
                    std::memcpy(T.mips.data(), p + T.desc.mipsOffset, static_cast<size_t>(need));
                }

                if (T.desc.source == TEX_SOURCE_EMBEDDED && texdBase) {
                    if (T.desc.dataOffset + T.desc.dataSize > texdSize)
                        return Fail("内嵌纹理数据越界");
                    T.embeddedData.assign(texdBase + T.desc.dataOffset,
                        texdBase + T.desc.dataOffset + T.desc.dataSize);
                }
            }
        }

        // ---- SMPL ----
        if (auto* c = Find(CHUNK_SMPL)) {
            uint32_t n = m_meta.samplerCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(SamplerDesc))
                return Fail("SMPL 过小");
            m_model.samplers.resize(n);
            std::memcpy(m_model.samplers.data(),
                m_buffer.data() + c->offset,
                static_cast<size_t>(n) * sizeof(SamplerDesc));
        }

        // ---- SKEL ----
        if (auto* c = Find(CHUNK_SKEL)) {
            uint32_t n = m_meta.skeletonCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(Skeleton)) return Fail("SKEL 过小");
            const uint8_t* p = m_buffer.data() + c->offset;
            m_model.skeletons.resize(n);
            std::memcpy(m_model.skeletons.data(), p,
                static_cast<size_t>(n) * sizeof(Skeleton));

            // 简化：只加载第一个 skeleton 的 bones / IBM
            if (n > 0) {
                const auto& s = m_model.skeletons[0];
                if (s.boneCount > 0 &&
                    s.bonesOffset + static_cast<uint64_t>(s.boneCount) * sizeof(Bone) <= c->size) {
                    m_model.bones.resize(s.boneCount);
                    std::memcpy(m_model.bones.data(), p + s.bonesOffset,
                        static_cast<size_t>(s.boneCount) * sizeof(Bone));
                }
                if (s.inverseBindMatricesOffset && s.boneCount > 0) {
                    uint64_t bytes = static_cast<uint64_t>(s.boneCount) * 16 * sizeof(float);
                    if (s.inverseBindMatricesOffset + bytes <= c->size) {
                        m_model.inverseBindMatrices.resize(static_cast<size_t>(s.boneCount) * 16);
                        std::memcpy(m_model.inverseBindMatrices.data(),
                            p + s.inverseBindMatricesOffset,
                            static_cast<size_t>(bytes));
                    }
                }
            }
        }

        // ---- ANIM ----
        if (auto* c = Find(CHUNK_ANIM)) {
            uint32_t n = m_meta.animationCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(Animation)) return Fail("ANIM 过小");
            const uint8_t* p = m_buffer.data() + c->offset;
            m_model.animations.resize(n);

            for (uint32_t i = 0; i < n; ++i) {
                auto& A = m_model.animations[i];
                std::memcpy(&A.anim, p + i * sizeof(Animation), sizeof(Animation));

                if (A.anim.channelCount == 0) continue;
                uint64_t chBytes = static_cast<uint64_t>(A.anim.channelCount) * sizeof(AnimationChannel);
                if (A.anim.channelsOffset + chBytes > c->size) return Fail("动画通道越界");

                A.channels.resize(A.anim.channelCount);
                std::memcpy(A.channels.data(), p + A.anim.channelsOffset,
                    static_cast<size_t>(chBytes));
                A.times.resize(A.channels.size());
                A.values.resize(A.channels.size());

                for (size_t j = 0; j < A.channels.size(); ++j) {
                    const auto& ch = A.channels[j];
                    uint32_t comps = (ch.path == 1) ? 4 : 3; // 1 = rotation

                    if (ch.timesOffset && ch.keyCount) {
                        uint64_t bytes = static_cast<uint64_t>(ch.keyCount) * sizeof(float);
                        if (ch.timesOffset + bytes <= c->size) {
                            A.times[j].resize(ch.keyCount);
                            std::memcpy(A.times[j].data(), p + ch.timesOffset,
                                static_cast<size_t>(bytes));
                        }
                    }
                    if (ch.valuesOffset && ch.keyCount) {
                        uint64_t bytes = static_cast<uint64_t>(ch.keyCount) * comps * sizeof(float);
                        if (ch.valuesOffset + bytes <= c->size) {
                            A.values[j].resize(static_cast<size_t>(ch.keyCount) * comps);
                            std::memcpy(A.values[j].data(), p + ch.valuesOffset,
                                static_cast<size_t>(bytes));
                        }
                    }
                }
            }
        }

        // ---- LODS ----
        if (auto* c = Find(CHUNK_LODS)) {
            uint32_t n = m_meta.lodCount;
            if (c->size < static_cast<uint64_t>(n) * sizeof(LodDesc)) return Fail("LODS 过小");
            m_model.lods.resize(n);
            std::memcpy(m_model.lods.data(),
                m_buffer.data() + c->offset,
                static_cast<size_t>(n) * sizeof(LodDesc));
        }

        return true;
    }

    // ============================================================================
    // GmdWriter
    // ============================================================================

    class GmdWriter {
    public:
        static bool Save(const std::string& path,
            const GmdModel& model,
            std::string* err = nullptr);
    };

    inline bool GmdWriter::Save(const std::string& path,
        const GmdModel& model,
        std::string* err) {
        auto Fail = [&](const char* m) { if (err) *err = m; return false; };

        // -------- 1. 构建字符串表 --------
        std::vector<uint8_t> strTab;
        std::unordered_map<std::string, uint32_t> strMap;
        strTab.push_back(0); // offset 0 = ""
        strMap[""] = 0;
        auto AddStr = [&](const std::string& s) -> uint32_t {
            auto it = strMap.find(s);
            if (it != strMap.end()) return it->second;
            uint32_t off = static_cast<uint32_t>(strTab.size());
            strTab.insert(strTab.end(), s.begin(), s.end());
            strTab.push_back(0);
            strMap[s] = off;
            return off;
            };

        uint32_t modelNameOff = AddStr(model.name);

        std::vector<uint32_t> matNameOff(model.materials.size());
        for (size_t i = 0; i < model.materials.size(); ++i)
            matNameOff[i] = AddStr(model.materials[i].name);

        std::vector<uint32_t> texNameOff(model.textures.size());
        std::vector<uint32_t> texUriOff(model.textures.size());
        for (size_t i = 0; i < model.textures.size(); ++i) {
            texNameOff[i] = AddStr(model.textures[i].name);
            texUriOff[i] = model.textures[i].uri.empty() ? 0 : AddStr(model.textures[i].uri);
        }

        std::vector<uint32_t> smpNameOff(model.samplers.size(), 0);   // SamplerDesc 无名字段
        std::vector<uint32_t> animNameOff(model.animations.size(), 0);

        // -------- 2. 构建每个块的数据 --------
        enum BlkId {
            BLK_META = 0, BLK_VRTX, BLK_INDX, BLK_SUBM, BLK_MATL,
            BLK_TEXI, BLK_TEXD, BLK_SMPL, BLK_SKEL, BLK_ANIM,
            BLK_LODS, BLK_COUNT
        };
        struct OutChunk {
            uint32_t type = 0;
            std::vector<uint8_t> data;
            uint64_t fileOffset = 0;
        };
        OutChunk blk[BLK_COUNT];
        blk[BLK_META].type = CHUNK_META;
        blk[BLK_VRTX].type = CHUNK_VRTX;
        blk[BLK_INDX].type = CHUNK_INDX;
        blk[BLK_SUBM].type = CHUNK_SUBM;
        blk[BLK_MATL].type = CHUNK_MATL;
        blk[BLK_TEXI].type = CHUNK_TEXI;
        blk[BLK_TEXD].type = CHUNK_TEXD;
        blk[BLK_SMPL].type = CHUNK_SMPL;
        blk[BLK_SKEL].type = CHUNK_SKEL;
        blk[BLK_ANIM].type = CHUNK_ANIM;
        blk[BLK_LODS].type = CHUNK_LODS;

        // ---- META ----
        {
            auto& d = blk[BLK_META].data;
            d.resize(sizeof(ModelMeta), 0);
            // 字符串表附在 ModelMeta 之后
            d.insert(d.end(), strTab.begin(), strTab.end());
        }

        // ---- VRTX ----
        {
            auto& d = blk[BLK_VRTX].data;
            uint32_t n = static_cast<uint32_t>(model.vertexStreams.size());
            size_t descBytes = static_cast<size_t>(n) * sizeof(VertexStreamDesc);
            size_t dataStart = static_cast<size_t>(AlignUp(descBytes, 4));
            d.resize(dataStart + model.vertexData.size(), 0);

            for (uint32_t i = 0; i < n; ++i) {
                VertexStreamDesc vs = model.vertexStreams[i];
                vs.dataOffset = dataStart + vs.dataOffset; // 整体平移
                std::memcpy(d.data() + i * sizeof(VertexStreamDesc), &vs, sizeof(vs));
            }
            if (!model.vertexData.empty())
                std::memcpy(d.data() + dataStart, model.vertexData.data(), model.vertexData.size());
        }

        // ---- INDX ----
        {
            auto& d = blk[BLK_INDX].data;
            uint32_t n = static_cast<uint32_t>(model.indexBuffers.size());
            size_t descBytes = static_cast<size_t>(n) * sizeof(IndexBufferDesc);
            size_t dataStart = static_cast<size_t>(AlignUp(descBytes, 4));
            d.resize(dataStart + model.indexData.size(), 0);

            for (uint32_t i = 0; i < n; ++i) {
                IndexBufferDesc ib = model.indexBuffers[i];
                ib.dataOffset = dataStart + ib.dataOffset;
                std::memcpy(d.data() + i * sizeof(IndexBufferDesc), &ib, sizeof(ib));
            }
            if (!model.indexData.empty())
                std::memcpy(d.data() + dataStart, model.indexData.data(), model.indexData.size());
        }

        // ---- SUBM ----
        {
            auto& d = blk[BLK_SUBM].data;
            uint32_t n = static_cast<uint32_t>(model.submeshes.size());
            d.resize(static_cast<size_t>(n) * sizeof(Submesh));
            if (n) std::memcpy(d.data(), model.submeshes.data(), d.size());
        }

        // ---- MATL ----
        {
            auto& d = blk[BLK_MATL].data;
            uint32_t n = static_cast<uint32_t>(model.materials.size());
            size_t headerBytes = static_cast<size_t>(n) * sizeof(Material);
            d.resize(headerBytes, 0);

            size_t cursor = headerBytes;
            for (uint32_t i = 0; i < n; ++i) {
                Material m = model.materials[i].mat;
                m.nameOffset = matNameOff[i];

                const auto& cd = model.materials[i].customData;
                if (!cd.empty()) {
                    cursor = static_cast<size_t>(AlignUp(cursor, 4));
                    m.customDataOffset = static_cast<uint32_t>(cursor);
                    m.customDataSize = static_cast<uint32_t>(cd.size());
                    d.resize(cursor + cd.size(), 0);
                    std::memcpy(d.data() + cursor, cd.data(), cd.size());
                    cursor += cd.size();
                }
                else {
                    m.customDataOffset = 0;
                    m.customDataSize = 0;
                }
                std::memcpy(d.data() + i * sizeof(Material), &m, sizeof(m));
            }
        }

        // ---- TEXI ----
        {
            auto& d = blk[BLK_TEXI].data;
            uint32_t n = static_cast<uint32_t>(model.textures.size());
            size_t descBytes = static_cast<size_t>(n) * sizeof(TextureDesc);
            d.resize(descBytes, 0);

            // 显式 mip 表紧跟在 desc 之后
            size_t cursor = descBytes;
            for (uint32_t i = 0; i < n; ++i) {
                TextureDesc td = model.textures[i].desc;
                td.nameOffset = texNameOff[i];
                td.uriOffset = texUriOff[i];

                const auto& mips = model.textures[i].mips;
                if (!mips.empty()) {
                    cursor = static_cast<size_t>(AlignUp(cursor, 8));
                    td.mipsOffset = cursor;
                    td.mipCount = static_cast<uint32_t>(mips.size());
                    d.resize(cursor + mips.size() * sizeof(TextureMipInfo), 0);
                    std::memcpy(d.data() + cursor, mips.data(),
                        mips.size() * sizeof(TextureMipInfo));
                    cursor += mips.size() * sizeof(TextureMipInfo);
                }
                else {
                    td.mipsOffset = 0;
                    if (td.mipCount == 0) td.mipCount = 1;
                }
                std::memcpy(d.data() + i * sizeof(TextureDesc), &td, sizeof(td));
            }
        }

        // ---- TEXD ----
        {
            auto& d = blk[BLK_TEXD].data;
            uint32_t n = static_cast<uint32_t>(model.textures.size());
            // 对齐累计
            size_t cursor = 0;
            // 先排序描述符偏移：临时副本
            std::vector<TextureDesc> descs(n);
            for (uint32_t i = 0; i < n; ++i)
                std::memcpy(&descs[i], blk[BLK_TEXI].data.data() + i * sizeof(TextureDesc),
                    sizeof(TextureDesc));

            for (uint32_t i = 0; i < n; ++i) {
                const auto& T = model.textures[i];
                if (T.desc.source != TEX_SOURCE_EMBEDDED || T.embeddedData.empty()) {
                    descs[i].dataOffset = 0;
                    descs[i].dataSize = 0;
                    continue;
                }
                cursor = static_cast<size_t>(AlignUp(cursor, kTextureDataAlignment));
                descs[i].dataOffset = cursor;
                descs[i].dataSize = T.embeddedData.size();
                descs[i].crc32 = Crc32(T.embeddedData.data(), T.embeddedData.size());
                d.resize(cursor + T.embeddedData.size(), 0);
                std::memcpy(d.data() + cursor, T.embeddedData.data(), T.embeddedData.size());
                cursor += T.embeddedData.size();
            }
            // 把更新过的描述符写回 TEXI
            for (uint32_t i = 0; i < n; ++i)
                std::memcpy(blk[BLK_TEXI].data.data() + i * sizeof(TextureDesc),
                    &descs[i], sizeof(TextureDesc));
        }

        // ---- SMPL ----
        {
            auto& d = blk[BLK_SMPL].data;
            uint32_t n = static_cast<uint32_t>(model.samplers.size());
            d.resize(static_cast<size_t>(n) * sizeof(SamplerDesc));
            for (uint32_t i = 0; i < n; ++i) {
                SamplerDesc s = model.samplers[i];
                s.nameOffset = smpNameOff[i];
                std::memcpy(d.data() + i * sizeof(SamplerDesc), &s, sizeof(s));
            }
        }

        // ---- SKEL ----
        {
            auto& d = blk[BLK_SKEL].data;
            uint32_t n = static_cast<uint32_t>(model.skeletons.size());
            size_t skelBytes = static_cast<size_t>(n) * sizeof(Skeleton);
            size_t boneBytes = model.bones.size() * sizeof(Bone);
            size_t ibmBytes = model.inverseBindMatrices.size() * sizeof(float);

            size_t cursor = skelBytes;
            size_t bonesStart = 0, ibmStart = 0;

            if (boneBytes) {
                cursor = static_cast<size_t>(AlignUp(cursor, 8));
                bonesStart = cursor;
                cursor += boneBytes;
            }
            if (ibmBytes) {
                cursor = static_cast<size_t>(AlignUp(cursor, 16));
                ibmStart = cursor;
                cursor += ibmBytes;
            }

            d.resize(cursor, 0);

            if (n) {
                std::memcpy(d.data(), model.skeletons.data(), skelBytes);
                if (boneBytes) {
                    std::memcpy(d.data() + bonesStart, model.bones.data(), boneBytes);
                    // 更新第一个 skeleton 的 bonesOffset
                    PatchU64(d, offsetof(Skeleton, bonesOffset), bonesStart);
                }
                if (ibmBytes) {
                    std::memcpy(d.data() + ibmStart,
                        model.inverseBindMatrices.data(), ibmBytes);
                    PatchU64(d, offsetof(Skeleton, inverseBindMatricesOffset), ibmStart);
                }
            }
        }

        // ---- ANIM ----
        {
            auto& d = blk[BLK_ANIM].data;
            uint32_t n = static_cast<uint32_t>(model.animations.size());
            size_t headerBytes = static_cast<size_t>(n) * sizeof(Animation);
            d.resize(headerBytes, 0);

            size_t cursor = headerBytes;
            for (uint32_t i = 0; i < n; ++i) {
                const auto& A = model.animations[i];
                Animation anim = A.anim;
                anim.nameOffset = animNameOff[i];

                size_t chBytes = A.channels.size() * sizeof(AnimationChannel);
                cursor = static_cast<size_t>(AlignUp(cursor, 8));
                size_t chStart = cursor;
                cursor += chBytes;

                // 拷贝通道结构并分配 times / values
                std::vector<AnimationChannel> chans = A.channels;
                for (size_t j = 0; j < chans.size(); ++j) {
                    uint32_t comps = (chans[j].path == 1) ? 4 : 3;
                    if (j < A.times.size() && !A.times[j].empty()) {
                        size_t tb = A.times[j].size() * sizeof(float);
                        cursor = static_cast<size_t>(AlignUp(cursor, 8));
                        chans[j].timesOffset = cursor;
                        chans[j].keyCount = static_cast<uint32_t>(A.times[j].size());
                        cursor += tb;
                    }
                    else {
                        chans[j].timesOffset = 0;
                    }
                    if (j < A.values.size() && !A.values[j].empty()) {
                        size_t vb = A.values[j].size() * sizeof(float);
                        cursor = static_cast<size_t>(AlignUp(cursor, 8));
                        chans[j].valuesOffset = cursor;
                        cursor += vb;
                    }
                    else {
                        chans[j].valuesOffset = 0;
                    }
                    (void)comps;
                }

                anim.channelCount = static_cast<uint32_t>(chans.size());
                anim.channelsOffset = chStart;

                d.resize(cursor, 0);
                std::memcpy(d.data() + i * sizeof(Animation), &anim, sizeof(anim));
                if (chBytes) std::memcpy(d.data() + chStart, chans.data(), chBytes);

                // times / values
                for (size_t j = 0; j < chans.size(); ++j) {
                    if (chans[j].timesOffset && j < A.times.size() && !A.times[j].empty()) {
                        std::memcpy(d.data() + chans[j].timesOffset,
                            A.times[j].data(),
                            A.times[j].size() * sizeof(float));
                    }
                    if (chans[j].valuesOffset && j < A.values.size() && !A.values[j].empty()) {
                        std::memcpy(d.data() + chans[j].valuesOffset,
                            A.values[j].data(),
                            A.values[j].size() * sizeof(float));
                    }
                }
            }
        }

        // ---- LODS ----
        {
            auto& d = blk[BLK_LODS].data;
            uint32_t n = static_cast<uint32_t>(model.lods.size());
            d.resize(static_cast<size_t>(n) * sizeof(LodDesc));
            if (n) std::memcpy(d.data(), model.lods.data(), d.size());
        }

        // -------- 3. 计算布局 --------
        // 只写出数据非空的块；META 永远写出
        std::vector<int> order;
        for (int i = 0; i < BLK_COUNT; ++i) {
            if (i == BLK_META || !blk[i].data.empty()) order.push_back(i);
        }

        uint32_t chunkCount = static_cast<uint32_t>(order.size());
        uint64_t tableEnd = kFileHeaderSize + static_cast<uint64_t>(chunkCount) * kChunkEntrySize;
        uint64_t cursor = AlignUp(tableEnd, kChunkAlignment);
        for (int id : order) {
            blk[id].fileOffset = cursor;
            cursor = AlignUp(cursor + blk[id].data.size(), kChunkAlignment);
        }
        uint64_t totalSize = cursor;

        // -------- 4. 修补跨块偏移 --------
        // 工具：某块的文件偏移
        auto FileOff = [&](int id) -> uint64_t { return blk[id].fileOffset; };

        // 字符串表绝对偏移 = META 块起始 + sizeof(ModelMeta)
        uint64_t stringTableAbs = FileOff(BLK_META) + sizeof(ModelMeta);

        // META 中的偏移全部写为绝对文件偏移（reader 端只看块表，不依赖这些字段）
        {
            auto& d = blk[BLK_META].data;
            ModelMeta meta{};
            meta.nameOffset = modelNameOff;
            meta.vertexStreamsOffset = FileOff(BLK_VRTX);
            meta.indexBuffersOffset = FileOff(BLK_INDX);
            meta.submeshesOffset = FileOff(BLK_SUBM);
            meta.materialsOffset = FileOff(BLK_MATL);
            meta.skeletonsOffset = FileOff(BLK_SKEL);
            meta.animationsOffset = FileOff(BLK_ANIM);
            meta.lodsOffset = FileOff(BLK_LODS);
            meta.stringTableOffset = stringTableAbs;
            meta.stringTableSize = strTab.size();
            for (int i = 0; i < 3; ++i) {
                meta.boundsMin[i] = model.boundsMin[i];
                meta.boundsMax[i] = model.boundsMax[i];
            }
            meta.nameLength = static_cast<uint32_t>(model.name.size());
            meta.flags = model.flags;
            meta.vertexStreamCount = static_cast<uint32_t>(model.vertexStreams.size());
            meta.indexBufferCount = static_cast<uint32_t>(model.indexBuffers.size());
            meta.submeshCount = static_cast<uint32_t>(model.submeshes.size());
            meta.materialCount = static_cast<uint32_t>(model.materials.size());
            meta.skeletonCount = static_cast<uint32_t>(model.skeletons.size());
            meta.animationCount = static_cast<uint32_t>(model.animations.size());
            meta.lodCount = static_cast<uint32_t>(model.lods.size());
            meta.textureCount = static_cast<uint32_t>(model.textures.size());
            meta.samplerCount = static_cast<uint32_t>(model.samplers.size());
            std::memcpy(d.data(), &meta, sizeof(meta));
        }

        // -------- 5. 写文件 --------
        std::vector<uint8_t> out(totalSize, 0);

        // 块表
        uint64_t tableOff = kFileHeaderSize;
        for (uint32_t i = 0; i < chunkCount; ++i) {
            int id = order[i];
            ChunkEntry e{};
            e.type = blk[id].type;
            e.flags = 0;
            e.offset = blk[id].fileOffset;
            e.size = blk[id].data.size();
            e.compressedSize = blk[id].data.size();
            e.alignment = kChunkAlignment;
            e.crc32 = Crc32(blk[id].data.data(), blk[id].data.size());
            std::memcpy(out.data() + tableOff + i * kChunkEntrySize, &e, sizeof(e));
        }

        // 块数据
        for (int id : order) {
            std::memcpy(out.data() + blk[id].fileOffset,
                blk[id].data.data(), blk[id].data.size());
        }

        // FileHeader
        FileHeader hdr{};
        hdr.magic = kMagic;
        hdr.versionMajor = kVersionMajor;
        hdr.versionMinor = kVersionMinor;
        hdr.flags = model.flags;
        hdr.headerSize = kFileHeaderSize;
        hdr.fileSize = totalSize;
        hdr.chunkTableOffset = kFileHeaderSize;
        hdr.chunkCount = chunkCount;
        // headerCRC32：先把 CRC 字段置 0 再计算
        hdr.headerCRC32 = 0;
        hdr.headerCRC32 = Crc32(&hdr, sizeof(hdr));
        std::memcpy(out.data(), &hdr, sizeof(hdr));

        // 落盘
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return Fail("无法写入文件");
        f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
        if (!f) return Fail("写入文件失败");

        return true;
    }

} // namespace gmd