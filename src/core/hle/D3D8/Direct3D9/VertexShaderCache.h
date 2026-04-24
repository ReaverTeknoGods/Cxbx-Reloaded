
#ifndef DIRECT3D9SHADERCACHE_H
#define DIRECT3D9SHADERCACHE_H

#include "VertexShader.h"
#include <map>

typedef uint64_t ShaderKey;

// Manages creation and caching of vertex shaders
class VertexShaderCache {

public:
	struct CompiledVertexShaderResult {
		ID3DBlob* pCompiledShader = nullptr;
		uint64_t shaderCacheHash = 0;
	};

	ShaderKey CreateShader(const xbox::dword_xt* pXboxFunction, DWORD* pXboxFunctionSize);
	IDirect3DVertexShader *GetShader(ShaderKey key);
	uint64_t GetShaderCacheHash(ShaderKey key);
	bool UsesIndexedBoneConstants(ShaderKey key);
	void ReleaseShader(ShaderKey key);

	void ResetD3DDevice(IDirect3DDevice9* pD3DDevice);
	void Clear();

	// TODO
	// WriteCacheToDisk
	// LoadCacheFromDisk

private:
	struct LazyVertexShader {
		bool isReady = false;
		std::future<CompiledVertexShaderResult> compileResult;
		IDirect3DVertexShader* pHostVertexShader = nullptr;
		uint64_t shaderCacheHash = 0;

		// TODO when is it a good idea to releas eshaders?
		int referenceCount = 0;

		// TODO persist shaders to disk
		// ShaderVersion?
		// OptimizationLevel?
		bool usesIndexedBoneConstants = false;
	};

	IDirect3DDevice9* pD3DDevice;
	std::mutex cacheMutex;
	std::map<ShaderKey, LazyVertexShader> cache;

	bool _FindShader(ShaderKey key, LazyVertexShader** ppLazyShader);
};

extern VertexShaderCache g_VertexShaderCache;

#endif
