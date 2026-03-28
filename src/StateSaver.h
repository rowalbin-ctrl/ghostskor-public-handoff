#pragma once
#include <d3d11.h>

#ifndef D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
#define D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE 16
#endif

#ifndef D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
#define D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT 8
#endif

class D3D11StateSaver {
  bool m_saved;
  ID3D11DeviceContext *m_pContext;

  // Shader Stages
  ID3D11VertexShader *m_pVS;
  ID3D11PixelShader *m_pPS;
  ID3D11GeometryShader *m_pGS;
  ID3D11HullShader *m_pHS;
  ID3D11DomainShader *m_pDS;
  ID3D11ComputeShader *m_pCS;

  // Input Assembly
  ID3D11InputLayout *m_pInputLayout;
  D3D11_PRIMITIVE_TOPOLOGY m_topology;
  ID3D11Buffer *m_pIndexBuffer;
  DXGI_FORMAT m_indexBufferFormat;
  UINT m_indexBufferOffset;
  ID3D11Buffer *m_pVB[2]; // Save 2 VBs just in case
  UINT m_vbStride[2];
  UINT m_vbOffset[2];

  // Rasterizer
  ID3D11RasterizerState *m_pRS;
  UINT m_numViewports;
  D3D11_VIEWPORT
  m_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  UINT m_numScissorRects;
  D3D11_RECT
  m_scissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];

  // Output Merger
  ID3D11DepthStencilState *m_pDSS;
  UINT m_stencilRef;
  ID3D11BlendState *m_pBlendState;
  FLOAT m_blendFactor[4];
  UINT m_sampleMask;
  ID3D11RenderTargetView *m_pRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
  ID3D11DepthStencilView *m_pDSV;

  // Resources (Saving first 4 slots to be safe)
  ID3D11ShaderResourceView *m_pSRV[4];
  ID3D11SamplerState *m_pSampler[4];

