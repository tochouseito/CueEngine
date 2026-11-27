// バッチのインスタンス範囲の先頭。Command Signature の先頭 DWORD と対応する
cbuffer DrawRecord : register(b0)
{
    uint instanceStart;
};

struct TransformRecord
{
    row_major float4x4 worldViewProjection;
};
StructuredBuffer<TransformRecord> transforms : register(t0);

struct Candidate
{
    uint transformIndex;
    uint groupIndex;
    uint materialId;
};

struct MaterialRecord
{
    float4 baseColor;
    uint textureIndex;
};

StructuredBuffer<Candidate> candidates : register(t1);
StructuredBuffer<uint> instanceIndices : register(t2);
StructuredBuffer<MaterialRecord> materials : register(t3);
Texture2D<float4> baseColorTextures[64] : register(t4);
SamplerState baseColorSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct PixelInput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
    nointerpolation uint materialId : TEXCOORD1;
};

// バッチの各インスタンスに対応するワールド変換と Material を参照する
PixelInput vs_main(VertexInput input, uint instanceId : SV_InstanceID)
{
    const Candidate candidate = candidates[instanceIndices[instanceStart + instanceId]];
    PixelInput output;
    output.position = mul(float4(input.position, 1.0), transforms[candidate.transformIndex].worldViewProjection);
    output.color = 0.15 + 0.75 * abs(input.normal);
    output.uv = input.uv;
    output.materialId = candidate.materialId;
    return output;
}

// FinalColor は UNORM のため Base Color の線形計算後に sRGB で保存する
float3 encode_srgb(float3 color)
{
    color = saturate(color);
    return lerp(color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055, step(0.0031308, color));
}

// 明示 Material の Texture が未指定なら白を掛ける。Slot 0 は従来の面色を表示する
float4 ps_main(PixelInput input) : SV_Target0
{
    if (input.materialId == 0)
    {
        return float4(input.color, 1.0);
    }

    const MaterialRecord material = materials[input.materialId];
    const float4 texel = material.textureIndex == 0
        ? float4(1, 1, 1, 1)
        : baseColorTextures[NonUniformResourceIndex(material.textureIndex)].Sample(baseColorSampler, input.uv);
    const float4 color = material.baseColor * texel;
    return float4(encode_srgb(color.rgb), color.a);
}
