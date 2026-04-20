// ******************************************************************
// * RenderTrace.cpp - Render-state tracing for blank-screen investigations
// ******************************************************************
#include "RenderTrace.h"

#include <d3d9.h>
#include <cstdio>
#include <cstring>

bool g_RenderTraceEnabled = false;

namespace {

constexpr DWORD kUnknownStateValue = 0xFFFFFFFFu;
constexpr size_t kMaxDrawSignatureBuckets = 32;

struct TrackedRenderState {
    DWORD colorWriteEnable;
    DWORD alphaBlendEnable;
    DWORD srcBlend;
    DWORD destBlend;
    DWORD blendOp;
    DWORD alphaTestEnable;
    DWORD alphaFunc;
    DWORD alphaRef;
    DWORD zEnable;
    DWORD zWriteEnable;
    DWORD zFunc;
    DWORD cullMode;
    DWORD stencilEnable;
    DWORD stencilFunc;
    DWORD stencilRef;
    DWORD stencilFail;
    DWORD stencilZFail;
    DWORD stencilPass;
    DWORD textureFactor;
};

struct TrackedTextureStageState {
    DWORD colorOp;
    DWORD colorArg1;
    DWORD colorArg2;
    DWORD alphaOp;
    DWORD alphaArg1;
    DWORD alphaArg2;
    DWORD textureTransformFlags;
    DWORD texCoordIndex;
};

struct TrackedTextureBinding {
    bool known;
    bool hasXboxTexture;
    bool hasXboxData;
    DWORD xboxDataAddress;
    bool hasXboxDataHash;
    unsigned long long xboxDataHash;
    DWORD xboxResourceType;
    DWORD xboxFormat;
    DWORD xboxWidth;
    DWORD xboxHeight;
    bool hasHostTexture;
    bool convertedFromSurface;
    bool aliasesBackBuffer;
    bool aliasesCurrentRenderTarget;
};

struct TrackedDrawSignature {
    bool known;
    bool indexed;
    bool userPointer;
    DWORD primitiveType;
    UINT primitiveCount;
    bool hasGeometryHash;
    unsigned long long geometryHash;
    DWORD vertexShaderHandle;
    DWORD pixelShaderHandle;
    bool hasPixelShaderKey;
    unsigned long long pixelShaderKey;
    DWORD alphaBlendEnable;
    DWORD srcBlend;
    DWORD destBlend;
    DWORD alphaTestEnable;
    DWORD stage0ColorOp;
    DWORD stage0AlphaOp;
    DWORD stage0TextureTransformFlags;
    DWORD stage0TexCoordIndex;
    bool stage0HasTextureHash;
    unsigned long long stage0TextureHash;
    DWORD stage0TextureDataAddress;
    DWORD stage0TextureFormat;
    DWORD stage0TextureWidth;
    DWORD stage0TextureHeight;
    bool stage0HasHostTexture;
    bool hasPositionBounds;
    float minX;
    float minY;
    float maxX;
    float maxY;
    bool hasTexCoord0Bounds;
    float minU;
    float minV;
    float maxU;
    float maxV;
    bool hasDiffuseColorBounds;
    DWORD minDiffuseR;
    DWORD minDiffuseG;
    DWORD minDiffuseB;
    DWORD minDiffuseA;
    DWORD maxDiffuseR;
    DWORD maxDiffuseG;
    DWORD maxDiffuseB;
    DWORD maxDiffuseA;
};

struct DrawSignatureBucket {
    bool known;
    unsigned count;
    TrackedDrawSignature signature;
};

struct StickyState {
    DWORD lastXboxViewportX;
    DWORD lastXboxViewportY;
    DWORD lastXboxViewportWidth;
    DWORD lastXboxViewportHeight;
    DWORD lastHostRenderTargetWidth;
    DWORD lastHostRenderTargetHeight;
    DWORD lastHostViewportX;
    DWORD lastHostViewportY;
    DWORD lastHostViewportWidth;
    DWORD lastHostViewportHeight;
    bool lastScissorEnabled;
    RECT lastScissorRect;

    DWORD lastValidatedHostRenderTargetWidth;
    DWORD lastValidatedHostRenderTargetHeight;
    DWORD lastValidatedXboxRenderTargetWidth;
    DWORD lastValidatedXboxRenderTargetHeight;

    TrackedRenderState renderState;
    TrackedTextureStageState textureStages[2];
    TrackedTextureBinding textureBindings[2];
    DWORD lastVertexShaderHandle;
    DWORD lastPixelShaderHandle;
    bool hasPixelShaderKey;
    unsigned long long pixelShaderKey;
};

struct FrameState {
    unsigned long long frameNumber;
    unsigned drawCalls;
    unsigned indexedDrawCalls;
    unsigned userPointerDrawCalls;
    unsigned failedDrawCalls;
    DWORD lastPrimitiveType;
    UINT lastPrimitiveCount;
    HRESULT lastDrawResult;
    DrawSignatureBucket drawSignatureBuckets[kMaxDrawSignatureBuckets];
    unsigned drawSignatureOverflowCount;

    unsigned clearCalls;
    DWORD lastClearFlags;
    DWORD lastClearColor;
    HRESULT lastClearResult;

    unsigned nullViewportCalls;
    unsigned unreadableRenderTargetCalls;
    unsigned hostRenderTargetDimensionFailures;
    unsigned viewportUpdates;
    unsigned fixedFunctionViewportUpdates;
    unsigned zeroHostViewportCount;
    unsigned zeroScissorCount;
    DWORD lastXboxViewportX;
    DWORD lastXboxViewportY;
    DWORD lastXboxViewportWidth;
    DWORD lastXboxViewportHeight;
    DWORD lastHostRenderTargetWidth;
    DWORD lastHostRenderTargetHeight;
    DWORD lastHostViewportX;
    DWORD lastHostViewportY;
    DWORD lastHostViewportWidth;
    DWORD lastHostViewportHeight;
    bool lastScissorEnabled;
    RECT lastScissorRect;

    unsigned renderTargetValidationCalls;
    unsigned renderTargetRecreateCalls;
    DWORD lastValidatedHostRenderTargetWidth;
    DWORD lastValidatedHostRenderTargetHeight;
    DWORD lastValidatedXboxRenderTargetWidth;
    DWORD lastValidatedXboxRenderTargetHeight;

    TrackedRenderState renderState;
    TrackedTextureStageState textureStages[2];
    TrackedTextureBinding textureBindings[2];
    DWORD lastVertexShaderHandle;
    DWORD lastPixelShaderHandle;
    bool hasPixelShaderKey;
    unsigned long long pixelShaderKey;

    unsigned visibilityBeginCalls;
    unsigned visibilityBeginDisabledCalls;
    unsigned visibilityEndCalls;
    unsigned visibilityEndDisabledCalls;
    unsigned visibilityMissingBeginCalls;
    unsigned visibilityGetCalls;
    unsigned visibilityFallbackCalls;
    unsigned visibilityZeroResults;
    unsigned visibilityNonZeroResults;
    DWORD lastVisibilityIndex;
    DWORD maxVisibilityResult;
    HRESULT lastVisibilityResultHr;

