#pragma once
// ============================================================================
// GMD (Geometry Model Data) 二进制格式规范
// 版本: 1.0
// 文件扩展名: .gmd
// ============================================================================
//
// 1. 总结构
// ----------------------------------------------------------------------------
//   [FileHeader 64B]
//   [Chunk 数据区...]         (每个 Chunk 按 alignment 对齐)
//   [ChunkTable]              (ChunkEntry[chunkCount], 位于 chunkTableOffset)
//
//   - 文件由固定 64 字节头部开始。
//   - 所有 Chunk 数据位于 FileHeader 之后、ChunkTable 之前。
//   - ChunkTable 位于文件末尾附近，由 FileHeader.chunkTableOffset 指向。
//   - Chunk 之间可任意顺序排列，读取器必须依据 ChunkTable 定位。
//
// 2. 字节序
// ----------------------------------------------------------------------------
//   - 文件固定为小端字节序 (Little-Endian)。
//   - 魔数 kMagic = 0x42444D47，在文件中按小端写为 47 4D 44 42 ("GMDB")。
//   - FILE_FLAG_LITTLE_ENDIAN 必须置位；若未置位，视为格式非法。
//   - 读取器若运行于大端平台，必须逐字段转换；禁止直接 reinterpret_cast
//     结构体指针 (对齐与端序均不保证)。
//   - 所有多字节整数、浮点均为小端 IEEE-754。
//
// 3. 对齐
// ----------------------------------------------------------------------------
//   - FileHeader 位于文件偏移 0，大小 64 字节，天然 64 字节对齐。
//   - 每个 Chunk 数据起始偏移必须按 ChunkEntry.alignment 对齐。
//   - ChunkEntry.alignment == 0 时使用默认值 kChunkAlignment (64)。
//   - ChunkTable 起始偏移必须按 8 字节对齐。
//   - 纹理数据 (TEXD 内每个 mip) 起始偏移必须按 kTextureDataAlignment
//     (256) 对齐。
//   - 对齐填充字节内容无定义，读取器必须跳过；写入器建议填 0。
//
// 4. 偏移基准 (关键)
// ----------------------------------------------------------------------------
//   为避免歧义，所有偏移按以下规则解释：
//
//   (a) 文件级偏移 (FileHeader / ChunkEntry 内)：
//       - 未使用文件级压缩时：相对文件起始 (文件绝对偏移)。
//       - 使用文件级压缩时：
//           * FileHeader / ChunkTable 内偏移仍是物理文件偏移；
//           * ChunkEntry.offset 相对"解压后的 Chunk 数据区起始"，
//             即 FileHeader 之后解压缓冲区的字节 0。
//       - 判断依据：FileHeader.codec != COMPRESSION_NONE。
//
//   (b) 块内偏移 (块自有结构体内的 offset 字段)：
//       相对该块"解压后数据起始"。
//       即使该块被 ChunkEntry.codec 压缩，也先解压再解释。
//
//   (c) 跨块引用 (例如 ModelMeta 引用 TEXI/SMPL 内数组)：
//       必须写明目标块类型，且偏移相对目标块"解压后数据起始"。
//       读取器先按 FourCC 在 ChunkTable 中定位目标块，再加偏移。
//
//   (d) 字符串偏移 (nameOffset/uriOffset 等)：
//       相对 StringTable 起始 (StringTable 位于 META 块内)，
//       由 ModelMeta.stringTableOffset 与 stringTableSize 界定。
//
//   (e) 0 值语义：
//       - StringTable 偏移 0 必须是一个空字符串 ('\0')。
//         因此 nameOffset == 0 一律表示"无名称"，与 nameLength == 0 等价。
//       - 无 URI 时，uriOffset 写 0 且对应长度写 0。
//       - "无效索引" 使用 kInvalidIndex (0xFFFFFFFF)。
//       - 结构体中 offset == 0 是否表示"无"由字段注释明确；
//         若该 offset 合法值可能为 0，则必须另有计数/标志位区分。
//
// 5. 字符串表 (StringTable)
// ----------------------------------------------------------------------------
//   - 编码: UTF-8。
//   - 终止符: 每个字符串以 '\0' 结尾。
//   - 布局: 连续存放，无固定对齐要求。
//   - StringTable 的字节 0 必须为 '\0' (空字符串)，作为"无名称"哨兵。
//   - 所有 nameOffset/uriOffset 指向某个 '\0' 终止字符串的起始字节。
//   - 读取器必须验证 offset < stringTableSize 且字符串在
//     [stringTableOffset, stringTableOffset + stringTableSize) 内终止。
//
// 6. 压缩
// ----------------------------------------------------------------------------
//   - 压缩算法编码位于 CompressionCodec。
//   - 三种压缩层级：文件级、块级、纹理级。
//
//   【文件级压缩】
//   - 由 FileHeader.codec 指定，FILE_FLAG_COMPRESSED 必须置位。
//   - 只压缩 Chunk 数据区；FileHeader 与 ChunkTable 始终不压缩。
//   - 物理布局：
//        偏移 0                                FileHeader 64B，未压缩
//        偏移 64                               压缩流
//        偏移 align8(64 + compressedDataSize)  填充 0
//        偏移 chunkTableOffset                 ChunkTable，未压缩
//        偏移 fileSize                         文件结束
//   - FileHeader.reserved[0..7] 解释为 compressedDataSize (小端 uint64_t)；
//     FileHeader.reserved[8..19] 必须为 0。
//   - 所有 ChunkEntry.codec 必须为 COMPRESSION_NONE。
//   - 所有 ChunkEntry.compressedSize 必须为 0。
//   - ChunkEntry.offset 相对解压后数据区起始，仍按 alignment 对齐。
//   - ChunkEntry.crc32 覆盖解压后 size 字节。
//   - 文件级压缩与块级压缩不可同时使用。
//   - 纹理级压缩仍可叠加，解压顺序：先解文件级数据区，再定位 TEXD，
//     再按 TextureDesc.codec 解纹理数据。
//
//   【块级压缩】
//   - 由 ChunkEntry.codec 指定，CHUNK_FLAG_COMPRESSED 必须置位；
//     FileHeader.codec 必须为 COMPRESSION_NONE。
//   - 压缩数据自 ChunkEntry.offset 起，长度 compressedSize。
//   - 解压后数据长度 ChunkEntry.size，且被视作该块的"块数据起始"，
//     所有块内偏移相对它解释。
//
//   【纹理级压缩】
//   - 由 TextureDesc.codec 指定，TEX_FLAG_COMPRESSED 必须置位。
//   - 压缩对象是该纹理的 dataSize 字节数据。
//   - TextureDesc.dataSize 表示"压缩后物理字节数"；
//     TextureDesc 不提供解压后大小，解压后大小由 TextureFormat 与
//     width/height/depth/arrayLayers/mipCount 唯一推导 (见第 13 节)。
//   - 解压后数据按第 13 节规则切分为 mip。
//
//   【通用约束】
//   - codec 为 COMPRESSION_NONE 时，对应压缩标志必须清除。
//   - compressedSize == 0 表示未压缩，等价于 compressedSize == size。
//
// 7. CRC32
// ----------------------------------------------------------------------------
//   - 算法: 标准 CRC-32/ISO-HDLC (多项式 0xEDB88320，反射，初值 0xFFFFFFFF，
//     结果取反，输出异或 0xFFFFFFFF)，即 zlib crc32()。
//   - FileHeader.headerCRC32: 覆盖偏移 0..35 (magic..chunkCount)。
//     headerCRC32 字段本身位于偏移 36..39，不在覆盖范围内。
//   - ChunkEntry.crc32: 覆盖该块"解压后"数据，长度 ChunkEntry.size。
//   - TextureDesc.crc32: 覆盖该纹理"解压后"数据 (dataSize 解压后)。
//     外置纹理 (TEX_SOURCE_EXTERNAL) 时该字段无意义，写 0。
//   - crc32 == 0 视为"未提供校验"，读取器可选择跳过校验。
//
// 8. 版本
// ----------------------------------------------------------------------------
//   - versionMajor 不同: 不兼容，读取器应拒绝。
//   - versionMinor 不同: 向后兼容，读取器应尝试解析并忽略未知字段。
//   - reserved 字段: 写入时必须为 0，读取时必须忽略。
//     (文件级压缩时 reserved[0..7] 例外，见第 6 节)
//
// 9. Chunk 类型
// ----------------------------------------------------------------------------
//   META : 模型元数据 (ModelMeta, StringTable, 全局信息)
//   VRTX : 顶点流描述 (VertexStreamDesc[]) 与顶点数据
//   INDX : 索引缓冲描述 (IndexBufferDesc[]) 与索引数据
//   SUBM : 子网格 (Submesh[])
//   MATL : 材质 (Material[])
//   SKEL : 骨骼与骨架 (Skeleton[], Bone[])
//   ANIM : 动画 (Animation[], AnimationChannel[])
//   LODS : LOD 描述 (LodDesc[])
//   TEXI : 纹理描述表 (TextureDesc[], TextureMipInfo[])
//   TEXD : 内嵌纹理数据 (原始字节)
//   SMPL : 采样器表 (SamplerDesc[])
//
//   未知 FourCC 的块必须被读取器安全跳过。
//
// 10. 顶点属性格式
// ----------------------------------------------------------------------------
//   - attributeMask 描述属性集合；按位序从低到高依次编号 slot 0..7。
//   - VertexStreamDesc.format 每 4 位表示一个 slot 的 VertexAttributeFormat。
//     未使用 slot 填 VAF_UNUSED (0)。
//   - slot 编号恒为 attributeMask 的位索引 (0..7)，与是否连续无关。
//   - 属性在顶点内按 slot 升序紧密排列，偏移由各格式字节大小累加得到。
//   - 分量数由属性语义决定（见下表），不编码进 format。
//   - stride 必须 >= 所有属性字节之和，且 4 字节对齐。
//   - quantization 仅在 FILE_FLAG_QUANTIZED 置位时有意义。
//
//   属性语义与分量数：
//     POSITION  : 3 (VAF_FLOAT32/SNORM16 等)
//     NORMAL    : 3 (或 OCT16_2 时为 2)
//     TANGENT   : 4 (或 OCT16_2 时为 2)
//     TEXCOORD0 : 2
//     TEXCOORD1 : 2
//     COLOR     : 4
//     JOINTS    : 4
//     WEIGHTS   : 4
//
//   属性分量类型字节大小：
//     VAF_FLOAT32 : 4 * 分量数
//     VAF_FLOAT16 : 2 * 分量数
//     VAF_SINT8 / VAF_SNORM8 / VAF_UINT8 / VAF_UNORM8   : 1 * 分量数
//     VAF_SINT16 / VAF_SNORM16 / VAF_UINT16 / VAF_UNORM16 : 2 * 分量数
//     VAF_OCT16_2 : 4 (2 分量，每分量 16 位)
//
//   打包规则：
//     format = SUM( f(slot) << (slot * 4) )，slot 按 attributeMask 位序。
//     未使用的 slot 填 VAF_UNUSED (0)。
//
// 11. 动画数据布局
// ----------------------------------------------------------------------------
//   - timesOffset 指向 float32[ keyCount ]，单位秒，严格递增。
//   - valuesOffset 布局由 path 决定:
//       translation: float32[ keyCount * 3 ]
//       rotation   : float32[ keyCount * 4 ]  (四元数, xyzw 顺序)
//       scale      : float32[ keyCount * 3 ]
//   - interpolation == cubic 时，valuesOffset 后紧跟:
//       inTangentsOffset  (隐式: valuesOffset + valuesSize)
//       outTangentsOffset (隐式: inTangentsOffset + valuesSize)
//     分量数与 values 相同。
//   - rotation 四元数必须已归一化。
//
// 12. 矩阵布局
// ----------------------------------------------------------------------------
//   - Skeleton.inverseBindMatricesOffset 指向 float32[ boneCount * 16 ]，
//     相对 SKEL 块解压后数据起始，必须 16 字节对齐。
//   - offset == 0 表示所有骨骼逆绑定矩阵为单位矩阵。
//   - 矩阵为列主序 (OpenGL 约定)，列向量右乘: v' = M * v。
//   - 元素索引 m[col * 4 + row]，col = 0..3, row = 0..3。
//   - 矩阵 i 对应 Bone[] 中索引 i 的骨骼。
//   - 蒙皮矩阵: skinMatrix(i) = globalTransform(i) * inverseBindMatrix(i)。
//   - Bone.parentIndex == kInvalidIndex 表示根骨骼。
//   - Bone.localRotation 为 xyzw 四元数，必须归一化。
//   - Skeleton.nodeHierarchyOffset 非 0 时指向 uint32[boneCount]，
//     给出骨骼索引的拓扑顺序 (根在前)；为 0 时读取器按 Bone[] 顺序
//     构建，但必须通过 parentIndex 验证无环。
//
// 13. 纹理格式与 mip 推导
// ----------------------------------------------------------------------------
//   - TextureFormat 混用像素格式与容器格式，读取器按以下规则处理:
//       TEX_FORMAT_PNG/JPEG/KTX2/DDS 为"容器格式"，数据为编码文件，
//       运行期解码；其余为"像素格式"，数据可直接上传 GPU。
//   - TEX_FLAG_SRGB 与像素格式中的 _SRGB 后缀等价，两者必须一致；
//     若冲突，以 TEX_FLAG_SRGB 为准。
//   - TEX_FORMAT_BGRA8_* 与 TEX_FORMAT_RGBA8_* 的通道顺序固定为
//     内存字节顺序 (B,G,R,A 或 R,G,B,A)。
//   - 未压缩像素数据按行紧密打包 (rowPitch = width * bpp)。
//   - 每个 mip 数据起始按 kTextureDataAlignment (256) 对齐，
//     填充字节无定义。
//
//   【容器格式】
//   - TextureDesc.mipCount 必须为 1，arrayLayers 必须为 1，depth 必须为 1。
//   - TEX_FLAG_HAS_MIPS / TEX_FLAG_CUBEMAP / TEX_FLAG_ARRAY 必须清除。
//   - dataOffset / dataSize 指向完整编码文件。
//   - 容器内部信息由运行期解码器处理，不参与隐式推导。
//
//   【mip 隐式推导】(mipsOffset == 0 且为像素/GPU 压缩格式)
//   - 设 layers = arrayLayers，mipCount = TextureDesc.mipCount。
//   - 对每个 mip i = 0 .. mipCount-1:
//       width_i  = max(1, width  >> i)
//       height_i = max(1, height >> i)
//       depth_i  = max(1, depth  >> i)
//       sliceSize = 单层单深度切片字节数 (见下)
//       mipSize_i = sliceSize * layers * depth_i
//   - 单层单深度切片:
//       未压缩像素:  sliceSize = width_i * height_i * bytesPerPixel
//       GPU 压缩:    sliceSize = ceil(width_i / blockW)
//                              * ceil(height_i / blockH)
//                              * blockBytes
//   - GPU 压缩块参数:
//       BC1 / BC4 / ETC2_RGB8           : 4x4,  8 字节
//       BC3 / BC5 / BC6H / BC7          : 4x4, 16 字节
//       ETC2_RGBA8                      : 4x4, 16 字节
//       ASTC_4x4                        : 4x4, 16 字节
//       ASTC_6x6                        : 6x6, 16 字节
//   - 偏移:
//       offset_0 = TextureDesc.dataOffset
//       offset_{i+1} = AlignUp(offset_i + mipSize_i, 256)
//   - TextureDesc.dataSize 必须等于
//       AlignUp(offset_last + mipSize_last, 256) - TextureDesc.dataOffset
//     即包含 mip 间 256 字节对齐填充。
//
//   【mip 显式】(mipsOffset != 0)
//   - 必须存在 mipCount 个 TextureMipInfo。
//   - TextureMipInfo.offset 相对 TEXD 块解压后起始，必须 256 字节对齐。
//   - TextureMipInfo.width / height 必须等于隐式推导值。
//   - TextureMipInfo.size 必须等于隐式推导的 mipSize_i。
//   - 否则拒绝加载。
//
//   【Cubemap / Array 规则】
//   - 普通 2D       : depth = 1, arrayLayers = 1, 两个标志均清除。
//   - 2D 数组       : depth = 1, arrayLayers >= 1,
//                     TEX_FLAG_ARRAY = (arrayLayers > 1),
//                     TEX_FLAG_CUBEMAP = 0。
//   - Cubemap       : depth = 1, width == height, arrayLayers % 6 == 0,
//                     TEX_FLAG_CUBEMAP = 1,
//                     cubemapCount = arrayLayers / 6。
//   - Cubemap 面顺序: +X, -X, +Y, -Y, +Z, -Z。
//   - Cubemap 数组层索引: layer = cubemapIndex * 6 + faceIndex。
//   - Cubemap 且 arrayLayers > 6 时 TEX_FLAG_ARRAY 必须置位。
//   - 3D 纹理       : depth > 1, arrayLayers = 1, 两个标志均清除。
//   - 层内布局: 对每个 mip，按 layer 0..arrayLayers-1、每个 layer 内
//     depth slice 0..depth_i-1 顺序紧密存储。
//   - 容器格式 (PNG/JPEG) 不支持 cubemap/array；
//     KTX2/DDS 内部标志必须与 TextureDesc.flags 一致，冲突则拒绝加载。
//
// 14. 安全验证规则 (读取器必须执行)
// ----------------------------------------------------------------------------
//   - 文件大小 >= kFileHeaderSize。
//   - magic == kMagic。
//   - headerSize == kFileHeaderSize。
//   - chunkTableOffset + chunkCount * kChunkEntrySize <= fileSize。
//   - chunkTableOffset % 8 == 0。
//   - headerCRC32 校验通过 (若非 0)。
//   - 文件级压缩时:
//       * FILE_FLAG_COMPRESSED 置位，codec 合法且非 NONE；
//       * reserved[8..19] == 0，compressedDataSize > 0；
//       * 64 + compressedDataSize <= chunkTableOffset；
//       * chunkTableOffset == AlignUp(64 + compressedDataSize, 8)；
//       * fileSize == chunkTableOffset + chunkCount * kChunkEntrySize；
//       * 所有 ChunkEntry.codec == COMPRESSION_NONE；
//       * 所有 ChunkEntry.compressedSize == 0；
//       * 解压后缓冲区大小 >= max(offset + size)。
//   - 块级压缩时:
//       * FileHeader.codec == COMPRESSION_NONE；
//       * CHUNK_FLAG_COMPRESSED 置位，codec 合法且非 NONE；
//       * offset + compressedSize <= fileSize；
//       * size >= 0。
//   - 每个 Chunk: offset + size <= (文件级压缩 ? 解压缓冲区 : fileSize)。
//   - Chunk 数据区不得与 FileHeader、ChunkTable 重叠。
//   - Chunk 之间不得重叠。
//   - ChunkEntry.alignment 必须为 2 的幂；为 0 时按 64 处理。
//   - 所有计数 (chunkCount/vertexCount/mipCount/...) 乘以元素大小时
//     必须检查整数溢出。
//   - 所有块内偏移 + 长度必须落在对应块范围内。
//   - 字符串 offset 必须在 StringTable 范围内且以 '\0' 终止。
//   - mipCount >= 1；arrayLayers >= 1；depth >= 1。
//   - codec 必须为合法的 CompressionCodec 值。
//   - 同一类型 Chunk 至多出现一次。
//   - Submesh.indexBufferIndex < ModelMeta.indexBufferCount。
//   - Submesh.indexOffset + Submesh.indexCount <=
//     IndexBufferDesc[indexBufferIndex].indexCount。
//   - Submesh.materialIndex < ModelMeta.materialCount。
//   - Submesh.lodIndex < ModelMeta.lodCount。
//   - TextureRef.textureIndex < ModelMeta.textureCount 或为 kInvalidIndex。
//   - TextureRef.samplerIndex < ModelMeta.samplerCount 或为 kInvalidIndex。
//   - LodDesc.submeshStart + LodDesc.submeshCount <= ModelMeta.submeshCount。
//   - AnimationChannel.nodeIndex < 对应 Skeleton.boneCount。
//   - AnimationChannel.times 严格递增；rotation 四元数已归一化。
//   - TextureDesc.source == TEX_SOURCE_EXTERNAL 时 uriOffset 必须非 0，
//     TEX_SOURCE_EMBEDDED 时 dataOffset 有效。
//   - 纹理 cubemap/array 形状符合第 13 节规则。
//   - TEXD 内 dataOffset + dataSize、每个 mip 范围合法。
//   - inverseBindMatrices 范围与对齐合法 (见第 12 节)。
//   - 任何校验失败必须拒绝加载，不得部分使用。
//
// 15. 写入约定
// ----------------------------------------------------------------------------
//   - 所有 reserved 字段写 0 (文件级压缩时 reserved[0..7] 除外)。
//   - 所有未使用 offset 写 0。
//   - 所有未使用索引写 kInvalidIndex。
//   - 未压缩时 codec 写 COMPRESSION_NONE (0)。
//   - 对齐填充写 0。
//   - StringTable 起始处写一个 '\0'。
//   - 结构体必须按本文件 static_assert 保证的大小与字段偏移写出。
//
// 16. 索引缓冲与子网格
// ----------------------------------------------------------------------------
//   - ModelMeta 可引用 0..N 个 IndexBufferDesc。
//   - 每个 Submesh 必须通过 indexBufferIndex 绑定到一个 IndexBufferDesc。
//   - indexOffset 单位为"索引元素"，相对该缓冲起始。
//   - indexCount 不得超过该缓冲的 indexCount。
//   - 索引值 (indexValue) 是全局顶点行号，作用于所有顶点流。
//   - 若 indexBufferCount == 1，indexBufferIndex 必须为 0。
//   - 子网格不允许跨多个索引缓冲。
//
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <cstring>

