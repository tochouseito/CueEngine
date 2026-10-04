// FinalColorTexture の線形色を BackBuffer へそのまま転送する。
Texture2D<float4> g_finalColor : register(t0);
SamplerState g_sampler : register(s0);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput vs_main(uint a_vertexId : SV_VertexID)
{
    // 頂点 Buffer を使わず、画面全体を覆う巨大三角形を生成する。
    const float2 coordinate = float2((a_vertexId << 1) & 2, a_vertexId & 2);
    VertexOutput output;
    output.position = float4(coordinate * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = coordinate;
    return output;
}

float4 ps_main(VertexOutput a_input) : SV_Target0
{
    return g_finalColor.Sample(g_sampler, a_input.uv);
}