    bool swapSeen;
    bool swapReturnedEarly;
    DWORD lastSwapFlags;
    bool hadHostBackBuffer;
    bool hadXboxBackBuffer;
    bool hadOverlay;
    bool hasDestRect;
    RECT lastSwapDestRect;
    HRESULT lastGetBackBufferResult;
    HRESULT lastBlitResult;
};

struct State {
    FILE* logFile;
    bool initialized;
    bool haveCurrentFrame;
    unsigned long long nextFrameNumber;
    StickyState sticky;
    FrameState current;
};

State g_state = {};

void ResetTrackedRenderState(TrackedRenderState* state)
{
    state->colorWriteEnable = kUnknownStateValue;
    state->alphaBlendEnable = kUnknownStateValue;
    state->srcBlend = kUnknownStateValue;
    state->destBlend = kUnknownStateValue;
    state->blendOp = kUnknownStateValue;
    state->alphaTestEnable = kUnknownStateValue;
    state->alphaFunc = kUnknownStateValue;
    state->alphaRef = kUnknownStateValue;
    state->zEnable = kUnknownStateValue;
    state->zWriteEnable = kUnknownStateValue;
    state->zFunc = kUnknownStateValue;
    state->cullMode = kUnknownStateValue;
    state->stencilEnable = kUnknownStateValue;
    state->stencilFunc = kUnknownStateValue;
    state->stencilRef = kUnknownStateValue;
    state->stencilFail = kUnknownStateValue;
    state->stencilZFail = kUnknownStateValue;
    state->stencilPass = kUnknownStateValue;
    state->textureFactor = kUnknownStateValue;
}

void ResetTrackedTextureStageState(TrackedTextureStageState* state)
{
    state->colorOp = kUnknownStateValue;
    state->colorArg1 = kUnknownStateValue;
    state->colorArg2 = kUnknownStateValue;
    state->alphaOp = kUnknownStateValue;
    state->alphaArg1 = kUnknownStateValue;
    state->alphaArg2 = kUnknownStateValue;
    state->textureTransformFlags = kUnknownStateValue;
    state->texCoordIndex = kUnknownStateValue;
}

void ResetTrackedTextureBinding(TrackedTextureBinding* binding)
{
    memset(binding, 0, sizeof(*binding));
}

void ResetStickyState(StickyState* sticky)
{
    memset(sticky, 0, sizeof(*sticky));
    ResetTrackedRenderState(&sticky->renderState);
    for (size_t stage = 0; stage < 2; ++stage) {
        ResetTrackedTextureStageState(&sticky->textureStages[stage]);
        ResetTrackedTextureBinding(&sticky->textureBindings[stage]);
    }
    sticky->lastVertexShaderHandle = kUnknownStateValue;
    sticky->lastPixelShaderHandle = kUnknownStateValue;
    sticky->hasPixelShaderKey = false;
    sticky->pixelShaderKey = 0;
}

void CopyStickyStateToFrame(FrameState* frame)
{
    frame->lastXboxViewportX = g_state.sticky.lastXboxViewportX;
    frame->lastXboxViewportY = g_state.sticky.lastXboxViewportY;
    frame->lastXboxViewportWidth = g_state.sticky.lastXboxViewportWidth;
    frame->lastXboxViewportHeight = g_state.sticky.lastXboxViewportHeight;
    frame->lastHostRenderTargetWidth = g_state.sticky.lastHostRenderTargetWidth;
    frame->lastHostRenderTargetHeight = g_state.sticky.lastHostRenderTargetHeight;
    frame->lastHostViewportX = g_state.sticky.lastHostViewportX;
    frame->lastHostViewportY = g_state.sticky.lastHostViewportY;
    frame->lastHostViewportWidth = g_state.sticky.lastHostViewportWidth;
    frame->lastHostViewportHeight = g_state.sticky.lastHostViewportHeight;
    frame->lastScissorEnabled = g_state.sticky.lastScissorEnabled;
    frame->lastScissorRect = g_state.sticky.lastScissorRect;

    frame->lastValidatedHostRenderTargetWidth = g_state.sticky.lastValidatedHostRenderTargetWidth;
    frame->lastValidatedHostRenderTargetHeight = g_state.sticky.lastValidatedHostRenderTargetHeight;
    frame->lastValidatedXboxRenderTargetWidth = g_state.sticky.lastValidatedXboxRenderTargetWidth;
    frame->lastValidatedXboxRenderTargetHeight = g_state.sticky.lastValidatedXboxRenderTargetHeight;

    frame->renderState = g_state.sticky.renderState;
    for (size_t stage = 0; stage < 2; ++stage) {
        frame->textureStages[stage] = g_state.sticky.textureStages[stage];
        frame->textureBindings[stage] = g_state.sticky.textureBindings[stage];
    }
    frame->lastVertexShaderHandle = g_state.sticky.lastVertexShaderHandle;
    frame->lastPixelShaderHandle = g_state.sticky.lastPixelShaderHandle;
    frame->hasPixelShaderKey = g_state.sticky.hasPixelShaderKey;
    frame->pixelShaderKey = g_state.sticky.pixelShaderKey;
}

bool IsKnownState(DWORD value)
{
    return value != kUnknownStateValue;
}

void UpdateTrackedRenderState(TrackedRenderState* state, DWORD renderState, DWORD value)
{
    switch (renderState) {
    case D3DRS_COLORWRITEENABLE:
        state->colorWriteEnable = value;
        break;
    case D3DRS_ALPHABLENDENABLE:
        state->alphaBlendEnable = value;
        break;
    case D3DRS_SRCBLEND:
        state->srcBlend = value;
        break;
    case D3DRS_DESTBLEND:
        state->destBlend = value;
        break;
    case D3DRS_BLENDOP:
        state->blendOp = value;
        break;
    case D3DRS_ALPHATESTENABLE:
        state->alphaTestEnable = value;
        break;
    case D3DRS_ALPHAFUNC:
        state->alphaFunc = value;
        break;
    case D3DRS_ALPHAREF:
        state->alphaRef = value;
        break;
    case D3DRS_ZENABLE:
        state->zEnable = value;
        break;
    case D3DRS_ZWRITEENABLE:
        state->zWriteEnable = value;
        break;
    case D3DRS_ZFUNC:
        state->zFunc = value;
        break;
    case D3DRS_CULLMODE:
        state->cullMode = value;
        break;
    case D3DRS_STENCILENABLE:
        state->stencilEnable = value;
        break;
    case D3DRS_STENCILFUNC:
        state->stencilFunc = value;
        break;
    case D3DRS_STENCILREF:
        state->stencilRef = value;
        break;
    case D3DRS_STENCILFAIL:
        state->stencilFail = value;
        break;
    case D3DRS_STENCILZFAIL:
        state->stencilZFail = value;
        break;
    case D3DRS_STENCILPASS:
        state->stencilPass = value;
        break;
    case D3DRS_TEXTUREFACTOR:
        state->textureFactor = value;
        break;
    default:
        break;
    }
}

void UpdateTrackedTextureStageState(TrackedTextureStageState* state, DWORD textureStageState, DWORD value)
{
    switch (textureStageState) {
    case D3DTSS_COLOROP:
        state->colorOp = value;
        break;
    case D3DTSS_COLORARG1:
        state->colorArg1 = value;
        break;
    case D3DTSS_COLORARG2:
        state->colorArg2 = value;
        break;
    case D3DTSS_ALPHAOP:
        state->alphaOp = value;
        break;
    case D3DTSS_ALPHAARG1:
        state->alphaArg1 = value;
        break;
    case D3DTSS_ALPHAARG2:
        state->alphaArg2 = value;
        break;
    case D3DTSS_TEXTURETRANSFORMFLAGS:
        state->textureTransformFlags = value;
        break;
    case D3DTSS_TEXCOORDINDEX:
        state->texCoordIndex = value;
        break;
    default:
        break;
    }
}

const char* KnownBoolName(bool known, bool value)
{
    if (!known) {
        return "?";
    }
    return value ? "1" : "0";
}

const char* TextureResourceTypeName(const TrackedTextureBinding& binding)
{
    if (!binding.known) {
        return "?";
    }
    if (!binding.hasXboxTexture) {
        return "none";
    }

    switch (binding.xboxResourceType) {
    case 1:
        return "tex";
    case 2:
        return "surf";
    case 3:
        return "other";
    default:
        return "unk";
    }
}

bool TextureArgUsesTexture(DWORD value)
{
    return IsKnownState(value) && (value & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
}

bool TextureStageUsesTexture(const TrackedTextureStageState& state)
{
    if ((IsKnownState(state.colorOp) && state.colorOp != D3DTOP_DISABLE &&
            (TextureArgUsesTexture(state.colorArg1) || TextureArgUsesTexture(state.colorArg2))) ||
        (IsKnownState(state.alphaOp) && state.alphaOp != D3DTOP_DISABLE &&
            (TextureArgUsesTexture(state.alphaArg1) || TextureArgUsesTexture(state.alphaArg2)))) {
        return true;
    }

    return false;
}

const char* PrimitiveTypeName(DWORD primitiveType);

void AppendText(char* buffer, size_t bufferSize, const char* text)
{
    strncat(buffer, text, bufferSize - strlen(buffer) - 1);
}

TrackedDrawSignature CaptureDrawSignature(
    const FrameState& frame,
    bool indexed,
    bool userPointer,
    DWORD primitiveType,
    UINT primitiveCount,
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
    DWORD maxDiffuseA)
{
    TrackedDrawSignature signature = {};
    signature.known = true;
    signature.indexed = indexed;
    signature.userPointer = userPointer;
    signature.primitiveType = primitiveType;
    signature.primitiveCount = primitiveCount;
    signature.hasGeometryHash = hasGeometryHash;
    signature.geometryHash = geometryHash;
    signature.vertexShaderHandle = frame.lastVertexShaderHandle;
    signature.pixelShaderHandle = frame.lastPixelShaderHandle;
    signature.hasPixelShaderKey = frame.hasPixelShaderKey;
    signature.pixelShaderKey = frame.pixelShaderKey;
    signature.alphaBlendEnable = frame.renderState.alphaBlendEnable;
    signature.srcBlend = frame.renderState.srcBlend;
    signature.destBlend = frame.renderState.destBlend;
    signature.alphaTestEnable = frame.renderState.alphaTestEnable;
    signature.stage0ColorOp = frame.textureStages[0].colorOp;
    signature.stage0AlphaOp = frame.textureStages[0].alphaOp;
    signature.stage0TextureTransformFlags = frame.textureStages[0].textureTransformFlags;
    signature.stage0TexCoordIndex = frame.textureStages[0].texCoordIndex;
    signature.stage0HasTextureHash = frame.textureBindings[0].hasXboxDataHash;
    signature.stage0TextureHash = frame.textureBindings[0].xboxDataHash;
    signature.stage0TextureDataAddress = frame.textureBindings[0].xboxDataAddress;
    signature.stage0TextureFormat = frame.textureBindings[0].xboxFormat;
    signature.stage0TextureWidth = frame.textureBindings[0].xboxWidth;
    signature.stage0TextureHeight = frame.textureBindings[0].xboxHeight;
    signature.stage0HasHostTexture = frame.textureBindings[0].hasHostTexture;
    signature.hasPositionBounds = hasPositionBounds;
    signature.minX = minX;
    signature.minY = minY;
    signature.maxX = maxX;
    signature.maxY = maxY;
    signature.hasTexCoord0Bounds = hasTexCoord0Bounds;
    signature.minU = minU;
    signature.minV = minV;
    signature.maxU = maxU;
    signature.maxV = maxV;
    signature.hasDiffuseColorBounds = hasDiffuseColorBounds;
    signature.minDiffuseR = minDiffuseR;
    signature.minDiffuseG = minDiffuseG;
    signature.minDiffuseB = minDiffuseB;
    signature.minDiffuseA = minDiffuseA;
    signature.maxDiffuseR = maxDiffuseR;
    signature.maxDiffuseG = maxDiffuseG;
    signature.maxDiffuseB = maxDiffuseB;
    signature.maxDiffuseA = maxDiffuseA;
    return signature;
}

bool DrawSignaturesMatch(const TrackedDrawSignature& left, const TrackedDrawSignature& right)
{
    return left.known == right.known &&
        left.indexed == right.indexed &&
        left.userPointer == right.userPointer &&
        left.primitiveType == right.primitiveType &&
        left.primitiveCount == right.primitiveCount &&
        left.hasGeometryHash == right.hasGeometryHash &&
        left.geometryHash == right.geometryHash &&
        left.vertexShaderHandle == right.vertexShaderHandle &&
        left.pixelShaderHandle == right.pixelShaderHandle &&
        left.hasPixelShaderKey == right.hasPixelShaderKey &&
        left.pixelShaderKey == right.pixelShaderKey &&
        left.alphaBlendEnable == right.alphaBlendEnable &&
        left.srcBlend == right.srcBlend &&
        left.destBlend == right.destBlend &&
        left.alphaTestEnable == right.alphaTestEnable &&
        left.stage0ColorOp == right.stage0ColorOp &&
        left.stage0AlphaOp == right.stage0AlphaOp &&
        left.stage0TextureTransformFlags == right.stage0TextureTransformFlags &&
        left.stage0TexCoordIndex == right.stage0TexCoordIndex &&
        left.stage0HasTextureHash == right.stage0HasTextureHash &&
        left.stage0TextureHash == right.stage0TextureHash &&
        left.stage0TextureDataAddress == right.stage0TextureDataAddress &&
        left.stage0TextureFormat == right.stage0TextureFormat &&
        left.stage0TextureWidth == right.stage0TextureWidth &&
        left.stage0TextureHeight == right.stage0TextureHeight &&
        left.stage0HasHostTexture == right.stage0HasHostTexture &&
        left.hasPositionBounds == right.hasPositionBounds &&
        left.minX == right.minX &&
        left.minY == right.minY &&
        left.maxX == right.maxX &&
        left.maxY == right.maxY &&
        left.hasTexCoord0Bounds == right.hasTexCoord0Bounds &&
        left.minU == right.minU &&
        left.minV == right.minV &&
        left.maxU == right.maxU &&
        left.maxV == right.maxV &&
        left.hasDiffuseColorBounds == right.hasDiffuseColorBounds &&
        left.minDiffuseR == right.minDiffuseR &&
        left.minDiffuseG == right.minDiffuseG &&
        left.minDiffuseB == right.minDiffuseB &&
        left.minDiffuseA == right.minDiffuseA &&
        left.maxDiffuseR == right.maxDiffuseR &&
        left.maxDiffuseG == right.maxDiffuseG &&
        left.maxDiffuseB == right.maxDiffuseB &&
        left.maxDiffuseA == right.maxDiffuseA;
}

void RecordDrawSignature(
    FrameState* frame,
    bool indexed,
    bool userPointer,
    DWORD primitiveType,
    UINT primitiveCount,
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
    DWORD maxDiffuseA)
{
    const TrackedDrawSignature signature = CaptureDrawSignature(
        *frame,
        indexed,
        userPointer,
        primitiveType,
        primitiveCount,
        hasGeometryHash,
        geometryHash,
        hasPositionBounds,
        minX,
        minY,
        maxX,
        maxY,
        hasTexCoord0Bounds,
        minU,
        minV,
        maxU,
        maxV,
        hasDiffuseColorBounds,
        minDiffuseR,
        minDiffuseG,
        minDiffuseB,
        minDiffuseA,
        maxDiffuseR,
        maxDiffuseG,
        maxDiffuseB,
        maxDiffuseA);

    for (size_t bucketIndex = 0; bucketIndex < kMaxDrawSignatureBuckets; ++bucketIndex) {
        DrawSignatureBucket& bucket = frame->drawSignatureBuckets[bucketIndex];
        if (bucket.known && DrawSignaturesMatch(bucket.signature, signature)) {
            bucket.count++;
            return;
        }
    }

    for (size_t bucketIndex = 0; bucketIndex < kMaxDrawSignatureBuckets; ++bucketIndex) {
        DrawSignatureBucket& bucket = frame->drawSignatureBuckets[bucketIndex];
        if (!bucket.known) {
            bucket.known = true;
            bucket.count = 1;
            bucket.signature = signature;
            return;
        }
    }

    frame->drawSignatureOverflowCount++;
}

void FormatDrawGroups(const FrameState& frame, char* buffer, size_t bufferSize)
{
    if (bufferSize == 0) {
        return;
    }

    strcpy_s(buffer, bufferSize, "[");
    bool haveEntry = false;
    for (size_t bucketIndex = 0; bucketIndex < kMaxDrawSignatureBuckets; ++bucketIndex) {
        const DrawSignatureBucket& bucket = frame.drawSignatureBuckets[bucketIndex];
        if (!bucket.known) {
            continue;
        }

        char fragment[896] = {};
        sprintf_s(
            fragment,
            "%ux(i=%u up=%u pt=%s/%u gh=%u/%016llX xy=%u/[%.2f,%.2f %.2f,%.2f] uv0=%u/[%.2f,%.2f %.2f,%.2f] diff=%u/[r%lu-%lu g%lu-%lu b%lu-%lu a%lu-%lu] vs=%08lX ps=%08lX psk=%s/%016llX ab=%08lX sb=%08lX db=%08lX at=%08lX co=%08lX ao=%08lX ttf=%08lX tci=%08lX xh=%s/%016llX xd=%08lX fmt=%08lX %lux%lu host=%u)",
            bucket.count,
            bucket.signature.indexed ? 1 : 0,
            bucket.signature.userPointer ? 1 : 0,
            PrimitiveTypeName(bucket.signature.primitiveType),
            bucket.signature.primitiveCount,
            bucket.signature.hasGeometryHash ? 1 : 0,
            bucket.signature.geometryHash,
            bucket.signature.hasPositionBounds ? 1 : 0,
            bucket.signature.minX,
            bucket.signature.minY,
            bucket.signature.maxX,
            bucket.signature.maxY,
            bucket.signature.hasTexCoord0Bounds ? 1 : 0,
            bucket.signature.minU,
            bucket.signature.minV,
            bucket.signature.maxU,
            bucket.signature.maxV,
            bucket.signature.hasDiffuseColorBounds ? 1 : 0,
            (unsigned long)bucket.signature.minDiffuseR,
            (unsigned long)bucket.signature.maxDiffuseR,
            (unsigned long)bucket.signature.minDiffuseG,
            (unsigned long)bucket.signature.maxDiffuseG,
            (unsigned long)bucket.signature.minDiffuseB,
            (unsigned long)bucket.signature.maxDiffuseB,
            (unsigned long)bucket.signature.minDiffuseA,
            (unsigned long)bucket.signature.maxDiffuseA,
            (unsigned long)bucket.signature.vertexShaderHandle,
            (unsigned long)bucket.signature.pixelShaderHandle,
            bucket.signature.hasPixelShaderKey ? "1" : "0",
            bucket.signature.pixelShaderKey,
            (unsigned long)bucket.signature.alphaBlendEnable,
            (unsigned long)bucket.signature.srcBlend,
            (unsigned long)bucket.signature.destBlend,
            (unsigned long)bucket.signature.alphaTestEnable,
            (unsigned long)bucket.signature.stage0ColorOp,
            (unsigned long)bucket.signature.stage0AlphaOp,
            (unsigned long)bucket.signature.stage0TextureTransformFlags,
            (unsigned long)bucket.signature.stage0TexCoordIndex,
            bucket.signature.stage0HasTextureHash ? "1" : "0",
            bucket.signature.stage0TextureHash,
            (unsigned long)bucket.signature.stage0TextureDataAddress,
            (unsigned long)bucket.signature.stage0TextureFormat,
            (unsigned long)bucket.signature.stage0TextureWidth,
            (unsigned long)bucket.signature.stage0TextureHeight,
            bucket.signature.stage0HasHostTexture ? 1 : 0);

        if (haveEntry) {
            AppendText(buffer, bufferSize, ";");
        }
        AppendText(buffer, bufferSize, fragment);
        haveEntry = true;
    }

    if (frame.drawSignatureOverflowCount != 0) {
        char fragment[64] = {};
        sprintf_s(fragment, "+%uoverflow", frame.drawSignatureOverflowCount);
        if (haveEntry) {
            AppendText(buffer, bufferSize, ";");
        }
        AppendText(buffer, bufferSize, fragment);
        haveEntry = true;
    }

    if (!haveEntry) {
        AppendText(buffer, bufferSize, "none");
    }

    AppendText(buffer, bufferSize, "]");
}

void AppendNote(char* buffer, size_t bufferSize, const char* note)
{
    if (buffer[0] != '\0') {
        strncat(buffer, ",", bufferSize - strlen(buffer) - 1);
    }
    strncat(buffer, note, bufferSize - strlen(buffer) - 1);
}

const char* PrimitiveTypeName(DWORD primitiveType)
{
    switch (primitiveType) {
    case 1: return "point";
    case 2: return "line";
    case 3: return "linestrip";
    case 4: return "tri";
    case 5: return "tristrip";
    case 6: return "trifan";
    default: return "other";
    }
}

LONG RectWidth(const RECT& rect)
{
    return rect.right - rect.left;
}

LONG RectHeight(const RECT& rect)
{
    return rect.bottom - rect.top;
}

void PrintFrame(const FrameState& frame)
{
    if (!g_state.logFile) {
        return;
    }

    char notes[256] = {};
    if (frame.drawCalls == 0) {
        AppendNote(notes, sizeof(notes), "no_draws");
    }
    if (frame.failedDrawCalls != 0) {
        AppendNote(notes, sizeof(notes), "draw_failed");
    }
    if (frame.drawSignatureOverflowCount != 0) {
        AppendNote(notes, sizeof(notes), "drawsig_overflow");
    }
    if (frame.zeroHostViewportCount != 0) {
        AppendNote(notes, sizeof(notes), "empty_viewport");
    }
    if (frame.zeroScissorCount != 0) {
        AppendNote(notes, sizeof(notes), "empty_scissor");
    }
    if (frame.unreadableRenderTargetCalls != 0) {
        AppendNote(notes, sizeof(notes), "rt_unreadable");
    }
    if (frame.hostRenderTargetDimensionFailures != 0) {
        AppendNote(notes, sizeof(notes), "hostrt_missing");
    }
    if (frame.renderTargetRecreateCalls != 0) {
        AppendNote(notes, sizeof(notes), "rt_recreated");
    }
    if (frame.drawCalls != 0 && IsKnownState(frame.renderState.colorWriteEnable) && frame.renderState.colorWriteEnable == 0) {
        AppendNote(notes, sizeof(notes), "color_mask_zero");
    }
    if (frame.drawCalls != 0 &&
        IsKnownState(frame.renderState.zEnable) && frame.renderState.zEnable != 0 &&
        IsKnownState(frame.renderState.zFunc) && frame.renderState.zFunc == D3DCMP_NEVER) {
        AppendNote(notes, sizeof(notes), "zfunc_never");
    }
    if (frame.drawCalls != 0 &&
        IsKnownState(frame.renderState.alphaTestEnable) && frame.renderState.alphaTestEnable != 0 &&
        IsKnownState(frame.renderState.alphaFunc) && frame.renderState.alphaFunc == D3DCMP_NEVER) {
        AppendNote(notes, sizeof(notes), "alphafunc_never");
    }
    if (frame.drawCalls != 0 &&
        IsKnownState(frame.renderState.stencilEnable) && frame.renderState.stencilEnable != 0 &&
        IsKnownState(frame.renderState.stencilFunc) && frame.renderState.stencilFunc == D3DCMP_NEVER) {
        AppendNote(notes, sizeof(notes), "stencilfunc_never");
    }
    if (frame.drawCalls != 0 && frame.lastPixelShaderHandle == 0 &&
        IsKnownState(frame.textureStages[0].colorOp) && frame.textureStages[0].colorOp == D3DTOP_DISABLE) {
        AppendNote(notes, sizeof(notes), "ff_stage0_colorop_disable");
    }
    if (frame.drawCalls != 0 && TextureStageUsesTexture(frame.textureStages[0]) && frame.textureBindings[0].known) {
        if (!frame.textureBindings[0].hasXboxTexture) {
            AppendNote(notes, sizeof(notes), "t0_unbound");
        } else if (!frame.textureBindings[0].hasXboxData) {
            AppendNote(notes, sizeof(notes), "t0_nodata");
        } else if (!frame.textureBindings[0].hasHostTexture) {
            AppendNote(notes, sizeof(notes), "t0_host_null");
        }
    }
    if (frame.drawCalls != 0 && TextureStageUsesTexture(frame.textureStages[1]) && frame.textureBindings[1].known) {
        if (!frame.textureBindings[1].hasXboxTexture) {
            AppendNote(notes, sizeof(notes), "t1_unbound");
        } else if (!frame.textureBindings[1].hasXboxData) {
            AppendNote(notes, sizeof(notes), "t1_nodata");
        } else if (!frame.textureBindings[1].hasHostTexture) {
            AppendNote(notes, sizeof(notes), "t1_host_null");
        }
    }
    if (frame.visibilityGetCalls != 0 && frame.drawCalls != 0 && frame.visibilityNonZeroResults == 0) {
        AppendNote(notes, sizeof(notes), "all_visibility_zero");
    }
    if (frame.visibilityMissingBeginCalls != 0) {
        AppendNote(notes, sizeof(notes), "visibility_missing_begin");
    }
    if (frame.swapSeen && !frame.hadXboxBackBuffer) {
        AppendNote(notes, sizeof(notes), "no_xbox_backbuffer");
    }
    if (frame.swapSeen && FAILED(frame.lastBlitResult)) {
        AppendNote(notes, sizeof(notes), "blit_failed");
    }
    if (frame.swapReturnedEarly) {
        AppendNote(notes, sizeof(notes), "swap_early_return");
    }
    if (notes[0] == '\0') {
        strcpy_s(notes, sizeof(notes), "ok");
    }

    char drawGroups[8192] = {};
    FormatDrawGroups(frame, drawGroups, sizeof(drawGroups));

    fprintf(
        g_state.logFile,
        "F%06llu draw=%u(idx=%u up=%u fail=%u last=%s/%u hr=%08lX) dg=%s clear=%u(lastFlags=%08lX color=%08lX hr=%08lX) vp=%u(ff=%u null=%u unread=%u missrt=%u) xboxvp=[%lu,%lu %lux%lu] hostrt=%lux%lu hostvp=[%lu,%lu %lux%lu] sc=%s[%ld,%ld %ldx%ld] rtval=%u(rec=%u host=%lux%lu xbox=%lux%lu) vis=b%u/db%u e%u/de%u miss=%u g%u fb=%u z%u nz=%u max=%lu last=%lu hr=%08lX rs=[cw=%08lX ab=%08lX sb=%08lX db=%08lX bo=%08lX at=%08lX af=%08lX ar=%08lX ze=%08lX zw=%08lX zf=%08lX cm=%08lX se=%08lX sc=%08lX sf=%08lX sr=%08lX sz=%08lX sp=%08lX tf=%08lX] sh=[vs=%08lX ps=%08lX psk=%s/%016llX] t0=[co=%08lX c1=%08lX c2=%08lX ao=%08lX a1=%08lX a2=%08lX ttf=%08lX tci=%08lX] t1=[co=%08lX c1=%08lX c2=%08lX ao=%08lX a1=%08lX a2=%08lX ttf=%08lX tci=%08lX] tb0=[xb=%s data=%s xdata=%08lX hk=%s xhash=%016llX rt=%s fmt=%08lX %lux%lu host=%s cvt=%s bb=%s crt=%s] tb1=[xb=%s data=%s xdata=%08lX hk=%s xhash=%016llX rt=%s fmt=%08lX %lux%lu host=%s cvt=%s bb=%s crt=%s] swap=%d early=%d flags=%08lX hostbb=%d xboxbb=%d ov=%d getbb=%08lX blit=%08lX dest=[%ld,%ld %ldx%ld] notes=%s\n",
        frame.frameNumber,
        frame.drawCalls,
        frame.indexedDrawCalls,
        frame.userPointerDrawCalls,
        frame.failedDrawCalls,
        PrimitiveTypeName(frame.lastPrimitiveType),
        frame.lastPrimitiveCount,
        (unsigned long)frame.lastDrawResult,
        drawGroups,
        frame.clearCalls,
        (unsigned long)frame.lastClearFlags,
        (unsigned long)frame.lastClearColor,
        (unsigned long)frame.lastClearResult,
        frame.viewportUpdates,
        frame.fixedFunctionViewportUpdates,
        frame.nullViewportCalls,
        frame.unreadableRenderTargetCalls,
        frame.hostRenderTargetDimensionFailures,
        (unsigned long)frame.lastXboxViewportX,
        (unsigned long)frame.lastXboxViewportY,
        (unsigned long)frame.lastXboxViewportWidth,
        (unsigned long)frame.lastXboxViewportHeight,
        (unsigned long)frame.lastHostRenderTargetWidth,
        (unsigned long)frame.lastHostRenderTargetHeight,
        (unsigned long)frame.lastHostViewportX,
        (unsigned long)frame.lastHostViewportY,
        (unsigned long)frame.lastHostViewportWidth,
        (unsigned long)frame.lastHostViewportHeight,
        frame.lastScissorEnabled ? "on" : "off",
        frame.lastScissorRect.left,
        frame.lastScissorRect.top,
        RectWidth(frame.lastScissorRect),
        RectHeight(frame.lastScissorRect),
        frame.renderTargetValidationCalls,
        frame.renderTargetRecreateCalls,
        (unsigned long)frame.lastValidatedHostRenderTargetWidth,
        (unsigned long)frame.lastValidatedHostRenderTargetHeight,
        (unsigned long)frame.lastValidatedXboxRenderTargetWidth,
        (unsigned long)frame.lastValidatedXboxRenderTargetHeight,
        frame.visibilityBeginCalls,
        frame.visibilityBeginDisabledCalls,
        frame.visibilityEndCalls,
        frame.visibilityEndDisabledCalls,
        frame.visibilityMissingBeginCalls,
        frame.visibilityGetCalls,
        frame.visibilityFallbackCalls,
        frame.visibilityZeroResults,
        frame.visibilityNonZeroResults,
        (unsigned long)frame.maxVisibilityResult,
        (unsigned long)frame.lastVisibilityIndex,
        (unsigned long)frame.lastVisibilityResultHr,
        (unsigned long)frame.renderState.colorWriteEnable,
        (unsigned long)frame.renderState.alphaBlendEnable,
        (unsigned long)frame.renderState.srcBlend,
        (unsigned long)frame.renderState.destBlend,
        (unsigned long)frame.renderState.blendOp,
        (unsigned long)frame.renderState.alphaTestEnable,
        (unsigned long)frame.renderState.alphaFunc,
        (unsigned long)frame.renderState.alphaRef,
        (unsigned long)frame.renderState.zEnable,
        (unsigned long)frame.renderState.zWriteEnable,
        (unsigned long)frame.renderState.zFunc,
        (unsigned long)frame.renderState.cullMode,
        (unsigned long)frame.renderState.stencilEnable,
        (unsigned long)frame.renderState.stencilFunc,
        (unsigned long)frame.renderState.stencilFail,
        (unsigned long)frame.renderState.stencilRef,
        (unsigned long)frame.renderState.stencilZFail,
        (unsigned long)frame.renderState.stencilPass,
        (unsigned long)frame.renderState.textureFactor,
        (unsigned long)frame.lastVertexShaderHandle,
        (unsigned long)frame.lastPixelShaderHandle,
        frame.hasPixelShaderKey ? "1" : "0",
        frame.pixelShaderKey,
        (unsigned long)frame.textureStages[0].colorOp,
        (unsigned long)frame.textureStages[0].colorArg1,
        (unsigned long)frame.textureStages[0].colorArg2,
        (unsigned long)frame.textureStages[0].alphaOp,
        (unsigned long)frame.textureStages[0].alphaArg1,
        (unsigned long)frame.textureStages[0].alphaArg2,
        (unsigned long)frame.textureStages[0].textureTransformFlags,
        (unsigned long)frame.textureStages[0].texCoordIndex,
        (unsigned long)frame.textureStages[1].colorOp,
        (unsigned long)frame.textureStages[1].colorArg1,
        (unsigned long)frame.textureStages[1].colorArg2,
        (unsigned long)frame.textureStages[1].alphaOp,
        (unsigned long)frame.textureStages[1].alphaArg1,
        (unsigned long)frame.textureStages[1].alphaArg2,
        (unsigned long)frame.textureStages[1].textureTransformFlags,
        (unsigned long)frame.textureStages[1].texCoordIndex,
        KnownBoolName(frame.textureBindings[0].known, frame.textureBindings[0].hasXboxTexture),
        KnownBoolName(frame.textureBindings[0].known, frame.textureBindings[0].hasXboxData),
        (unsigned long)frame.textureBindings[0].xboxDataAddress,
        KnownBoolName(frame.textureBindings[0].known, frame.textureBindings[0].hasXboxDataHash),
        frame.textureBindings[0].xboxDataHash,
        TextureResourceTypeName(frame.textureBindings[0]),
        (unsigned long)frame.textureBindings[0].xboxFormat,
        (unsigned long)frame.textureBindings[0].xboxWidth,
        (unsigned long)frame.textureBindings[0].xboxHeight,
        KnownBoolName(frame.textureBindings[0].known, frame.textureBindings[0].hasHostTexture),
        KnownBoolName(frame.textureBindings[0].known, frame.textureBindings[0].convertedFromSurface),
        KnownBoolName(frame.textureBindings[0].known, frame.textureBindings[0].aliasesBackBuffer),
        KnownBoolName(frame.textureBindings[0].known, frame.textureBindings[0].aliasesCurrentRenderTarget),
        KnownBoolName(frame.textureBindings[1].known, frame.textureBindings[1].hasXboxTexture),
        KnownBoolName(frame.textureBindings[1].known, frame.textureBindings[1].hasXboxData),
        (unsigned long)frame.textureBindings[1].xboxDataAddress,
        KnownBoolName(frame.textureBindings[1].known, frame.textureBindings[1].hasXboxDataHash),
        frame.textureBindings[1].xboxDataHash,
        TextureResourceTypeName(frame.textureBindings[1]),
        (unsigned long)frame.textureBindings[1].xboxFormat,
        (unsigned long)frame.textureBindings[1].xboxWidth,
        (unsigned long)frame.textureBindings[1].xboxHeight,
        KnownBoolName(frame.textureBindings[1].known, frame.textureBindings[1].hasHostTexture),
        KnownBoolName(frame.textureBindings[1].known, frame.textureBindings[1].convertedFromSurface),
        KnownBoolName(frame.textureBindings[1].known, frame.textureBindings[1].aliasesBackBuffer),
        KnownBoolName(frame.textureBindings[1].known, frame.textureBindings[1].aliasesCurrentRenderTarget),
        frame.swapSeen ? 1 : 0,
        frame.swapReturnedEarly ? 1 : 0,
        (unsigned long)frame.lastSwapFlags,
        frame.hadHostBackBuffer ? 1 : 0,
        frame.hadXboxBackBuffer ? 1 : 0,
        frame.hadOverlay ? 1 : 0,
        (unsigned long)frame.lastGetBackBufferResult,
        (unsigned long)frame.lastBlitResult,
        frame.hasDestRect ? frame.lastSwapDestRect.left : 0,
        frame.hasDestRect ? frame.lastSwapDestRect.top : 0,
        frame.hasDestRect ? RectWidth(frame.lastSwapDestRect) : 0,
        frame.hasDestRect ? RectHeight(frame.lastSwapDestRect) : 0,
        notes);
    fflush(g_state.logFile);
}

FrameState* CurrentFrame()
{
    if (!g_RenderTraceEnabled || !g_state.logFile || !g_state.haveCurrentFrame) {
        return nullptr;
    }
    return &g_state.current;
}

} // namespace

void RenderTrace_Init(const char* logPath)
{
    if (!g_RenderTraceEnabled || g_state.initialized) {
        return;
    }

    memset(&g_state, 0, sizeof(g_state));
    ResetStickyState(&g_state.sticky);
    g_state.logFile = fopen(logPath, "w");
    if (g_state.logFile) {
        fprintf(g_state.logFile,
            "# Cxbx-Reloaded RenderTrace\n"
            "# One line per swap call. Use it to compare blank test-menu frames against known-good frames.\n"
            "# fields: draw/draw-groups/clear/viewport/scissor/visibility/render-target-validation/render-state/shaders/texture-stage/texture-binding/swap\n\n");
        fflush(g_state.logFile);
        fprintf(stdout, "[RenderTrace] logging to %s\n", logPath);
    } else {
        fprintf(stderr, "[RenderTrace] ERROR: cannot open %s\n", logPath);
    }

    g_state.initialized = true;
}

void RenderTrace_OnSwapBegin()
{
    if (!g_RenderTraceEnabled || !g_state.logFile) {
        return;
    }

    if (g_state.haveCurrentFrame) {
        PrintFrame(g_state.current);
    }

    memset(&g_state.current, 0, sizeof(g_state.current));
    g_state.current.frameNumber = g_state.nextFrameNumber++;
    CopyStickyStateToFrame(&g_state.current);
    g_state.haveCurrentFrame = true;
}

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
    DWORD maxDiffuseA)
{
    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    frame->drawCalls++;
    if (indexed) {
        frame->indexedDrawCalls++;
    }
    if (userPointer) {
        frame->userPointerDrawCalls++;
    }
    if (FAILED(hResult)) {
        frame->failedDrawCalls++;
    }
    frame->lastPrimitiveType = primitiveType;
    frame->lastPrimitiveCount = primitiveCount;
    frame->lastDrawResult = hResult;
    RecordDrawSignature(
        frame,
        indexed,
        userPointer,
        primitiveType,
        primitiveCount,
        hasGeometryHash,
        geometryHash,
        hasPositionBounds,
        minX,
        minY,
        maxX,
        maxY,
        hasTexCoord0Bounds,
        minU,
        minV,
        maxU,
        maxV,
        hasDiffuseColorBounds,
        minDiffuseR,
        minDiffuseG,
        minDiffuseB,
        minDiffuseA,
        maxDiffuseR,
        maxDiffuseG,
        maxDiffuseB,
        maxDiffuseA);
}