#pragma pack(push, 1)

namespace gmd {

    // ============================================================================
    // 常量与魔数
    // ============================================================================

    // 魔数：内存字节 'G','M','D','B' -> 小端 uint32_t = 0x42444D47
    constexpr uint32_t kMagic = 0x42444D47;

    constexpr uint16_t kVersionMajor = 1;
    constexpr uint16_t kVersionMinor = 0;

    constexpr uint32_t kFileHeaderSize = 64;
    constexpr uint32_t kChunkEntrySize = 48;
    constexpr uint32_t kChunkAlignment = 64;

    // 纹理数据对齐
    constexpr uint32_t kTextureDataAlignment = 256;

    // 通用“无效”索引
    constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    // FourCC 辅助函数
    constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
        return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
            (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
    }

    // 块类型 FourCC
    constexpr uint32_t CHUNK_META = MakeFourCC('M', 'E', 'T', 'A');
    constexpr uint32_t CHUNK_VRTX = MakeFourCC('V', 'R', 'T', 'X');
    constexpr uint32_t CHUNK_INDX = MakeFourCC('I', 'N', 'D', 'X');
    constexpr uint32_t CHUNK_SUBM = MakeFourCC('S', 'U', 'B', 'M');
    constexpr uint32_t CHUNK_MATL = MakeFourCC('M', 'A', 'T', 'L');
    constexpr uint32_t CHUNK_SKEL = MakeFourCC('S', 'K', 'E', 'L');
    constexpr uint32_t CHUNK_ANIM = MakeFourCC('A', 'N', 'I', 'M');
    constexpr uint32_t CHUNK_LODS = MakeFourCC('L', 'O', 'D', 'S');

