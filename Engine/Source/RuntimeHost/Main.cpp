#include "RuntimePackage.h"

#include <Cue/RuntimeHost/RuntimeHostProcess.h>

/// @brief Dynamic Package Providerを選択して共通Runtime Host Processを開始する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    const cue::runtime_host::RuntimeHostProcessDescriptor descriptor = {
        &cue::runtime_host::load_runtime_package,
        false,
    };
    return cue::runtime_host::run_runtime_host_process(a_argumentCount, a_arguments, descriptor);
}
