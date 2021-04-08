//
// RT64
//

#include "../public/rt64.h"

#include <map>
#include <set>

#include "rt64_device.h"
#include "rt64_instance.h"
#include "rt64_mesh.h"
#include "rt64_scene.h"
#include "rt64_texture.h"
#include "rt64_view.h"

namespace {
	const int MaxQueries = 12 + 1;
};

// Private

RT64::View::View(Scene *scene) {
	assert(scene != nullptr);
	this->scene = scene;
	descriptorHeap = nullptr;
	descriptorHeapEntryCount = 0;
	sbtStorageSize = 0;
	activeInstancesBufferPropsSize = 0;
	cameraBufferSize = 0;
	previousViewProj = XMMatrixIdentity();

	setPerspectiveLookAt({ 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 45.0f, 0.1f, 1000.0f);

	// Add this view to the parent scene.
	scene->addView(this);

	// Allocate the buffer storing the raytracing output, with the same dimensions as the target image.
	createOutputBuffers();

	// Create a buffer to store the modelview and perspective camera matrices
	createCameraBuffer();
}

RT64::View::~View() {
	scene->removeView(this);

	releaseOutputBuffers();
}

void RT64::View::createOutputBuffers() {
	releaseOutputBuffers();

	outputRtvDescriptorSize = scene->getDevice()->getD3D12Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	D3D12_CLEAR_VALUE clearValue = { };
	clearValue.Color[0] = 0.0f;
	clearValue.Color[1] = 0.0f;
	clearValue.Color[2] = 0.0f;
	clearValue.Color[3] = 0.0f;
	clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	D3D12_RESOURCE_DESC resDesc = { };
	resDesc.DepthOrArraySize = 1;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	resDesc.Width = scene->getDevice()->getWidth();
	resDesc.Height = scene->getDevice()->getHeight();
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resDesc.MipLevels = 1;
	resDesc.SampleDesc.Count = 1;

	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	rasterResources[0] = scene->getDevice()->allocateResource(D3D12_HEAP_TYPE_DEFAULT, &resDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clearValue);
	rasterResources[1] = scene->getDevice()->allocateResource(D3D12_HEAP_TYPE_DEFAULT, &resDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clearValue);

	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	rtOutputResource = scene->getDevice()->allocateResource(D3D12_HEAP_TYPE_DEFAULT, &resDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr);
	
	UINT64 hitCountBufferSize = scene->getDevice()->getWidth() * scene->getDevice()->getHeight() * MaxQueries;
	rtHitDistanceResource = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_DEFAULT, hitCountBufferSize * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	rtHitColorResource = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_DEFAULT, hitCountBufferSize * 8, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	rtHitNormalResource = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_DEFAULT, hitCountBufferSize * 8, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	rtHitInstanceIdResource = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_DEFAULT, hitCountBufferSize * 2, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// Create the RTVs for the raster resources.
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = 1;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	for (int i = 0; i < 2; i++) {
		ThrowIfFailed(scene->getDevice()->getD3D12Device()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rasterRtvHeaps[i])));
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rasterRtvHeaps[i]->GetCPUDescriptorHandleForHeapStart());
		scene->getDevice()->getD3D12Device()->CreateRenderTargetView(rasterResources[i].Get(), nullptr, rtvHandle);
		rtvHandle.Offset(1, outputRtvDescriptorSize);
	}
}

void RT64::View::releaseOutputBuffers() {
	rasterResources[0].Release();
	rasterResources[1].Release();
	rtOutputResource.Release();
	rtHitDistanceResource.Release();
	rtHitColorResource.Release();
	rtHitNormalResource.Release();
	rtHitInstanceIdResource.Release();
}