    // 贴图相关块
    constexpr uint32_t CHUNK_TEXI = MakeFourCC('T', 'E', 'X', 'I'); // 纹理描述表
    constexpr uint32_t CHUNK_TEXD = MakeFourCC('T', 'E', 'X', 'D'); // 内嵌纹理数据
    constexpr uint32_t CHUNK_SMPL = MakeFourCC('S', 'M', 'P', 'L'); // 采样器表

    // ============================================================================
    // 枚举
    // ============================================================================

    // 文件标志
    enum FileFlags : uint32_t {
        FILE_FLAG_LITTLE_ENDIAN = 1u << 0,
        FILE_FLAG_COMPRESSED = 1u << 1,
        FILE_FLAG_EMBEDDED_TEX = 1u << 2, // 至少存在一条内嵌纹理（TEXD 存在）
        FILE_FLAG_QUANTIZED = 1u << 3,
        FILE_FLAG_UV_TOP_LEFT = 1u << 4,  // 若置位，UV 为 D3D 约定；否则为 OpenGL 约定
    };

    // 块标志
    enum ChunkFlags : uint32_t {
        CHUNK_FLAG_COMPRESSED = 1u << 0,
        CHUNK_FLAG_OPTIONAL = 1u << 1,
    };

    // 顶点属性位
    enum VertexAttribute : uint32_t {
        ATTR_POSITION = 1u << 0,
        ATTR_NORMAL = 1u << 1,
        ATTR_TANGENT = 1u << 2,
        ATTR_TEXCOORD0 = 1u << 3,
        ATTR_TEXCOORD1 = 1u << 4,
        ATTR_COLOR = 1u << 5,
        ATTR_JOINTS = 1u << 6,
        ATTR_WEIGHTS = 1u << 7,
    };