void RenderTrace_RecordClear(DWORD hostFlags, DWORD color, HRESULT hResult)
{
    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    frame->clearCalls++;
    frame->lastClearFlags = hostFlags;
    frame->lastClearColor = color;
    frame->lastClearResult = hResult;
}

void RenderTrace_RecordNullViewport()
{
    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->nullViewportCalls++;
    }
}

void RenderTrace_RecordUnreadableRenderTarget()
{
    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->unreadableRenderTargetCalls++;
    }
}

void RenderTrace_RecordHostRenderTargetDimensionFailure()
{
    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->hostRenderTargetDimensionFailures++;
    }
}

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
    const RECT* scissorRect)
{
    if (!g_RenderTraceEnabled || !g_state.initialized) {
        return;
    }

    g_state.sticky.lastXboxViewportX = xboxX;
    g_state.sticky.lastXboxViewportY = xboxY;
    g_state.sticky.lastXboxViewportWidth = xboxWidth;
    g_state.sticky.lastXboxViewportHeight = xboxHeight;
    g_state.sticky.lastHostRenderTargetWidth = hostRenderTargetWidth;
    g_state.sticky.lastHostRenderTargetHeight = hostRenderTargetHeight;
    g_state.sticky.lastHostViewportX = hostViewportX;
    g_state.sticky.lastHostViewportY = hostViewportY;
    g_state.sticky.lastHostViewportWidth = hostViewportWidth;
    g_state.sticky.lastHostViewportHeight = hostViewportHeight;
    g_state.sticky.lastScissorEnabled = scissorEnabled;
    if (scissorRect) {
        g_state.sticky.lastScissorRect = *scissorRect;
    } else {
        SetRectEmpty(&g_state.sticky.lastScissorRect);
    }

    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    frame->viewportUpdates++;
    if (fixedFunction) {
        frame->fixedFunctionViewportUpdates++;
    }
    frame->lastXboxViewportX = xboxX;
    frame->lastXboxViewportY = xboxY;
    frame->lastXboxViewportWidth = xboxWidth;
    frame->lastXboxViewportHeight = xboxHeight;
    frame->lastHostRenderTargetWidth = hostRenderTargetWidth;
    frame->lastHostRenderTargetHeight = hostRenderTargetHeight;
    frame->lastHostViewportX = hostViewportX;
    frame->lastHostViewportY = hostViewportY;
    frame->lastHostViewportWidth = hostViewportWidth;
    frame->lastHostViewportHeight = hostViewportHeight;
    frame->lastScissorEnabled = scissorEnabled;
    if (scissorRect) {
        frame->lastScissorRect = *scissorRect;
    } else {
        SetRectEmpty(&frame->lastScissorRect);
    }
    if (hostViewportWidth == 0 || hostViewportHeight == 0) {
        frame->zeroHostViewportCount++;
    }
    if (scissorEnabled && scissorRect != nullptr && (RectWidth(*scissorRect) <= 0 || RectHeight(*scissorRect) <= 0)) {
        frame->zeroScissorCount++;
    }
}