void RT64::View::createInstancePropertiesBuffer() {
	uint32_t totalInstances = static_cast<uint32_t>(rtInstances.size() + rasterBgInstances.size() + rasterFgInstances.size());
	uint32_t newBufferSize = ROUND_UP(totalInstances * sizeof(InstanceProperties), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	if (activeInstancesBufferPropsSize != newBufferSize) {
		activeInstancesBufferProps.Release();
		activeInstancesBufferProps = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_UPLOAD, newBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
		activeInstancesBufferPropsSize = newBufferSize;
	}
}

void RT64::View::updateInstancePropertiesBuffer() {
	InstanceProperties *current = nullptr;
	CD3DX12_RANGE readRange(0, 0);

	ThrowIfFailed(activeInstancesBufferProps.Get()->Map(0, &readRange, reinterpret_cast<void **>(&current)));

	for (const RenderInstance &inst : rtInstances) {
		// Store world transform.
		current->objectToWorld = inst.transform;

		// Store matrix to transform normal.
		XMMATRIX upper3x3 = current->objectToWorld;
		upper3x3.r[0].m128_f32[3] = 0.f;
		upper3x3.r[1].m128_f32[3] = 0.f;
		upper3x3.r[2].m128_f32[3] = 0.f;
		upper3x3.r[3].m128_f32[0] = 0.f;
		upper3x3.r[3].m128_f32[1] = 0.f;
		upper3x3.r[3].m128_f32[2] = 0.f;
		upper3x3.r[3].m128_f32[3] = 1.f;

		XMVECTOR det;
		current->objectToWorldNormal = XMMatrixTranspose(XMMatrixInverse(&det, upper3x3));

		// Store material.
		current->material = inst.material;
		current++;
	}

	for (const RenderInstance &inst : rasterBgInstances) {
		current->material = inst.material;
		current++;
	}

	for (const RenderInstance& inst : rasterFgInstances) {
		current->material = inst.material;
		current++;
	}

	activeInstancesBufferProps.Get()->Unmap(0, nullptr);
}

void RT64::View::createTopLevelAS(const std::vector<RenderInstance>& rtInstances) {
	// Reset the generator.
	topLevelASGenerator.Reset();

	// Gather all the instances into the builder helper
	for (size_t i = 0; i < rtInstances.size(); i++) {
		topLevelASGenerator.AddInstance(rtInstances[i].bottomLevelAS, rtInstances[i].transform, static_cast<UINT>(i), static_cast<UINT>(2 * i));
	}

	// As for the bottom-level AS, the building the AS requires some scratch
	// space to store temporary data in addition to the actual AS. In the case
	// of the top-level AS, the instance descriptors also need to be stored in
	// GPU memory. This call outputs the memory requirements for each (scratch,
	// results, instance descriptors) so that the application can allocate the
	// corresponding memory
	UINT64 scratchSize, resultSize, instanceDescsSize;
	topLevelASGenerator.ComputeASBufferSizes(scene->getDevice()->getD3D12Device(), true, &scratchSize, &resultSize, &instanceDescsSize);
	
	// Release the previous buffers and reallocate them if they're not big enough.
	if ((topLevelASBuffers.scratchSize < scratchSize) || (topLevelASBuffers.resultSize < resultSize) || (topLevelASBuffers.instanceDescSize < instanceDescsSize)) {
		topLevelASBuffers.Release();

		// Create the scratch and result buffers. Since the build is all done on
		// GPU, those can be allocated on the default heap
		topLevelASBuffers.scratch = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_DEFAULT, scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		topLevelASBuffers.result = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_DEFAULT, resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		// The buffer describing the instances: ID, shader binding information,
		// matrices ... Those will be copied into the buffer by the helper through
		// mapping, so the buffer has to be allocated on the upload heap.
		topLevelASBuffers.instanceDesc = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_UPLOAD, instanceDescsSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);

		topLevelASBuffers.scratchSize = scratchSize;
		topLevelASBuffers.resultSize = resultSize;
		topLevelASBuffers.instanceDescSize = instanceDescsSize;
	}

	// After all the buffers are allocated, or if only an update is required, we can build the acceleration structure. 
	// Note that in the case of the update we also pass the existing AS as the 'previous' AS, so that it can be refitted in place.
	topLevelASGenerator.Generate(scene->getDevice()->getD3D12CommandList(), topLevelASBuffers.scratch.Get(), topLevelASBuffers.result.Get(), topLevelASBuffers.instanceDesc.Get(), false, topLevelASBuffers.result.Get());
}