    // 顶点属性分量类型（4 位，每属性一项，打包进 VertexStreamDesc.format）
    enum VertexAttributeFormat : uint32_t {
        VAF_UNUSED = 0,   // 该槽位未使用
        VAF_FLOAT32 = 1,
        VAF_FLOAT16 = 2,
        VAF_SINT8 = 3,
        VAF_SNORM8 = 4,
        VAF_UINT8 = 5,
        VAF_UNORM8 = 6,
        VAF_SINT16 = 7,
        VAF_SNORM16 = 8,
        VAF_UINT16 = 9,
        VAF_UNORM16 = 10,
        VAF_OCT16_2 = 11,  // 八面体编码法线/切线，2 分量
        // 12..15 保留
    };

    // 索引类型
    enum IndexType : uint32_t {
        INDEX_TYPE_UINT16 = 0,
        INDEX_TYPE_UINT32 = 1,
    };

    // 量化标志
    enum QuantizationFlags : uint32_t {
        QUANT_POSITION_INT16 = 1u << 0,
        QUANT_NORMAL_OCT = 1u << 1,
        QUANT_TANGENT_OCT = 1u << 2,
        QUANT_UV_UINT16 = 1u << 3,
    };

    // 压缩算法编码
    enum CompressionCodec : uint32_t {
        COMPRESSION_NONE = 0,
        COMPRESSION_ZSTD = 1,
        COMPRESSION_LZ4 = 2,
        COMPRESSION_DEFLATE = 3,
    };