void RenderTrace_RecordRenderTargetValidation(
    DWORD hostRenderTargetWidth,
    DWORD hostRenderTargetHeight,
    DWORD xboxRenderTargetWidth,
    DWORD xboxRenderTargetHeight,
    bool recreated)
{
    if (!g_RenderTraceEnabled || !g_state.initialized) {
        return;
    }

    g_state.sticky.lastHostRenderTargetWidth = hostRenderTargetWidth;
    g_state.sticky.lastHostRenderTargetHeight = hostRenderTargetHeight;
    g_state.sticky.lastValidatedHostRenderTargetWidth = hostRenderTargetWidth;
    g_state.sticky.lastValidatedHostRenderTargetHeight = hostRenderTargetHeight;
    g_state.sticky.lastValidatedXboxRenderTargetWidth = xboxRenderTargetWidth;
    g_state.sticky.lastValidatedXboxRenderTargetHeight = xboxRenderTargetHeight;

    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    frame->renderTargetValidationCalls++;
    if (recreated) {
        frame->renderTargetRecreateCalls++;
    }
    frame->lastValidatedHostRenderTargetWidth = hostRenderTargetWidth;
    frame->lastValidatedHostRenderTargetHeight = hostRenderTargetHeight;
    frame->lastValidatedXboxRenderTargetWidth = xboxRenderTargetWidth;
    frame->lastValidatedXboxRenderTargetHeight = xboxRenderTargetHeight;
}

