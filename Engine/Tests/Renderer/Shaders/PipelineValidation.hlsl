RWByteAddressBuffer g_output : register(u0);

[numthreads(1, 1, 1)] void cs_main(uint3 a_id : SV_DispatchThreadID) { g_output.Store(0, 0x12345678); }

    // 抽象 Context の Dispatch 経路を Resource Binding なしで検証する
    [numthreads(1, 1, 1)] void cs_empty_main(uint3 a_id : SV_DispatchThreadID)
{
}
