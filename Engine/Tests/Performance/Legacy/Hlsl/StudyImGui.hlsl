// #101 調査専用。ImDrawVert の位置、UV、頂点色を通常 ImGui と同じ計算で描画する
cbuffer Projection : register(b0)
{
    float4x4 transform;
};
struct VertexInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
/// @brief UI 座標を Clip 座標へ投影する
PixelInput vertex_main(VertexInput input)
{
    PixelInput output;
    output.position = mul(transform, float4(input.position, 0.0f, 1.0f));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
Texture2D image : register(t0);
SamplerState imageSampler : register(s0);
/// @brief Font または UI Texture の値を頂点色と合成する
float4 pixel_main(PixelInput input) : SV_Target
{
    return input.color * image.Sample(imageSampler, input.uv);
}