void RenderTrace_RecordRenderState(DWORD state, DWORD value)
{
    if (!g_RenderTraceEnabled || !g_state.initialized) {
        return;
    }

    UpdateTrackedRenderState(&g_state.sticky.renderState, state, value);

    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->renderState = g_state.sticky.renderState;
    }
}

void RenderTrace_RecordTextureStageState(DWORD stage, DWORD state, DWORD value)
{
    if (!g_RenderTraceEnabled || !g_state.initialized || stage >= 2) {
        return;
    }

    UpdateTrackedTextureStageState(&g_state.sticky.textureStages[stage], state, value);

    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->textureStages[stage] = g_state.sticky.textureStages[stage];
    }
}

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
    bool aliasesCurrentRenderTarget)
{
    if (!g_RenderTraceEnabled || !g_state.initialized || stage >= 2) {
        return;
    }

    TrackedTextureBinding* binding = &g_state.sticky.textureBindings[stage];
    binding->known = true;
    binding->hasXboxTexture = hasXboxTexture;
    binding->hasXboxData = hasXboxData;
    binding->xboxDataAddress = xboxDataAddress;
    binding->hasXboxDataHash = hasXboxDataHash;
    binding->xboxDataHash = xboxDataHash;
    binding->xboxResourceType = xboxResourceType;
    binding->xboxFormat = xboxFormat;
    binding->xboxWidth = xboxWidth;
    binding->xboxHeight = xboxHeight;
    binding->hasHostTexture = hasHostTexture;
    binding->convertedFromSurface = convertedFromSurface;
    binding->aliasesBackBuffer = aliasesBackBuffer;
    binding->aliasesCurrentRenderTarget = aliasesCurrentRenderTarget;

    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->textureBindings[stage] = *binding;
    }
}