    // 纹理来源
    enum TextureSource : uint32_t {
        TEX_SOURCE_EMBEDDED = 0, // 数据在 TEXD 块中
        TEX_SOURCE_EXTERNAL = 1, // 通过 URI 从外部加载
    };

    // 纹理格式
    enum TextureFormat : uint32_t {
        TEX_FORMAT_UNKNOWN = 0,

        // 未压缩像素
        TEX_FORMAT_R8_UNORM = 1,
        TEX_FORMAT_RG8_UNORM = 2,
        TEX_FORMAT_RGBA8_UNORM = 3,
        TEX_FORMAT_RGBA8_SRGB = 4,
        TEX_FORMAT_BGRA8_UNORM = 5,
        TEX_FORMAT_BGRA8_SRGB = 6,
        TEX_FORMAT_RGBA16_FLOAT = 7,
        TEX_FORMAT_RGBA32_FLOAT = 8,

        // GPU 压缩
        TEX_FORMAT_BC1_UNORM = 16,
        TEX_FORMAT_BC1_SRGB = 17,
        TEX_FORMAT_BC3_UNORM = 18,
        TEX_FORMAT_BC3_SRGB = 19,
        TEX_FORMAT_BC4_UNORM = 20,
        TEX_FORMAT_BC5_UNORM = 21,
        TEX_FORMAT_BC6H_UF16 = 22,
        TEX_FORMAT_BC7_UNORM = 23,
        TEX_FORMAT_BC7_SRGB = 24,
        TEX_FORMAT_ASTC_4x4_UNORM = 25,
        TEX_FORMAT_ASTC_4x4_SRGB = 26,
        TEX_FORMAT_ASTC_6x6_UNORM = 27,
        TEX_FORMAT_ASTC_6x6_SRGB = 28,
        TEX_FORMAT_ETC2_RGB8 = 29,
        TEX_FORMAT_ETC2_RGBA8 = 30,

        // 容器/编码图像（运行时解码）
        TEX_FORMAT_PNG = 64,
        TEX_FORMAT_JPEG = 65,
        TEX_FORMAT_KTX2 = 66,
        TEX_FORMAT_DDS = 67,
    };

    // 纹理标志
    enum TextureFlags : uint32_t {
        TEX_FLAG_SRGB = 1u << 0, // 颜色空间为 sRGB
        TEX_FLAG_HAS_MIPS = 1u << 1, // 含 mipmap
        TEX_FLAG_CUBEMAP = 1u << 2, // 立方体贴图
        TEX_FLAG_ARRAY = 1u << 3, // 纹理数组
        TEX_FLAG_COMPRESSED = 1u << 4, // TEXD 中该纹理数据被纹理级压缩
        TEX_FLAG_PREMULTIPLIED = 1u << 5, // Alpha 预乘
        TEX_FLAG_GENERATE_MIPS = 1u << 6, // 运行时生成 mip
    };

    // 纹理引用标志
    enum TextureRefFlags : uint32_t {
        TEX_REF_FLAG_KEEP_ALPHA = 1u << 0,
        TEX_REF_FLAG_IGNORE_SRGB = 1u << 1,
    };

    // 采样器过滤
    enum TextureFilter : uint32_t {
        TEX_FILTER_NEAREST = 0,
        TEX_FILTER_LINEAR = 1,
    };

    enum TextureMipFilter : uint32_t {
        TEX_MIP_NEAREST = 0,
        TEX_MIP_LINEAR = 1,
    };

    enum TextureWrap : uint32_t {
        TEX_WRAP_REPEAT = 0,
        TEX_WRAP_CLAMP_TO_EDGE = 1,
        TEX_WRAP_MIRRORED_REPEAT = 2,
        TEX_WRAP_CLAMP_TO_BORDER = 3,
    };

    enum TextureCompareFunc : uint32_t {
        TEX_CMP_NONE = 0,
        TEX_CMP_LESS = 1,
        TEX_CMP_LEQUAL = 2,
        TEX_CMP_GREATER = 3,
        TEX_CMP_GEQUAL = 4,
        TEX_CMP_EQUAL = 5,
        TEX_CMP_NOT_EQUAL = 6,
        TEX_CMP_ALWAYS = 7,
    };

    // 动画通道 path
    enum AnimationPath : uint32_t {
        ANIM_PATH_TRANSLATION = 0,
        ANIM_PATH_ROTATION = 1,
        ANIM_PATH_SCALE = 2,
    };

    // 动画通道 interpolation
    enum AnimationInterpolation : uint32_t {
        ANIM_INTERP_LINEAR = 0,
        ANIM_INTERP_STEP = 1,
        ANIM_INTERP_CUBIC = 2,
    };

    // ============================================================================
    // 辅助函数
    // ============================================================================

    // 将顶点属性格式打包到指定 slot（0..7）
    constexpr uint32_t PackVertexFormat(uint32_t attrSlot, VertexAttributeFormat f) {
        return (static_cast<uint32_t>(f) & 0xF) << (attrSlot * 4);
    }

    // 从打包的 format 中解出指定 slot 的格式
    constexpr VertexAttributeFormat UnpackVertexFormat(uint32_t packed, uint32_t attrSlot) {
        return static_cast<VertexAttributeFormat>((packed >> (attrSlot * 4)) & 0xF);
    }

    // 向上对齐，a 必须为 2 的幂
    constexpr uint64_t AlignUp(uint64_t v, uint64_t a) {
        return (v + a - 1) & ~(a - 1);
    }

    // 判断是否为容器格式
    constexpr bool IsContainerFormat(uint32_t fmt) {
        return fmt == TEX_FORMAT_PNG || fmt == TEX_FORMAT_JPEG ||
            fmt == TEX_FORMAT_KTX2 || fmt == TEX_FORMAT_DDS;
    }

    // 判断是否为 GPU 压缩格式
    constexpr bool IsGPUCompressedFormat(uint32_t fmt) {
        return (fmt >= TEX_FORMAT_BC1_UNORM && fmt <= TEX_FORMAT_ETC2_RGBA8);
    }

    // 判断是否为未压缩像素格式
    constexpr bool IsPixelFormat(uint32_t fmt) {
        if (fmt == TEX_FORMAT_UNKNOWN) return false;
        if (IsContainerFormat(fmt)) return false;
        if (IsGPUCompressedFormat(fmt)) return false;
        return true;
    }

