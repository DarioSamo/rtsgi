//
// RT64
//

#ifdef SHADER_AS_STRING
R"raw(
#else

#define RAY_MIN_DISTANCE					0.1f
#define RAY_MAX_DISTANCE					100000.0f

// Structures

struct HitInfo {
	uint nhits;
	uint ohits;
};

struct Attributes {
	float2 bary;
};

struct ShadowHitInfo {
	float shadowHit;
};

// Root signature

RaytracingAccelerationStructure SceneBVH : register(t0);
//)raw"
#endif