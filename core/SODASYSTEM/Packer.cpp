#include "Packer.h"
#include "framework.h"

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <cctype>

namespace fs = std::filesystem;

// ============================================================
// 工具函数
// ============================================================
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

// ============================================================
// PIMPL 实现
// ============================================================
struct Packer::Impl {
    HANDLE         hFile = INVALID_HANDLE_VALUE;
    HANDLE         hMapping = NULL;
    const uint8_t* baseAddr = nullptr;
    size_t         fileSize = 0;
    std::vector<FileEntry> entries;
    const char* stringTable = nullptr;

    ~Impl() { Unload(); }

    void Unload() {
        if (baseAddr) {
            UnmapViewOfFile(const_cast<uint8_t*>(baseAddr));
            baseAddr = nullptr;
        }
        if (hMapping) { CloseHandle(hMapping); hMapping = NULL; }
        if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; }
        fileSize = 0;
        entries.clear();
        stringTable = nullptr;
    }

    const FileEntry* FindEntry(const std::string& virtualPath) const {
        uint64_t hash = FNV1a_64(virtualPath);
        auto it = std::lower_bound(entries.begin(), entries.end(), hash,
            [](const FileEntry& e, uint64_t v) { return e.Hash < v; });
        if (it != entries.end() && it->Hash == hash) return &(*it);
        return nullptr;
    }
};

// ============================================================
// 构造/析构
// ============================================================
Packer::Packer() : m_impl(new Impl()) {}
Packer::~Packer() { delete m_impl; m_impl = nullptr; }

// ============================================================
// 打包
// ============================================================
bool Packer::Pack(const std::string& rootDirectory, const std::string& outputPak) {
    struct FileInfo {
        std::string virtualPath;
        uint64_t size;
        uint64_t hash;
        uint32_t flags;
    };
    std::vector<FileInfo> files;

    try {
        fs::path root(rootDirectory);
        if (!fs::exists(root) || !fs::is_directory(root)) return false;

        for (auto& entry : fs::recursive_directory_iterator(
            root, fs::directory_options::skip_permission_denied)) {
            if (!fs::is_regular_file(entry.status())) continue;
            fs::path rel = fs::relative(entry.path(), root);
            std::string virt = rel.generic_string();
            uint64_t size = fs::file_size(entry.path());
            if (size == 0) continue;
            files.push_back({ virt, size, FNV1a_64(virt), GetTypeFromPath(virt) });
        }
    }
    catch (const std::exception&) {
        return false;
    }

    if (files.empty()) return false;

    std::sort(files.begin(), files.end(),
        [](const FileInfo& a, const FileInfo& b) { return a.hash < b.hash; });

    size_t stringTableSize = 0;
    for (const auto& f : files) stringTableSize += f.virtualPath.size() + 1;

    const size_t headerSize = sizeof(PackageHeader);
    const size_t entriesSize = files.size() * sizeof(FileEntry);
    const size_t totalHeader = headerSize + entriesSize + stringTableSize;
    const uint64_t dataBlockOffset = (totalHeader + 0xFFFF) & ~0xFFFFULL;

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

    std::ofstream pak(outputPak, std::ios::binary | std::ios::trunc);
    if (!pak) return false;

    PackageHeader header = {};
    header.Magic = 0x474B5030;
    header.Version = 0x0100;
    header.EntryCount = files.size();
    header.StringTableOffset = headerSize + entriesSize;
    header.StringTableSize = stringTableSize;
    header.DataBlockOffset = dataBlockOffset;

    pak.write(reinterpret_cast<const char*>(&header), sizeof(header));
    pak.write(reinterpret_cast<const char*>(entries.data()), entriesSize);

    for (const auto& info : files)
        pak.write(info.virtualPath.c_str(), info.virtualPath.size() + 1);

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
        if (!src) continue;
        uint64_t remaining = info.size;
        while (remaining > 0) {
            size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, bufferSize));
            src.read(buffer.data(), toRead);
            if (src.gcount() != static_cast<std::streamsize>(toRead)) return false;
            pak.write(buffer.data(), toRead);
            remaining -= toRead;
        }
    }

    pak.close();
    return true;
}

// ============================================================
// 加载
// ============================================================
bool Packer::Load(const std::string& pakPath) {
    m_impl->Unload();

    HANDLE hFile = CreateFileA(pakPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart == 0) {
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY,
        liSize.HighPart, liSize.LowPart, NULL);
    if (!hMapping) { CloseHandle(hFile); return false; }

    const uint8_t* base = static_cast<const uint8_t*>(
        MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
    if (!base) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return false;
    }

    const PackageHeader* pHeader = reinterpret_cast<const PackageHeader*>(base);
    if (pHeader->Magic != 0x474B5030) {
        UnmapViewOfFile(const_cast<uint8_t*>(base));
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return false;
    }

    m_impl->entries.resize(pHeader->EntryCount);
    const FileEntry* pSrc = reinterpret_cast<const FileEntry*>(base + sizeof(PackageHeader));
    memcpy(m_impl->entries.data(), pSrc, pHeader->EntryCount * sizeof(FileEntry));

    m_impl->hFile = hFile;
    m_impl->hMapping = hMapping;
    m_impl->baseAddr = base;
    m_impl->fileSize = static_cast<size_t>(liSize.QuadPart);
    m_impl->stringTable = reinterpret_cast<const char*>(base + pHeader->StringTableOffset);

    return true;
}

void Packer::Unload() { m_impl->Unload(); }

bool Packer::IsLoaded() const { return m_impl->baseAddr != nullptr; }

// ============================================================
// 查询
// ============================================================
const uint8_t* Packer::GetFile(const std::string& virtualPath, size_t* outSize) const {
    const FileEntry* entry = m_impl->FindEntry(virtualPath);
    if (!entry) return nullptr;
    if (outSize) *outSize = static_cast<size_t>(entry->Size);
    return m_impl->baseAddr + entry->Offset;
}

uint32_t Packer::GetFileType(const std::string& virtualPath) const {
    const FileEntry* entry = m_impl->FindEntry(virtualPath);
    return entry ? entry->Flags : TYPE_OTHER;
}

size_t Packer::GetFileSize(const std::string& virtualPath) const {
    const FileEntry* entry = m_impl->FindEntry(virtualPath);
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

size_t Packer::GetEntryCount() const { return m_impl->entries.size(); }

std::vector<std::string> Packer::ListFiles() const {
    std::vector<std::string> result;
    result.reserve(m_impl->entries.size());
    if (!m_impl->stringTable) return result;

    const char* p = m_impl->stringTable;
    for (size_t i = 0; i < m_impl->entries.size(); ++i) {
        result.emplace_back(p);
        p += std::strlen(p) + 1;
    }
    return result;
}