Texture2D<float4> FinalColorTexture : register(t0);
SamplerState FinalColorSampler : register(s0);

struct FullscreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

FullscreenVertex VSMain(uint a_vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.uv = float2((a_vertexId << 1) & 2, a_vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PSMain(FullscreenVertex a_input) : SV_Target0
{
    return FinalColorTexture.Sample(FinalColorSampler, a_input.uv);
}