void RenderTrace_RecordVertexShader(DWORD handle)
{
    if (!g_RenderTraceEnabled || !g_state.initialized) {
        return;
    }

    g_state.sticky.lastVertexShaderHandle = handle;

    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->lastVertexShaderHandle = handle;
    }
}

void RenderTrace_RecordPixelShader(DWORD handle)
{
    if (!g_RenderTraceEnabled || !g_state.initialized) {
        return;
    }

    g_state.sticky.lastPixelShaderHandle = handle;

    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->lastPixelShaderHandle = handle;
    }
}

void RenderTrace_RecordPixelShaderKey(bool hasKey, unsigned long long key)
{
    if (!g_RenderTraceEnabled || !g_state.initialized) {
        return;
    }

    g_state.sticky.hasPixelShaderKey = hasKey;
    g_state.sticky.pixelShaderKey = hasKey ? key : 0;

    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->hasPixelShaderKey = hasKey;
        frame->pixelShaderKey = hasKey ? key : 0;
    }
}

void RenderTrace_RecordVisibilityBegin(bool hostQueryEnabled)
{
    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    if (hostQueryEnabled) {
        frame->visibilityBeginCalls++;
    } else {
        frame->visibilityBeginDisabledCalls++;
    }
}

void RenderTrace_RecordVisibilityEnd(DWORD index, bool hostQueryEnabled, bool missingBegin, HRESULT hResult)
{
    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    frame->lastVisibilityIndex = index;
    frame->lastVisibilityResultHr = hResult;
    if (missingBegin) {
        frame->visibilityMissingBeginCalls++;
    }
    if (hostQueryEnabled) {
        frame->visibilityEndCalls++;
    } else {
        frame->visibilityEndDisabledCalls++;
    }
}

