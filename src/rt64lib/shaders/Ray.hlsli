//
// RT64
//

// Structures

struct HitInfo {
	uint nhits;
};

struct Attributes {
	float2 bary;
};

struct ShadowHitInfo {
	float shadowHit;
};

// Root signature

RaytracingAccelerationStructure SceneBVH : register(t0);