    // 纹理格式块信息
    struct TextureFormatInfo {
        uint32_t blockWidth;    // 像素/GPU 压缩块宽度
        uint32_t blockHeight;   // 像素/GPU 压缩块高度
        uint32_t blockBytes;    // 每块字节数
        uint32_t bytesPerPixel; // 未压缩格式每像素字节数；压缩格式为 0
        bool     compressed;    // 是否 GPU 压缩
        bool     valid;         // 是否已知格式
    };

    constexpr TextureFormatInfo GetTextureFormatInfo(uint32_t fmt) {
        switch (fmt) {
        case TEX_FORMAT_R8_UNORM:        return { 1, 1, 1, 1, false, true };
        case TEX_FORMAT_RG8_UNORM:       return { 1, 1, 2, 2, false, true };
        case TEX_FORMAT_RGBA8_UNORM:
        case TEX_FORMAT_RGBA8_SRGB:
        case TEX_FORMAT_BGRA8_UNORM:
        case TEX_FORMAT_BGRA8_SRGB:      return { 1, 1, 4, 4, false, true };
        case TEX_FORMAT_RGBA16_FLOAT:    return { 1, 1, 8, 8, false, true };
        case TEX_FORMAT_RGBA32_FLOAT:    return { 1, 1, 16, 16, false, true };

        case TEX_FORMAT_BC1_UNORM:
        case TEX_FORMAT_BC1_SRGB:
        case TEX_FORMAT_BC4_UNORM:
        case TEX_FORMAT_ETC2_RGB8:       return { 4, 4, 8, 0, true, true };

        case TEX_FORMAT_BC3_UNORM:
        case TEX_FORMAT_BC3_SRGB:
        case TEX_FORMAT_BC5_UNORM:
        case TEX_FORMAT_BC6H_UF16:
        case TEX_FORMAT_BC7_UNORM:
        case TEX_FORMAT_BC7_SRGB:
        case TEX_FORMAT_ASTC_4x4_UNORM:
        case TEX_FORMAT_ASTC_4x4_SRGB:
        case TEX_FORMAT_ETC2_RGBA8:      return { 4, 4, 16, 0, true, true };

        case TEX_FORMAT_ASTC_6x6_UNORM:
        case TEX_FORMAT_ASTC_6x6_SRGB:   return { 6, 6, 16, 0, true, true };

        default:                          return { 0, 0, 0, 0, false, false };
        }
    }

    // 计算单个 mip 的切片大小 (单层单深度)
    constexpr uint64_t ComputeMipSliceSize(uint32_t fmt, uint32_t w, uint32_t h) {
        TextureFormatInfo info = GetTextureFormatInfo(fmt);
        if (!info.valid) return 0;
        if (info.compressed) {
            uint32_t bx = (w + info.blockWidth - 1) / info.blockWidth;
            uint32_t by = (h + info.blockHeight - 1) / info.blockHeight;
            return static_cast<uint64_t>(bx) * static_cast<uint64_t>(by) *
                static_cast<uint64_t>(info.blockBytes);
        }
        return static_cast<uint64_t>(w) * static_cast<uint64_t>(h) *
            static_cast<uint64_t>(info.bytesPerPixel);
    }

    // 计算单个 mip 的总大小 (含层与深度)
    constexpr uint64_t ComputeMipSize(uint32_t fmt,
        uint32_t w, uint32_t h, uint32_t depth,
        uint32_t arrayLayers) {
        return ComputeMipSliceSize(fmt, w, h) *
            static_cast<uint64_t>(depth) *
            static_cast<uint64_t>(arrayLayers);
    }

    // 计算某个 mip 级别的宽高
    constexpr uint32_t MipWidth(uint32_t base, uint32_t level) {
        uint32_t v = base >> level;
        return v == 0 ? 1u : v;
    }

    constexpr uint32_t MipHeight(uint32_t base, uint32_t level) {
        uint32_t v = base >> level;
        return v == 0 ? 1u : v;
    }

    constexpr uint32_t MipDepth(uint32_t base, uint32_t level) {
        uint32_t v = base >> level;
        return v == 0 ? 1u : v;
    }

    // 纹理形状校验
    constexpr bool IsValidTextureShape(uint32_t flags,
        uint32_t width, uint32_t height,
        uint32_t depth, uint32_t arrayLayers) {
        if (width == 0 || height == 0 || depth == 0 || arrayLayers == 0) return false;
        bool cubemap = (flags & TEX_FLAG_CUBEMAP) != 0;
        bool isArray = (flags & TEX_FLAG_ARRAY) != 0;
        if (cubemap) {
            if (depth != 1) return false;
            if (width != height) return false;
            if ((arrayLayers % 6) != 0) return false;
            if (arrayLayers > 6 && !isArray) return false;
        }
        else {
            if (depth > 1) {
                // 3D 纹理
                if (arrayLayers != 1) return false;
                if (isArray) return false;
            }
            else {
                if (isArray != (arrayLayers > 1)) return false;
            }
        }
        return true;
    }

    // ============================================================================
    // 结构体定义
    // ============================================================================

    // 文件头（固定 64 字节）
    struct FileHeader {
        uint32_t magic;              // 0
        uint16_t versionMajor;       // 4
        uint16_t versionMinor;       // 6
        uint32_t flags;              // 8
        uint32_t headerSize;         // 12
        uint64_t fileSize;           // 16
        uint64_t chunkTableOffset;   // 24
        uint32_t chunkCount;         // 32
        uint32_t headerCRC32;        // 36: 覆盖偏移 0..35
        uint32_t codec;              // 40: CompressionCodec（文件级压缩时有效）
        // 44: 文件级压缩时 reserved[0..7] = compressedDataSize (uint64 LE)；
        //     未压缩时 reserved[0..7] = 0。
        //     文件级压缩时 reserved[8..19] 必须为 0。
        uint8_t  reserved[20];       // 44
    };
    static_assert(sizeof(FileHeader) == kFileHeaderSize, "FileHeader size mismatch");

    // 块表项（固定 48 字节）
    struct ChunkEntry {
        uint32_t type;            // 0
        uint32_t flags;           // 4
        uint64_t offset;          // 8:  文件级压缩时相对解压后数据区起始；
        //     否则相对文件起始
        uint64_t size;            // 16: 解压后大小
        uint64_t compressedSize;  // 24: 块级压缩后大小；文件级压缩时必须为 0
        uint64_t alignment;       // 32: 0 表示使用 kChunkAlignment
        uint32_t crc32;           // 40: 覆盖解压后 size 字节
        uint32_t codec;           // 44: CompressionCodec（块级压缩时有效）
    };
    static_assert(sizeof(ChunkEntry) == kChunkEntrySize, "ChunkEntry size mismatch");

