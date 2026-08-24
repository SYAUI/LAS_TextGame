#include "Packer.h"

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cctype>

// Windows 平台相关
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fs = std::filesystem;

// ---------- 工具函数 ----------
static uint64_t FNV1a_64(const std::string& str) {
    const uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    const uint64_t FNV_PRIME = 0x100000001b3ULL;
    uint64_t hash = FNV_OFFSET_BASIS;
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= FNV_PRIME;
    }
    return hash;
}

// 根据虚拟路径的第一级目录名判断资源类型
static uint32_t GetTypeFromPath(const std::string& virtualPath) {
    size_t pos = virtualPath.find('/');
    if (pos == std::string::npos) return TYPE_OTHER;
    std::string rootDir = virtualPath.substr(0, pos);
    std::transform(rootDir.begin(), rootDir.end(), rootDir.begin(), ::tolower);

    if (rootDir == "text")   return TYPE_TEXT;
    if (rootDir == "image")  return TYPE_IMAGE;
    if (rootDir == "video")  return TYPE_VIDEO;
    if (rootDir == "sound")  return TYPE_SOUND;
    return TYPE_OTHER;
}

// ---------- 打包实现 ----------
bool Packer::Pack(const std::string& rootDirectory, const std::string& outputPak) {
    // 1. 递归遍历目录，收集所有文件信息
    struct FileInfo {
        std::string virtualPath;
        uint64_t size;
        uint64_t hash;
        uint32_t flags;
    };
    std::vector<FileInfo> files;

    try {
        fs::path root(rootDirectory);
        if (!fs::exists(root) || !fs::is_directory(root)) {
            std::cerr << "Error: root directory does not exist: " << rootDirectory << std::endl;
            return false;
        }

        for (auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            if (!fs::is_regular_file(entry.status())) continue;

            fs::path rel = fs::relative(entry.path(), root);
            std::string virt = rel.generic_string();

            uint64_t size = fs::file_size(entry.path());
            if (size == 0) continue;

            files.push_back({
                virt,
                size,
                FNV1a_64(virt),
                GetTypeFromPath(virt)
                });
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Exception during directory scan: " << e.what() << std::endl;
        return false;
    }

    if (files.empty()) {
        std::cerr << "No files found to pack." << std::endl;
        return false;
    }

    // 2. 按哈希排序
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
        return a.hash < b.hash;
        });

    // 3. 计算 StringTable 大小
    size_t stringTableSize = 0;
    for (const auto& f : files) {
        stringTableSize += f.virtualPath.size() + 1;
    }

    // 4. 计算偏移量
    const size_t headerSize = sizeof(PackageHeader);
    const size_t entriesSize = files.size() * sizeof(FileEntry);
    const size_t totalHeader = headerSize + entriesSize + stringTableSize;
    const uint64_t dataBlockOffset = (totalHeader + 0xFFFF) & ~0xFFFFULL;

    // 5. 构建 FileEntry 数组
    std::vector<FileEntry> entries;
    entries.reserve(files.size());
    uint64_t currentDataOffset = 0;
    for (const auto& info : files) {
        FileEntry entry;
        entry.Hash = info.hash;
        entry.Offset = dataBlockOffset + currentDataOffset;
        entry.Size = info.size;
        entry.Flags = info.flags;
        entry.NameLen = static_cast<uint32_t>(info.virtualPath.size());
        entries.push_back(entry);
        currentDataOffset += info.size;
    }

    // 6. 写入文件
    std::ofstream pak(outputPak, std::ios::binary | std::ios::trunc);
    if (!pak) {
        std::cerr << "Failed to create output file: " << outputPak << std::endl;
        return false;
    }

    PackageHeader header = {};
    header.Magic = 0x474B5030;
    header.Version = 0x0100;
    header.EntryCount = files.size();
    header.StringTableOffset = headerSize + entriesSize;
    header.StringTableSize = stringTableSize;
    header.DataBlockOffset = dataBlockOffset;
    pak.write(reinterpret_cast<const char*>(&header), sizeof(header));

    pak.write(reinterpret_cast<const char*>(entries.data()), entriesSize);

    for (const auto& info : files) {
        pak.write(info.virtualPath.c_str(), info.virtualPath.size() + 1);
    }

    std::streampos currentPos = pak.tellp();
    if (static_cast<uint64_t>(currentPos) < dataBlockOffset) {
        size_t padCount = dataBlockOffset - static_cast<uint64_t>(currentPos);
        std::vector<char> zeros(padCount, 0);
        pak.write(zeros.data(), zeros.size());
    }

    const size_t bufferSize = 64 * 1024;
    std::vector<char> buffer(bufferSize);
    for (const auto& info : files) {
        fs::path fullPath = fs::path(rootDirectory) / info.virtualPath;
        std::ifstream src(fullPath, std::ios::binary);
        if (!src) {
            std::cerr << "Warning: cannot open file: " << fullPath << ", skipping." << std::endl;
            continue;
        }
        uint64_t remaining = info.size;
        while (remaining > 0) {
            size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, bufferSize));
            src.read(buffer.data(), toRead);
            if (src.gcount() != static_cast<std::streamsize>(toRead)) {
                std::cerr << "Error reading file: " << fullPath << std::endl;
                return false;
            }
            pak.write(buffer.data(), toRead);
            remaining -= toRead;
        }
    }

    pak.close();
    std::cout << "Pack completed successfully. " << files.size() << " files, "
        << currentDataOffset << " bytes data, pak size: "
        << (dataBlockOffset + currentDataOffset) << " bytes." << std::endl;
    return true;
}

