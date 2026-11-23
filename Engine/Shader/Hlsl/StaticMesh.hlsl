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
    uint meshIndex;
    uint materialId;
};

StructuredBuffer<Candidate> candidates : register(t1);
StructuredBuffer<uint> instanceIndices : register(t2);
StructuredBuffer<float4> materialColors : register(t3);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};
struct PixelInput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
    nointerpolation uint materialId : TEXCOORD0;
};

// バッチの各インスタンスに対応するワールド変換と Material を参照する
PixelInput vs_main(VertexInput input, uint instanceId : SV_InstanceID)
{
    const Candidate candidate = candidates[instanceIndices[instanceStart + instanceId]];
    PixelInput output;
    output.position = mul(float4(input.position, 1.0), transforms[candidate.transformIndex].worldViewProjection);
    output.color = 0.15 + 0.75 * abs(input.normal);
    output.materialId = candidate.materialId;
    return output;
}

// 既定 Material の白では従来の面色を維持する
float4 ps_main(PixelInput input) : SV_Target0
{
    return float4(input.color, 1.0) * materialColors[input.materialId];
}
