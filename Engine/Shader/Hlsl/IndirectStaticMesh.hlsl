struct TransformRecord
{
    row_major float4x4 worldViewProjection;
};

struct Candidate
{
    uint transformIndex;
    uint groupIndex;
    uint materialId;
};

struct MeshRange
{
    uint indexCount;
    uint firstIndex;
    int baseVertex;
};

struct Page
{
    uint firstGroup;
    uint groupCount;
    uint firstCommand;
};

struct Batch
{
    uint count;
    uint instanceStart;
    uint written;
};

// Command Signature の Root Constant と DRAW_INDEXED に対応する 24 Byte のレコード
struct IndirectDraw
{
    uint instanceStart;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int baseVertex;
    uint firstInstance;
};

cbuffer FrameCounts : register(b0)
{
    uint candidateCount;
    uint groupCount;
    uint pageCount;
};

StructuredBuffer<TransformRecord> transforms : register(t0);
StructuredBuffer<Candidate> candidates : register(t1);
StructuredBuffer<MeshRange> meshes : register(t2);
StructuredBuffer<Page> pages : register(t3);
StructuredBuffer<Batch> filledBatches : register(t4);
RWStructuredBuffer<Batch> batches : register(u0);
RWStructuredBuffer<uint> instanceIndices : register(u1);
RWStructuredBuffer<IndirectDraw> commands : register(u2);
RWByteAddressBuffer commandCounts : register(u3);

static const uint k_depthBinCount = 8;

// 不透明描画の順序付けに使う。画面外やカメラ後方の候補もバッチへ入れる
uint batch_index(Candidate candidate)
{
    const float4 center = transforms[candidate.transformIndex].worldViewProjection[3];
    const float depth = center.w > 0.0f ? saturate(center.z / center.w) : 0.0f;
    const uint depthBin = min((uint)(depth * k_depthBinCount), k_depthBinCount - 1);
    return candidate.groupIndex * k_depthBinCount + depthBin;
}

// 直前の GPU 枠の結果が現在のバッチに残らないよう、使用範囲を初期化する
[numthreads(64, 1, 1)]
void reset_batches(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x < groupCount * k_depthBinCount)
    {
        batches[threadId.x] = (Batch)0;
    }
}

// 同じ Mesh・Material・深度区分に属するインスタンス数を数える
[numthreads(64, 1, 1)]
void count_batches(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x < candidateCount)
    {
        InterlockedAdd(batches[batch_index(candidates[threadId.x])].count, 1);
    }
}

// Page ごとの連続領域を手前の深度区分から割り当て、散布先を確定する
[numthreads(64, 1, 1)]
void prefix_batches(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x >= pageCount)
    {
        return;
    }
    const Page page = pages[threadId.x];
    uint nextInstance = page.firstCommand;
    for (uint depthBin = 0; depthBin < k_depthBinCount; ++depthBin)
    {
        const uint end = (page.firstGroup + page.groupCount) * k_depthBinCount;
        for (uint batch = page.firstGroup * k_depthBinCount + depthBin;
             batch < end; batch += k_depthBinCount)
        {
            batches[batch].instanceStart = nextInstance;
            nextInstance += batches[batch].count;
        }
    }
}

// 頂点 Shader が SV_InstanceID で引けるよう、候補番号をバッチの連続範囲へ置く
[numthreads(64, 1, 1)]
void fill_batches(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x < candidateCount)
    {
        const uint batch = batch_index(candidates[threadId.x]);
        uint offset;
        InterlockedAdd(batches[batch].written, 1, offset);
        instanceIndices[batches[batch].instanceStart + offset] = threadId.x;
    }
}

// VB/IB が同じ Page の範囲内で、空でないバッチを連続した間接コマンドへ変換する
[numthreads(64, 1, 1)]
void emit_commands(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x >= pageCount)
    {
        return;
    }
    const Page page = pages[threadId.x];
    uint count = 0;
    for (uint depthBin = 0; depthBin < k_depthBinCount; ++depthBin)
    {
        const uint end = (page.firstGroup + page.groupCount) * k_depthBinCount;
        for (uint index = page.firstGroup * k_depthBinCount + depthBin;
             index < end; index += k_depthBinCount)
        {
            const Batch batch = filledBatches[index];
            if (batch.count > 0)
            {
                const MeshRange mesh = meshes[index / k_depthBinCount];
                IndirectDraw draw;
                draw.instanceStart = batch.instanceStart;
                draw.indexCount = mesh.indexCount;
                draw.instanceCount = batch.count;
                draw.firstIndex = mesh.firstIndex;
                draw.baseVertex = mesh.baseVertex;
                draw.firstInstance = 0;
                commands[page.firstCommand + count] = draw;
                ++count;
            }
        }
    }
    commandCounts.Store(threadId.x * 4, count);
}