// ---------- 加载器辅助查找 ----------
const FileEntry* Packer::FindEntry(const std::string& virtualPath) const {
    uint64_t hash = FNV1a_64(virtualPath);
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), hash,
        [](const FileEntry& entry, uint64_t value) {
            return entry.Hash < value;
        });
    if (it != m_entries.end() && it->Hash == hash) {
        return &(*it);
    }
    return nullptr;
}

// ---------- 加载实现 ----------
bool Packer::Load(const std::string& pakPath) {
    // 如果已加载，先卸载
    if (IsLoaded()) Unload();

    // 1. 打开文件
    HANDLE hFile = CreateFileA(pakPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Load: Cannot open file " << pakPath << std::endl;
        return false;
    }

    // 2. 获取文件大小
    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart == 0) {
        CloseHandle(hFile);
        std::cerr << "Load: Invalid file size." << std::endl;
        return false;
    }
    m_fileSize = static_cast<size_t>(liSize.QuadPart);

    // 3. 创建文件映射
    HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY,
        liSize.HighPart, liSize.LowPart, NULL);
    if (!hMapping) {
        CloseHandle(hFile);
        std::cerr << "Load: CreateFileMapping failed." << std::endl;
        return false;
    }

    // 4. 映射整个文件
    const uint8_t* base = static_cast<const uint8_t*>(
        MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0)
        );
    if (!base) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        std::cerr << "Load: MapViewOfFile failed." << std::endl;
        return false;
    }

    // 5. 解析头部
    const PackageHeader* pHeader = reinterpret_cast<const PackageHeader*>(base);
    if (pHeader->Magic != 0x474B5030) {
        Unload();
        std::cerr << "Load: Invalid magic number." << std::endl;
        return false;
    }

    // 6. 拷贝索引表到内存
    m_entries.resize(pHeader->EntryCount);
    const FileEntry* pSrc = reinterpret_cast<const FileEntry*>(base + sizeof(PackageHeader));
    memcpy(m_entries.data(), pSrc, pHeader->EntryCount * sizeof(FileEntry));

    // 7. 保存映射资源
    m_hFile = hFile;
    m_hMapping = hMapping;
    m_pBaseAddr = base;

    std::cout << "Load: " << pakPath << " loaded, entries: " << m_entries.size() << std::endl;
    return true;
}

void Packer::Unload() {
    if (m_pBaseAddr) {
        UnmapViewOfFile(const_cast<uint8_t*>(m_pBaseAddr));
        m_pBaseAddr = nullptr;
    }
    if (m_hMapping) {
        CloseHandle(reinterpret_cast<HANDLE>(m_hMapping));
        m_hMapping = nullptr;
    }
    if (m_hFile) {
        CloseHandle(reinterpret_cast<HANDLE>(m_hFile));
        m_hFile = nullptr;
    }
    m_fileSize = 0;
    m_entries.clear();
}

// ---------- 查询接口 ----------
const uint8_t* Packer::GetFile(const std::string& virtualPath, size_t* outSize) const {
    const FileEntry* entry = FindEntry(virtualPath);
    if (!entry) return nullptr;
    if (outSize) *outSize = static_cast<size_t>(entry->Size);
    return m_pBaseAddr + entry->Offset;
}

uint32_t Packer::GetFileType(const std::string& virtualPath) const {
    const FileEntry* entry = FindEntry(virtualPath);
    return entry ? entry->Flags : TYPE_OTHER;
}

size_t Packer::GetFileSize(const std::string& virtualPath) const {
    const FileEntry* entry = FindEntry(virtualPath);
    return entry ? static_cast<size_t>(entry->Size) : 0;
}

std::string_view Packer::GetFileAsStringView(const std::string& virtualPath) const {
    size_t size = 0;
    const uint8_t* data = GetFile(virtualPath, &size);
    if (data && GetFileType(virtualPath) == TYPE_TEXT) {
        return std::string_view(reinterpret_cast<const char*>(data), size);
    }
    return {};
}