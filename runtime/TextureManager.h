#pragma once
#include <d3d11.h>
#include <string>
#include <unordered_map>

class TextureManager {
public:
    bool Init(ID3D11Device* device);
    void Shutdown();
    ID3D11ShaderResourceView* GetOrLoad(const std::string& path);

private:
    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::string, ID3D11ShaderResourceView*> m_cache;
};