void RT64::View::createShaderResourceHeap() {
	assert(usedTextures.size() <= 1024);

	uint32_t entryCount = 11 + (uint32_t)(usedTextures.size());

	// Recreate descriptor heap to be bigger if necessary.
	if (descriptorHeapEntryCount < entryCount) {
		if (descriptorHeap != nullptr) {
			descriptorHeap->Release();
			descriptorHeap = nullptr;
		}

		descriptorHeap = nv_helpers_dx12::CreateDescriptorHeap(scene->getDevice()->getD3D12Device(), entryCount, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);
		descriptorHeapEntryCount = entryCount;
	}

	const UINT handleIncrement = scene->getDevice()->getD3D12Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Get a handle to the heap memory on the CPU side, to be able to write the
	// descriptors directly
	D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();

	// UAV for output buffer.
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	scene->getDevice()->getD3D12Device()->CreateUnorderedAccessView(rtOutputResource.Get(), nullptr, &uavDesc, handle);
	handle.ptr += handleIncrement;
	
	// UAV for hit distance buffer.
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = scene->getDevice()->getWidth() * scene->getDevice()->getHeight() * MaxQueries;
	uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	scene->getDevice()->getD3D12Device()->CreateUnorderedAccessView(rtHitDistanceResource.Get(), nullptr, &uavDesc, handle);
	handle.ptr += handleIncrement;

	// UAV for hit color buffer.
	uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	scene->getDevice()->getD3D12Device()->CreateUnorderedAccessView(rtHitColorResource.Get(), nullptr, &uavDesc, handle);
	handle.ptr += handleIncrement;
	
	// UAV for hit normal buffer.
	uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	scene->getDevice()->getD3D12Device()->CreateUnorderedAccessView(rtHitNormalResource.Get(), nullptr, &uavDesc, handle);
	handle.ptr += handleIncrement;

	// UAV for hit shading buffer.
	uavDesc.Format = DXGI_FORMAT_R16_UINT;
	scene->getDevice()->getD3D12Device()->CreateUnorderedAccessView(rtHitInstanceIdResource.Get(), nullptr, &uavDesc, handle);
	handle.ptr += handleIncrement;

	// SRVs for background and foreground textures.
	D3D12_SHADER_RESOURCE_VIEW_DESC textureSRVDesc = {};
	textureSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	textureSRVDesc.Texture2D.MipLevels = 1;
	textureSRVDesc.Texture2D.MostDetailedMip = 0;
	textureSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	textureSRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scene->getDevice()->getD3D12Device()->CreateShaderResourceView(rasterResources[0].Get(), &textureSRVDesc, handle);
	handle.ptr += handleIncrement;

	scene->getDevice()->getD3D12Device()->CreateShaderResourceView(rasterResources[1].Get(), &textureSRVDesc, handle);
	handle.ptr += handleIncrement;

	// Add the Top Level AS SRV right after the raytracing output buffer
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	if (!topLevelASBuffers.result.IsNull()) {
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.RaytracingAccelerationStructure.Location = topLevelASBuffers.result.Get()->GetGPUVirtualAddress();
		scene->getDevice()->getD3D12Device()->CreateShaderResourceView(nullptr, &srvDesc, handle);
	}

	handle.ptr += handleIncrement;

	// Describe and create a constant buffer view for the camera
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = cameraBuffer.Get()->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = cameraBufferSize;
	scene->getDevice()->getD3D12Device()->CreateConstantBufferView(&cbvDesc, handle);
	handle.ptr += handleIncrement;

	// Describe and create a constant buffer view for the lights
	if (scene->getLightsCount() > 0) {
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = scene->getLightsCount();
		srvDesc.Buffer.StructureByteStride = sizeof(RT64_LIGHT);
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		scene->getDevice()->getD3D12Device()->CreateShaderResourceView(scene->getLightsBuffer(), &srvDesc, handle);
	}

	handle.ptr += handleIncrement;

	// Describe the properties buffer per instance.
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = static_cast<UINT>(rtInstances.size() + rasterBgInstances.size() + rasterFgInstances.size());
	srvDesc.Buffer.StructureByteStride = sizeof(InstanceProperties);
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	scene->getDevice()->getD3D12Device()->CreateShaderResourceView(activeInstancesBufferProps.Get(), &srvDesc, handle);
	handle.ptr += handleIncrement;

	// Add the texture SRV.
	for (size_t i = 0; i < usedTextures.size(); i++) {
		D3D12_SHADER_RESOURCE_VIEW_DESC textureSRVDesc = {};
		textureSRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		textureSRVDesc.Texture2D.MipLevels = 1;
		textureSRVDesc.Texture2D.MostDetailedMip = 0;
		textureSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		scene->getDevice()->getD3D12Device()->CreateShaderResourceView(usedTextures[i]->getTexture(), &textureSRVDesc, handle);
		handle.ptr += handleIncrement;
	}
}