    // 模型元数据（META 块固定头部，共 168 字节）
    struct ModelMeta {
        uint64_t nameOffset;          // 0:  StringTable 偏移
        uint64_t vertexStreamsOffset; // 8:  相对 VRTX 块解压后起始
        uint64_t indexBuffersOffset;  // 16: 相对 INDX 块解压后起始
        uint64_t submeshesOffset;     // 24: 相对 SUBM 块解压后起始
        uint64_t materialsOffset;     // 32: 相对 MATL 块解压后起始
        uint64_t skeletonsOffset;     // 40: 相对 SKEL 块解压后起始
        uint64_t animationsOffset;    // 48: 相对 ANIM 块解压后起始
        uint64_t lodsOffset;          // 56: 相对 LODS 块解压后起始
        uint64_t stringTableOffset;   // 64: 相对 META 块解压后起始
        uint64_t stringTableSize;     // 72
        uint64_t texturesOffset;      // 80: 相对 TEXI 块解压后起始
        uint64_t samplersOffset;      // 88: 相对 SMPL 块解压后起始
        float    boundsMin[3];        // 96
        float    boundsMax[3];        // 108
        uint32_t nameLength;          // 120
        uint32_t flags;               // 124
        uint32_t vertexStreamCount;   // 128
        uint32_t indexBufferCount;    // 132
        uint32_t submeshCount;        // 136
        uint32_t materialCount;       // 140
        uint32_t skeletonCount;       // 144
        uint32_t animationCount;      // 148
        uint32_t lodCount;            // 152
        uint32_t textureCount;        // 156
        uint32_t samplerCount;        // 160
        uint32_t reserved;            // 164
    };
    static_assert(sizeof(ModelMeta) == 168, "ModelMeta size mismatch");

    // 顶点流描述（VRTX 块内，共 72 字节）
    struct VertexStreamDesc {
        uint32_t streamIndex;         // 0
        uint32_t attributeMask;       // 4
        uint32_t vertexCount;         // 8
        uint32_t stride;              // 12
        uint64_t dataOffset;          // 16: 相对 VRTX 块解压后起始
        uint64_t dataSize;            // 24
        uint32_t format;              // 32: 每 4 位一个 VertexAttributeFormat，slot 按位序
        uint32_t quantization;        // 36
        float    boundsMin[3];        // 40
        float    boundsMax[3];        // 52
        uint32_t reserved[2];         // 64
    };
    static_assert(sizeof(VertexStreamDesc) == 72, "VertexStreamDesc size mismatch");

    // 索引缓冲描述（INDX 块内，共 32 字节）
    struct IndexBufferDesc {
        uint32_t indexCount;   // 0
        uint32_t indexType;    // 4
        uint64_t dataOffset;   // 8:  相对 INDX 块解压后起始
        uint64_t dataSize;     // 16
        uint32_t reserved[2];  // 24
    };
    static_assert(sizeof(IndexBufferDesc) == 32, "IndexBufferDesc size mismatch");

    // 子网格（SUBM 块内，共 56 字节）
    struct Submesh {
        uint32_t indexOffset;      // 0:  单位 = 索引元素，相对 indexBufferIndex 缓冲起始
        uint32_t indexCount;       // 4
        uint32_t vertexOffset;     // 8:  相对所有顶点流起点的顶点偏移
        uint32_t materialIndex;    // 12
        uint32_t lodIndex;         // 16
        uint32_t flags;            // 20
        float    boundsMin[3];     // 24
        float    boundsMax[3];     // 36
        uint32_t indexBufferIndex; // 48: 指向 IndexBufferDesc[] 的索引
        uint32_t reserved;         // 52
    };
    static_assert(sizeof(Submesh) == 56, "Submesh size mismatch");

    // 纹理引用（材质内使用，共 16 字节）
    struct TextureRef {
        uint32_t textureIndex; // 0:  指向 TextureDesc[] 的索引；kInvalidIndex 表示无
        uint32_t uvSet;        // 4:  使用的 UV 通道，0 表示 TEXCOORD0
        uint32_t samplerIndex; // 8:  覆盖采样器；kInvalidIndex 表示使用纹理默认采样器
        uint32_t flags;        // 12: TextureRefFlags
    };
    static_assert(sizeof(TextureRef) == 16, "TextureRef size mismatch");

    // 材质（MATL 块内，共 152 字节）
    struct Material {
        uint64_t   nameOffset;                // 0:  StringTable 偏移
        float      baseColorFactor[4];        // 8
        float      metallicFactor;            // 24
        float      roughnessFactor;           // 28
        float      emissiveFactor[3];         // 32
        float      normalScale;               // 44
        float      occlusionStrength;         // 48
        TextureRef baseColorTexture;          // 52
        TextureRef metallicRoughnessTexture;  // 68
        TextureRef normalTexture;             // 84
        TextureRef occlusionTexture;          // 100
        TextureRef emissiveTexture;           // 116
        uint32_t   flags;                     // 132
        uint32_t   customDataOffset;          // 136: 相对 MATL 块解压后起始
        uint32_t   customDataSize;            // 140
        uint32_t   reserved[2];               // 144
    };
    static_assert(sizeof(Material) == 152, "Material size mismatch");

    // 纹理描述（TEXI 块内，共 88 字节）
    struct TextureDesc {
        uint64_t nameOffset;    // 0:  StringTable 偏移
        uint64_t uriOffset;     // 8:  外置时指向 StringTable 路径；0 表示内嵌
        uint64_t dataOffset;    // 16: 相对 TEXD 块解压后起始
        uint64_t dataSize;      // 24: 未压缩时 = 解压后数据总字节数 (含 mip 对齐填充)；
        //     纹理级压缩时 = 压缩后物理字节数
        uint64_t mipsOffset;    // 32: 相对 TEXI 块解压后起始；0 = 隐式推导
        uint32_t width;         // 40
        uint32_t height;        // 44
        uint32_t depth;         // 48: 3D 纹理深度，普通 2D 为 1
        uint32_t arrayLayers;   // 52: 数组层数；cubemap 时为 6 的倍数
        uint32_t mipCount;      // 56: 至少为 1
        uint32_t format;        // 60: TextureFormat
        uint32_t samplerIndex;  // 64: 默认采样器索引，kInvalidIndex = 无
        uint32_t source;        // 68: TextureSource
        uint32_t flags;         // 72: TextureFlags
        uint32_t crc32;         // 76: 覆盖解压后数据 (dataSize 解压后)
        uint32_t codec;         // 80: CompressionCodec（TEX_FLAG_COMPRESSED 时有效）
        uint32_t reserved;      // 84
    };
    static_assert(sizeof(TextureDesc) == 88, "TextureDesc size mismatch");

