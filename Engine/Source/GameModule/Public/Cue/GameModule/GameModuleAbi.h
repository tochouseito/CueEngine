#pragma once

#include <stdint.h>

#if defined(__cplusplus)
#define CUE_GAME_MODULE_EXTERN_C extern "C"
#define CUE_GAME_MODULE_NOEXCEPT noexcept
#else
#define CUE_GAME_MODULE_EXTERN_C extern
#define CUE_GAME_MODULE_NOEXCEPT
#endif

#if defined(_WIN32)
#define CUE_GAME_MODULE_CALL __cdecl
#if defined(CUE_GAME_MODULE_BUILD)
#define CUE_GAME_MODULE_EXPORT __declspec(dllexport)
#else
#define CUE_GAME_MODULE_EXPORT __declspec(dllimport)
#endif
#else
#define CUE_GAME_MODULE_CALL
#define CUE_GAME_MODULE_EXPORT
#endif

#define CUE_GAME_MODULE_ABI_VERSION_1 UINT32_C(1)
#define CUE_GAME_MODULE_STRUCTURE_VERSION_1 UINT32_C(1)

#define CUE_GAME_MODULE_RESULT_SUCCESS UINT32_C(0)
#define CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT UINT32_C(1)
#define CUE_GAME_MODULE_RESULT_UNSUPPORTED_ABI UINT32_C(2)
#define CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY UINT32_C(3)
#define CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED UINT32_C(4)
#define CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED UINT32_C(5)

#define CUE_GAME_MODULE_CONFIGURATION_DEBUG UINT32_C(1)
#define CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT UINT32_C(2)
#define CUE_GAME_MODULE_CONFIGURATION_RELEASE UINT32_C(3)

#define CUE_GAME_MODULE_ARCHITECTURE_X64 UINT32_C(1)

#define CUE_GAME_MODULE_SYSTEM_PHASE_PRE_UPDATE UINT32_C(1)
#define CUE_GAME_MODULE_SYSTEM_PHASE_UPDATE UINT32_C(2)
#define CUE_GAME_MODULE_SYSTEM_PHASE_POST_UPDATE UINT32_C(3)

typedef uint32_t CueGameModuleResult;
typedef void *CueGameModuleHandle;
typedef void *CueGameSystemState;

typedef struct CueGameUtf8ViewV1
{
    uint32_t structSize;
    uint32_t version;
    const char *data;
    uint64_t size;
} CueGameUtf8ViewV1;

typedef struct CueGameUuidV1
{
    uint32_t structSize;
    uint32_t version;
    uint8_t bytes[16];
} CueGameUuidV1;

typedef struct CueGameModuleDiagnosticV1
{
    uint32_t structSize;
    uint32_t version;
    CueGameModuleResult code;
    uint32_t reserved;
    CueGameUtf8ViewV1 message;
} CueGameModuleDiagnosticV1;

typedef struct CueGameFieldDescriptorV1
{
    uint32_t structSize;
    uint32_t version;
    uint32_t fieldId;
    uint32_t reserved;
    CueGameUtf8ViewV1 diagnosticName;
} CueGameFieldDescriptorV1;

typedef struct CueGameSchemaDescriptorV1
{
    uint32_t structSize;
    uint32_t version;
    CueGameUuidV1 typeId;
    CueGameUtf8ViewV1 diagnosticName;
    uint32_t schemaVersion;
    uint32_t reserved;
    const CueGameFieldDescriptorV1 *fields;
    uint64_t fieldCount;
    const uint32_t *reservedFieldIds;
    uint64_t reservedFieldIdCount;
} CueGameSchemaDescriptorV1;

typedef struct CueGameComponentDescriptorV1
{
    uint32_t structSize;
    uint32_t version;
    CueGameUuidV1 typeId;
    uint32_t flags;
    uint32_t reserved;
} CueGameComponentDescriptorV1;

