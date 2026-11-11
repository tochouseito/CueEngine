#pragma once

#include <stdint.h>

#if defined(_MSC_VER)
#define CUE_SCRIPT_CALL __cdecl
#define CUE_SCRIPT_EXPORT __declspec(dllexport)
#else
#define CUE_SCRIPT_CALL
#define CUE_SCRIPT_EXPORT __attribute__((visibility("default")))
#endif

#define CUE_SCRIPT_ABI_VERSION 3u
#define CUE_SCRIPT_ABI_VERSION_2 2u
#define CUE_SCRIPT_MESH_ID_CAPACITY 256u

#ifdef __cplusplus
extern "C"
{
#endif

    // ABI は固定幅の値と呼出中だけ有効な借用を渡す。所有値は生成した Module で回収する
    typedef struct CueScriptString
    {
        const char *data;
        uint32_t size;
    } CueScriptString;

    // code は 0=成功、1=入力、2=Platform、3=Fatal。文字列は必ず終端する
    typedef struct CueScriptStatus
    {
        uint32_t code;
        char message[384];
    } CueScriptStatus;

    typedef struct CueScriptEntity
    {
        uint64_t world;
        uint32_t index;
        uint64_t generation;
    } CueScriptEntity;

    typedef struct CueScriptSceneInstance
    {
        uint64_t world;
        uint32_t index;
        uint64_t generation;
    } CueScriptSceneInstance;

    typedef struct CueScriptTransform
    {
        float position[3];
        float rotation[4];
        float scale[3];
    } CueScriptTransform;

    enum
    {
        CUE_SCRIPT_TRANSFORM_POSITION = 0,
        CUE_SCRIPT_TRANSFORM_SCALE = 1,
        CUE_SCRIPT_TRANSFORM_SET = 0,
        CUE_SCRIPT_TRANSFORM_ADD = 1,
        CUE_SCRIPT_TRANSFORM_MULTIPLY = 2,
        CUE_SCRIPT_TRANSFORM_TRANSLATE = 3
    };

    // 座標の操作を一度の呼出で検証・反映する。TRANSLATE は value の全軸を使う
    typedef struct CueScriptTransformChange
    {
        uint32_t field;
        uint32_t axis;
        uint32_t operation;
        float value[3];
    } CueScriptTransformChange;

    typedef struct CueScriptCamera
    {
        float verticalFovRadians;
        float nearClip;
        float farClip;
    } CueScriptCamera;

    typedef struct CueScriptMeshRenderer
    {
        char assetId[CUE_SCRIPT_MESH_ID_CAPACITY];
        uint32_t assetIdSize;
    } CueScriptMeshRenderer;

    typedef struct CueScriptUpdateContext
    {
        int64_t deltaNanoseconds;
        uint64_t tick;
    } CueScriptUpdateContext;

    typedef struct CueScriptPropertyValue
    {
        uint32_t kind;
        double number;
        uint32_t boolean;
    } CueScriptPropertyValue;

    typedef struct CueScriptTypeInfo
    {
        CueScriptString typeId;
        CueScriptString displayName;
        uint32_t schemaVersion;
        uint32_t propertyCount;
    } CueScriptTypeInfo;

    typedef struct CueScriptPropertyInfo
    {
        CueScriptString propertyId;
        CueScriptString displayName;
        CueScriptPropertyValue defaultValue;
    } CueScriptPropertyInfo;

    typedef uint64_t CueScriptInstance;

    // World や Component の Pointer は公開せず、世代付き Handle と所有値で操作する
    typedef struct CueScriptEngineApi
    {
        uint32_t abiVersion;
        uint32_t structSize;
        void *context;
        CueScriptStatus(CUE_SCRIPT_CALL *is_alive)(void *, CueScriptEntity, uint32_t *);
        CueScriptStatus(CUE_SCRIPT_CALL *get_transform)(void *, CueScriptEntity, CueScriptTransform *);
        CueScriptStatus(CUE_SCRIPT_CALL *set_transform)(void *, CueScriptEntity, const CueScriptTransform *);
        CueScriptStatus(CUE_SCRIPT_CALL *find_by_name)(void *, CueScriptSceneInstance, CueScriptString,
                                                       CueScriptEntity *);
        CueScriptStatus(CUE_SCRIPT_CALL *report)(void *, uint32_t, CueScriptString);
        CueScriptStatus(CUE_SCRIPT_CALL *get_camera)(void *, CueScriptEntity, CueScriptCamera *);
        CueScriptStatus(CUE_SCRIPT_CALL *get_mesh_renderer)(void *, CueScriptEntity, CueScriptMeshRenderer *);
        CueScriptStatus(CUE_SCRIPT_CALL *find_script)(void *, CueScriptEntity, CueScriptString, CueScriptString,
                                                       CueScriptInstance *);
        CueScriptStatus(CUE_SCRIPT_CALL *get_scene_instance)(void *, CueScriptEntity, CueScriptSceneInstance *);
        CueScriptStatus(CUE_SCRIPT_CALL *change_transform)(void *, CueScriptEntity, const CueScriptTransformChange *);
    } CueScriptEngineApi;

    // context は Module 自身を借用する。Unload の前に全 Instance を destroy し、この Table を失効させる
    typedef struct CueScriptModuleApi
    {
        uint32_t abiVersion;
        uint32_t structSize;
        uint32_t pointerSize;
        uint32_t configuration;
        void *context;
        uint32_t(CUE_SCRIPT_CALL *type_count)(void *);
        CueScriptStatus(CUE_SCRIPT_CALL *type_info)(void *, uint32_t, CueScriptTypeInfo *);
        CueScriptStatus(CUE_SCRIPT_CALL *property_info)(void *, uint32_t, uint32_t, CueScriptPropertyInfo *);
        CueScriptStatus(CUE_SCRIPT_CALL *create)(void *, uint32_t, CueScriptInstance *);
        CueScriptStatus(CUE_SCRIPT_CALL *set_property)(void *, CueScriptInstance, CueScriptString,
                                                       CueScriptPropertyValue);
        CueScriptStatus(CUE_SCRIPT_CALL *start)(void *, CueScriptInstance, const CueScriptEngineApi *, CueScriptEntity);
        CueScriptStatus(CUE_SCRIPT_CALL *update)(void *, CueScriptInstance, const CueScriptEngineApi *, CueScriptEntity,
                                                 CueScriptUpdateContext);
        CueScriptStatus(CUE_SCRIPT_CALL *stop)(void *, CueScriptInstance, const CueScriptEngineApi *, CueScriptEntity);
        CueScriptStatus(CUE_SCRIPT_CALL *destroy)(void *, CueScriptInstance);
        CueScriptStatus(CUE_SCRIPT_CALL *set_active)(void *, CueScriptInstance, uint32_t);
    } CueScriptModuleApi;

    // Export 名は CueScriptGetModule。Module の登録を構築する例外もこの境界内で Status に変換する
    typedef CueScriptStatus(CUE_SCRIPT_CALL *CueScriptGetModuleFunction)(uint32_t, CueScriptModuleApi *);

#ifdef __cplusplus
}
#endif
