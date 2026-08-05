// ******************************************************************
// * RenderTrace.cpp - Render-state tracing for blank-screen investigations
// ******************************************************************
#include "RenderTrace.h"

#include <algorithm>
#include <d3d9.h>
#include <cstdio>
#include <cstring>

bool g_RenderTraceEnabled = false;

namespace {

constexpr DWORD kUnknownStateValue = 0xFFFFFFFFu;
constexpr size_t kMaxDrawSignatureBuckets = 32;
constexpr size_t kTrackedTextureStages = 4;
constexpr LONG kMaxTextureProducerRecords = 256;
constexpr LONG kMaxTextureHookRecords = 64;
constexpr LONG kMaxTextureUploadRecords = 512;
constexpr LONG kMaxFileOpenRecords = 512;
volatile LONG g_textureProducerRecordCount = 0;
volatile LONG g_textureHookRecordCount = 0;
volatile LONG g_textureUploadRecordCount = 0;
volatile LONG g_fileOpenRecordCount = 0;

unsigned long long HashBytes(const void* data, size_t byteCount)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    unsigned long long hash = 1469598103934665603ull;
    for (size_t i = 0; i < byteCount; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

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
    bool hasVertexShaderDetails;
    bool vertexShaderProgram;
    unsigned long long vertexShaderKey;
    unsigned long long vertexShaderCacheHash;
    bool vertexShaderUsesIndexedBoneConstants;
    unsigned vertexShaderInputOverrideCount;
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
    TrackedTextureStageState textureStages[kTrackedTextureStages];
    TrackedTextureBinding textureBindings[kTrackedTextureStages];
    DWORD lastVertexShaderHandle;
    bool hasVertexShaderDetails;
    bool vertexShaderProgram;
    unsigned long long vertexShaderKey;
    unsigned long long vertexShaderCacheHash;
    bool vertexShaderUsesIndexedBoneConstants;
    unsigned vertexShaderInputOverrideCount;
    UINT vertexShaderInputStride;
    UINT vertexShaderInputOffset;
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
    bool hasIndexData;
    unsigned long long indexCount;
    DWORD lowIndex;
    DWORD highIndex;
    unsigned long long restartIndexCount;
    INT lastBaseVertexIndex;
    INT minBaseVertexIndex;
    INT maxBaseVertexIndex;
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
    TrackedTextureStageState textureStages[kTrackedTextureStages];
    TrackedTextureBinding textureBindings[kTrackedTextureStages];
    DWORD lastVertexShaderHandle;
    bool hasVertexShaderDetails;
    bool vertexShaderProgram;
    unsigned long long vertexShaderKey;
    unsigned long long vertexShaderCacheHash;
    bool vertexShaderUsesIndexedBoneConstants;
    unsigned vertexShaderInputOverrideCount;
    unsigned maxVertexShaderInputOverrideCount;
    bool hasVertexShaderInputDetails;
    UINT lastVertexShaderInputStride;
    UINT minVertexShaderInputStride;
    UINT maxVertexShaderInputStride;
    UINT lastVertexShaderInputOffset;
    UINT minVertexShaderInputOffset;
    UINT maxVertexShaderInputOffset;
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
    for (size_t stage = 0; stage < kTrackedTextureStages; ++stage) {
        ResetTrackedTextureStageState(&sticky->textureStages[stage]);
        ResetTrackedTextureBinding(&sticky->textureBindings[stage]);
    }
    sticky->lastVertexShaderHandle = kUnknownStateValue;
    sticky->hasVertexShaderDetails = false;
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
    for (size_t stage = 0; stage < kTrackedTextureStages; ++stage) {
        frame->textureStages[stage] = g_state.sticky.textureStages[stage];
        frame->textureBindings[stage] = g_state.sticky.textureBindings[stage];
    }
    frame->lastVertexShaderHandle = g_state.sticky.lastVertexShaderHandle;
    frame->hasVertexShaderDetails = g_state.sticky.hasVertexShaderDetails;
    frame->vertexShaderProgram = g_state.sticky.vertexShaderProgram;
    frame->vertexShaderKey = g_state.sticky.vertexShaderKey;
    frame->vertexShaderCacheHash = g_state.sticky.vertexShaderCacheHash;
    frame->vertexShaderUsesIndexedBoneConstants = g_state.sticky.vertexShaderUsesIndexedBoneConstants;
    frame->vertexShaderInputOverrideCount = g_state.sticky.vertexShaderInputOverrideCount;
    frame->maxVertexShaderInputOverrideCount = g_state.sticky.vertexShaderInputOverrideCount;
    if (g_state.sticky.vertexShaderInputOverrideCount > 0) {
        frame->hasVertexShaderInputDetails = true;
        frame->lastVertexShaderInputStride = g_state.sticky.vertexShaderInputStride;
        frame->minVertexShaderInputStride = g_state.sticky.vertexShaderInputStride;
        frame->maxVertexShaderInputStride = g_state.sticky.vertexShaderInputStride;
        frame->lastVertexShaderInputOffset = g_state.sticky.vertexShaderInputOffset;
        frame->minVertexShaderInputOffset = g_state.sticky.vertexShaderInputOffset;
        frame->maxVertexShaderInputOffset = g_state.sticky.vertexShaderInputOffset;
    }
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
    signature.hasVertexShaderDetails = frame.hasVertexShaderDetails;
    signature.vertexShaderProgram = frame.vertexShaderProgram;
    signature.vertexShaderKey = frame.vertexShaderKey;
    signature.vertexShaderCacheHash = frame.vertexShaderCacheHash;
    signature.vertexShaderUsesIndexedBoneConstants = frame.vertexShaderUsesIndexedBoneConstants;
    signature.vertexShaderInputOverrideCount = frame.vertexShaderInputOverrideCount;
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
        left.hasVertexShaderDetails == right.hasVertexShaderDetails &&
        left.vertexShaderProgram == right.vertexShaderProgram &&
        left.vertexShaderKey == right.vertexShaderKey &&
        left.vertexShaderCacheHash == right.vertexShaderCacheHash &&
        left.vertexShaderUsesIndexedBoneConstants == right.vertexShaderUsesIndexedBoneConstants &&
        left.vertexShaderInputOverrideCount == right.vertexShaderInputOverrideCount &&
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
            "%ux(i=%u up=%u pt=%s/%u gh=%u/%016llX xy=%u/[%.2f,%.2f %.2f,%.2f] uv0=%u/[%.2f,%.2f %.2f,%.2f] diff=%u/[r%lu-%lu g%lu-%lu b%lu-%lu a%lu-%lu] vs=%08lX vsp=%s key=%016llX vsh=%016llX bone=%u vio=%u ps=%08lX psk=%s/%016llX ab=%08lX sb=%08lX db=%08lX at=%08lX co=%08lX ao=%08lX ttf=%08lX tci=%08lX xh=%s/%016llX xd=%08lX fmt=%08lX %lux%lu host=%u)",
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
            KnownBoolName(bucket.signature.hasVertexShaderDetails, bucket.signature.vertexShaderProgram),
            bucket.signature.vertexShaderKey,
            bucket.signature.vertexShaderCacheHash,
            bucket.signature.vertexShaderUsesIndexedBoneConstants ? 1 : 0,
            bucket.signature.vertexShaderInputOverrideCount,
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
    for (size_t stage = 1; stage < kTrackedTextureStages; ++stage) {
        if (frame.drawCalls != 0 &&
            TextureStageUsesTexture(frame.textureStages[stage]) &&
            frame.textureBindings[stage].known) {
            char note[32] = {};
            if (!frame.textureBindings[stage].hasXboxTexture) {
                sprintf_s(note, sizeof(note), "t%zu_unbound", stage);
            } else if (!frame.textureBindings[stage].hasXboxData) {
                sprintf_s(note, sizeof(note), "t%zu_nodata", stage);
            } else if (!frame.textureBindings[stage].hasHostTexture) {
                sprintf_s(note, sizeof(note), "t%zu_host_null", stage);
            }
            if (note[0] != '\0') {
                AppendNote(notes, sizeof(notes), note);
            }
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
        "F%06llu draw=%u(idx=%u up=%u fail=%u last=%s/%u hr=%08lX) ix=[has=%u n=%llu lo=%lu hi=%lu rst=%llu base=%ld/%ld-%ld] dg=%s clear=%u(lastFlags=%08lX color=%08lX hr=%08lX) vp=%u(ff=%u null=%u unread=%u missrt=%u) xboxvp=[%lu,%lu %lux%lu] hostrt=%lux%lu hostvp=[%lu,%lu %lux%lu] sc=%s[%ld,%ld %ldx%ld] rtval=%u(rec=%u host=%lux%lu xbox=%lux%lu) vis=b%u/db%u e%u/de%u miss=%u g%u fb=%u z%u nz=%u max=%lu last=%lu hr=%08lX rs=[cw=%08lX ab=%08lX sb=%08lX db=%08lX bo=%08lX at=%08lX af=%08lX ar=%08lX ze=%08lX zw=%08lX zf=%08lX cm=%08lX se=%08lX sc=%08lX sf=%08lX sr=%08lX sz=%08lX sp=%08lX tf=%08lX] sh=[vs=%08lX vsp=%s key=%016llX vsh=%016llX bone=%u vio=%u/%u vi=%u stride=%u/%u-%u off=%u/%u-%u ps=%08lX psk=%s/%016llX] t0=[co=%08lX c1=%08lX c2=%08lX ao=%08lX a1=%08lX a2=%08lX ttf=%08lX tci=%08lX] t1=[co=%08lX c1=%08lX c2=%08lX ao=%08lX a1=%08lX a2=%08lX ttf=%08lX tci=%08lX] t2=[co=%08lX c1=%08lX c2=%08lX ao=%08lX a1=%08lX a2=%08lX ttf=%08lX tci=%08lX] t3=[co=%08lX c1=%08lX c2=%08lX ao=%08lX a1=%08lX a2=%08lX ttf=%08lX tci=%08lX] tb0=[xb=%s data=%s xdata=%08lX hk=%s xhash=%016llX rt=%s fmt=%08lX %lux%lu host=%s cvt=%s bb=%s crt=%s] tb1=[xb=%s data=%s xdata=%08lX hk=%s xhash=%016llX rt=%s fmt=%08lX %lux%lu host=%s cvt=%s bb=%s crt=%s] tb2=[xb=%s data=%s xdata=%08lX hk=%s xhash=%016llX rt=%s fmt=%08lX %lux%lu host=%s cvt=%s bb=%s crt=%s] tb3=[xb=%s data=%s xdata=%08lX hk=%s xhash=%016llX rt=%s fmt=%08lX %lux%lu host=%s cvt=%s bb=%s crt=%s] swap=%d early=%d flags=%08lX hostbb=%d xboxbb=%d ov=%d getbb=%08lX blit=%08lX dest=[%ld,%ld %ldx%ld] notes=%s\n",
        frame.frameNumber,
        frame.drawCalls,
        frame.indexedDrawCalls,
        frame.userPointerDrawCalls,
        frame.failedDrawCalls,
        PrimitiveTypeName(frame.lastPrimitiveType),
        frame.lastPrimitiveCount,
        (unsigned long)frame.lastDrawResult,
        frame.hasIndexData ? 1 : 0,
        frame.indexCount,
        (unsigned long)frame.lowIndex,
        (unsigned long)frame.highIndex,
        frame.restartIndexCount,
        (long)frame.lastBaseVertexIndex,
        (long)frame.minBaseVertexIndex,
        (long)frame.maxBaseVertexIndex,
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
        KnownBoolName(frame.hasVertexShaderDetails, frame.vertexShaderProgram),
        frame.vertexShaderKey,
        frame.vertexShaderCacheHash,
        frame.vertexShaderUsesIndexedBoneConstants ? 1 : 0,
        frame.vertexShaderInputOverrideCount,
        frame.maxVertexShaderInputOverrideCount,
        frame.hasVertexShaderInputDetails ? 1 : 0,
        frame.lastVertexShaderInputStride,
        frame.minVertexShaderInputStride,
        frame.maxVertexShaderInputStride,
        frame.lastVertexShaderInputOffset,
        frame.minVertexShaderInputOffset,
        frame.maxVertexShaderInputOffset,
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
        (unsigned long)frame.textureStages[2].colorOp,
        (unsigned long)frame.textureStages[2].colorArg1,
        (unsigned long)frame.textureStages[2].colorArg2,
        (unsigned long)frame.textureStages[2].alphaOp,
        (unsigned long)frame.textureStages[2].alphaArg1,
        (unsigned long)frame.textureStages[2].alphaArg2,
        (unsigned long)frame.textureStages[2].textureTransformFlags,
        (unsigned long)frame.textureStages[2].texCoordIndex,
        (unsigned long)frame.textureStages[3].colorOp,
        (unsigned long)frame.textureStages[3].colorArg1,
        (unsigned long)frame.textureStages[3].colorArg2,
        (unsigned long)frame.textureStages[3].alphaOp,
        (unsigned long)frame.textureStages[3].alphaArg1,
        (unsigned long)frame.textureStages[3].alphaArg2,
        (unsigned long)frame.textureStages[3].textureTransformFlags,
        (unsigned long)frame.textureStages[3].texCoordIndex,
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
        KnownBoolName(frame.textureBindings[2].known, frame.textureBindings[2].hasXboxTexture),
        KnownBoolName(frame.textureBindings[2].known, frame.textureBindings[2].hasXboxData),
        (unsigned long)frame.textureBindings[2].xboxDataAddress,
        KnownBoolName(frame.textureBindings[2].known, frame.textureBindings[2].hasXboxDataHash),
        frame.textureBindings[2].xboxDataHash,
        TextureResourceTypeName(frame.textureBindings[2]),
        (unsigned long)frame.textureBindings[2].xboxFormat,
        (unsigned long)frame.textureBindings[2].xboxWidth,
        (unsigned long)frame.textureBindings[2].xboxHeight,
        KnownBoolName(frame.textureBindings[2].known, frame.textureBindings[2].hasHostTexture),
        KnownBoolName(frame.textureBindings[2].known, frame.textureBindings[2].convertedFromSurface),
        KnownBoolName(frame.textureBindings[2].known, frame.textureBindings[2].aliasesBackBuffer),
        KnownBoolName(frame.textureBindings[2].known, frame.textureBindings[2].aliasesCurrentRenderTarget),
        KnownBoolName(frame.textureBindings[3].known, frame.textureBindings[3].hasXboxTexture),
        KnownBoolName(frame.textureBindings[3].known, frame.textureBindings[3].hasXboxData),
        (unsigned long)frame.textureBindings[3].xboxDataAddress,
        KnownBoolName(frame.textureBindings[3].known, frame.textureBindings[3].hasXboxDataHash),
        frame.textureBindings[3].xboxDataHash,
        TextureResourceTypeName(frame.textureBindings[3]),
        (unsigned long)frame.textureBindings[3].xboxFormat,
        (unsigned long)frame.textureBindings[3].xboxWidth,
        (unsigned long)frame.textureBindings[3].xboxHeight,
        KnownBoolName(frame.textureBindings[3].known, frame.textureBindings[3].hasHostTexture),
        KnownBoolName(frame.textureBindings[3].known, frame.textureBindings[3].convertedFromSurface),
        KnownBoolName(frame.textureBindings[3].known, frame.textureBindings[3].aliasesBackBuffer),
        KnownBoolName(frame.textureBindings[3].known, frame.textureBindings[3].aliasesCurrentRenderTarget),
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

void RenderTrace_RecordIndexData(
    UINT indexCount,
    DWORD lowIndex,
    DWORD highIndex,
    UINT restartIndexCount,
    INT baseVertexIndex)
{
    FrameState* frame = CurrentFrame();
    if (!frame) {
        return;
    }

    if (!frame->hasIndexData) {
        frame->hasIndexData = true;
        frame->lowIndex = lowIndex;
        frame->highIndex = highIndex;
        frame->minBaseVertexIndex = baseVertexIndex;
        frame->maxBaseVertexIndex = baseVertexIndex;
    } else {
        if (lowIndex < frame->lowIndex) {
            frame->lowIndex = lowIndex;
        }
        if (highIndex > frame->highIndex) {
            frame->highIndex = highIndex;
        }
        if (baseVertexIndex < frame->minBaseVertexIndex) {
            frame->minBaseVertexIndex = baseVertexIndex;
        }
        if (baseVertexIndex > frame->maxBaseVertexIndex) {
            frame->maxBaseVertexIndex = baseVertexIndex;
        }
    }

    frame->indexCount += indexCount;
    frame->restartIndexCount += restartIndexCount;
    frame->lastBaseVertexIndex = baseVertexIndex;
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
    if (!g_RenderTraceEnabled || !g_state.initialized ||
        stage >= kTrackedTextureStages) {
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
    if (!g_RenderTraceEnabled || !g_state.initialized ||
        stage >= kTrackedTextureStages) {
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

void RenderTrace_RecordTextureProducer(
    const char* eventName,
    DWORD stage,
    const void* xboxTexture,
    DWORD xboxDataAddress,
    const void* callerAddress)
{
    if (!g_RenderTraceEnabled || !g_state.logFile || eventName == nullptr) {
        return;
    }

    const LONG sequence = InterlockedIncrement(
        &g_textureProducerRecordCount);
    if (sequence > kMaxTextureProducerRecords) {
        return;
    }

    fprintf(
        g_state.logFile,
        "TEXSRC seq=%ld event=%s stage=%lu xb=%p xdata=%08lX caller=%p tid=%08lX\n",
        sequence,
        eventName,
        (unsigned long)stage,
        xboxTexture,
        (unsigned long)xboxDataAddress,
        callerAddress,
        (unsigned long)GetCurrentThreadId());
    fflush(g_state.logFile);
}

void RenderTrace_RecordTextureHookState(
    DWORD sourceAddress,
    const BYTE* sourceBytes,
    size_t sourceByteCount,
    const void* trampolineAddress,
    const BYTE* trampolineBytes,
    size_t trampolineByteCount,
    const void* activeTexture)
{
    if (!g_RenderTraceEnabled || !g_state.logFile) {
        return;
    }

    const LONG sequence = InterlockedIncrement(&g_textureHookRecordCount);
    if (sequence > kMaxTextureHookRecords) {
        return;
    }

    char sourceHex[33] = {};
    char trampolineHex[33] = {};
    sourceByteCount = std::min<size_t>(sourceByteCount, 16);
    trampolineByteCount = std::min<size_t>(trampolineByteCount, 16);
    for (size_t index = 0; index < sourceByteCount; ++index) {
        sprintf_s(
            sourceHex + index * 2,
            sizeof(sourceHex) - index * 2,
            "%02X",
            sourceBytes[index]);
    }
    for (size_t index = 0; index < trampolineByteCount; ++index) {
        sprintf_s(
            trampolineHex + index * 2,
            sizeof(trampolineHex) - index * 2,
            "%02X",
            trampolineBytes[index]);
    }

    fprintf(
        g_state.logFile,
        "TEXHOOK seq=%ld src=%08lX bytes=%s tramp=%p tbytes=%s active0=%p tid=%08lX\n",
        sequence,
        (unsigned long)sourceAddress,
        sourceHex,
        trampolineAddress,
        trampolineHex,
        activeTexture,
        (unsigned long)GetCurrentThreadId());
    fflush(g_state.logFile);
}

void RenderTrace_RecordTextureUpload(
    DWORD xboxResourceType,
    DWORD xboxFormat,
    DWORD hostFormat,
    DWORD width,
    DWORD height,
    DWORD mipLevels,
    HRESULT result)
{
    if (!g_RenderTraceEnabled || !g_state.logFile) {
        return;
    }

    const LONG sequence = InterlockedIncrement(&g_textureUploadRecordCount);
    if (sequence > kMaxTextureUploadRecords) {
        return;
    }

    fprintf(
        g_state.logFile,
        "TEXUPLOAD seq=%ld type=%08lX xfmt=%08lX hfmt=%08lX size=%lux%lu levels=%lu hr=%08lX tid=%08lX\n",
        sequence,
        (unsigned long)xboxResourceType,
        (unsigned long)xboxFormat,
        (unsigned long)hostFormat,
        (unsigned long)width,
        (unsigned long)height,
        (unsigned long)mipLevels,
        (unsigned long)result,
        (unsigned long)GetCurrentThreadId());
    fflush(g_state.logFile);
}

void RenderTrace_RecordFileOpen(
    const char* path,
    size_t pathLength,
    LONG result)
{
    if (!g_RenderTraceEnabled || !g_state.logFile || path == nullptr) {
        return;
    }

    const LONG sequence = InterlockedIncrement(&g_fileOpenRecordCount);
    if (sequence > kMaxFileOpenRecords) {
        return;
    }

    const int printableLength = static_cast<int>(
        std::min<size_t>(pathLength, 800));
    fprintf(
        g_state.logFile,
        "FILEOPEN seq=%ld result=%08lX path=\"%.*s\" tid=%08lX\n",
        sequence,
        (unsigned long)result,
        printableLength,
        path,
        (unsigned long)GetCurrentThreadId());
    fflush(g_state.logFile);
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

void RenderTrace_RecordVertexDeclaration(
    unsigned long long key,
    bool fixedFunction,
    UINT streamOrdinal,
    UINT xboxStreamIndex,
    bool needsPatch,
    UINT hostStride,
    UINT elementCount)
{
    if (!g_RenderTraceEnabled || !g_state.initialized || !g_state.logFile) {
        return;
    }

    fprintf(
        g_state.logFile,
        "DECL key=%016llX ff=%u stream=%u xbox_stream=%u patch=%u host_stride=%u elements=%u\n",
        key,
        fixedFunction ? 1u : 0u,
        streamOrdinal,
        xboxStreamIndex,
        needsPatch ? 1u : 0u,
        hostStride,
        elementCount);
}

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
    UINT hostSize)
{
    if (!g_RenderTraceEnabled || !g_state.initialized || !g_state.logFile) {
        return;
    }

    fprintf(
        g_state.logFile,
        "DELE key=%016llX stream=%u elem=%u xbox=[reg=%u off=%u type=%u size=%u] host=[off=%u type=%u size=%u]\n",
        key,
        streamOrdinal,
        elementOrdinal,
        xboxRegister,
        xboxOffset,
        xboxType,
        xboxSize,
        hostOffset,
        hostType,
        hostSize);
}

void RenderTrace_RecordFixedFunctionVertexBlend(DWORD vertexBlend)
{
    if (!g_RenderTraceEnabled || !g_state.initialized || !g_state.logFile) {
        return;
    }

    static unsigned long long lastFrameNumber = ~0ull;
    static DWORD frameVertexBlendMask = 0;
    const unsigned long long frameNumber =
        g_state.haveCurrentFrame ? g_state.current.frameNumber : g_state.nextFrameNumber;
    if (lastFrameNumber != frameNumber) {
        lastFrameNumber = frameNumber;
        frameVertexBlendMask = 0;
    }

    const DWORD blendBit = vertexBlend < 32 ? (1u << vertexBlend) : 0;
    if (blendBit != 0 && (frameVertexBlendMask & blendBit) != 0) {
        return;
    }
    frameVertexBlendMask |= blendBit;

    fprintf(
        g_state.logFile,
        "FFVB frame=%llu value=%lu\n",
        frameNumber,
        static_cast<unsigned long>(vertexBlend));
}

void RenderTrace_RecordFixedFunctionBlendMatrices(
    bool fromNv2a,
    DWORD vertexBlend,
    UINT matrixCount,
    const float* transposedWorldViewMatrices)
{
    if (!g_RenderTraceEnabled || !g_state.initialized || !g_state.logFile ||
        transposedWorldViewMatrices == nullptr || matrixCount == 0 || matrixCount > 4) {
        return;
    }

    const unsigned long long frameNumber =
        g_state.haveCurrentFrame ? g_state.current.frameNumber : g_state.nextFrameNumber;
    if ((frameNumber % 60) != 0 || vertexBlend >= 32) {
        return;
    }

    static unsigned long long lastFrameNumber = ~0ull;
    static unsigned long long frameSourceBlendMask = 0;
    if (lastFrameNumber != frameNumber) {
        lastFrameNumber = frameNumber;
        frameSourceBlendMask = 0;
    }

    const unsigned long long blendBit =
        1ull << (vertexBlend + (fromNv2a ? 32 : 0));
    if ((frameSourceBlendMask & blendBit) != 0) {
        return;
    }
    frameSourceBlendMask |= blendBit;

    fprintf(
        g_state.logFile,
        "FFMX frame=%llu src=%s value=%lu matrices=%u",
        frameNumber,
        fromNv2a ? "nv2a" : "hle",
        static_cast<unsigned long>(vertexBlend),
        matrixCount);
    for (UINT matrixIndex = 0; matrixIndex < matrixCount; ++matrixIndex) {
        const float* matrix = transposedWorldViewMatrices + matrixIndex * 16;
        fprintf(
            g_state.logFile,
            " m%u=[%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g]",
            matrixIndex,
            matrix[0], matrix[1], matrix[2], matrix[3],
            matrix[4], matrix[5], matrix[6], matrix[7],
            matrix[8], matrix[9], matrix[10], matrix[11],
            matrix[12], matrix[13], matrix[14], matrix[15]);
    }
    fputc('\n', g_state.logFile);
}

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
    bool gpuWasInvalid)
{
    if (!g_RenderTraceEnabled || !g_state.initialized || !g_state.logFile ||
        cpuState == nullptr || cpuStateSize < (16 * sizeof(float))) {
        return;
    }

    const unsigned long long frameNumber =
        g_state.haveCurrentFrame ? g_state.current.frameNumber : g_state.nextFrameNumber;
    static unsigned long long lastFrameNumber = ~0ull;
    if (lastFrameNumber == frameNumber) {
        return;
    }
    lastFrameNumber = frameNumber;

    const size_t transformByteCount =
        cpuStateSize < (14 * 16 * sizeof(float))
            ? cpuStateSize
            : (14 * 16 * sizeof(float));
    const bool hostReadSucceeded =
        SUCCEEDED(getHostConstantsResult) &&
        hostConstants != nullptr &&
        hostFloatCount * sizeof(float) >= transformByteCount;
    const unsigned long long cpuStateHash = HashBytes(cpuState, cpuStateSize);
    const unsigned long long cpuTransformHash =
        HashBytes(cpuState, transformByteCount);
    const unsigned long long hostStateHash =
        hostReadSucceeded
            ? HashBytes(hostConstants, hostFloatCount * sizeof(float))
            : 0;
    const unsigned long long hostTransformHash =
        hostReadSucceeded
            ? HashBytes(hostConstants, transformByteCount)
            : 0;

    const float* cpu = static_cast<const float*>(cpuState);
    const float* host = hostReadSucceeded ? hostConstants : nullptr;
    fprintf(
        g_state.logFile,
        "FFSTATE frame=%llu serial=%llu dirty=%u,%u,%u"
        " set=%08lX,%08lX get=%08lX"
        " cpu=%016llX cpuXf=%016llX host=%016llX hostXf=%016llX"
        " view=[%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g]"
        " proj=[%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g]"
        " world0=[%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g;%.7g,%.7g,%.7g,%.7g]"
        " hostView0=%.7g hostProj0=%.7g hostWorld0=%.7g\n",
        frameNumber,
        uploadSerial,
        transformWasDirty ? 1u : 0u,
        nonTransformWasDirty ? 1u : 0u,
        gpuWasInvalid ? 1u : 0u,
        static_cast<unsigned long>(transformUploadResult),
        static_cast<unsigned long>(restUploadResult),
        static_cast<unsigned long>(getHostConstantsResult),
        cpuStateHash,
        cpuTransformHash,
        hostStateHash,
        hostTransformHash,
        cpu[0], cpu[1], cpu[2], cpu[3],
        cpu[4], cpu[5], cpu[6], cpu[7],
        cpu[8], cpu[9], cpu[10], cpu[11],
        cpu[12], cpu[13], cpu[14], cpu[15],
        cpu[16], cpu[17], cpu[18], cpu[19],
        cpu[20], cpu[21], cpu[22], cpu[23],
        cpu[24], cpu[25], cpu[26], cpu[27],
        cpu[28], cpu[29], cpu[30], cpu[31],
        cpu[96], cpu[97], cpu[98], cpu[99],
        cpu[100], cpu[101], cpu[102], cpu[103],
        cpu[104], cpu[105], cpu[106], cpu[107],
        cpu[108], cpu[109], cpu[110], cpu[111],
        host ? host[0] : 0.0f,
        host ? host[16] : 0.0f,
        host ? host[96] : 0.0f);
    fflush(g_state.logFile);
}

void RenderTrace_RecordVertexConstantState(
    DWORD vertexShaderMode,
    const float* hostConstants,
    UINT hostRegisterCount,
    HRESULT getHostConstantsResult)
{
    constexpr UINT kDefaultValuesBase = 192;
    constexpr UINT kDefaultFlagsBase = 208;
    constexpr UINT kScreenScaleRegister = 212;
    constexpr UINT kScreenOffsetRegister = 213;
    constexpr UINT kTextureScaleBase = 214;
    constexpr UINT kFogRegister = 218;
    constexpr UINT kRequiredRegisterCount = kFogRegister + 1;

    if (!g_RenderTraceEnabled || !g_state.initialized || !g_state.logFile) {
        return;
    }

    const unsigned long long frameNumber =
        g_state.haveCurrentFrame ? g_state.current.frameNumber : g_state.nextFrameNumber;
    static unsigned long long lastFrameNumber = ~0ull;
    static DWORD frameModeMask = 0;
    if (lastFrameNumber != frameNumber) {
        lastFrameNumber = frameNumber;
        frameModeMask = 0;
    }
    const DWORD modeBit = vertexShaderMode < 32 ? (1u << vertexShaderMode) : 0;
    if (modeBit != 0 && (frameModeMask & modeBit) != 0) {
        return;
    }
    frameModeMask |= modeBit;

    const bool hostReadSucceeded =
        SUCCEEDED(getHostConstantsResult) &&
        hostConstants != nullptr &&
        hostRegisterCount >= kRequiredRegisterCount;
    if (!hostReadSucceeded) {
        fprintf(
            g_state.logFile,
            "VCONST frame=%llu mode=%lu get=%08lX regs=%u\n",
            frameNumber,
            static_cast<unsigned long>(vertexShaderMode),
            static_cast<unsigned long>(getHostConstantsResult),
            hostRegisterCount);
        fflush(g_state.logFile);
        return;
    }

    auto reg = [hostConstants](UINT registerIndex) {
        return hostConstants + registerIndex * 4;
    };
    const float* screenScale = reg(kScreenScaleRegister);
    const float* screenOffset = reg(kScreenOffsetRegister);
    const float* textureScale0 = reg(kTextureScaleBase);
    const float* textureScale1 = reg(kTextureScaleBase + 1);
    const float* textureScale2 = reg(kTextureScaleBase + 2);
    const float* textureScale3 = reg(kTextureScaleBase + 3);
    const float* fog = reg(kFogRegister);

    fprintf(
        g_state.logFile,
        "VCONST frame=%llu mode=%lu get=%08lX regs=%u"
        " all=%016llX xbox=%016llX defaults=%016llX flags=%016llX helper=%016llX"
        " screenScale=[%.7g,%.7g,%.7g,%.7g]"
        " screenOffset=[%.7g,%.7g,%.7g,%.7g]"
        " tex0=[%.7g,%.7g,%.7g,%.7g]"
        " tex1=[%.7g,%.7g,%.7g,%.7g]"
        " tex2=[%.7g,%.7g,%.7g,%.7g]"
        " tex3=[%.7g,%.7g,%.7g,%.7g]"
        " fog=[%.7g,%.7g,%.7g,%.7g]\n",
        frameNumber,
        static_cast<unsigned long>(vertexShaderMode),
        static_cast<unsigned long>(getHostConstantsResult),
        hostRegisterCount,
        HashBytes(hostConstants, hostRegisterCount * 4 * sizeof(float)),
        HashBytes(hostConstants, kDefaultValuesBase * 4 * sizeof(float)),
        HashBytes(reg(kDefaultValuesBase), 16 * 4 * sizeof(float)),
        HashBytes(reg(kDefaultFlagsBase), 4 * 4 * sizeof(float)),
        HashBytes(reg(kScreenScaleRegister), 7 * 4 * sizeof(float)),
        screenScale[0], screenScale[1], screenScale[2], screenScale[3],
        screenOffset[0], screenOffset[1], screenOffset[2], screenOffset[3],
        textureScale0[0], textureScale0[1], textureScale0[2], textureScale0[3],
        textureScale1[0], textureScale1[1], textureScale1[2], textureScale1[3],
        textureScale2[0], textureScale2[1], textureScale2[2], textureScale2[3],
        textureScale3[0], textureScale3[1], textureScale3[2], textureScale3[3],
        fog[0], fog[1], fog[2], fog[3]);
    fflush(g_state.logFile);
}

void RenderTrace_RecordPackedByte4(
    UINT xboxRegister,
    unsigned long long vertexDataHash,
    UINT vertexCount,
    UINT vertexStride,
    UINT elementOffset,
    const BYTE* vertexData)
{
    if (!g_RenderTraceEnabled || !g_state.initialized || !g_state.logFile ||
        vertexData == nullptr || vertexCount == 0 || vertexStride < elementOffset + 4) {
        return;
    }

    constexpr size_t kMaxLoggedStreams = 128;
    static unsigned long long loggedSignatures[kMaxLoggedStreams] = {};
    static size_t loggedSignatureCount = 0;
    const unsigned long long signature =
        vertexDataHash ^
        (static_cast<unsigned long long>(vertexStride) << 32) ^
        (static_cast<unsigned long long>(elementOffset) << 16) ^
        xboxRegister;
    for (size_t i = 0; i < loggedSignatureCount; ++i) {
        if (loggedSignatures[i] == signature) {
            return;
        }
    }
    if (loggedSignatureCount >= kMaxLoggedStreams) {
        return;
    }
    loggedSignatures[loggedSignatureCount++] = signature;

    BYTE minimum[4] = { 255, 255, 255, 255 };
    BYTE maximum[4] = {};
    unsigned long long total[4] = {};
    UINT zeroCount[4] = {};
    UINT fullCount[4] = {};
    UINT firstThreeSumMinimum = 3 * 255;
    UINT firstThreeSumMaximum = 0;
    for (UINT vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
        const BYTE* packed = vertexData + vertexIndex * vertexStride + elementOffset;
        UINT firstThreeSum = 0;
        for (UINT component = 0; component < 4; ++component) {
            const BYTE value = packed[component];
            if (value < minimum[component]) {
                minimum[component] = value;
            }
            if (value > maximum[component]) {
                maximum[component] = value;
            }
            total[component] += value;
            zeroCount[component] += value == 0 ? 1u : 0u;
            fullCount[component] += value == 255 ? 1u : 0u;
            if (component < 3) {
                firstThreeSum += value;
            }
        }
        if (firstThreeSum < firstThreeSumMinimum) {
            firstThreeSumMinimum = firstThreeSum;
        }
        if (firstThreeSum > firstThreeSumMaximum) {
            firstThreeSumMaximum = firstThreeSum;
        }
    }

    fprintf(
        g_state.logFile,
        "PBYTE4 reg=%u hash=%016llX vertices=%u stride=%u off=%u"
        " c0=%u-%u/avg%.2f/z%u/f%u c1=%u-%u/avg%.2f/z%u/f%u"
        " c2=%u-%u/avg%.2f/z%u/f%u c3=%u-%u/avg%.2f/z%u/f%u sum012=%u-%u samples=",
        xboxRegister,
        vertexDataHash,
        vertexCount,
        vertexStride,
        elementOffset,
        minimum[0], maximum[0], static_cast<double>(total[0]) / vertexCount, zeroCount[0], fullCount[0],
        minimum[1], maximum[1], static_cast<double>(total[1]) / vertexCount, zeroCount[1], fullCount[1],
        minimum[2], maximum[2], static_cast<double>(total[2]) / vertexCount, zeroCount[2], fullCount[2],
        minimum[3], maximum[3], static_cast<double>(total[3]) / vertexCount, zeroCount[3], fullCount[3],
        firstThreeSumMinimum,
        firstThreeSumMaximum);
    const UINT sampleCount = vertexCount < 8 ? vertexCount : 8;
    for (UINT sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        const BYTE* packed = vertexData + sampleIndex * vertexStride + elementOffset;
        fprintf(
            g_state.logFile,
            "%s%02X%02X%02X%02X",
            sampleIndex == 0 ? "" : ",",
            packed[0],
            packed[1],
            packed[2],
            packed[3]);
    }
    fputc('\n', g_state.logFile);
}

void RenderTrace_RecordVertexShaderDetails(
    bool shaderProgram,
    unsigned long long shaderKey,
    unsigned long long shaderCacheHash,
    bool usesIndexedBoneConstants,
    unsigned vertexShaderInputOverrideCount,
    UINT vertexShaderInputStride,
    UINT vertexShaderInputOffset)
{
    if (!g_RenderTraceEnabled || !g_state.initialized) {
        return;
    }

    g_state.sticky.hasVertexShaderDetails = true;
    g_state.sticky.vertexShaderProgram = shaderProgram;
    g_state.sticky.vertexShaderKey = shaderProgram ? shaderKey : 0;
    g_state.sticky.vertexShaderCacheHash = shaderProgram ? shaderCacheHash : 0;
    g_state.sticky.vertexShaderUsesIndexedBoneConstants =
        shaderProgram && usesIndexedBoneConstants;
    g_state.sticky.vertexShaderInputOverrideCount =
        vertexShaderInputOverrideCount;
    if (vertexShaderInputOverrideCount > 0) {
        g_state.sticky.vertexShaderInputStride = vertexShaderInputStride;
        g_state.sticky.vertexShaderInputOffset = vertexShaderInputOffset;
    }

    FrameState* frame = CurrentFrame();
    if (frame) {
        frame->hasVertexShaderDetails = true;
        frame->vertexShaderProgram = g_state.sticky.vertexShaderProgram;
        frame->vertexShaderKey = g_state.sticky.vertexShaderKey;
        frame->vertexShaderCacheHash = g_state.sticky.vertexShaderCacheHash;
        frame->vertexShaderUsesIndexedBoneConstants =
            g_state.sticky.vertexShaderUsesIndexedBoneConstants;
        frame->vertexShaderInputOverrideCount =
            g_state.sticky.vertexShaderInputOverrideCount;
        if (frame->maxVertexShaderInputOverrideCount <
            g_state.sticky.vertexShaderInputOverrideCount) {
            frame->maxVertexShaderInputOverrideCount =
                g_state.sticky.vertexShaderInputOverrideCount;
        }
        if (vertexShaderInputOverrideCount > 0) {
            if (!frame->hasVertexShaderInputDetails) {
                frame->hasVertexShaderInputDetails = true;
                frame->minVertexShaderInputStride = vertexShaderInputStride;
                frame->maxVertexShaderInputStride = vertexShaderInputStride;
                frame->minVertexShaderInputOffset = vertexShaderInputOffset;
                frame->maxVertexShaderInputOffset = vertexShaderInputOffset;
            } else {
                if (vertexShaderInputStride < frame->minVertexShaderInputStride) {
                    frame->minVertexShaderInputStride = vertexShaderInputStride;
                }
                if (vertexShaderInputStride > frame->maxVertexShaderInputStride) {
                    frame->maxVertexShaderInputStride = vertexShaderInputStride;
                }
                if (vertexShaderInputOffset < frame->minVertexShaderInputOffset) {
                    frame->minVertexShaderInputOffset = vertexShaderInputOffset;
                }
                if (vertexShaderInputOffset > frame->maxVertexShaderInputOffset) {
                    frame->maxVertexShaderInputOffset = vertexShaderInputOffset;
                }
            }
            frame->lastVertexShaderInputStride = vertexShaderInputStride;
            frame->lastVertexShaderInputOffset = vertexShaderInputOffset;
        }
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