void RT64::View::createShaderBindingTable() {
	// The SBT helper class collects calls to Add*Program.  If called several
	// times, the helper must be emptied before re-adding shaders.
	sbtHelper.Reset();

	// The pointer to the beginning of the heap is the only parameter required by
	// shaders without root parameters
	D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
	
	// The helper treats both root parameter pointers and heap pointers as void*,
	// while DX12 uses the
	// D3D12_GPU_DESCRIPTOR_HANDLE to define heap pointers. The pointer in this
	// struct is a UINT64, which then has to be reinterpreted as a pointer.
	auto heapPointer = reinterpret_cast<UINT64 *>(srvUavHeapHandle.ptr);

	// The ray generation only uses heap data.
	sbtHelper.AddRayGenerationProgram(L"TraceRayGen", { heapPointer });

	// The shadow miss shader does not use any external data.
	sbtHelper.AddMissProgram(L"ShadowMiss", {});

	// Add the vertex buffers from all the meshes used by the instances to the hit group.
	for (const RenderInstance &rtInstance :rtInstances) {
		sbtHelper.AddHitGroup(L"SurfaceHitGroup", {
			(void *)(rtInstance.vertexBufferView->BufferLocation),
			(void *)(rtInstance.indexBufferView->BufferLocation),
			heapPointer
		});

		sbtHelper.AddHitGroup(L"ShadowHitGroup", {
			(void*)(rtInstance.vertexBufferView->BufferLocation),
			(void*)(rtInstance.indexBufferView->BufferLocation),
			heapPointer
		});
	}
	
	// Compute the size of the SBT given the number of shaders and their parameters.
	uint32_t sbtSize = sbtHelper.ComputeSBTSize();
	if (sbtStorageSize < sbtSize) {
		// Release previously allocated SBT storage.
		sbtStorage.Release();

		// Create the SBT on the upload heap. This is required as the helper will use
		// mapping to write the SBT contents. After the SBT compilation it could be
		// copied to the default heap for performance.
		sbtStorage = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_UPLOAD, sbtSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
		sbtStorageSize = sbtSize;
	}

	// Compile the SBT from the shader and parameters info
	sbtHelper.Generate(sbtStorage.Get(), scene->getDevice()->getD3D12RtStateObjectProperties());
}