public:
  D3D11StateSaver(ID3D11DeviceContext *pContext)
      : m_pContext(pContext), m_saved(false) {
    m_pVS = nullptr;
    m_pPS = nullptr;
    m_pGS = nullptr;
    m_pHS = nullptr;
    m_pDS = nullptr;
    m_pCS = nullptr;
    m_pInputLayout = nullptr;
    m_pIndexBuffer = nullptr;
    for (int i = 0; i < 2; i++)
      m_pVB[i] = nullptr;
    m_pRS = nullptr;
    m_pDSS = nullptr;
    m_pBlendState = nullptr;
    m_pDSV = nullptr;
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
      m_pRTVs[i] = nullptr;
    for (int i = 0; i < 4; i++) {
      m_pSRV[i] = nullptr;
      m_pSampler[i] = nullptr;
    }
  }

  ~D3D11StateSaver() {
    if (m_saved)
      Restore();
  }

  void Save() {
    if (!m_pContext)
      return;
    m_pContext->AddRef();

    // Shaders
    m_pContext->VSGetShader(&m_pVS, NULL, NULL);
    m_pContext->PSGetShader(&m_pPS, NULL, NULL);
    m_pContext->GSGetShader(&m_pGS, NULL, NULL);
    m_pContext->HSGetShader(&m_pHS, NULL, NULL);
    m_pContext->DSGetShader(&m_pDS, NULL, NULL);
    m_pContext->CSGetShader(&m_pCS, NULL, NULL);

    // IA
    m_pContext->IAGetInputLayout(&m_pInputLayout);
    m_pContext->IAGetPrimitiveTopology(&m_topology);
    m_pContext->IAGetIndexBuffer(&m_pIndexBuffer, &m_indexBufferFormat,
                                 &m_indexBufferOffset);
    m_pContext->IAGetVertexBuffers(0, 2, m_pVB, m_vbStride, m_vbOffset);

    // RS
    m_pContext->RSGetState(&m_pRS);
    m_numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    m_pContext->RSGetViewports(&m_numViewports, m_viewports);
    m_numScissorRects =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    m_pContext->RSGetScissorRects(&m_numScissorRects, m_scissorRects);

    // OM
    m_pContext->OMGetDepthStencilState(&m_pDSS, &m_stencilRef);
    m_pContext->OMGetBlendState(&m_pBlendState, m_blendFactor, &m_sampleMask);
    m_pContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                   m_pRTVs, &m_pDSV);

    // Resources (PS only for now as that's where we usually conflict)
    m_pContext->PSGetShaderResources(0, 4, m_pSRV);
    m_pContext->PSGetSamplers(0, 4, m_pSampler);

    m_saved = true;
  }

  void Restore() {
    if (!m_saved || !m_pContext)
      return;

    // Shaders
    m_pContext->VSSetShader(m_pVS, NULL, 0);
    m_pContext->PSSetShader(m_pPS, NULL, 0);
    m_pContext->GSSetShader(m_pGS, NULL, 0);
    m_pContext->HSSetShader(m_pHS, NULL, 0);
    m_pContext->DSSetShader(m_pDS, NULL, 0);
    m_pContext->CSSetShader(m_pCS, NULL, 0);

    // IA
    m_pContext->IASetInputLayout(m_pInputLayout);
    m_pContext->IASetPrimitiveTopology(m_topology);
    m_pContext->IASetIndexBuffer(m_pIndexBuffer, m_indexBufferFormat,
                                 m_indexBufferOffset);
    m_pContext->IASetVertexBuffers(0, 2, m_pVB, m_vbStride, m_vbOffset);

    // RS
    m_pContext->RSSetState(m_pRS);
    m_pContext->RSSetViewports(m_numViewports, m_viewports);
    m_pContext->RSSetScissorRects(m_numScissorRects, m_scissorRects);

    // OM
    m_pContext->OMSetDepthStencilState(m_pDSS, m_stencilRef);
    m_pContext->OMSetBlendState(m_pBlendState, m_blendFactor, m_sampleMask);
    m_pContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                   m_pRTVs, m_pDSV);

    // Resources
    m_pContext->PSSetShaderResources(0, 4, m_pSRV);
    m_pContext->PSSetSamplers(0, 4, m_pSampler);

    Release();
    m_saved = false;
    m_pContext->Release();
    m_pContext = nullptr;
  }

  void Release() {
    if (m_pVS) {
      m_pVS->Release();
      m_pVS = nullptr;
    }
    if (m_pPS) {
      m_pPS->Release();
      m_pPS = nullptr;
    }
    if (m_pGS) {
      m_pGS->Release();
      m_pGS = nullptr;
    }
    if (m_pHS) {
      m_pHS->Release();
      m_pHS = nullptr;
    }
    if (m_pDS) {
      m_pDS->Release();
      m_pDS = nullptr;
    }
    if (m_pCS) {
      m_pCS->Release();
      m_pCS = nullptr;
    }

    if (m_pInputLayout) {
      m_pInputLayout->Release();
      m_pInputLayout = nullptr;
    }
    if (m_pIndexBuffer) {
      m_pIndexBuffer->Release();
      m_pIndexBuffer = nullptr;
    }
    for (int i = 0; i < 2; i++) {
      if (m_pVB[i]) {
        m_pVB[i]->Release();
        m_pVB[i] = nullptr;
      }
    }

    if (m_pRS) {
      m_pRS->Release();
      m_pRS = nullptr;
    }

    if (m_pDSS) {
      m_pDSS->Release();
      m_pDSS = nullptr;
    }
    if (m_pBlendState) {
      m_pBlendState->Release();
      m_pBlendState = nullptr;
    }
    if (m_pDSV) {
      m_pDSV->Release();
      m_pDSV = nullptr;
    }
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
      if (m_pRTVs[i]) {
        m_pRTVs[i]->Release();
        m_pRTVs[i] = nullptr;
      }
    }

    for (int i = 0; i < 4; i++) {
      if (m_pSRV[i]) {
        m_pSRV[i]->Release();
        m_pSRV[i] = nullptr;
      }
      if (m_pSampler[i]) {
        m_pSampler[i]->Release();
        m_pSampler[i] = nullptr;
      }
    }
  }
};