typedef struct CueGameSystemUpdateV1
{
    uint32_t structSize;
    uint32_t version;
    uint64_t frameIndex;
    int64_t deltaNanoseconds;
    int64_t elapsedNanoseconds;
} CueGameSystemUpdateV1;

typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameSystemCreateV1)(
    CueGameModuleHandle a_module, CueGameSystemState *a_state,
    CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;
typedef void(CUE_GAME_MODULE_CALL *CueGameSystemDestroyV1)(CueGameSystemState a_state) CUE_GAME_MODULE_NOEXCEPT;
typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameSystemStartV1)(
    CueGameSystemState a_state, CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;
typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameSystemUpdateCallbackV1)(
    CueGameSystemState a_state, const CueGameSystemUpdateV1 *a_update,
    CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;
typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameSystemStopV1)(
    CueGameSystemState a_state, CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;

typedef struct CueGameSystemDescriptorV1
{
    uint32_t structSize;
    uint32_t version;
    CueGameUtf8ViewV1 stableId;
    uint32_t phase;
    int32_t order;
    const CueGameUtf8ViewV1 *dependencies;
    uint64_t dependencyCount;
    CueGameSystemCreateV1 createState;
    CueGameSystemDestroyV1 destroyState;
    CueGameSystemStartV1 start;
    CueGameSystemUpdateCallbackV1 update;
    CueGameSystemStopV1 stop;
} CueGameSystemDescriptorV1;

typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameRegisterSchemaV1)(
    void *a_context, const CueGameSchemaDescriptorV1 *a_descriptor,
    CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;
typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameRegisterComponentV1)(
    void *a_context, const CueGameComponentDescriptorV1 *a_descriptor,
    CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;
typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameRegisterSystemV1)(
    void *a_context, const CueGameSystemDescriptorV1 *a_descriptor,
    CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;

typedef struct CueGameRegistrationSinkV1
{
    uint32_t structSize;
    uint32_t version;
    void *context;
    CueGameRegisterSchemaV1 registerSchema;
    CueGameRegisterComponentV1 registerComponent;
    CueGameRegisterSystemV1 registerSystem;
    uint64_t reserved[4];
} CueGameRegistrationSinkV1;

typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameModuleCreateV1)(
    CueGameModuleHandle *a_module, CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;
typedef CueGameModuleResult(CUE_GAME_MODULE_CALL *CueGameModuleRegisterV1)(
    CueGameModuleHandle a_module, const CueGameRegistrationSinkV1 *a_sink,
    CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;
typedef void(CUE_GAME_MODULE_CALL *CueGameModuleDestroyV1)(CueGameModuleHandle a_module) CUE_GAME_MODULE_NOEXCEPT;

typedef struct CueGameModuleApiV1
{
    uint32_t structSize;
    uint32_t version;
    uint32_t abiVersion;
    uint32_t configuration;
    uint32_t architecture;
    uint32_t reserved;
    CueGameUuidV1 projectId;
    CueGameModuleCreateV1 createModule;
    CueGameModuleRegisterV1 registerSchemas;
    CueGameModuleRegisterV1 registerComponents;
    CueGameModuleRegisterV1 registerSystems;
    CueGameModuleDestroyV1 destroyModule;
    uint64_t reservedTail[4];
} CueGameModuleApiV1;

typedef struct CueGameModuleQueryOutputV1
{
    uint32_t structSize;
    uint32_t version;
    const CueGameModuleApiV1 *api;
    uint64_t reserved[2];
} CueGameModuleQueryOutputV1;

/// @brief Host要求Versionと互換なDLL所有API Tableを借用出力へ返す
CUE_GAME_MODULE_EXTERN_C CUE_GAME_MODULE_EXPORT CueGameModuleResult CUE_GAME_MODULE_CALL
cue_game_module_query(uint32_t a_requestedAbiVersion, CueGameModuleQueryOutputV1 *a_output,
                      CueGameModuleDiagnosticV1 *a_diagnostic) CUE_GAME_MODULE_NOEXCEPT;