void RenderTrace_RecordVisibilityResult(DWORD index, bool hostQueryEnabled, bool fallbackPath, DWORD result, HRESULT hResult)
{
    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    frame->visibilityGetCalls++;
    if (!hostQueryEnabled || fallbackPath) {
        frame->visibilityFallbackCalls++;
    }
    if (result == 0) {
        frame->visibilityZeroResults++;
    } else {
        frame->visibilityNonZeroResults++;
    }
    if (result > frame->maxVisibilityResult) {
        frame->maxVisibilityResult = result;
    }
    frame->lastVisibilityIndex = index;
    frame->lastVisibilityResultHr = hResult;
}

void RenderTrace_RecordSwap(
    DWORD flags,
    bool returnedEarly,
    bool hadHostBackBuffer,
    bool hadXboxBackBuffer,
    const RECT* destRect,
    HRESULT getBackBufferResult,
    HRESULT blitResult,
    bool hadOverlay)
{
    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    frame->swapSeen = true;
    frame->swapReturnedEarly = returnedEarly;
    frame->lastSwapFlags = flags;
    frame->hadHostBackBuffer = hadHostBackBuffer;
    frame->hadXboxBackBuffer = hadXboxBackBuffer;
    frame->hadOverlay = hadOverlay;
    frame->lastGetBackBufferResult = getBackBufferResult;
    frame->lastBlitResult = blitResult;
    frame->hasDestRect = (destRect != nullptr);
    if (destRect) {
        frame->lastSwapDestRect = *destRect;
    } else {
        SetRectEmpty(&frame->lastSwapDestRect);
    }
}