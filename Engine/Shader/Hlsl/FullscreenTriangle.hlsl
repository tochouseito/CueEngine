// FinalColorTexture の線形色を BackBuffer へそのまま転送する。
// 表示元 Texture を Root Signature の t0 に対応付ける
Texture2D<float4> g_finalColor : register(t0);
// FinalColor の読取りに使う固定 Sampler を s0 に対応付ける
SamplerState g_sampler : register(s0);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput vs_main(uint a_vertexId : SV_VertexID)
{
    // 頂点 Buffer を使わず、画面全体を覆う巨大三角形を生成する。
    // 三頂点の座標を (0,0)、(2,0)、(0,2) とし、画面を覆う巨大三角形を作る
    const float2 coordinate = float2((a_vertexId << 1) & 2, a_vertexId & 2);
    VertexOutput output;
    // UV の向きに合わせて Y を反転し、Clip 座標へ変換する
    output.position = float4(coordinate * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    // 補間後に画面内で 0〜1 となる Texture 座標を渡す
    output.uv = coordinate;
    return output;
}

float4 ps_main(VertexOutput a_input) : SV_Target0
{
    // 補間された UV で FinalColor を読み、Back Buffer へ出力する
    return g_finalColor.Sample(g_sampler, a_input.uv);
}
