//
// RT64
//

#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <d3d12.h>
#include <d3dx12.h>
#include <dxcapi.h>
#include <d3dcompiler.h>

#include <vector>

#include <dxgi1_4.h>
#include <d3d12.h>

#include "D3D12MemoryAllocator/D3D12MemAlloc.h"

#include <DirectXMath.h>

#include "../public/rt64.h"

#define DLLEXPORT extern "C" __declspec(dllexport)  

using namespace DirectX;

namespace RT64 {
	class AllocatedResource {
	private:
		D3D12MA::Allocation *d3dMaAllocation;
	public:
		AllocatedResource() {
			d3dMaAllocation = nullptr;
		}

		AllocatedResource(D3D12MA::Allocation *d3dMaAllocation) {
			this->d3dMaAllocation = d3dMaAllocation;
		}

		~AllocatedResource() { }

		inline ID3D12Resource *Get() const {
			if (!IsNull()) {
				return d3dMaAllocation->GetResource();
			}
			else {
				return nullptr;
			}
		}

		inline bool IsNull() const {
			return (d3dMaAllocation == nullptr);
		}

		void Release() {
			if (!IsNull()) {
				ID3D12Resource *d3dResource = d3dMaAllocation->GetResource();
				d3dMaAllocation->Release();
				d3dResource->Release();
				d3dMaAllocation = nullptr;
			}
		}
	};

	struct InstanceProperties {
		XMMATRIX objectToWorld;
		XMMATRIX objectToWorldNormal;
		RT64_MATERIAL material;
	};

	struct AccelerationStructureBuffers {
		AllocatedResource scratch;
		UINT64 scratchSize;
		AllocatedResource result;
		UINT64 resultSize;
		AllocatedResource instanceDesc;
		UINT64 instanceDescSize;

		AccelerationStructureBuffers() {
			scratchSize = resultSize = instanceDescSize = 0;
		}

		void Release() {
			scratch.Release();
			result.Release();
			instanceDesc.Release();
			scratchSize = resultSize = instanceDescSize = 0;
		}
	};

	// Points to a static array of data in memory using the IDxcBlob interface.
	class StaticBlob : public IDxcBlob {
	private:
		const unsigned char *data;
		size_t dataSize;
		std::atomic<int> refCount;
	public:
		StaticBlob(const unsigned char *data, size_t dataSize) {
			this->data = data;
			this->dataSize = dataSize;
		}

		~StaticBlob() { }

		virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject) override {
			// Always set out parameter to NULL, validating it first.
			if (!ppvObject) {
				return E_INVALIDARG;
			}

			*ppvObject = NULL;
			if (riid == IID_IUnknown) {
				// Increment the reference count and return the pointer.
				*ppvObject = (LPVOID)this;
				AddRef();
				return NOERROR;
			}

			return E_NOINTERFACE;
		}

		virtual ULONG STDMETHODCALLTYPE AddRef(void) override {
			refCount++;
			return refCount;
		}

		virtual ULONG STDMETHODCALLTYPE Release(void) override {
			refCount--;
			if (refCount == 0) {
				delete this;
			}

			return refCount;
		}

		virtual LPVOID STDMETHODCALLTYPE GetBufferPointer(void) override {
			return (LPVOID)(data);
		}

		virtual SIZE_T STDMETHODCALLTYPE GetBufferSize(void) override {
			return dataSize;
		}
	};
};

inline void ThrowIfFailed(HRESULT hr)
{
	if (FAILED(hr))
	{
		throw std::exception();
	}
}

namespace nv_helpers_dx12
{
#ifndef ROUND_UP
#define ROUND_UP(v, powerOf2Alignment) (((v) + (powerOf2Alignment)-1) & ~((powerOf2Alignment)-1))
#endif

	ID3D12DescriptorHeap* CreateDescriptorHeap(ID3D12Device* device, uint32_t count, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisible);
}