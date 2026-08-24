#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

// ---------- 包文件结构（与加载器共用） ----------
#pragma pack(push, 1)

struct PackageHeader {
    uint32_t Magic;          // 'PKG0' -> 0x474B5030
    uint32_t Version;        // 0x0100
    uint64_t EntryCount;
    uint64_t StringTableOffset;
    uint64_t StringTableSize;
    uint64_t DataBlockOffset; // 数据区起始偏移（对齐到 64KB）
    uint8_t  Reserved[128];
};

struct FileEntry {
    uint64_t Hash;      // FNV-1a 64位哈希
    uint64_t Offset;    // 文件数据在包中的绝对偏移
    uint64_t Size;      // 文件原始大小
    uint32_t Flags;     // 资源类型：0=文本，1=图片，2=视频，3=声音，4=其他
    uint32_t NameLen;   // 虚拟路径字符串长度（用于调试）
};

#pragma pack(pop)

// ---------- 资源类型常量 ----------
enum ResourceType : uint32_t {
    TYPE_TEXT = 0,
    TYPE_IMAGE = 1,
    TYPE_VIDEO = 2,
    TYPE_SOUND = 3,
    TYPE_OTHER = 4
};

// ---------- 打包/加载器类 ----------
class Packer {
public:
    Packer() = default;
    ~Packer() { Unload(); }

    // -------- 打包功能 --------
    /**
     * @brief 将 rootDirectory 下的所有文件打包成 output.pak
     * @param rootDirectory 待打包的根目录（例如 "./assets"）
     * @param outputPak     输出的 .pak 文件路径
     * @return true 成功，false 失败
     */
    bool Pack(const std::string& rootDirectory, const std::string& outputPak);

    // -------- 加载功能 --------
    /**
     * @brief 加载一个 .pak 文件到内存映射
     * @param pakPath .pak 文件路径
     * @return true 成功，false 失败
     */
    bool Load(const std::string& pakPath);

    /** 卸载当前加载的包，释放映射资源 */
    void Unload();

    /** 检查是否已成功加载包 */
    bool IsLoaded() const { return m_pBaseAddr != nullptr; }

    /**
     * @brief 根据虚拟路径获取文件数据指针（零拷贝）
     * @param virtualPath 虚拟路径（如 "/image/logo.png"）
     * @param outSize     输出文件大小（字节）
     * @return 指向文件数据的指针（只读），若不存在返回 nullptr
     */
    const uint8_t* GetFile(const std::string& virtualPath, size_t* outSize = nullptr) const;

    /**
     * @brief 获取文件的资源类型
     * @param virtualPath 虚拟路径
     * @return 资源类型枚举值，若文件不存在返回 TYPE_OTHER
     */
    uint32_t GetFileType(const std::string& virtualPath) const;

    /**
     * @brief 获取文件大小（不读取数据）
     * @param virtualPath 虚拟路径
     * @return 文件大小（字节），若不存在返回 0
     */
    size_t GetFileSize(const std::string& virtualPath) const;

    /**
     * @brief 将文本文件直接作为 std::string_view 返回（便捷函数）
     * @param virtualPath 虚拟路径
     * @return 字符串视图，若不存在或不是文本类型则返回空
     */
    std::string_view GetFileAsStringView(const std::string& virtualPath) const;

private:
    // 加载器内部状态
    void* m_hFile = nullptr;          // 实际为 HANDLE
    void* m_hMapping = nullptr;       // 实际为 HANDLE
    const uint8_t* m_pBaseAddr = nullptr;   // 映射基址
    size_t      m_fileSize = 0;
    std::vector<FileEntry> m_entries;       // 索引表副本（按 Hash 排序）

    // 辅助查找函数
    const FileEntry* FindEntry(const std::string& virtualPath) const;
};