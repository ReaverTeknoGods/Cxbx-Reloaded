// ******************************************************************
// * RenderTrace.h - Lightweight render-state tracing for blank-screen issues
// ******************************************************************
#pragma once
#ifndef RENDERTRACE_H
#define RENDERTRACE_H

#include <windows.h>

extern bool g_RenderTraceEnabled;

void RenderTrace_Init(const char* logPath);
void RenderTrace_OnSwapBegin();

void RenderTrace_RecordDraw(
    bool indexed,
    bool userPointer,
    DWORD primitiveType,
    UINT primitiveCount,
    HRESULT hResult,
    bool hasGeometryHash,
    unsigned long long geometryHash,
    bool hasPositionBounds,
    float minX,
    float minY,
    float maxX,
    float maxY,
    bool hasTexCoord0Bounds,
    float minU,
    float minV,
    float maxU,
    float maxV,
    bool hasDiffuseColorBounds,
    DWORD minDiffuseR,
    DWORD minDiffuseG,
    DWORD minDiffuseB,
    DWORD minDiffuseA,
    DWORD maxDiffuseR,
    DWORD maxDiffuseG,
    DWORD maxDiffuseB,
    DWORD maxDiffuseA);
void RenderTrace_RecordIndexData(
    UINT indexCount,
    DWORD lowIndex,
    DWORD highIndex,
    UINT restartIndexCount,
    INT baseVertexIndex);
void RenderTrace_RecordClear(DWORD hostFlags, DWORD color, HRESULT hResult);
void RenderTrace_RecordNullViewport();
void RenderTrace_RecordUnreadableRenderTarget();
void RenderTrace_RecordHostRenderTargetDimensionFailure();
void RenderTrace_RecordViewportState(
    bool fixedFunction,
    DWORD xboxX,
    DWORD xboxY,
    DWORD xboxWidth,
    DWORD xboxHeight,
    DWORD hostRenderTargetWidth,
    DWORD hostRenderTargetHeight,
    DWORD hostViewportX,
    DWORD hostViewportY,
    DWORD hostViewportWidth,
    DWORD hostViewportHeight,
    bool scissorEnabled,
    const RECT* scissorRect);
void RenderTrace_RecordRenderTargetValidation(
    DWORD hostRenderTargetWidth,
    DWORD hostRenderTargetHeight,
    DWORD xboxRenderTargetWidth,
    DWORD xboxRenderTargetHeight,
    bool recreated);
void RenderTrace_RecordRenderState(DWORD state, DWORD value);
void RenderTrace_RecordTextureStageState(DWORD stage, DWORD state, DWORD value);
void RenderTrace_RecordTextureBinding(
    DWORD stage,
    bool hasXboxTexture,
    bool hasXboxData,
    DWORD xboxDataAddress,
    bool hasXboxDataHash,
    unsigned long long xboxDataHash,
    DWORD xboxResourceType,
    DWORD xboxFormat,
    DWORD xboxWidth,
    DWORD xboxHeight,
    bool hasHostTexture,
    bool convertedFromSurface,
    bool aliasesBackBuffer,
    bool aliasesCurrentRenderTarget);
void RenderTrace_RecordTextureProducer(
    const char* eventName,
    DWORD stage,
    const void* xboxTexture,
    DWORD xboxDataAddress,
    const void* callerAddress);
void RenderTrace_RecordTextureHookState(
    DWORD sourceAddress,
    const BYTE* sourceBytes,
    size_t sourceByteCount,
    const void* trampolineAddress,
    const BYTE* trampolineBytes,
    size_t trampolineByteCount,
    const void* activeTexture);
void RenderTrace_RecordTextureUpload(
    DWORD xboxResourceType,
    DWORD xboxFormat,
    DWORD hostFormat,
    DWORD width,
    DWORD height,
    DWORD mipLevels,
    HRESULT result);
void RenderTrace_RecordFileOpen(
    const char* path,
    size_t pathLength,
    LONG result);
void RenderTrace_RecordVertexShader(DWORD handle);
void RenderTrace_RecordVertexDeclaration(
    unsigned long long key,
    bool fixedFunction,
    UINT streamOrdinal,
    UINT xboxStreamIndex,
    bool needsPatch,
    UINT hostStride,
    UINT elementCount);
void RenderTrace_RecordVertexDeclarationElement(
    unsigned long long key,
    UINT streamOrdinal,
    UINT elementOrdinal,
    UINT xboxRegister,
    UINT xboxOffset,
    UINT xboxType,
    UINT xboxSize,
    UINT hostOffset,
    UINT hostType,
    UINT hostSize);
void RenderTrace_RecordFixedFunctionVertexBlend(DWORD vertexBlend);
void RenderTrace_RecordFixedFunctionBlendMatrices(
    bool fromNv2a,
    DWORD vertexBlend,
    UINT matrixCount,
    const float* transposedWorldViewMatrices);
void RenderTrace_RecordFixedFunctionState(
    const void* cpuState,
    size_t cpuStateSize,
    const float* hostConstants,
    UINT hostFloatCount,
    HRESULT getHostConstantsResult,
    HRESULT transformUploadResult,
    HRESULT restUploadResult,
    unsigned long long uploadSerial,
    bool transformWasDirty,
    bool nonTransformWasDirty,
    bool gpuWasInvalid);
void RenderTrace_RecordVertexConstantState(
    DWORD vertexShaderMode,
    const float* hostConstants,
    UINT hostRegisterCount,
    HRESULT getHostConstantsResult);
void RenderTrace_RecordPackedByte4(
    UINT xboxRegister,
    unsigned long long vertexDataHash,
    UINT vertexCount,
    UINT vertexStride,
    UINT elementOffset,
    const BYTE* vertexData);
void RenderTrace_RecordVertexShaderDetails(
    bool shaderProgram,
    unsigned long long shaderKey,
    unsigned long long shaderCacheHash,
    bool usesIndexedBoneConstants,
    unsigned vertexShaderInputOverrideCount,
    UINT vertexShaderInputStride,
    UINT vertexShaderInputOffset);
void RenderTrace_RecordPixelShader(DWORD handle);
void RenderTrace_RecordPixelShaderKey(bool hasKey, unsigned long long key);
void RenderTrace_RecordVisibilityBegin(bool hostQueryEnabled);
void RenderTrace_RecordVisibilityEnd(DWORD index, bool hostQueryEnabled, bool missingBegin, HRESULT hResult);
void RenderTrace_RecordVisibilityResult(DWORD index, bool hostQueryEnabled, bool fallbackPath, DWORD result, HRESULT hResult);
void RenderTrace_RecordSwap(
    DWORD flags,
    bool returnedEarly,
    bool hadHostBackBuffer,
    bool hadXboxBackBuffer,
    const RECT* destRect,
    HRESULT getBackBufferResult,
    HRESULT blitResult,
    bool hadOverlay);

#endif // RENDERTRACE_H
