cbuffer DrawConstants : register(b0)
{
    float4 tint;
};

struct VertexInput
{
    float3 position : POSITION;
    float3 color : COLOR;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = float4(input.position, 1.0);
    output.color = input.color * tint.rgb;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    return float4(input.color, 1.0);
}
