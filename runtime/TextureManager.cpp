#define STB_IMAGE_IMPLEMENTATION
#include "..\Libraries\stb\stb_image.h"

#include "TextureManager.h"
#include <iostream>

bool TextureManager::Init(ID3D11Device* device) {
    m_device = device;
    return m_device != nullptr;
}

void TextureManager::Shutdown() {
    for (auto& kv : m_cache) if (kv.second) kv.second->Release();
    m_cache.clear();
    m_device = nullptr;
}

ID3D11ShaderResourceView* TextureManager::GetOrLoad(const std::string& path) {
    if (path.empty() || !m_device) return nullptr;

    auto it = m_cache.find(path);
    if (it != m_cache.end()) return it->second;

    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        std::cerr << "[Tex] ¼ÓÔØÊ§°Ü: " << path
            << " (" << stbi_failure_reason() << ")\n";
        m_cache[path] = nullptr;   // »º´æÊ§°Ü£¬±ÜÃâ·´¸´³¢ÊÔ
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels;
    init.SysMemPitch = (UINT)(w * 4);

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&desc, &init, &tex);
    if (FAILED(hr)) {
        std::cerr << "[Tex] CreateTexture2D Ê§°Ü: " << path << "\n";
        stbi_image_free(pixels);
        m_cache[path] = nullptr;
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = m_device->CreateShaderResourceView(tex, &srvDesc, &srv);
    tex->Release();
    stbi_image_free(pixels);

    if (FAILED(hr)) {
        std::cerr << "[Tex] CreateSRV Ê§°Ü: " << path << "\n";
        m_cache[path] = nullptr;
        return nullptr;
    }

    m_cache[path] = srv;
    return srv;
}