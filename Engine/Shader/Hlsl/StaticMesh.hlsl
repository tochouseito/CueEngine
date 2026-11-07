// 描画レコードが指定する行列番号。Command Signature の先頭 DWORD と対応する
cbuffer DrawRecord : register(b0)
{
    uint transformIndex;
};

struct TransformRecord
{
    row_major float4x4 worldViewProjection;
};
StructuredBuffer<TransformRecord> transforms : register(t0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};
struct PixelInput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

// 分離 VB の頂点を行ベクトルとして変換し、面を識別できる固定色を出す
PixelInput vs_main(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0), transforms[transformIndex].worldViewProjection);
    output.color = 0.15 + 0.75 * abs(input.normal);
    return output;
}

// 最初の Material は Texture / Lighting を必要としない不透明な面色とする
float4 ps_main(PixelInput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
