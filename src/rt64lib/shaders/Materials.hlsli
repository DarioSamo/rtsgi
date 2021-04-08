//
// RT64
//

struct MaterialProperties {
	int background;
	int filterMode;
	int diffuseTexIndex;
	int normalTexIndex;
	int hAddressMode;
	int vAddressMode;
	float ignoreNormalFactor;
	float normalMapScale;
	float reflectionFactor;
	float reflectionShineFactor;
	float refractionFactor;
	float specularIntensity;
	float specularExponent;
	float solidAlphaMultiplier;
	float shadowAlphaMultiplier;
	float3 selfLight;
	uint lightGroupMaskBits;
	float3 fogColor;
	float4 diffuseColorMix;
	float fogMul;
	float fogOffset;
};