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
void RenderTrace_RecordVertexShader(DWORD handle);
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