    // 每个 mip 的信息（TEXI 块内，共 24 字节）
    struct TextureMipInfo {
        uint64_t offset;   // 0: 相对 TEXD 块解压后起始，必须 256 字节对齐
        uint64_t size;     // 8: 该 mip 字节数
        uint32_t width;    // 16
        uint32_t height;   // 20
    };
    static_assert(sizeof(TextureMipInfo) == 24, "TextureMipInfo size mismatch");

    // 采样器（SMPL 块内，共 56 字节）
    struct SamplerDesc {
        uint64_t nameOffset;    // 0:  StringTable 偏移
        uint32_t minFilter;     // 8:  TextureFilter
        uint32_t magFilter;     // 12: TextureFilter
        uint32_t mipFilter;     // 16: TextureMipFilter
        uint32_t wrapS;         // 20: TextureWrap
        uint32_t wrapT;         // 24: TextureWrap
        uint32_t wrapR;         // 28: TextureWrap
        float    lodBias;       // 32
        float    maxAnisotropy; // 36
        uint32_t compareFunc;   // 40: TextureCompareFunc
        uint32_t flags;         // 44
        uint32_t reserved[2];   // 48
    };
    static_assert(sizeof(SamplerDesc) == 56, "SamplerDesc size mismatch");

    // 骨骼（SKEL 块内，共 52 字节）
    struct Bone {
        uint32_t parentIndex;         // 0:  kInvalidIndex 表示根骨骼
        uint32_t nameOffset;          // 4:  StringTable 偏移；0 表示无名称
        float    localTranslation[3]; // 8
        float    localRotation[4];    // 20: xyzw 四元数，必须归一化
        float    localScale[3];       // 36
        uint32_t reserved;            // 48
    };
    static_assert(sizeof(Bone) == 52, "Bone size mismatch");

    // 骨骼集合（SKEL 块内，共 40 字节）
    struct Skeleton {
        uint32_t boneCount;                 // 0
        uint32_t rootBone;                  // 4
        uint64_t bonesOffset;               // 8:  相对 SKEL 块解压后起始
        uint64_t inverseBindMatricesOffset; // 16: 相对 SKEL 块解压后起始，16 字节对齐
        //     0 = 全单位矩阵
        uint64_t nodeHierarchyOffset;       // 24: 相对 SKEL 块解压后起始；
        //     0 = 按 Bone[] 顺序
        uint32_t reserved[2];               // 32
    };
    static_assert(sizeof(Skeleton) == 40, "Skeleton size mismatch");

    // 动画通道（ANIM 块内，共 40 字节）
    struct AnimationChannel {
        uint32_t nodeIndex;      // 0:  骨骼索引，对应 Skeleton.boneCount
        uint32_t path;           // 4:  AnimationPath
        uint32_t interpolation;  // 8:  AnimationInterpolation
        uint32_t keyCount;       // 12
        uint64_t timesOffset;    // 16: 相对 ANIM 块解压后起始
        uint64_t valuesOffset;   // 24: 相对 ANIM 块解压后起始
        uint32_t reserved[2];    // 32
    };
    static_assert(sizeof(AnimationChannel) == 40, "AnimationChannel size mismatch");

    // 动画（ANIM 块内，共 32 字节）
    struct Animation {
        uint64_t nameOffset;      // 0:  StringTable 偏移
        float    duration;        // 8
        uint32_t channelCount;    // 12
        uint64_t channelsOffset;  // 16: 相对 ANIM 块解压后起始
        uint32_t reserved[2];     // 24
    };
    static_assert(sizeof(Animation) == 32, "Animation size mismatch");

    // LOD 描述（LODS 块内，共 16 字节）
    struct LodDesc {
        float    distance;        // 0:  切换距离 (最大距离)
        uint32_t submeshStart;    // 4:  全局 Submesh 索引
        uint32_t submeshCount;    // 8
        uint32_t reserved;        // 12
    };
    static_assert(sizeof(LodDesc) == 16, "LodDesc size mismatch");

    // ============================================================================
    // 字段偏移校验
    // ============================================================================

    static_assert(offsetof(FileHeader, magic) == 0, "FileHeader.magic offset");
    static_assert(offsetof(FileHeader, versionMajor) == 4, "FileHeader.versionMajor offset");
    static_assert(offsetof(FileHeader, versionMinor) == 6, "FileHeader.versionMinor offset");
    static_assert(offsetof(FileHeader, flags) == 8, "FileHeader.flags offset");
    static_assert(offsetof(FileHeader, headerSize) == 12, "FileHeader.headerSize offset");
    static_assert(offsetof(FileHeader, fileSize) == 16, "FileHeader.fileSize offset");
    static_assert(offsetof(FileHeader, chunkTableOffset) == 24, "FileHeader.chunkTableOffset offset");
    static_assert(offsetof(FileHeader, chunkCount) == 32, "FileHeader.chunkCount offset");
    static_assert(offsetof(FileHeader, headerCRC32) == 36, "FileHeader.headerCRC32 offset");
    static_assert(offsetof(FileHeader, codec) == 40, "FileHeader.codec offset");
    static_assert(offsetof(FileHeader, reserved) == 44, "FileHeader.reserved offset");

    static_assert(offsetof(ChunkEntry, type) == 0, "ChunkEntry.type offset");
    static_assert(offsetof(ChunkEntry, flags) == 4, "ChunkEntry.flags offset");
    static_assert(offsetof(ChunkEntry, offset) == 8, "ChunkEntry.offset offset");
    static_assert(offsetof(ChunkEntry, size) == 16, "ChunkEntry.size offset");
    static_assert(offsetof(ChunkEntry, compressedSize) == 24, "ChunkEntry.compressedSize offset");
    static_assert(offsetof(ChunkEntry, alignment) == 32, "ChunkEntry.alignment offset");
    static_assert(offsetof(ChunkEntry, crc32) == 40, "ChunkEntry.crc32 offset");
    static_assert(offsetof(ChunkEntry, codec) == 44, "ChunkEntry.codec offset");

    static_assert(offsetof(TextureDesc, mipsOffset) == 32, "TextureDesc.mipsOffset offset");
    static_assert(offsetof(TextureDesc, flags) == 72, "TextureDesc.flags offset");
    static_assert(offsetof(TextureDesc, codec) == 80, "TextureDesc.codec offset");

    static_assert(offsetof(Skeleton, inverseBindMatricesOffset) == 16, "Skeleton.inverseBindMatricesOffset offset");

    // 编译期 FourCC 校验
    static_assert(kMagic == MakeFourCC('G', 'M', 'D', 'B'), "kMagic mismatch");

} // namespace gmd

#pragma pack(pop)