void RT64::View::createCameraBuffer() {
	cameraBufferSize = ROUND_UP(6 * sizeof(XMMATRIX), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	// Create the constant buffer for all matrices
	cameraBuffer = scene->getDevice()->allocateBuffer(D3D12_HEAP_TYPE_UPLOAD, cameraBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
}

void RT64::View::updateCameraBuffer() {
	std::vector<XMMATRIX> matrices(6);
	
	assert(fovRadians > 0.0f);
	matrices[0] = XMMatrixLookAtRH(XMVectorSet(eyePosition.x, eyePosition.y, eyePosition.z, 0.0f), XMVectorSet(eyeFocus.x, eyeFocus.y, eyeFocus.z, 0.0f), XMVectorSet(eyeUpDirection.x, eyeUpDirection.y, eyeUpDirection.z, 0.0f));
	matrices[1] = XMMatrixPerspectiveFovRH(fovRadians, scene->getDevice()->getAspectRatio(), nearDist, farDist);

	// Inverse matrices required for raytracing.
	XMVECTOR det;
	matrices[2] = XMMatrixInverse(&det, matrices[0]);
	matrices[3] = XMMatrixInverse(&det, matrices[1]);
	matrices[4] = previousViewProj;
	matrices[5] = XMMatrixMultiply(matrices[0], matrices[1]);
	previousViewProj = matrices[5];

	// Copy the matrix contents.
	uint8_t *pData;
	ThrowIfFailed(cameraBuffer.Get()->Map(0, nullptr, (void **)&pData));
	memcpy(pData, matrices.data(), cameraBufferSize);
	cameraBuffer.Get()->Unmap(0, nullptr);
}

void RT64::View::update() {
	if (!scene->getInstances().empty()) {
		// Create the active instance vectors.
		RenderInstance renderInstance;
		Mesh* usedMesh = nullptr;
		size_t totalInstances = scene->getInstances().size();
		rtInstances.clear();
		rasterBgInstances.clear();
		rasterFgInstances.clear();
		usedTextures.clear();

		rtInstances.reserve(totalInstances);
		rasterBgInstances.reserve(totalInstances);
		rasterFgInstances.reserve(totalInstances);
		usedTextures.reserve(1024);

		for (Instance *instance : scene->getInstances()) {
			usedMesh = instance->getMesh();
			renderInstance.bottomLevelAS = usedMesh->getBottomLevelASResult();
			renderInstance.transform = instance->getTransform();
			renderInstance.material = instance->getMaterial();
			renderInstance.indexCount = usedMesh->getIndexCount();
			renderInstance.indexBufferView = usedMesh->getIndexBufferView();
			renderInstance.vertexBufferView = usedMesh->getVertexBufferView();
			renderInstance.material.diffuseTexIndex = (int)(usedTextures.size());
			usedTextures.push_back(instance->getDiffuseTexture());

			if (instance->getNormalTexture() != nullptr) {
				renderInstance.material.normalTexIndex = (int)(usedTextures.size());
				usedTextures.push_back(instance->getNormalTexture());
			}
			else {
				renderInstance.material.normalTexIndex = -1;
			}

			if (renderInstance.bottomLevelAS != nullptr) {
				rtInstances.push_back(renderInstance);
			}
			else if (renderInstance.material.background) {
				rasterBgInstances.push_back(renderInstance);
			}
			else {
				rasterFgInstances.push_back(renderInstance);
			}
		}

		// Create the acceleration structures used by the raytracer.
		if (!rtInstances.empty()) {
			createTopLevelAS(rtInstances);
		}

		// Create the instance properties buffer for the active instances (if necessary).
		createInstancePropertiesBuffer();
		
		// Create the buffer containing the raytracing result (always output in a
		// UAV), and create the heap referencing the resources used by the raytracing,
		// such as the acceleration structure
		createShaderResourceHeap();
		
		// Create the shader binding table and indicating which shaders
		// are invoked for each instance in the AS.
		createShaderBindingTable();

		// Update the instance properties buffer for the active instances.
		updateInstancePropertiesBuffer();
	}
	else {
		rtInstances.clear();
		rasterBgInstances.clear();
		rasterFgInstances.clear();
	}

	// Update the camera buffer.
	updateCameraBuffer();
}

void RT64::View::render() {
	if (descriptorHeap == nullptr) {
		return;
	}
	
	CD3DX12_RESOURCE_BARRIER rasterBarriers[2];
	auto d3dCommandList = scene->getDevice()->getD3D12CommandList();
	auto d3d12RenderTarget = scene->getDevice()->getD3D12RenderTarget();

	// Set the right pipeline state.
	d3dCommandList->SetPipelineState(scene->getDevice()->getD3D12PipelineState());
	
	// Set the root graphics signature used for rasterization.
	auto viewport = scene->getDevice()->getD3D12Viewport();
	auto scissorRect = scene->getDevice()->getD3D12ScissorRect();
	d3dCommandList->SetGraphicsRootSignature(scene->getDevice()->getD3D12RootSignature());

	// Bind the descriptor heap and the set heap as a descriptor table.
	std::vector<ID3D12DescriptorHeap *> heaps = { descriptorHeap };
	d3dCommandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
	d3dCommandList->SetGraphicsRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

	// Configure the current viewport.
	d3dCommandList->RSSetViewports(1, &viewport);
	d3dCommandList->RSSetScissorRects(1, &scissorRect);

	// Rasterization.
	{
		// Transition the background and foreground to Render Targets.
		rasterBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(rasterResources[0].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		rasterBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(rasterResources[1].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		d3dCommandList->ResourceBarrier(_countof(rasterBarriers), rasterBarriers);

		UINT instanceIndex = (UINT)(rtInstances.size());
		for (int i = 0; i < 2; i++) {
			// Set the output resource as the render target and clear it.
			CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rasterRtvHeaps[i]->GetCPUDescriptorHandleForHeapStart(), 0, outputRtvDescriptorSize);
			d3dCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

			const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
			d3dCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

			// Render all rasterization instances.
			d3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			const std::vector<RenderInstance>& rasterInstances = i ? rasterFgInstances : rasterBgInstances;
			for (size_t j = 0; j < rasterInstances.size(); j++) {
				const RenderInstance& renderInstance = rasterInstances[j];
				d3dCommandList->SetGraphicsRoot32BitConstant(0, instanceIndex++, 0);
				d3dCommandList->IASetVertexBuffers(0, 1, renderInstance.vertexBufferView);
				d3dCommandList->IASetIndexBuffer(renderInstance.indexBufferView);
				d3dCommandList->DrawIndexedInstanced(renderInstance.indexCount, 1, 0, 0, 0);
			}
		}

		// Transition the the background and foreground from render targets to SRV.
		rasterBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(rasterResources[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		rasterBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(rasterResources[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		d3dCommandList->ResourceBarrier(_countof(rasterBarriers), rasterBarriers);
	}

	// Raytracing.
	{
		// Transition the output resource from a copy source to a UAV.
		CD3DX12_RESOURCE_BARRIER rtBarrier = CD3DX12_RESOURCE_BARRIER::Transition(rtOutputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		d3dCommandList->ResourceBarrier(1, &rtBarrier);

		if (!rtInstances.empty()) {
			D3D12_DISPATCH_RAYS_DESC desc = {};

			// Ray generation.
			uint32_t rayGenerationSectionSizeInBytes = sbtHelper.GetRayGenSectionSize();
			desc.RayGenerationShaderRecord.StartAddress = sbtStorage.Get()->GetGPUVirtualAddress();
			desc.RayGenerationShaderRecord.SizeInBytes = rayGenerationSectionSizeInBytes;

			// Miss shader table.
			uint32_t missSectionSizeInBytes = sbtHelper.GetMissSectionSize();
			desc.MissShaderTable.StartAddress = sbtStorage.Get()->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
			desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
			desc.MissShaderTable.StrideInBytes = sbtHelper.GetMissEntrySize();

			// Hit group table.
			uint32_t hitGroupsSectionSize = sbtHelper.GetHitGroupSectionSize();
			desc.HitGroupTable.StartAddress = sbtStorage.Get()->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes + missSectionSizeInBytes;
			desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
			desc.HitGroupTable.StrideInBytes = sbtHelper.GetHitGroupEntrySize();

			// Dimensions.
			desc.Width = scene->getDevice()->getWidth();
			desc.Height = scene->getDevice()->getHeight();
			desc.Depth = 1;

			// Bind pipeline and dispatch rays.
			d3dCommandList->SetPipelineState1(scene->getDevice()->getD3D12RtStateObject());
			d3dCommandList->DispatchRays(&desc);
		}

		// Transition the output resource from a UAV to a copy source.
		rtBarrier = CD3DX12_RESOURCE_BARRIER::Transition(rtOutputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		d3dCommandList->ResourceBarrier(1, &rtBarrier);
	}
	
	// Copy raytracing output to render target.
	{
		// Transition the render target into a copy destination.
		CD3DX12_RESOURCE_BARRIER targetBarrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12RenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCommandList->ResourceBarrier(1, &targetBarrier);

		// Copy from output resource to render target.
		d3dCommandList->CopyResource(d3d12RenderTarget, rtOutputResource.Get());

		// Transition the render target back into its original state.
		targetBarrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12RenderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		d3dCommandList->ResourceBarrier(1, &targetBarrier);
	}
}

void RT64::View::setPerspectiveLookAt(RT64_VECTOR3 eyePosition, RT64_VECTOR3 eyeFocus, RT64_VECTOR3 eyeUpDirection, float fovRadians, float nearDist, float farDist) {
	this->eyePosition = eyePosition;
	this->eyeFocus = eyeFocus;
	this->eyeUpDirection = eyeUpDirection;
	this->fovRadians = fovRadians;
	this->nearDist = nearDist;
	this->farDist = farDist;
}

void RT64::View::resize() {
	createOutputBuffers();
}

// Public

DLLEXPORT RT64_VIEW *RT64_CreateView(RT64_SCENE *scenePtr) {
	assert(scenePtr != nullptr);
	RT64::Scene *scene = (RT64::Scene *)(scenePtr);
	return (RT64_VIEW *)(new RT64::View(scene));
}

DLLEXPORT void RT64_SetViewPerspective(RT64_VIEW* viewPtr, RT64_VECTOR3 eyePosition, RT64_VECTOR3 eyeFocus, RT64_VECTOR3 eyeUpDirection, float fovRadians, float nearDist, float farDist) {
	assert(viewPtr != nullptr);
	RT64::View *view = (RT64::View *)(viewPtr);
	view->setPerspectiveLookAt(eyePosition, eyeFocus, eyeUpDirection, fovRadians, nearDist, farDist);
}

DLLEXPORT void RT64_DestroyView(RT64_VIEW *viewPtr) {
	delete (RT64::View *)(viewPtr);
}