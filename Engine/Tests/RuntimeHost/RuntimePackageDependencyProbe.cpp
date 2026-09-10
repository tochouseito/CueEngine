/// @brief RuntimeHost Test用App-local DLLが検証済み絶対PathからLoadされたことを識別する
extern "C" __declspec(dllexport) int cue_runtime_package_dependency_probe() noexcept
{
    return 42;
}
