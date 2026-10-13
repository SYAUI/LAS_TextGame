#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

// ============================================================
// DLL 导出/导入宏
// ============================================================
#ifdef PACKER_EXPORTS
#define PACKER_API __declspec(dllexport)
#else
#define PACKER_API __declspec(dllimport)
#endif

// ============================================================
// 包文件结构（与加载器共用）
// ============================================================
#pragma pack(push, 1)

struct PackageHeader {
    uint32_t Magic;          // 'PKG0' -> 0x474B5030
    uint32_t Version;        // 0x0100
    uint64_t EntryCount;
    uint64_t StringTableOffset;
    uint64_t StringTableSize;
    uint64_t DataBlockOffset;
    uint8_t  Reserved[128];
};

struct FileEntry {
    uint64_t Hash;
    uint64_t Offset;
    uint64_t Size;
    uint32_t Flags;
    uint32_t NameLen;
};

#pragma pack(pop)

// ============================================================
// 资源类型常量
// ============================================================
enum ResourceType : uint32_t {
    TYPE_TEXT = 0,
    TYPE_IMAGE = 1,
    TYPE_VIDEO = 2,
    TYPE_SOUND = 3,
    TYPE_OTHER = 4
};

// ============================================================
// Packer 类（导出）
// ============================================================
class PACKER_API Packer {
public:
    Packer();
    ~Packer();

    Packer(const Packer&) = delete;
    Packer& operator=(const Packer&) = delete;

    bool Pack(const std::string& rootDirectory, const std::string& outputPak);

    bool Load(const std::string& pakPath);
    void Unload();
    bool IsLoaded() const;

    const uint8_t* GetFile(const std::string& virtualPath, size_t* outSize = nullptr) const;
    uint32_t GetFileType(const std::string& virtualPath) const;
    size_t GetFileSize(const std::string& virtualPath) const;
    std::string_view GetFileAsStringView(const std::string& virtualPath) const;

    size_t GetEntryCount() const;
    std::vector<std::string> ListFiles() const;

private:
    struct Impl;
    Impl* m_impl;
};

// ============================================================
// C 风格工厂函数
// ============================================================
extern "C" {
    PACKER_API Packer* CreatePacker();
    PACKER_API void    DestroyPacker(Packer* p);
}